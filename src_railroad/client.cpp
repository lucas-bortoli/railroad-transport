#include "datagram.h"
#include "transport.h"
#include "utils.h"
#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unistd.h>

#ifdef RR_SIMULATE_PACKET_LOSS_CHANCE
#include <stdlib.h> // rand()
#endif RR_SIMULATE_PACKET_LOSS_CHANCE

struct RRClient
{
    int fd;

    rr_sock_handle handle;

    sockaddr_in serverAddress;

    std::thread* ioThread;

    // Sequência de quadros de transmissão. Ao receber um quadro, caso o campo Sequence do quadro
    // seja menor que esse valor, não deverá ser colocado na fila currentRx, pois entende-se que já
    // foi processado pela aplicação. No entanto, ainda deve-se responder ACK para o
    // quadro, para impedir a retransmissão do mesmo.
    unsigned long sequenceRx;

    // Contador simples de quadros transmitidos com sucesso (aqueles que receberam ACK com
    // respostas). A cada quadro NOVO gerado pelo cliente, esse valor deverá ser incrementado.
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

    // Quanto tempo, em millisegundos, esperar um ACK antes de retransmitir um quadro
    unsigned long long ackTimeout;
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
        {
            printf("rr_client_thread_loop(%ld): Handle de cliente não existe mais, finalizando\n", handle);
            break;
        }

        RRClient& client = clientHandles.at(handle);

        // Receber todos os pacotes da fila do kernel
        while (true)
        {
            auto datagramOrNull = rr_datagram_receive<Frame>(client.fd);

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
                case FrameKind::Syn:
                    fprintf(stderr, "rr_client_thread_loop: Pacote SYN recebido pelo cliente, operação inválida.\n");
                    abort();
                    break;
                case FrameKind::Ack: {
                    printf("rr_client_thread_loop: Pacote ACK\n");

                    client.statusWaitingForAck = false;
                    // Ao receber um ACK, remover pacote da fila de transmissão para evitar reenvio
                    client.txLock->lock();
                    if (!client.tx->empty())
                        client.tx->pop_front();
                    client.txLock->unlock();

                    break;
                }
                case FrameKind::Data: {
                    if (datagram.Body.SequenceId < client.sequenceRx)
                    {
                        printf("rr_client_thread_loop: Pacote DATA (sequence %lu / %lu, descartado)\n",
                               datagram.Body.SequenceId, client.sequenceRx);
                    }
                    else
                    {
                        printf("rr_client_thread_loop: Pacote DATA (sequence %lu / %lu, aceito)\n",
                               datagram.Body.SequenceId, client.sequenceRx);
                        client.rxLock->lock();
                        client.rx->push_back(datagram.Body);
                        client.sequenceRx++;
                        client.rxLock->unlock();
                    }

                    // Responder ACK
                    auto ackReply = SentDatagram<Frame>{
                        .TargetAddress = client.serverAddress,
                        .Body = {.Kind = FrameKind::Ack,
                                 .SequenceId = datagram.Body.SequenceId,
                                 .BodyLength = 0,
                                 .Body = {0}},
                    };
                    rr_datagram_send<Frame>(client.fd, ackReply);

                    break;
                }
            }
        };

        // Transmitir o primeiro pacote da fila de envio:
        // 1. Não esteja aguardando nenhum ACK
        // 2. Esperou tempo demais para um ACK e está retransmitindo
        auto now = timeInMilliseconds();
        if (!client.statusWaitingForAck ||
            (client.statusWaitingForAck && now - client.lastTxTimestamp >= client.ackTimeout))
        {
            client.txLock->lock();
            if (!client.tx->empty())
            {
                auto& frame = client.tx->front();

                client.statusWaitingForAck = true;
                client.lastTxTimestamp = timeInMilliseconds();
                rr_datagram_send<Frame>(client.fd, SentDatagram<Frame>{
                                                       .TargetAddress = client.serverAddress,
                                                       .Body = frame,
                                                   });
            }
            client.txLock->unlock();
        }

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
    tv.tv_usec = 100; // 0.1 ms
    setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    rr_sock_handle handle = idAllocator++;

    clientHandles[handle] = RRClient{
        .fd = socketFd,
        .handle = handle,
        .serverAddress = serverAddress,
        .ioThread = nullptr,
        .sequenceRx = 1,
        .sequenceTx = 1,
        .statusWaitingForAck = false,
        .rx = new std::deque<Frame>(),
        .rxLock = new std::mutex(),
        .tx = new std::deque<Frame>(),
        .txLock = new std::mutex(),
        .lastTxTimestamp = 0,
        .ackTimeout = 500,
    };
    RRClient& client = clientHandles.at(handle);

    // Deixar thread executando após essa função retornar
    client.ioThread = new std::thread(&rr_client_thread_loop, handle);
    client.ioThread->detach();

    // Enfileirar SYN para (re)transmissão
    client.txLock->lock();
    auto synFrame = Frame{
        .Kind = FrameKind::Syn,
        .SequenceId = client.sequenceTx++,
        .BodyLength = 0,
        .Body = {0},
    };
    client.tx->push_back(synFrame);
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
            if (queuedFrame.SequenceId == synFrame.SequenceId)
            {
                found = true;
                break;
            }
        }
        client.txLock->unlock();

        // Quadro saiu da fila de transmissão?
        if (!found)
        {
            return handle;
        }

        std::this_thread::sleep_for(1ms);
    }

    return handle;
}

void rr_client_send(rr_sock_handle handle, const char* buffer, int bufferSize)
{
    if (!clientHandles.count(handle))
    {
        fprintf(stderr, "rr_client_send: chamado mas não havia um socket aberto com o handle %ld\n", handle);
        abort();
    }

    unsigned int bytesToCopy = std::min(bufferSize, FRAME_BODY_LENGTH);

    RRClient& client = clientHandles.at(handle);

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

        std::this_thread::sleep_for(1ms);
    }
}

size_t rr_client_receive(rr_sock_handle handle, char* buffer, int bufferSize)
{
    if (!clientHandles.count(handle))
    {
        fprintf(stderr, "rr_client_receive: chamado mas não havia um socket aberto com o handle %ld\n", handle);
        abort();
    }

    printf("rr_client_receive: Aguardando dados...\n");

    // Aguardar quadro de dados na fila de recepção
    while (true)
    {
        using namespace std::chrono_literals;

        RRClient& client = clientHandles.at(handle);

        client.rxLock->lock();
        if (!client.rx->empty())
        {
            auto& receivedFrame = client.rx->front();

            // Escrever no buffer de destino
            size_t bytesToRead = std::min(std::min(bufferSize, FRAME_BODY_LENGTH), (int)receivedFrame.BodyLength);
            std::memcpy(buffer, receivedFrame.Body, bytesToRead);

            client.rx->pop_front();
            client.rxLock->unlock();

            return bytesToRead;
        }
        client.rxLock->unlock();

        std::this_thread::sleep_for(1ms);
    }

    // Nunca vai acontecer (TODO implementar timeout de read)
    return -1;
}

void rr_client_close(rr_sock_handle handle)
{
    if (!clientHandles.count(handle))
    {
        // avisar, mas não tomar nenhuma ação
        fprintf(stderr, "rr_client_close: chamado mas não havia um socket aberto com o handle %ld\n", handle);
        return;
    }

    RRClient& client = clientHandles[handle];
    delete client.ioThread;
    delete client.rx;
    delete client.rxLock;
    delete client.tx;
    delete client.txLock;
    clientHandles.erase(handle);
}