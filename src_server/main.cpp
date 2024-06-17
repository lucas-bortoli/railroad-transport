#include "transport.h"
#include <cstdio>
#include <string>

void HandleClient(PeerConnection* client)
{
    printf("HandleClient: novo cliente\n");

    std::string packet = "Hello Man";

    while (true)
    {
        client->Send(packet.c_str(), packet.size());
    }
}

int main(int argc, char** argv)
{
    printf("main: Starting server\n");

    Server server("127.0.0.1", 7426);

    server.Bind();

    while (true)
    {
        PeerConnection* client = server.Accept();

        HandleClient(client);

        // Forget about client
        delete client;
    }

    server.Close();

    return 0;
}