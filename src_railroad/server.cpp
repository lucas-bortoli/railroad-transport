#include "transport.h"
#include "utils.h"
#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/time.h>
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

    // Inicializar o listener
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
    struct sockaddr_in senderAddress;
    socklen_t senderAddressLen = sizeof(senderAddress);

    // recvsg/recvfrom retorna uma (1) mensagem por call, e será a mensagem inteira, desde que o buffer dado tenha
    // tamanho suficiente https://stackoverflow.com/a/2547598 https://linux.die.net/man/2/recvfrom
    ssize_t bytesRead =
        recvfrom(this->pSockFd, buffer, sizeof(buffer), 0, (struct sockaddr*)&senderAddress, &senderAddressLen);

    if (bytesRead < 0)
    {
        fprintf(stderr, "Erro ao ler datagrama: %s (errno: %d)\n", strerror(errno), errno);
        close(this->pSockFd);
        abort();
    }

    printf("Datagrama lido - %ld bytes\n", bytesRead);

    PeerConnection* client = new PeerConnection();
    client->SocketFd = this->pSockFd;
    client->PeerEndpoint = senderAddress;

    return client;
}