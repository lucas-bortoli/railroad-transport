#include "transport.h"
#include "utils.h"
#include <arpa/inet.h>
#include <cstdio>
#include <netinet/in.h>
#include <string.h>
#include <string>
#include <sys/socket.h>

Client::Client(std::string peerAddress, unsigned short peerPort) : PeerConnection()
{
    // Setar o IP e a porta do destino do socket
    memset(&this->PeerEndpoint, 0, sizeof(this->PeerEndpoint));
    this->PeerEndpoint.sin_family = AF_INET;                                      // IPv4
    this->PeerEndpoint.sin_addr.s_addr = ConvertIPv4StringToAddress(peerAddress); // IP dado
    this->PeerEndpoint.sin_port = htons(peerPort);                                // Porta dada
}

void Client::Connect()
{
    this->SocketFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (this->SocketFd < 0)
    {
        fprintf(stderr, "Client::Connect: Erro ao criar o socket: %d\n", this->SocketFd);
        abort();
    }

    // Handshake inicial...
    sendto(this->SocketFd, 0, 512, 0, (const struct sockaddr*)&this->PeerEndpoint, sizeof(this->PeerEndpoint));
}
