#include "transport.h"
#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv)
{
    rr_sock_handle socket = rr_client_connect("127.0.0.1", 9999);

    printf("main: Conexão estabelecida!\n");

    int i = 0;
    while (true)
    {
        using namespace std::chrono_literals;

        i++;
        std::string packet = "Hello Man " + std::to_string(i);
        rr_client_send(socket, packet.c_str(), packet.size());

        char buffer[FRAME_BODY_LENGTH];
        std::memset(buffer, 0, sizeof(buffer));

        size_t bytesLidos = rr_client_receive(socket, buffer, sizeof(buffer));
        printf("main: %zu bytes lidos: %s\n", bytesLidos, buffer);
    }

    rr_client_close(socket);

    return 0;
}