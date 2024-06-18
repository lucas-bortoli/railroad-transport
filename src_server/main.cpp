#include "transport.h"
#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv)
{
    printf("main: Iniciando servidor\n");

    rr_server_handle server = rr_server_bind("127.0.0.1", 9999);

    while (true)
    {
        rr_sock_handle client = rr_server_accept_client(server);

        printf("main: Novo cliente: %ld\n", client);
        // do something with client

        while (true)
        {
            char buffer[FRAME_BODY_LENGTH];
            std::memset(buffer, 0, sizeof(buffer));

            size_t bytesLidos = rr_server_receive(server, client, buffer, sizeof(buffer));
            printf("main: %d bytes lidos\n", bytesLidos);
        }
    }

    rr_server_close(server);

    return 0;
}