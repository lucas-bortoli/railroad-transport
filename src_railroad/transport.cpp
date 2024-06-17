#include "transport.h"
#include "utils.h"
#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

struct RRServer
{
    sockaddr_in serverAddress;
    std::thread* loopThread;
    std::unique_lock<std::mutex>* rxLock;
    std::unique_lock<std::mutex>* txLock;
    std::deque<Frame>* rx;
    std::deque<Frame>* tx;
};

std::atomic<unsigned long> idAllocator{0};
std::unordered_map<rr_server_handle, RRServer> serverHandles;

void rr_server_thread_loop(rr_server_handle serverHandle)
{
    using namespace std::chrono_literals;

    printf("rr_server_thread_loop(%ld): started\n", serverHandle);

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
        .serverAddress = serverAddress,
        .loopThread = new std::thread(&rr_server_thread_loop, handle),
        .rxLock = new std::unique_lock<std::mutex>(),
        .txLock = new std::unique_lock<std::mutex>(),
        .rx = new std::deque<Frame>(),
        .tx = new std::deque<Frame>(),
    };
}

rr_sock_handle rr_server_accept_client(rr_server_handle serverHandle)
{
}

void rr_server_close(rr_server_handle serverHandle)
{
    if (!serverHandles.contains(serverHandle))
    {
        // avisar, mas não tomar nenhuma ação
        fprintf(stderr, "rr_server_close: chamado mas não havia um servidor aberto com o handle %ld\n", serverHandle);
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

rr_sock_handle rr_connect(std::string address, unsigned short port)
{
}

void rr_send(rr_sock_handle handle, char* buffer, int bufferSize)
{
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
    server.tx->push_front(frame);
    server.txLock->release();

    // aguardar ACK
    while (true)
    {
        using namespace std::chrono_literals;

        server.rxLock->lock();

        // Verificar se algum item da fila de recepção corresponde ao datagrama enviado
        for (auto it = server.rx->begin(); it != server.rx->end();)
        {
            Frame& item = (*it);
        }

        server.rxLock->release();

        std::this_thread::sleep_for(1ms);
    }
}

size_t rr_receive(rr_sock_handle handle)
{
}