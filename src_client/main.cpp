#include "transport.h"
#include <cstdio>
#include <string>

int main(int argc, char** argv)
{
    rr_sock_handle socket = rr_client_connect("127.0.0.1", 9999);

    printf("main: Conexão estabelecida!\n");

    while (true)
    {
        using namespace std::chrono_literals;

        std::string packet = "Hello Man";
        rr_client_send(socket, packet.c_str(), packet.size());

        std::this_thread::sleep_for(5ms);
    }

    rr_client_close(socket);

    /*
    const char* serverIp = "127.0.0.1";     // Server IP address
    const int serverPort = 7426;            // Server port
    const char* message = "Hello, Server!"; // Message to send

    // Create a socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
    {
        perror("socket creation failed");
        return EXIT_FAILURE;
    }

    // Set up the server address structure
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));

    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(serverPort);
    if (inet_pton(AF_INET, serverIp, &servaddr.sin_addr) <= 0)
    {
        perror("invalid address/ address not supported");
        close(sockfd);
        return EXIT_FAILURE;
    }

    // Send the message to the server
    if (sendto(sockfd, message, strlen(message), 0, (const struct sockaddr*)&servaddr,
    sizeof(servaddr)) < 0)
    {
        perror("sendto failed");
        close(sockfd);
        return EXIT_FAILURE;
    }

    printf("Datagrama enviado\n");

    // Close the socket
    close(sockfd);*/

    /*
     */

    return 0;
}