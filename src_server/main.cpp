#include "transport.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

void client_thread(rr_server_handle server, rr_sock_handle socket)
{
    using namespace std::chrono_literals;
    int i = 0;
    while (true)
    {
        char buffer[FRAME_BODY_LENGTH];
        std::memset(buffer, 0, sizeof(buffer));

        //  size_t bytesLidos = rr_server_receive(server, socket, buffer, sizeof(buffer));
        //     printf("main: %zu bytes lidos: %s\n", bytesLidos, buffer);

        i++;
        std::string payload = "Hello " + std::to_string(i);
        ssize_t err = rr_server_send(server, socket, payload.c_str(), payload.size());
        if (err < 0)
        {
            fprintf(stderr, "main: Erro ao enviar: %s\n", strerror(errno));
            break;
        }
    }
}

int main(int argc, char** argv)
{
    using namespace std::chrono_literals;
    printf("main: Iniciando servidor\n");

    rr_server_handle server = rr_server_bind("0.0.0.0", 9999);

    while (true)
    {
        rr_sock_handle client = rr_server_accept_client(server);

        printf("main: Novo cliente: %ld\n", client);

        auto t = new std::thread(&client_thread, server, client);
        t->detach();

        std::this_thread::sleep_for(1000ms);
    }

    rr_server_close(server);

    return 0;
}