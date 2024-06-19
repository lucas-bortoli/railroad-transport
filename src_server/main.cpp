#include "transport.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

void client_thread(rr_server_handle server, rr_sock_handle socket)
{
    int i = 0;
    while (true)
    {
        char buffer[FRAME_BODY_LENGTH];
        std::memset(buffer, 0, sizeof(buffer));

        size_t bytesLidos = rr_server_receive(server, socket, buffer, sizeof(buffer));
        printf("main: %zu bytes lidos: %s\n", bytesLidos, buffer);

        i++;
        std::string packet = "Hello Man From Server " + std::to_string(i);
        rr_server_send(server, socket, packet.c_str(), packet.size());
    }
}

int main(int argc, char** argv)
{
    printf("main: Iniciando servidor\n");

    rr_server_handle server = rr_server_bind("127.0.0.1", 9999);

    while (true)
    {
        rr_sock_handle client = rr_server_accept_client(server);

        printf("main: Novo cliente: %ld\n", client);

        auto t = new std::thread(&client_thread, server, client);
        t->detach();
    }

    rr_server_close(server);

    return 0;
}