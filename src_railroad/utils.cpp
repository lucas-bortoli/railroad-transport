#include "utils.h"
#include <arpa/inet.h>
#include <cstdio>
#include <optional>
#include <string>

// Converter uma std::string contendo uma representação textual do IPv4 para um endereço
in_addr_t ConvertIPv4StringToAddress(const std::string& ipAddress)
{
    struct in_addr addr;

    if (inet_pton(AF_INET, ipAddress.c_str(), &addr) != 1)
    {
        fprintf(stderr, "ConvertIPv4StringToAddress: string passada não é um IPv4 válido: %s\n",
                ipAddress.c_str());
        abort();
    }

    return addr.s_addr;
}

// Oposto do ConvertIPv4StringToAddress: converte um endereço para representação textual do endereço
// IPv4
std::string ConvertAddressToIPv4String(const struct sockaddr_in& addr)
{
    char ipStr[INET_ADDRSTRLEN];

    if (inet_ntop(AF_INET, &(addr.sin_addr), ipStr, INET_ADDRSTRLEN) == nullptr)
    {
        fprintf(stderr, "ConvertAddressToIPv4String: erro na conversão\n");
        abort();
    }

    return std::string(ipStr);
}

// Comparar igualidade de dois endereços
bool socketAddressEqual(const sockaddr_in& addr1, const sockaddr_in& addr2)
{
    // Compare the address family
    if (addr1.sin_family != addr2.sin_family)
    {
        return false;
    }

    // Compare the IP address
    if (addr1.sin_addr.s_addr != addr2.sin_addr.s_addr)
    {
        return false;
    }

    // Compare the port number
    if (addr1.sin_port != addr2.sin_port)
    {
        return false;
    }

    return true;
}