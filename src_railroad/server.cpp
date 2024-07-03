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

#ifdef RR_SIMULATE_PACKET_LOSS_CHANCE
#include <stdlib.h> // rand()
#endif

struct RRServerClient
{
    // Identificador desse cliente na biblioteca
    rr_sock_handle handle;

    // O endereço desse cliente, onde os quadros transmitidos serão enviados ("destinatário")
    sockaddr_in address;

    // Sequência de quadros de transmissão. Ao receber um quadro, caso o campo Sequence do quadro
    // seja menor que esse valor, não deverá ser colocado na fila currentRx, pois entende-se que já
    // foi processado pela aplicação de servidor. No entanto, ainda deve-se responder ACK para o
    // quadro, para impedir a retransmissão do mesmo.
    unsigned long sequenceRx;

    // Contador simples de quadros transmitidos com sucesso (aqueles que receberam ACK com
    // respostas). A cada quadro NOVO gerado pelo servidor, esse valor deverá ser incrementado.
    unsigned long sequenceTx;

    // Indica se esse cliente enviou um quadro e ainda não recebeu um ACK.
    bool statusWaitingForAck;

    // Representa os quadros recebidos. Essa fila contém apenas quadros de FrameKind::Data. Quadros
    // de SYN/ACK são processados independentemente no loop principal de IO.
    std::deque<Frame>* rx;
    std::mutex* rxLock;

    // Representa os quadros de dados a serem transmitidos ainda. Ao receber um ACK, o quadro
    // recém-enviado é removido da fila.
    std::deque<Frame>* tx;
    std::mutex* txLock;

    // Timestamp em millisegundos do momento de envio do último datagrama de transmissão, usado para calcular timeout
    // dos ACKs e subsequente reenvio
    unsigned long long lastTxTimestamp;
};

struct RRServer
{
    int fd;
    sockaddr_in serverAddress;
    std::thread* ioThread;
    std::mutex* clientsLock;
    std::unordered_map<rr_sock_handle, RRServerClient>* clients;

    // Representa uma fila de clientes potenciais aguardando estabelecer conexão. Quando um quadro
    // SYN é recebido de um cliente desconhecido, o endereço do remetente é colocado nessa fila. A
    // função rr_server_accept_client deve ler essa fila, transmitir um datagrama ACK, e alocar
    // recursos para um novo RRServerClient.
    std::queue<sockaddr_in>* pendingSyn;
    std::mutex* pendingSynLock;

    // Quanto tempo, em millisegundos, esperar um ACK antes de retransmitir um quadro
    unsigned long long ackTimeout;
};

static std::atomic<unsigned long> idAllocator{0};
static std::unordered_map<rr_server_handle, RRServer> serverHandles;

RRServerClient* get_client_from_client_address(rr_server_handle serverHandle, sockaddr_in clientAddress)
{

    if (!serverHandles.count(serverHandle))
        return nullptr;

    // serverHandles is a std::unordered_map
    RRServer server = serverHandles.at(serverHandle);

    for (auto& [_, clientInfo] : *server.clients)
    {
        if (socketAddressEqual(clientInfo.address, clientAddress))
        {
            return &clientInfo;
        }
    }

    return nullptr;
}

void rr_invalidate_client(RRServerClient& client)
{
    delete client.rx;
    delete client.rxLock;
    delete client.tx;
    delete client.txLock;
}

void rr_server_thread_loop(rr_server_handle serverHandle)
{
    using namespace std::chrono_literals;

    printf("rr_server_thread_loop(%ld): Thread iniciada\n", serverHandle);

    while (true)
    {
        // Se o handle não existe mais, finalizar thread
        if (!serverHandles.count(serverHandle))
        {
            printf("rr_server_thread_loop(%ld): Handle de servidor não existe mais, finalizando\n", serverHandle);
            break;
        }

        RRServer& server = serverHandles.at(serverHandle);

        // Receber todos os pacotes da fila do kernel
        while (true)
        {
            auto datagramOrNull = rr_datagram_receive<Frame>(server.fd);

#ifdef RR_SIMULATE_PACKET_LOSS_CHANCE
            // simular packet loss ("RR_SIMULATE_PACKET_LOSS_CHANCE" % de chance de perder pacote)
            if ((rand() % 100) < RR_SIMULATE_PACKET_LOSS_CHANCE)
                continue;
#endif

            // Não há mais datagramas a receber
            if (!datagramOrNull.has_value())
                break;

            // Processar datagrama recebido
            auto& datagram = datagramOrNull.value();
            switch (datagram.Body.Kind)
            {
                case FrameKind::Syn: {
                    printf("rr_server_thread_loop: Pacote SYN\n");
                    server.pendingSynLock->lock();
                    // TODO: talvez verificar se já tem um SYN na fila para evitar duplicados?
                    server.pendingSyn->push(datagram.SourceAddress);
                    server.pendingSynLock->unlock();
                    break;
                }
                case FrameKind::Ack: {
                    // Direcionar ao cliente correto
                    std::lock_guard clientLock(*server.clientsLock);
                    auto clientOrNull = get_client_from_client_address(serverHandle, datagram.SourceAddress);
                    if (clientOrNull == nullptr)
                    {
                        fprintf(stderr, "rr_server_thread_loop: Pacote ACK de um cliente com endereço desconhecido\n");
                        break;
                    }

                    printf("rr_server_thread_loop: Pacote ACK\n");

                    auto& client = *clientOrNull;
                    client.statusWaitingForAck = false;
                    // Ao receber um ACK, remover pacote da fila de transmissão para evitar reenvio
                    client.txLock->lock();
                    if (!client.tx->empty())
                        client.tx->pop_front();
                    client.txLock->unlock();

                    break;
                }
                case FrameKind::Data: {
                    // Direcionar ao cliente correto
                    std::lock_guard clientLock(*server.clientsLock);
                    auto clientOrNull = get_client_from_client_address(serverHandle, datagram.SourceAddress);
                    if (clientOrNull == nullptr)
                    {
                        fprintf(stderr, "rr_server_thread_loop: Pacote DATA de um cliente com endereço desconhecido\n");
                        break;
                    }

                    auto& client = *clientOrNull;

                    if (datagram.Body.SequenceId < client.sequenceRx)
                    {
                        printf("rr_server_thread_loop: Pacote DATA (sequence %lu / %lu, descartado)\n",
                               datagram.Body.SequenceId, client.sequenceRx);
                    }
                    else
                    {
                        printf("rr_server_thread_loop: Pacote DATA (sequence %lu / %lu, aceito)\n",
                               datagram.Body.SequenceId, client.sequenceRx);
                        client.rxLock->lock();
                        client.rx->push_back(datagram.Body);
                        client.sequenceRx++;
                        client.rxLock->unlock();
                    }

                    // Responder ACK
                    auto ackReply = SentDatagram<Frame>{
                        .TargetAddress = datagram.SourceAddress,
                        .Body = {.Kind = FrameKind::Ack,
                                 .SequenceId = datagram.Body.SequenceId,
                                 .BodyLength = 0,
                                 .Body = {0}},
                    };
                    rr_datagram_send<Frame>(server.fd, ackReply);

                    break;
                }
            }
        };

        server.clientsLock->lock();
        for (auto& [_, client] : *server.clients)
        {
            // Transmitir o primeiro pacote da fila de envio:
            // 1. Não esteja aguardando nenhum ACK
            // 2. Esperou tempo demais para um ACK e está retransmitindo
            auto now = timeInMilliseconds();
            if (!client.statusWaitingForAck ||
                (client.statusWaitingForAck && now - client.lastTxTimestamp >= server.ackTimeout))
            {
                client.txLock->lock();
                if (!client.tx->empty())
                {
                    auto& frame = client.tx->front();

                    client.statusWaitingForAck = true;
                    client.lastTxTimestamp = timeInMilliseconds();
                    rr_datagram_send<Frame>(server.fd, SentDatagram<Frame>{
                                                           .TargetAddress = client.address,
                                                           .Body = frame,
                                                       });
                }
                client.txLock->unlock();
            }
        }
        server.clientsLock->unlock();

        // std::this_thread::sleep_for(1ms);
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
    tv.tv_usec = 100; // 0.1 ms
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
        .ioThread = nullptr,
        .clientsLock = new std::mutex(),
        .clients = new std::unordered_map<rr_sock_handle, RRServerClient>(),
        .pendingSyn = new std::queue<sockaddr_in>(),
        .pendingSynLock = new std::mutex(),
        .ackTimeout = 500, // timeout de 500 ms
    };

    // Criar thread e deixar thread executando após essa função retornar
    serverHandles.at(handle).ioThread = new std::thread(&rr_server_thread_loop, handle);
    serverHandles.at(handle).ioThread->detach();

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

    printf("rr_server_accept_client: Aguardando nova conexão...\n");

    while (true)
    {
        RRServer& server = serverHandles[serverHandle];

        using namespace std::chrono_literals;

        server.pendingSynLock->lock();
        if (!server.pendingSyn->empty())
        {
            // Aceitar conexão pendente
            auto clientAddress = server.pendingSyn->front();
            printf("rr_server_accept_client: Nova conexão encontrada, criando cliente...\n");

            rr_sock_handle socketHandle = idAllocator++;

            // Enviar ACK para sinalizar ao cliente que a conexão foi aceita
            auto ackReply = SentDatagram<Frame>{
                .TargetAddress = clientAddress,
                .Body =
                    {
                        .Kind = FrameKind::Ack,
                        .SequenceId = 1,
                        .BodyLength = 0,
                        .Body = {0},
                    },
            };
            rr_datagram_send<Frame>(server.fd, ackReply);

            server.clientsLock->lock();
            auto client = RRServerClient{
                .handle = socketHandle,
                .address = clientAddress,
                .sequenceRx = 1,
                .sequenceTx = 1,
                .statusWaitingForAck = false,
                .rx = new std::deque<Frame>(),
                .rxLock = new std::mutex(),
                .tx = new std::deque<Frame>(),
                .txLock = new std::mutex(),
                .lastTxTimestamp = 0,
            };
            server.clients->insert_or_assign(socketHandle, client);
            server.clientsLock->unlock();

            server.pendingSyn->pop();
            server.pendingSynLock->unlock();

            return socketHandle;
        }
        server.pendingSynLock->unlock();

        // std::this_thread::sleep_for(1ms);
    }

    // Nunca vai acontecer; loop infinito até receber novo cliente
    return -1;
}

void rr_server_send(rr_server_handle serverHandle, rr_sock_handle clientHandle, const char* buffer, int bufferSize)
{
    if (!serverHandles.count(serverHandle))
    {
        fprintf(stderr,
                "rr_server_send: Tentou usar uma handle de servidor que não existe "
                "(%ld)\n",
                serverHandle);
        abort();
    }

    unsigned int bytesToCopy = std::min(bufferSize, FRAME_BODY_LENGTH);

    RRServer& server = serverHandles[serverHandle];
    RRServerClient& client = server.clients->at(clientHandle);

    client.txLock->lock();
    Frame frame = {
        .Kind = FrameKind::Data,
        .SequenceId = client.sequenceTx++,
        .BodyLength = bytesToCopy,
        .Body = {0},
    };
    std::memcpy(&frame.Body, buffer, bytesToCopy);
    client.tx->push_back(frame);
    client.txLock->unlock();

    // Aguardar ACK
    while (true)
    {
        using namespace std::chrono_literals;

        client.txLock->lock();

        // Enquanto esse quadro estiver na fila de transmissão, ele não foi recebido ainda
        bool found = false;
        for (auto& queuedFrame : *client.tx)
        {
            if (queuedFrame.SequenceId == frame.SequenceId)
            {
                found = true;
                break;
            }
        }
        client.txLock->unlock();

        // Quadro saiu da fila de transmissão?
        if (!found)
        {
            return;
        }

        // std::this_thread::sleep_for(1ms);
    }
}

size_t rr_server_receive(rr_server_handle serverHandle, rr_sock_handle clientHandle, char* buffer, int bufferSize)
{
    if (!serverHandles.count(serverHandle))
    {
        fprintf(stderr,
                "rr_server_send: Tentou usar uma handle de servidor que não existe "
                "(%ld)\n",
                serverHandle);
        abort();
    }

    printf("rr_server_receive: Aguardando dados...\n");

    // Aguardar quadro de dados na fila de recepção
    while (true)
    {
        using namespace std::chrono_literals;

        RRServer& server = serverHandles.at(serverHandle);
        server.clientsLock->lock();
        RRServerClient& client = server.clients->at(clientHandle);

        client.rxLock->lock();
        if (!client.rx->empty())
        {
            auto& receivedFrame = client.rx->front();

            // Escrever no buffer de destino
            size_t bytesToRead = std::min(std::min(bufferSize, FRAME_BODY_LENGTH), (int)receivedFrame.BodyLength);
            std::memcpy(buffer, receivedFrame.Body, bytesToRead);

            client.rx->pop_front();

            client.rxLock->unlock();
            server.clientsLock->unlock();

            return bytesToRead;
        }
        client.rxLock->unlock();
        server.clientsLock->unlock();

        // std::this_thread::sleep_for(1ms);
    }

    // Nunca vai acontecer (TODO implementar timeout de read)
    return -1;
}

// Termina a conexão com um cliente específico
void rr_server_close_client(rr_server_handle serverHandle, rr_sock_handle clientHandle)
{
    if (!serverHandles.count(serverHandle))
    {
        // avisar, mas não tomar nenhuma ação
        fprintf(stderr, "rr_server_close_client: chamado mas não havia um servidor aberto com o handle %lu\n",
                serverHandle);
        return;
    }

    RRServer& server = serverHandles.at(serverHandle);

    std::lock_guard clientsLock(*server.clientsLock);
    if (server.clients->count(clientHandle))
    {
        fprintf(stderr, "rr_server_close_client: cliente dado (%lu) não existe\n", clientHandle);
        return;
    }

    RRServerClient& client = server.clients->at(clientHandle);

    rr_invalidate_client(client);
    server.clients->erase(client.handle);
}

void rr_server_close(rr_server_handle serverHandle)
{
    if (!serverHandles.count(serverHandle))
    {
        // avisar, mas não tomar nenhuma ação
        fprintf(stderr, "rr_server_close: chamado mas não havia um servidor aberto com o handle %ld\n", serverHandle);
        return;
    }

    RRServer& server = serverHandles.at(serverHandle);

    server.clientsLock->lock();
    for (auto& [_, client] : *server.clients)
        rr_invalidate_client(client);
    server.clientsLock->unlock();

    delete server.ioThread;
    delete server.clientsLock;
    delete server.clients;
    delete server.pendingSyn;
    delete server.pendingSynLock;

    serverHandles.erase(serverHandle);
}
