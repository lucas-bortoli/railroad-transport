#pragma once
#include <cstdint>
#include <list>
#include <netinet/in.h>
#include <string>

#define DATAFRAME_BODY_LENGTH 256

enum class FrameId : uint8_t
{
    SIGNAL,
    DATA
};

struct __attribute__((packed)) FrameSignal
{
    FrameId Id;
};

struct __attribute__((packed)) FrameData
{
    FrameId Id;
    unsigned int BodyLength;
    unsigned char Body[DATAFRAME_BODY_LENGTH];
};

class PeerConnection
{
  private:
    struct FrameData _DatagramReceive(bool filter(struct FrameData frame));
    void _DatagramSendWithAck(struct FrameData frame);

  protected:
    std::list<struct FrameData> ReceiveQueue;
    unsigned long long CurrentSequence;

  public:
    struct sockaddr_in PeerEndpoint;
    int SocketFd;

    PeerConnection();
    void Send(const char* buffer, size_t bufferSize);
    size_t Receive(char* buffer, size_t bufferSize);
};

class Server
{
  private:
    std::string ListenAddress;
    unsigned short ListenPort;

    // Uma handle para o socket criado no método Bind(), inicialmente -1
    int pSockFd;

  public:
    Server(std::string listenAddress, unsigned short listenPort);

    // Cria um socket escutando no endereço e porta dado usando a API POSIX
    void Bind();

    void Close();

    // Bloquear thread até receber um cliente, fazer um handshake funcional com o mesmo, estabelecendo a conexão
    PeerConnection* Accept();
};

class Client : public PeerConnection
{
  public:
    Client(std::string peerAddress, unsigned short peerPort);
    void Connect();
};