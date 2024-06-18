#include "transport.h"
#include "utils.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
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
    Frame Frame;
};

struct RRServer
{
    int fd;
    sockaddr_in serverAddress;
    std::thread* loopThread;
    std::mutex* rxLock;
    std::mutex* txLock;
    std::deque<ClientFrame>* rx;
    std::deque<ClientFrame>* tx;
};

std::atomic<unsigned long> idAllocator{0};
std::unordered_map<rr_server_handle, RRServer> serverHandles;

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

void rr_server_thread_loop(rr_server_handle serverHandle)
{
    using namespace std::chrono_literals;

    printf("rr_server_thread_loop(%ld): Thread iniciada\n", serverHandle);

    while (true)
    {
        // transmitir itens da fila de transmissão...

        // aguardar ACK respectivo...

        std::this_thread::sleep_for(1ms);
    }
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
    tv.tv_usec = 1000; // 1 ms
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
        .rx = new std::deque<ClientFrame>(),
        .tx = new std::deque<ClientFrame>(),
    };

    // Deixar thread executando após essa função retornar
    serverHandles[handle].loopThread->detach();

    return handle;
}

rr_sock_handle rr_server_accept_client(rr_server_handle serverHandle)
{
    printf("rr_server_accept_client: Aguardando pacote de SYN...\n");

    // Aguardar mensagem de SYN na fila
    // Criar registro para o cliente
    // Enviar datagrama de ACK

    while (true)
    {
        RRServer& server = serverHandles[serverHandle];

        using namespace std::chrono_literals;

        // Retornar o primeiro pacote SYN da fila de recepção
        server.rxLock->lock();
        auto newSynPackets = queue_filter_truthy(server.rx, [](ClientFrame& frame, bool* stop) {
            if (frame.Socket == -1 && frame.Frame.Flags.Syn)
            {
                (*stop) = true;
                return true;
            }
            return false;
        });
        server.rxLock->unlock();

        if (newSynPackets.size() > 0)
        {
            // Nova conexão; registrar novos clientes
            auto& packet = newSynPackets.front();

            printf("rr_server_accept_client: Novo cliente\n");

            break;
        }

        std::this_thread::sleep_for(1ms);
    }

    return idAllocator++;
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
    delete server.rx;
    delete server.tx;
    serverHandles.erase(serverHandle);
}

// Bloqueia até a conexão ser aceita
rr_sock_handle rr_connect(std::string address, unsigned short port)
{
    printf("rr_connect: Conectando ao servidor %s com porta %d\n", address.c_str(), port);

    return idAllocator++;
}

void rr_send(rr_sock_handle handle, const char* buffer, int bufferSize)
{
    printf("rr_send: Enviando %d bytes\n", bufferSize);

    Frame frame = {
        .SequenceId = 0,
        .Flags = {.Syn = false, .Ack = true, .Reserved = 0},
        .BodyLength = bufferSize,
        .Body = {0},
    };

    size_t bytesToCopy = std::min(bufferSize, FRAME_BODY_LENGTH);
    std::memcpy(frame.Body, buffer, bytesToCopy);

    RRServer& server = serverHandles[handle];
    server.txLock->lock();
    server.tx->push_front(ClientFrame{.Socket = handle, .Frame = frame});
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

size_t rr_receive(rr_sock_handle handle, char* buffer, int bufferSize)
{
    size_t bytesToRead = std::min(bufferSize, FRAME_BODY_LENGTH);

    return 0;
}