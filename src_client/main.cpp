#include "transport.h"
#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv)
{
    using namespace std::chrono_literals;
    rr_sock_handle socket = rr_client_connect("127.0.0.1", 9999);

    printf("main: Conexão estabelecida!\n");

    std::this_thread::sleep_for(1000ms);

    int i = 0;
    int batch = 0;
    std::string packet;
    while (true)
    {

        batch++;

        i++;
        packet = "Hello Man " + std::to_string(i) + " (batch " + std::to_string(batch) + ")";
        rr_client_send(socket, packet.c_str(), packet.size());
        i++;
        packet = "Hello Man " + std::to_string(i) + " (batch " + std::to_string(batch) + ")";
        rr_client_send(socket, packet.c_str(), packet.size());
        i++;
        packet = "Hello Man " + std::to_string(i) + " (batch " + std::to_string(batch) + ")";
        rr_client_send(socket, packet.c_str(), packet.size());

        std::this_thread::sleep_for(50000ms);
    }

    rr_client_close(socket);

    return 0;
}