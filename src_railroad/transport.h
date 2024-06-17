#pragma once
#include <cstdint>
#include <netinet/in.h>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define FRAME_BODY_LENGTH 256

struct __attribute__((packed)) Frame
{
    unsigned long long SequenceId;
    struct
    {
        bool Syn : 1;
        bool Ack : 1;
        bool Reserved : 6;
    } Flags;
    unsigned int BodyLength;
    unsigned char Body[FRAME_BODY_LENGTH];
};

typedef unsigned long rr_server_handle;
typedef unsigned long rr_sock_handle;

rr_server_handle rr_server_bind(std::string address, unsigned short port);
rr_sock_handle rr_server_accept_client(rr_server_handle serverHandle);
void rr_server_close(rr_server_handle serverHandle);

rr_sock_handle rr_connect(std::string address, unsigned short port);

void rr_send(rr_sock_handle handle);
size_t rr_receive(rr_sock_handle handle);

/*

class ServerClient
{
  private:
    unsigned long long CurrentSequence;
    // Função da thread desse cliente
    void Update();

  public:
    std::mutex SendQueueLock;
    std::mutex ReceiveQueueLock;
    std::unordered_map<unsigned long long, Frame> SendQueue;
    std::unordered_map<unsigned long long, Frame> ReceiveQueue;

    struct sockaddr_in ClientAddress;
    int SocketFd;

    ServerClient(int socketFd, struct sockaddr_in clientAddress);

    // Envia um quadro para o cliente, aguardando um ACK e retransmitindo em tempo hábil
    void Send(const Frame* frame);

    // Retorna um quadro de dados da fila interna
    bool Receive(Frame* frameOutput);

    std::thread UpdateThread;
};

class Server
{
  private:
    // Uma handle para o socket criado no método Bind()
    int pSockFd;
    std::unordered_map<std::string, ServerClient*> Clients;

  public:
    // Cria um socket escutando no endereço e porta dado usando a API POSIX
    void Bind(std::string listenAddress, unsigned short listenPort);
    void Update();
    void Close();
};

class Client
{
  private:
    // Uma handle para o socket criado
    int pSockFd;
    struct sockaddr_in ServerAddress;
    unsigned long long CurrentSequence;
    void _WaitForMessageMatchingFilter();

  public:
    void Connect(std::string listenAddress, unsigned short listenPort);

    // Envia um quadro para o cliente, aguardando um ACK e retransmitindo em tempo hábil
    void Send(const Frame* frame);

    // Retorna um quadro de dados da fila interna
    bool Receive(Frame* frameOutput);
};*/