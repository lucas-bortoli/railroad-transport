#include "transport.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <string>

PeerConnection::PeerConnection()
{
    this->SocketFd = -1;
    memset(&this->PeerEndpoint, 0, sizeof(this->PeerEndpoint));
}

void PeerConnection::Send(const char* buffer, size_t buffer_length)
{
    if (buffer_length > DATAFRAME_BODY_LENGTH)
    {
        fprintf(stderr, "PeerConnection::Send: tentou enviar payload de %ld bytes, sendo que o máximo é %d bytes\n",
                buffer_length, DATAFRAME_BODY_LENGTH);
        abort();
    }

    FrameData frame;
    memset(&frame, 0, sizeof(frame));
    frame.Id = FrameId::DATA;
    frame.BodyLength = buffer_length;
    memcpy(&frame.Body, buffer, buffer_length);

    sendto(this->SocketFd, &frame, sizeof(frame), 0, (const struct sockaddr*)&this->PeerEndpoint,
           sizeof(this->PeerEndpoint));
}

size_t PeerConnection::Receive(char* buffer)
{
    return 0;
}