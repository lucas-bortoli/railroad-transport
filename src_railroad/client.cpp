#include "datagram.h"
#include "transport.h"
#include "utils.h"
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <sys/time.h>
#include <thread>
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
    unsigned long nextSequenceRx;

    // Contador simples de quadros transmitidos com sucesso (aqueles que receberam ACK com
    // respostas). A cada quadro NOVO gerado pelo cliente, esse valor deverá ser incrementado.
    unsigned long sequenceTx;

    // Representa os quadros recebidos. Essa fila contém apenas quadros de FrameKind::Data. Quadros
    // de SYN/ACK são processados independentemente no loop principal de IO.
    std::map<unsigned long, Frame>* rx;
    std::mutex* rxLock;

    // Representa os quadros de dados a serem transmitidos ainda. Ao receber um ACK, o quadro
    // recém-enviado é removido da fila.
    std::map<unsigned long, PendingFrame>* tx;
    std::mutex* txLock;

    // Tamanho da janela de recepção/envio simultâneo. Deve ser >= 1.
    unsigned int windowSize;

    // Quanto tempo, em millisegundos, esperar um ACK antes de retransmitir um quadro
    unsigned long long ackTimeout;

    // Número máximo de transmissões da fila. Chamadas subsequentes de rr_server_send irão bloquear a thread até haver
    // espaço suficiente na fila.
    int maximumTxQueueSize;
};

static std::atomic<unsigned long> idAllocator{0};
static std::unordered_map<rr_sock_handle, RRClient> clientHandles;

void rr_client_thread_loop(rr_sock_handle handle)
{

    printf("rr_client_thread_loop(%ld): Thread iniciada\n", handle);

    while (true)
    {
        using namespace std::chrono_literals;

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
                    printf("rr_client_thread_loop: Pacote ACK, seq=%lu\n", datagram.Body.SequenceId);

                    // Ao receber um ACK, remover pacote da fila de transmissão para evitar reenvio
                    client.txLock->lock();
                    client.tx->erase(datagram.Body.SequenceId);
                    client.txLock->unlock();

                    break;
                }
                case FrameKind::Data: {
                    client.rxLock->lock();
                    // É um pacote já processado pela aplicação, ou já está na fila de processamento?
                    if (datagram.Body.SequenceId < client.nextSequenceRx || client.rx->count(datagram.Body.SequenceId))
                    {
                        printf("rr_client_thread_loop: Pacote DATA (sequence %lu descartado)\n",
                               datagram.Body.SequenceId);
                    }
                    else
                    {
                        printf("rr_client_thread_loop: Pacote DATA (sequence %lu aceito)\n", datagram.Body.SequenceId);

                        client.rx->insert_or_assign(datagram.Body.SequenceId, datagram.Body);
                    }
                    client.rxLock->unlock();

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
        }

        // (re?)transmitir os primeiros N pacotes da fila de envio (aqueles que receberam um ACK não estão nessa
        // fila)
        client.txLock->lock();
        auto txCandidates = rr_peek_first_n_items_tx<PendingFrame>(*client.tx, client.windowSize);
        for (const auto& sentFrameCopy : txCandidates)
        {
            auto now = timeInMilliseconds();
            if (now - sentFrameCopy.TxTimestamp >= client.ackTimeout)
            {
                client.tx->at(sentFrameCopy.Inner.SequenceId).TxTimestamp = timeInMilliseconds();
                rr_datagram_send<Frame>(client.fd, SentDatagram<Frame>{
                                                       .TargetAddress = client.serverAddress,
                                                       .Body = sentFrameCopy.Inner,
                                                   });
            }
        }
        client.txLock->unlock();

        // std::this_thread::sleep_for(1ms);
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
        .nextSequenceRx = 1,
        .sequenceTx = 1,
        .rx = new std::map<unsigned long, Frame>(),
        .rxLock = new std::mutex(),
        .tx = new std::map<unsigned long, PendingFrame>(),
        .txLock = new std::mutex(),
        .windowSize = 8,
        .ackTimeout = 500,
        .maximumTxQueueSize = 64,
    };
    RRClient& client = clientHandles.at(handle);

    // Deixar thread executando após essa função retornar
    client.ioThread = new std::thread(&rr_client_thread_loop, handle);
    client.ioThread->detach();

    // Enfileirar SYN para (re)transmissão
    client.txLock->lock();
    auto synFrame = PendingFrame{
        .TxTimestamp = 0,
        .Inner =
            Frame{
                .Kind = FrameKind::Syn,
                .SequenceId = client.sequenceTx++,
                .BodyLength = 0,
                .Body = {0},
            },
    };
    client.tx->insert_or_assign(synFrame.Inner.SequenceId, synFrame);
    client.txLock->unlock();

    // Aguardar ACK
    while (true)
    {
        using namespace std::chrono_literals;

        // Enquanto esse quadro estiver na fila de transmissão, ele não foi recebido ainda
        client.txLock->lock();
        if (!client.tx->count(synFrame.Inner.SequenceId))
        {
            // Foi transmitido e recebeu seu ACK
            client.txLock->unlock();
            return handle;
        }
        client.txLock->unlock();

        // std::this_thread::sleep_for(1ms);
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

    // Aguardar fila de transmissão haver espaço
    while (true)
    {
        using namespace std::chrono_literals;
        client.txLock->lock();
        if (client.tx->size() >= client.maximumTxQueueSize)
        {
            printf("rr_client_send: Fila de transmissão cheia, bloqueando thread até haver espaço...\n");
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

size_t rr_client_receive(rr_sock_handle handle, char* buffer, int bufferSize)
{
    if (!clientHandles.count(handle))
    {
        fprintf(stderr, "rr_client_receive: chamado mas não havia um socket aberto com o handle %ld\n", handle);
        abort();
    }

    RRClient& client = clientHandles.at(handle);
    unsigned long wantedSeq = client.nextSequenceRx;

    printf("rr_client_receive: Aguardando quadro #%lu...\n", wantedSeq);

    // Aguardar quadro de dados na fila de recepção
    while (true)
    {
        using namespace std::chrono_literals;

        client.rxLock->lock();
        if (client.rx->count(wantedSeq))
        {
            auto& receivedFrame = client.rx->at(wantedSeq);
            printf("rr_client_receive: Quadro #%lu recebido\n", receivedFrame.SequenceId);

            // Escrever no buffer de destino
            size_t bytesToRead = std::min(std::min(bufferSize, FRAME_BODY_LENGTH), (int)receivedFrame.BodyLength);
            std::memcpy(buffer, receivedFrame.Body, bytesToRead);

            client.nextSequenceRx = receivedFrame.SequenceId + 1;
            printf("rr_client_receive: Próximo quadro será #%lu\n", client.nextSequenceRx);
            client.rx->erase(receivedFrame.SequenceId);
            client.rxLock->unlock();

            return bytesToRead;
        }
        client.rxLock->unlock();

        // std::this_thread::sleep_for(1ms);
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