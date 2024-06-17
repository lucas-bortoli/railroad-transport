#include "transport.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <unistd.h>

PeerConnection::PeerConnection()
{
    this->SocketFd = -1;
    memset(&this->PeerEndpoint, 0, sizeof(this->PeerEndpoint));
}

struct FrameData PeerConnection::_DatagramReceive(bool filter(struct FrameData frame))
{
    // Primeiramente, vamos retornar um possível datagrama na fila de recepção que atenda aos requisitos
    auto it = this->ReceiveQueue.begin();
    while (it != ReceiveQueue.end())
    {
        auto item = *it;

        if (filter(item))
        {
            printf("PeerConnection::_DatagramReceive: datagrama compatível já estava na fila\n");

            // Remover datagrama dos dados recebidos
            ReceiveQueue.erase(it);

            return item;
        }

        ++it;
    }

    // Não havia nenhum datagrama compatível na fila, aguardar a chegada
    while (true)
    {
        // Buffer para armazenar mensagem de ACK iminente
        struct FrameData receivedFrame;
        memset(&receivedFrame, 0, sizeof(receivedFrame));

        struct sockaddr_in senderAddress;
        socklen_t senderAddrLen = sizeof(senderAddress);

        // Ler um quadro da rede
        ssize_t bytesRead = recvfrom(this->SocketFd, (void*)&receivedFrame, sizeof(receivedFrame), 0,
                                     (struct sockaddr*)&this->PeerEndpoint, &senderAddrLen);

        if (bytesRead < 0)
        {
            fprintf(stderr, "Erro ao ler datagrama: %s (errno: %d)\n", strerror(errno), errno);
            close(this->SocketFd);
            abort();
        }

        if (filter(receivedFrame))
        {
            printf("PeerConnection::_DatagramReceive: datagrama compatível recebido\n");
            return receivedFrame;
        }
        else
        {
            printf("PeerConnection::_DatagramReceive: datagrama incompatível recebido, armazenando na fila\n");
            ReceiveQueue.emplace_back(receivedFrame);
        }
    }
}

void PeerConnection::_DatagramSendWithAck(struct FrameData frame)
{
    bool ackReceived = false;

    do
    {
        ssize_t bytesWritten = sendto(this->SocketFd, &frame, sizeof(frame), 0,
                                      (const struct sockaddr*)&this->PeerEndpoint, sizeof(this->PeerEndpoint));

        if (bytesWritten < 0)
        {
            fprintf(stderr, "Erro ao enviar datagrama: %s (errno: %d)\n", strerror(errno), errno);
            close(this->SocketFd);
            abort();
        }

        printf("PeerConnection::_DatagramSendWithAck: Datagrama enviado, aguardando resposta...\n");

        struct FrameData ackFrame = this->_DatagramReceive([](struct FrameData frame) { return true; });

        ackReceived = true;
    } while (!ackReceived);
}

void PeerConnection::Send(const char* buffer, size_t bufferSize)
{
    if (bufferSize > DATAFRAME_BODY_LENGTH)
    {
        fprintf(stderr, "PeerConnection::Send: tentou enviar payload de %ld bytes, sendo que o máximo é %d bytes\n",
                bufferSize, DATAFRAME_BODY_LENGTH);
        abort();
    }

    FrameData frame;
    memset(&frame, 0, sizeof(frame));
    frame.Id = FrameId::DATA;
    frame.BodyLength = bufferSize;
    memcpy(&frame.Body, buffer, bufferSize);

    this->_DatagramSendWithAck(frame);
}

size_t PeerConnection::Receive(char* buffer, size_t bufferSize)
{
    auto testDatagram = [](struct FrameData frame) { return true; };

    struct FrameData frame = this->_DatagramReceive(testDatagram);

    size_t bytes = std::min(bufferSize, sizeof(DATAFRAME_BODY_LENGTH));
    memcpy(buffer, frame.Body, bytes);
}