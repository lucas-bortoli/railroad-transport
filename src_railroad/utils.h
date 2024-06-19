#pragma once
#include <arpa/inet.h>
#include <cstdio>
#include <netinet/in.h>
#include <string>

// Converter uma std::string contendo uma representação textual do IPv4 para um endereço
in_addr_t ConvertIPv4StringToAddress(const std::string& ipAddress);

// Oposto do ConvertIPv4StringToAddress: converte um endereço para representação textual do endereço
// IPv4
std::string ConvertAddressToIPv4String(const struct sockaddr_in& addr);

// Comparar igualidade de dois endereços
bool socketAddressEqual(const sockaddr_in& addr1, const sockaddr_in& addr2);

// Retorna o tempo atual em milissegundos desde 1970-01-01.
// https://stackoverflow.com/a/44896326
unsigned long long timeInMilliseconds();