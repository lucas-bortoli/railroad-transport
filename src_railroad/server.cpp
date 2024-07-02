#include "datagram.h"
#include "transport.h"
#include "utils.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

#ifdef RR_SIMULATE_PACKET_LOSS_CHANCE
#include <stdlib.h> // rand()
#endif

struct PendingFrame
{
    // Timestamp em millisegundos do momento de envio desse quadro, usado para calcular timeout dos ACKs e subsequente
    // reenvio
    unsigned long long TxTimestamp;

    // O quadro em questão.
    Frame Inner;
};

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
    unsigned long nextSequenceRx;

    // Contador simples de quadros transmitidos com sucesso (aqueles que receberam ACK com
    // respostas). A cada quadro NOVO gerado pelo servidor, esse valor deverá ser incrementado.
    unsigned long sequenceTx;

    // Representa os quadros recebidos. Esse container contém apenas quadros de FrameKind::Data. Quadros
    // de SYN/ACK são processados independentemente no loop principal de IO.
    std::map<unsigned long, Frame>* rx;
    std::mutex* rxLock;

    // Representa os quadros de dados a serem transmitidos ainda. Ao receber um ACK, o quadro
    // recém-enviado é removido do container.
    std::map<unsigned long, PendingFrame>* tx;
    std::mutex* txLock;

    // Tamanho da janela de recepção/envio simultâneo. Deve ser >= 1.
    unsigned int windowSize;
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
    std::deque<sockaddr_in>* pendingSyn;
    std::mutex* pendingSynLock;

    // Quanto tempo, em millisegundos, esperar um ACK antes de retransmitir um quadro
    unsigned long long ackTimeout;

    // Número máximo de transmissões da fila. Chamadas subsequentes de rr_server_send irão bloquear a thread até haver
    // espaço suficiente na fila.
    int maximumTxQueueSize;
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
                    server.clientsLock->lock();
                    auto existingClientOrNull = get_client_from_client_address(serverHandle, datagram.SourceAddress);
                    server.clientsLock->unlock();

                    if (existingClientOrNull != nullptr)
                    {
                        printf("rr_server_thread_loop: Pacote SYN recebido, mas já existia um cliente registrado nesse "
                               "endereço e porta. Ignorando pacote.\n");

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

                    server.pendingSynLock->lock();
                    bool isDuplicate = false;
                    for (auto previousSyn : *server.pendingSyn)
                    {
                        if (socketAddressEqual(previousSyn, datagram.SourceAddress))
                        {
                            isDuplicate = true;
                            break;
                        }
                    }
                    if (!isDuplicate)
                    {
                        printf("rr_server_thread_loop: Pacote SYN recebido, nova conexão pendente...\n");
                        server.pendingSyn->push_back(datagram.SourceAddress);
                    }
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

                    printf("rr_server_thread_loop: Pacote ACK, seq=%lu\n", datagram.Body.SequenceId);

                    auto& client = *clientOrNull;
                    // Ao receber um ACK, remover aquele pacote da fila de transmissão para evitar reenvio
                    client.txLock->lock();
                    client.tx->erase(datagram.Body.SequenceId);
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
                    client.rxLock->lock();
                    // É um pacote já processado pela aplicação, ou já está na fila de processamento?
                    if (datagram.Body.SequenceId < client.nextSequenceRx || client.rx->count(datagram.Body.SequenceId))
                    {
                        printf("rr_server_thread_loop: Pacote DATA (sequence %lu descartado)\n",
                               datagram.Body.SequenceId);
                    }
                    else
                    {
                        printf("rr_server_thread_loop: Pacote DATA (sequence %lu aceito)\n", datagram.Body.SequenceId);

                        client.rx->insert_or_assign(datagram.Body.SequenceId, datagram.Body);
                    }
                    client.rxLock->unlock();

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
            // (re?)transmitir os primeiros N pacotes da fila de envio (aqueles que receberam um ACK não estão nessa
            // fila)
            client.txLock->lock();
            auto txCandidates = rr_peek_first_n_items_tx<PendingFrame>(*client.tx, client.windowSize);

            for (const auto& sentFrameCopy : txCandidates)
            {
                auto now = timeInMilliseconds();
                if (now - sentFrameCopy.TxTimestamp >= server.ackTimeout)
                {
                    client.tx->at(sentFrameCopy.Inner.SequenceId).TxTimestamp = timeInMilliseconds();
                    rr_datagram_send<Frame>(server.fd, SentDatagram<Frame>{
                                                           .TargetAddress = client.address,
                                                           .Body = sentFrameCopy.Inner,
                                                       });
                }
            }
            client.txLock->unlock();
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
        .pendingSyn = new std::deque<sockaddr_in>(),
        .pendingSynLock = new std::mutex(),
        .ackTimeout = 500,        // timeout de 500 ms
        .maximumTxQueueSize = 64, // até 64 itens na fila de transmissão
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
                .nextSequenceRx = 2,
                .sequenceTx = 1,
                .rx = new std::map<unsigned long, Frame>(),
                .rxLock = new std::mutex(),
                .tx = new std::map<unsigned long, PendingFrame>(),
                .txLock = new std::mutex(),
                .windowSize = 8,
            };
            server.clients->insert_or_assign(socketHandle, client);
            server.clientsLock->unlock();

            server.pendingSyn->pop_front();
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

    // Aguardar fila de transmissão haver espaço
    while (true)
    {
        using namespace std::chrono_literals;
        client.txLock->lock();
        if (client.tx->size() >= server.maximumTxQueueSize)
        {
            printf("rr_server_send: Fila de transmissão cheia, bloqueando thread até haver espaço...\n");
            client.txLock->unlock();
            // std::this_thread::sleep_for(1ms);
        }
        else
        {
            // Sair do loop sem abandonar trava da TX, usaremos imediatamente
            break;
        }
    }

    PendingFrame frame = {
        // Inicialmente, tempo infinito no passado para garantir primeira transmissão
        .TxTimestamp = 0,
        .Inner =
            Frame{
                .Kind = FrameKind::Data,
                .SequenceId = client.sequenceTx++,
                .BodyLength = bytesToCopy,
                .Body = {0},
            },
    };
    std::memcpy(&frame.Inner.Body, buffer, bytesToCopy);
    client.tx->insert_or_assign(frame.Inner.SequenceId, frame);
    client.txLock->unlock();
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

    RRServer& server = serverHandles.at(serverHandle);
    RRServerClient& client = server.clients->at(clientHandle);
    client.rxLock->lock();
    unsigned long wantedSeq = client.nextSequenceRx;
    client.rxLock->unlock();

    printf("rr_server_receive: Aguardando quadro #%lu...\n", wantedSeq);

    // Aguardar quadro de dados na fila de recepção
    while (true)
    {
        using namespace std::chrono_literals;

        server.clientsLock->lock();
        client.rxLock->lock();
        if (client.rx->count(wantedSeq))
        {
            auto& receivedFrame = client.rx->at(wantedSeq);
            printf("rr_server_receive: Quadro #%lu recebido\n", receivedFrame.SequenceId);

            // Escrever no buffer de destino
            size_t bytesToRead = std::min(std::min(bufferSize, FRAME_BODY_LENGTH), (int)receivedFrame.BodyLength);
            std::memcpy(buffer, receivedFrame.Body, bytesToRead);

            client.nextSequenceRx = receivedFrame.SequenceId + 1;
            printf("rr_server_receive: Próximo quadro será #%lu\n", client.nextSequenceRx);
            client.rx->erase(receivedFrame.SequenceId);

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
