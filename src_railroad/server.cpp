#include "datagram.h"
#include "transport.h"
#include "utils.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

struct RRServerClient
{
    rr_sock_handle Socket;
    sockaddr_in Address;
    unsigned long Sequence;
};

struct ClientFrame
{
    rr_sock_handle Socket;
    Frame FrameContent;
};

struct RRServer
{
    int fd;
    sockaddr_in serverAddress;
    std::thread* loopThread;
    std::mutex* rxLock;
    std::mutex* txLock;
    std::mutex* clientsLock;
    std::deque<ClientFrame>* rx;
    std::deque<ClientFrame>* tx;
    std::unordered_map<rr_sock_handle, RRServerClient>* clients;
};

static std::atomic<unsigned long> idAllocator{0};
static std::unordered_map<rr_server_handle, RRServer> serverHandles;

// Itera sobre a fila dada, removendo os elementos cuja função de teste retornar true
std::vector<ClientFrame> queue_filter_truthy(std::deque<ClientFrame>* queue,
                                             bool filter(ClientFrame& frame, bool* stop))
{
    std::vector<ClientFrame> removed;
    bool stop = false;

    // Verificar se algum item da fila de recepção corresponde ao datagrama enviado
    for (auto it = queue->begin(); it != queue->end();)
    {
        ClientFrame& item = (*it);

        if (filter(item, &stop))
        {
            removed.push_back(item);
            queue->erase(it);
        }
        else
        {
            ++it;
        }

        if (stop)
            break;
    }

    return removed;
}

std::optional<ClientFrame> queue_take_first(std::deque<ClientFrame>* queue,
                                            const std::function<bool(ClientFrame frame)>& filter)
{
    // Verificar se algum item da fila de recepção corresponde ao datagrama enviado
    for (auto it = queue->begin(); it != queue->end();)
    {
        ClientFrame& item = (*it);

        if (filter(item))
        {
            queue->erase(it);
            return item;
        }
        else
        {
            ++it;
        }
    }

    return std::nullopt;
}

void rr_server_thread_loop(rr_server_handle serverHandle)
{
    using namespace std::chrono_literals;

    printf("rr_server_thread_loop(%ld): Thread iniciada\n", serverHandle);

    while (true)
    {
        // Se o handle não existe mais, finalizar thread
        if (!serverHandles.count(serverHandle))
            break;

        RRServer& server = serverHandles.at(serverHandle);

        // transmitir itens da fila de transmissão...
        server.txLock->lock();
        while (!server.tx->empty())
        {
            // Lidar com mensagem na frente da fila
            ClientFrame message = server.tx->front();

            server.clientsLock->lock();

            if (server.clients->count(message.Socket))
            {
                RRServerClient& client = server.clients->at(message.Socket);

                // Loop de (re)envio de datagrama com a mensagem
                while (true)
                {
                    printf("rr_server_thread_loop: Enviando packet para socket %ld\n",
                           client.Socket);
                    sendto(server.fd, &message, sizeof(message), 0, (sockaddr*)&client.Address,
                           sizeof(client.Address));

                    printf("rr_server_thread_loop: Aguardando ACK\n");
                    auto receivedDatagramOrNull = rr_datagram_receive<Frame>(server.fd);

                    if (receivedDatagramOrNull.has_value())
                    {
                        // Verificar identidade do datagrama
                        auto& receivedFrame = receivedDatagramOrNull.value();

                        // Se o pacote recebido for um pacote não-ACK, descartá-lo (o remetente vai
                        // reenviar eventualmente). Queremos exclusivamente um ACK
                        if (receivedFrame.Body.Flags.Ack)
                        {
                            // Ack!
                            printf("rr_server_thread_loop: ACK recebido\n");
                            break;
                        }
                        else
                        {
                            // Não-Ack
                            printf("rr_server_thread_loop: Esperava ACK, recebeu algo não-ACK; "
                                   "descartando-o na esperança de ser reenviado\n");
                        }
                    }
                    else
                    {
                        // Nenhum datagrama recebido durante o timeout; reenviar pacote
                        fprintf(stderr,
                                "rr_server_thread_loop: Erro ao ler datagrama: %s (errno: %d)\n",
                                strerror(errno), errno);
                        continue;
                    }

                    std::this_thread::sleep_for(250ms);
                }
            }
            else
            {
                // O socket para qual essa mensagem está enfileirada não existe mais. Descartar
                // mensagem
                fprintf(stderr, "rr_server_thread_loop: O socket para qual essa mensagem está "
                                "enfileirada não existe mais, descartando...\n");
            }

            server.clientsLock->unlock();

            // Avançar fila
            server.tx->pop_front();
        }
        server.txLock->unlock();

        // receber datagramas pendentes...
        server.rxLock->lock();
        auto receivedDatagramOrNull = rr_datagram_receive<Frame>(server.fd);
        if (receivedDatagramOrNull.has_value())
        {
            // Verificar identidade do datagrama
            auto& receivedFrame = receivedDatagramOrNull.value();

            // Não queremos ACK nesse estágio; descartá-los (os ACKs nunca devem ser enfileirados)
            if (receivedFrame.Body.Flags.Ack)
            {
                printf("rr_server_thread_loop: ACK recebido em momento inesperado, ignorando-o\n");
            }
            else
            {
                // Correlacionar address:port do datagrama com o socket_handle do cliente
                rr_sock_handle foundSocketHandle = -1;

                for (auto& [socketHandle, searchClient] : *server.clients)
                {
                    if (socketAddressEqual(searchClient.Address, receivedFrame.SourceAddress))
                    {
                        foundSocketHandle = socketHandle;
                        break;
                    }
                }

                printf("rr_server_thread_loop: Quadro recebido\n");
                server.rx->push_back(ClientFrame{
                    .Socket = foundSocketHandle,
                    .FrameContent = receivedFrame.Body,
                });
            }
        }
        else
        {
            // Nenhum datagrama recebido durante o timeout; reenviar pacote
            fprintf(stderr, "rr_server_thread_loop: Erro ao ler datagrama: %s (errno: %d)\n",
                    strerror(errno), errno);
        }
        server.rxLock->unlock();

        std::this_thread::sleep_for(1ms);
    }

    printf("rr_server_thread_loop: finalizando");
}

rr_server_handle rr_server_bind(std::string listenAddress, unsigned short listenPort)
{
    printf("rr_server_bind: Bind no endereço %s e porta %d\n", listenAddress.c_str(), listenPort);

    struct sockaddr_in serverAddress;

    // Setar o IP e a porta no qual o servidor escutará
    std::memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;                                        // IPv4
    serverAddress.sin_addr.s_addr = ConvertIPv4StringToAddress(listenAddress); // IP dado
    serverAddress.sin_port = htons(listenPort);                                // Porta dada

    // Declarar um socket
    int socketFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketFd < 0)
    {
        fprintf(stderr, "rr_server_bind: erro ao criar o socket: %s\n", strerror(errno));
        abort();
    }

    // Definir timeouts para funções de leitura (ex. receber um datagrama)
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000; // 500 ms
    setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    // Fazer o listening
    int err = bind(socketFd, (const struct sockaddr*)&serverAddress, sizeof(serverAddress));
    if (err < 0)
    {
        fprintf(stderr, "rr_server_bind: Erro ao fazer bind: %s\n", strerror(errno));
        close(socketFd);
        abort();
    }

    printf("rr_server_bind: Bind OK! Abrindo thread...\n");

    rr_server_handle handle = idAllocator++;

    serverHandles[handle] = RRServer{
        .fd = socketFd,
        .serverAddress = serverAddress,
        .loopThread = new std::thread(&rr_server_thread_loop, handle),
        .rxLock = new std::mutex(),
        .txLock = new std::mutex(),
        .clientsLock = new std::mutex(),
        .rx = new std::deque<ClientFrame>(),
        .tx = new std::deque<ClientFrame>(),
        .clients = new std::unordered_map<rr_sock_handle, RRServerClient>(),
    };

    // Deixar thread executando após essa função retornar
    serverHandles[handle].loopThread->detach();

    return handle;
}

rr_sock_handle rr_server_accept_client(rr_server_handle serverHandle)
{
    printf("rr_server_accept_client: Aguardando nova conexão (quadro SYN)...\n");

    // Aguardar mensagem de SYN na fila
    // Criar registro para o cliente
    // Enviar datagrama de ACK

    while (true)
    {
        RRServer& server = serverHandles[serverHandle];

        using namespace std::chrono_literals;

        // Retornar o primeiro pacote SYN da fila de recepção
        server.rxLock->lock();
        auto filter = [](ClientFrame frame) { return frame.FrameContent.Flags.Syn; };
        auto synFrame = queue_take_first(server.rx, filter);
        server.rxLock->unlock();

        if (synFrame.has_value())
        {
            // Nova conexão; registrar novos clientes
            auto packet = synFrame.value();
            printf("rr_server_accept_client: Novo quadro de SYN!\n");
            break;
        }

        std::this_thread::sleep_for(1ms);
    }

    return idAllocator++;
}

void rr_server_send(rr_sock_handle handle, const char* buffer, int bufferSize)
{
    printf("rr_send: Enviando %d bytes\n", bufferSize);

    size_t bytesToCopy = std::min(bufferSize, FRAME_BODY_LENGTH);

    Frame frame = {
        .SequenceId = 0,
        .Flags = {.Syn = false, .Ack = true, .Reserved = 0},
        .BodyLength = bytesToCopy,
        .Body = {0},
    };

    std::memcpy(&frame.Body, buffer, bytesToCopy);

    RRServer& server = serverHandles[handle];
    server.txLock->lock();
    server.tx->push_back(ClientFrame{.Socket = handle, .FrameContent = frame});
    server.txLock->unlock();

    // aguardar ACK
    while (true)
    {
        using namespace std::chrono_literals;

        server.rxLock->lock();

        // Verificar se algum item da fila de recepção corresponde ao datagrama enviado
        for (auto it = server.rx->begin(); it != server.rx->end();)
        {
            // Frame& item = (*it);
        }

        server.rxLock->unlock();

        std::this_thread::sleep_for(1ms);
    }
}

size_t rr_server_receive(rr_sock_handle handle, char* buffer, int bufferSize)
{
    size_t bytesToRead = std::min(bufferSize, FRAME_BODY_LENGTH);

    // Aguardar quadro de dados na fila de recepção
    while (true)
    {
        using namespace std::chrono_literals;

        RRServer& server = serverHandles[handle];

        server.rxLock->lock();

        // Retornar a primeira mensagem da fila que corresponde ao socket e não é um ACK/SYN
        auto filter = [handle](ClientFrame frame) {
            return frame.Socket == handle && !frame.FrameContent.Flags.Ack &&
                   !frame.FrameContent.Flags.Syn;
        };

        auto receivedFrame = queue_take_first(server.rx, filter);

        server.rxLock->unlock();

        if (receivedFrame.has_value())
        {
            // Encontramos o pacote correspondente; escrever no buffer
            std::memcpy(buffer, receivedFrame.value().FrameContent.Body, bytesToRead);
            return bytesToRead;
        }

        std::this_thread::sleep_for(1ms);
    }

    return -1;
}

void rr_server_close(rr_server_handle serverHandle)
{
    if (!serverHandles.count(serverHandle))
    {
        // avisar, mas não tomar nenhuma ação
        fprintf(stderr,
                "rr_server_close: chamado mas não havia um servidor aberto com o handle %ld\n",
                serverHandle);
        return;
    }

    RRServer& server = serverHandles[serverHandle];
    delete server.loopThread;
    delete server.rxLock;
    delete server.txLock;
    delete server.clientsLock;
    delete server.rx;
    delete server.tx;
    delete server.clients;
    serverHandles.erase(serverHandle);
}
