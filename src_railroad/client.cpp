#include "datagram.h"
#include "transport.h"
#include "utils.h"
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <unistd.h>

struct RRClient
{
    int fd;
    sockaddr_in serverAddress;
    std::thread* loopThread;
    std::mutex* rxLock;
    std::mutex* txLock;
    std::deque<Frame>* rx;
    std::deque<Frame>* tx;
};

static std::atomic<unsigned long> idAllocator{0};
static std::unordered_map<rr_sock_handle, RRClient> clientHandles;

void rr_client_thread_loop(rr_sock_handle handle)
{
    using namespace std::chrono_literals;

    printf("rr_client_thread_loop(%ld): Thread iniciada\n", handle);

    while (true)
    {
        // Se o handle não existe mais, finalizar thread
        if (!clientHandles.count(handle))
            break;

        RRClient& client = clientHandles.at(handle);

        // receber datagramas pendentes...
        client.rxLock->lock();
        // ...
        client.rxLock->unlock();

        // transmitir itens da fila de transmissão...
        client.txLock->lock();
        while (!client.tx->empty())
        {
            // Lidar com mensagem na frente da fila
            Frame message = client.tx->front();

            // Loop de (re)envio de datagrama com a mensagem
            while (true)
            {
                printf("rr_client_thread_loop: Enviando packet\n");
                sendto(client.fd, &message, sizeof(message), 0, (sockaddr*)&client.serverAddress,
                       sizeof(client.serverAddress));

                printf("rr_client_thread_loop: Aguardando ACK\n");

                auto receivedDatagramOrNull = rr_datagram_receive<Frame>(client.fd);
                if (receivedDatagramOrNull.has_value())
                {
                    // Se o pacote recebido for um pacote não-ACK, descartá-lo (o remetente vai
                    // reenviar eventualmente). Queremos exclusivamente um ACK

                    auto& receivedFrame = receivedDatagramOrNull.value();

                    if (receivedFrame.Body.Flags.Ack)
                    {
                        // Ack!
                        printf("rr_client_thread_loop: ACK recebido\n");
                        // TODO: ação após receber ack (dar return no client.send)
                        break;
                    }
                    else
                    {
                        // Não-Ack
                        printf("rr_client_thread_loop: Esperava ACK, recebeu algo não-ACK; "
                               "descartando-o na esperança de ser reenviado\n");
                    }
                }
                else
                {
                    // Nenhum datagrama recebido durante o timeout; reenviar pacote
                    fprintf(stderr,
                            "rr_client_thread_loop: Erro ao ler datagrama: %s (errno: %d)\n",
                            strerror(errno), errno);
                    continue;
                }

                std::this_thread::sleep_for(250ms);
            }

            // Avançar fila de transmissão
            client.tx->pop_front();
        }
        client.txLock->unlock();

        // receber datagramas pendentes...
        client.rxLock->lock();
        auto receivedDatagramOrNull = rr_datagram_receive<Frame>(client.fd);
        if (receivedDatagramOrNull.has_value())
        {
            // Verificar identidade do datagrama
            auto& receivedFrame = receivedDatagramOrNull.value();

            // Não queremos ACK nesse estágio; descartá-los (os ACKs nunca devem ser enfileirados)
            if (receivedFrame.Body.Flags.Ack)
            {
                printf("rr_client_thread_loop: ACK recebido em momento inesperado, ignorando-o\n");
            }
            else
            {
                printf("rr_client_thread_loop: Quadro recebido\n");
                client.rx->push_back(receivedFrame.Body);
            }
        }
        else
        {
            // Nenhum datagrama recebido durante o timeout; reenviar pacote
            fprintf(stderr, "rr_client_thread_loop: Erro ao ler datagrama: %s (errno: %d)\n",
                    strerror(errno), errno);
        }
        client.rxLock->unlock();

        std::this_thread::sleep_for(1ms);
    }

    printf("rr_client_thread_loop: finalizando");
}

// Bloqueia até a conexão ser aceita
rr_sock_handle rr_client_connect(std::string address, unsigned short port)
{
    printf("rr_client_connect: Conectando ao servidor %s com porta %d\n", address.c_str(), port);

    struct sockaddr_in serverAddress;

    // Setar o IP e a porta no qual conectaremos
    std::memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;                                  // IPv4
    serverAddress.sin_addr.s_addr = ConvertIPv4StringToAddress(address); // IP dado
    serverAddress.sin_port = htons(port);                                // Porta dada

    // Declarar um socket
    int socketFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socketFd < 0)
    {
        fprintf(stderr, "rr_client_connect: erro ao criar o socket: %s\n", strerror(errno));
        abort();
    }

    // Definir timeouts para funções de leitura (ex. receber um datagrama)
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000; // 500 ms
    setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    rr_sock_handle handle = idAllocator++;

    clientHandles[handle] = RRClient{
        .fd = socketFd,
        .serverAddress = serverAddress,
        .loopThread = new std::thread(&rr_client_thread_loop, handle),
        .rxLock = new std::mutex(),
        .txLock = new std::mutex(),
        .rx = new std::deque<Frame>(),
        .tx = new std::deque<Frame>(),
    };

    // Deixar thread executando após essa função retornar
    clientHandles[handle].loopThread->detach();

    clientHandles[handle].txLock->lock();
    clientHandles[handle].tx->push_back(Frame{
        .SequenceId = 1,
        .Flags = {.Syn = true, .Ack = false, .Reserved = 0},
        .BodyLength = 0,
        .Body = {0},
    });
    clientHandles[handle].txLock->unlock();

    return handle;
}

void rr_client_send(rr_sock_handle handle, const char* buffer, int bufferSize)
{
    if (!clientHandles.count(handle))
    {
        fprintf(stderr, "rr_client_send: chamado mas não havia um socket aberto com o handle %ld\n",
                handle);
        abort();
    }

    size_t bytesToCopy = std::min(bufferSize, FRAME_BODY_LENGTH);

    Frame frame = {
        .SequenceId = 1,
        .Flags = {.Syn = false, .Ack = true, .Reserved = 0},
        .BodyLength = bytesToCopy,
        .Body = {0},
    };

    std::memcpy(&frame.Body, buffer, bytesToCopy);

    clientHandles.at(handle).txLock->lock();
    clientHandles.at(handle).tx->push_back(frame);
    clientHandles.at(handle).txLock->unlock();
}

size_t rr_client_receive(rr_sock_handle handle, char* buffer, int bufferSize)
{
    if (!clientHandles.count(handle))
    {
        fprintf(stderr,
                "rr_client_receive: chamado mas não havia um socket aberto com o handle %ld\n",
                handle);
        abort();
    }

    return 0;
}

void rr_client_close(rr_sock_handle handle)
{
    if (!clientHandles.count(handle))
    {
        // avisar, mas não tomar nenhuma ação
        fprintf(stderr,
                "rr_client_close: chamado mas não havia um socket aberto com o handle %ld\n",
                handle);
        return;
    }

    RRClient& client = clientHandles[handle];
    delete client.loopThread;
    delete client.rxLock;
    delete client.txLock;
    delete client.rx;
    delete client.tx;
    clientHandles.erase(handle);
}