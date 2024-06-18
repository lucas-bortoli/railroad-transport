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
    sockaddr_in Address;
    rr_sock_handle RRHandle;
    unsigned long Sequence;
};

struct ClientFrame
{
    sockaddr_in ClientAddress;
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

template <typename T>
static std::optional<T> queue_take_first(std::deque<T>* queue,
                                         const std::function<bool(T item)>& filter)
{
    // Verificar se algum item da fila de recepção corresponde ao datagrama enviado
    for (auto it = queue->begin(); it != queue->end();)
    {
        T& item = (*it);

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

static std::optional<RRServerClient> get_client_from_client_handle(rr_server_handle serverHandle,
                                                                   rr_sock_handle clientHandle)
{
    if (!serverHandles.count(serverHandle))
        return std::nullopt;

    RRServer server = serverHandles.at(serverHandle);

    for (auto& [clientAddress, clientInfo] : *server.clients)
    {
        if (clientInfo.RRHandle == clientHandle)
        {
            return clientInfo;
        }
    }

    return std::nullopt;
}

std::optional<RRServerClient> get_client_from_client_address(rr_server_handle serverHandle,
                                                             sockaddr_in clientAddress)
{
    if (!serverHandles.count(serverHandle))
        return std::nullopt;

    RRServer server = serverHandles.at(serverHandle);

    for (auto& [_, clientInfo] : *server.clients)
    {
        if (socketAddressEqual(clientInfo.Address, clientAddress))
        {
            return clientInfo;
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
            auto clientOrNull = get_client_from_client_address(serverHandle, message.ClientAddress);
            if (clientOrNull.has_value())
            {
                RRServerClient& client = clientOrNull.value();

                // Loop de (re)envio de datagrama com a mensagem
                while (true)
                {
                    printf("rr_server_thread_loop: Enviando packet para socket %ld\n",
                           client.RRHandle);
                    rr_datagram_send(server.fd, SentDatagram<Frame>{
                                                    .TargetAddress = message.ClientAddress,
                                                    .Body = message.FrameContent,
                                                });

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
                printf("rr_server_thread_loop: Quadro (S=%d,A=%d) recebido, enviando ACK\n",
                       receivedFrame.Body.Flags.Syn, receivedFrame.Body.Flags.Ack);
                server.rx->push_back(ClientFrame{
                    .ClientAddress = receivedFrame.SourceAddress,
                    .FrameContent = receivedFrame.Body,
                });

                bool synAck = receivedFrame.Body.Flags.Syn;

                // Ao receber alguma coisa, temos que devolver um ACK
                rr_datagram_send<Frame>(
                    server.fd, SentDatagram<Frame>{
                                   .TargetAddress = receivedFrame.SourceAddress,
                                   .Body =
                                       {
                                           .SequenceId = receivedFrame.Body.SequenceId,
                                           .Flags = {.Syn = synAck, .Ack = true, .Reserved = 0},
                                           .BodyLength = 0,
                                           .Body = {0},
                                       },
                               });
            }
        }
        server.rxLock->unlock();

        std::this_thread::sleep_for(1ms);
    }

    printf("rr_server_thread_loop: Finalizando");
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
        fprintf(stderr, "rr_server_bind: Erro ao criar o socket: %s\n", strerror(errno));
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
    serverHandles.at(handle).loopThread->detach();

    return handle;
}

rr_sock_handle rr_server_accept_client(rr_server_handle serverHandle)
{
    if (!serverHandles.count(serverHandle))
    {
        fprintf(stderr,
                "rr_server_accept_client: Tentou usar uma handle de servidor que não existe "
                "(%ld)\n",
                serverHandle);
        abort();
    }

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
        server.clientsLock->lock();

        // Retornar quadro SYN de um cliente não registrado ainda
        auto synFrameOrNull = queue_take_first<ClientFrame>(server.rx, [&server, serverHandle](
                                                                           ClientFrame frame) {
            return frame.FrameContent.Flags.Syn &&
                   !get_client_from_client_address(serverHandle, frame.ClientAddress).has_value();
        });
        server.clientsLock->unlock();
        server.rxLock->unlock();

        if (synFrameOrNull.has_value())
        {
            // Nova conexão; registrar novos clientes
            auto synFrame = synFrameOrNull.value();
            printf("rr_server_accept_client: Quadro de SYN na fila, removido\n");

            rr_sock_handle socketHandle = idAllocator++;

            server.clientsLock->lock();
            server.clients->insert_or_assign(socketHandle, RRServerClient{
                                                               .Address = synFrame.ClientAddress,
                                                               .RRHandle = socketHandle,
                                                               .Sequence = 1,
                                                           });

            // Nesse ponto, a thread de loop já mandou um SYN/ACK.

            server.clientsLock->unlock();

            return socketHandle;
        }

        std::this_thread::sleep_for(1ms);
    }

    // Nunca vai acontecer; loop infinito até receber novo cliente
    return -1;
}

void rr_server_send(rr_server_handle serverHandle, rr_sock_handle clientHandle, const char* buffer,
                    int bufferSize)
{
    if (!serverHandles.count(serverHandle))
    {
        fprintf(stderr,
                "rr_server_send: Tentou usar uma handle de servidor que não existe "
                "(%ld)\n",
                serverHandle);
        abort();
    }

    printf("rr_send: Enviando %d bytes\n", bufferSize);

    unsigned int bytesToCopy = std::min(bufferSize, FRAME_BODY_LENGTH);

    Frame frame = {
        .SequenceId = 0,
        .Flags = {.Syn = false, .Ack = false, .Reserved = 0},
        .BodyLength = bytesToCopy,
        .Body = {0},
    };

    std::memcpy(&frame.Body, buffer, bytesToCopy);

    RRServer& server = serverHandles[serverHandle];
    server.clientsLock->lock();
    auto targetClient = get_client_from_client_handle(serverHandle, clientHandle);
    if (targetClient.has_value())
    {
        server.txLock->lock();
        server.tx->push_back(ClientFrame{
            .ClientAddress = targetClient.value().Address,
            .FrameContent = frame,
        });
        server.txLock->unlock();
    }
    server.clientsLock->unlock();

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

size_t rr_server_receive(rr_server_handle serverHandle, rr_sock_handle clientHandle, char* buffer,
                         int bufferSize)
{
    if (!serverHandles.count(serverHandle))
    {
        fprintf(stderr,
                "rr_server_send: Tentou usar uma handle de servidor que não existe "
                "(%ld)\n",
                serverHandle);
        abort();
    }

    RRServer& server = serverHandles[serverHandle];

    printf("rr_server_receive: Aguardando dados...\n");

    // Aguardar quadro de dados na fila de recepção
    while (true)
    {
        using namespace std::chrono_literals;

        RRServer& server = serverHandles.at(serverHandle);

        server.clientsLock->lock();
        auto targetClient = get_client_from_client_handle(serverHandle, clientHandle);
        server.clientsLock->unlock();

        if (!targetClient.has_value())
        {
            fprintf(stderr, "rr_server_receive: tentou ler de um cliente que não existe (%ld)\n",
                    clientHandle);
            abort();
        }

        auto targetClientAddress = targetClient.value().Address;

        // Retornar a primeira mensagem da fila que corresponde ao socket e não é um ACK/SYN
        server.rxLock->lock();
        auto receivedFrame =
            queue_take_first<ClientFrame>(server.rx, [targetClientAddress](ClientFrame frame) {
                return socketAddressEqual(frame.ClientAddress, targetClientAddress) &&
                       !frame.FrameContent.Flags.Ack && !frame.FrameContent.Flags.Syn;
            });
        server.rxLock->unlock();

        if (receivedFrame.has_value())
        {
            // Encontramos o pacote correspondente; escrever no buffer de destino
            size_t bytesToRead = std::min(bufferSize, FRAME_BODY_LENGTH);
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

    RRServer& server = serverHandles.at(serverHandle);
    delete server.loopThread;
    delete server.rxLock;
    delete server.txLock;
    delete server.clientsLock;
    delete server.rx;
    delete server.tx;
    delete server.clients;
    serverHandles.erase(serverHandle);
}
