#include "transport.h"
#include "utils.h"
#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <unistd.h>

Server::Server(std::string listenAddress, unsigned short listenPort)
{
    this->ListenAddress = listenAddress;
    this->ListenPort = listenPort;
    this->pSockFd = -1;
}

void Server::Bind()
{
    printf("Server::Bind: Bind no endereço %s e porta %d\n", this->ListenAddress.c_str(), this->ListenPort);

    struct sockaddr_in server_addr;

    // Setar o IP e a porta no qual o servidor escutará
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;                                              // IPv4
    server_addr.sin_addr.s_addr = ConvertIPv4StringToAddress(this->ListenAddress); // IP dado
    server_addr.sin_port = htons(this->ListenPort);                                // Porta dada

    // Declarar um socket
    this->pSockFd = socket(AF_INET, SOCK_DGRAM, 0);
    if (this->pSockFd < 0)
    {
        fprintf(stderr, "Erro ao criar o socket: %d\n", this->pSockFd);
        abort();
    }

    // Bind the socket with the server address
    int err = bind(this->pSockFd, (const struct sockaddr*)&server_addr, sizeof(server_addr));
    if (err < 0)
    {
        fprintf(stderr, "Server::Bind: Erro ao fazer bind: %d\n", err);
        close(this->pSockFd);
        abort();
    }

    printf("Server::Bind: Bind OK!\n");
}

void Server::Close()
{
    if (this->pSockFd >= 0)
    {
        close(this->pSockFd);
    }

    printf("Server::Close: Close OK!\n");
}

PeerConnection* Server::Accept()
{
    printf("Server::Accept: Aguardando cliente (bloqueando thread)...\n");

    // Buffer para armazenar mensagem de SYN iminente
    char buffer[sizeof(struct FrameData)];
    memset(buffer, 0, sizeof(buffer));

    // Struct que receberá as informações de IP do cliente para comunicação bilateral
    struct sockaddr_in clientAddress;
    socklen_t clientAddressLen;

    // recvsg/recvfrom retorna uma (1) mensagem por call, e será a mensagem inteira, desde que o buffer dado tenha
    // tamanho suficiente https://stackoverflow.com/a/2547598 https://linux.die.net/man/2/recvfrom
    // Além disso, MSG_WAITALL especifica que a thread vai bloquear até que a mensagem seja dada
    ssize_t bytesLidos = recvfrom(this->pSockFd, buffer, sizeof(buffer), MSG_WAITALL, (struct sockaddr*)&clientAddress,
                                  &clientAddressLen);

    printf("Datagrama lido - %ld bytes\n", bytesLidos);

    PeerConnection* client = new PeerConnection();
    client->SocketFd = this->pSockFd;
    client->PeerEndpoint = clientAddress;

    return client;
}