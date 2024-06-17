#include "transport.h"
#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    printf("main: Iniciando servidor\n");

    rr_server_handle server = rr_server_bind("127.0.0.1", 9999);

    while (true)
    {
        rr_sock_handle client = rr_server_accept_client(server);
    }

    rr_server_close(server);

    return 0;
}