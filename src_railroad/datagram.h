#pragma once
#include <cstdio>
#include <netinet/in.h>
#include <optional>

template <typename T> struct ReceivedDatagram
{
    sockaddr_in SourceAddress;
    T Body;
};

template <typename T> struct SentDatagram
{
    sockaddr_in TargetAddress;
    T Body;
};

// Declaração e implementação dessas funções genéricas no header (e não no .cpp) de acordo com
// https://stackoverflow.com/a/8752870

template <typename T> std::optional<ReceivedDatagram<T>> rr_datagram_receive(int fd)
{
    printf("rr_datagram_receive: Aguardando datagrama...\n");

    // Buffer para armazenar mensagem iminente
    T receivedBody;
    memset(&receivedBody, 0, sizeof(receivedBody));

    struct sockaddr_in senderAddress;
    socklen_t senderAddrLen = sizeof(senderAddress);

    // Ler um quadro da rede
    ssize_t bytesRead = recvfrom(fd, (void*)&receivedBody, sizeof(receivedBody), 0,
                                 (struct sockaddr*)&senderAddress, &senderAddrLen);

    printf("rr_datagram_receive: Datagrama recebido\n");

    if (bytesRead >= 0)
    {
        return ReceivedDatagram<T>{
            .SourceAddress = senderAddress,
            .Body = receivedBody,
        };
    }
    else
    {
        return std::nullopt;
    }
}

template <typename T> void rr_datagram_send(int fd, SentDatagram<T>& datagram)
{
    printf("rr_datagram_send: Enviando datagrama de %ld bytes...\n", sizeof(datagram.Body));

    sendto(fd, &datagram.Body, sizeof(datagram.Body), 0, (sockaddr*)&datagram.TargetAddress,
           sizeof(datagram.TargetAddress));
}