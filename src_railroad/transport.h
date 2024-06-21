#pragma once
#include <cstdint>
#include <netinet/in.h>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#define FRAME_BODY_LENGTH 256

enum class FrameKind : uint8_t
{
    Syn,
    Ack,
    Data
};

struct __attribute__((packed)) Frame
{
    FrameKind Kind;
    unsigned long SequenceId;
    unsigned int BodyLength;
    unsigned char Body[FRAME_BODY_LENGTH];

    // Usado para implementar uma Fila de Prioridade composta por Frames, na metodologia de sliding window.
    // Dessa forma, podemos usar a comparação Frame A < Frame B, que significa que Frame A vem antes de Frame B no
    // ordenamento correto de pacotes. A classe std::priority_queue depende desse operador para fazer seu sorting.
    // https://stackoverflow.com/a/15602044
    bool operator<(const Frame& rhs) const
    {
        return this->SequenceId < rhs.SequenceId;
    }
};

typedef unsigned long rr_server_handle;
typedef unsigned long rr_sock_handle;

rr_server_handle rr_server_bind(std::string address, unsigned short port);
rr_sock_handle rr_server_accept_client(rr_server_handle serverHandle);
void rr_server_send(rr_server_handle serverHandle, rr_sock_handle clientHandle, const char* buffer, int bufferSize);
size_t rr_server_receive(rr_server_handle serverHandle, rr_sock_handle clientHandle, char* buffer, int bufferSize);
// Termina a conexão com um cliente específico
void rr_server_close_client(rr_server_handle serverHandle, rr_sock_handle clientHandle);
void rr_server_close(rr_server_handle serverHandle);

rr_sock_handle rr_client_connect(std::string address, unsigned short port);
void rr_client_send(rr_sock_handle handle, const char* buffer, int bufferSize);
size_t rr_client_receive(rr_sock_handle handle, char* buffer, int bufferSize);
// Termina a conexão com o servidor
void rr_client_close(rr_sock_handle handle);