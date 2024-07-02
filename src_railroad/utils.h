#pragma once
#include <arpa/inet.h>
#include <cstdio>
#include <map>
#include <netinet/in.h>
#include <string>
#include <vector>

// Comente para reativar logs
#define printf(...)
#define fprintf(...)

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

// Retorna uma CÓPIA**** dos N primeiros itens do mapa ordenado de transmissão, sem afetá-lo.
template <typename T> std::vector<T> rr_peek_first_n_items_tx(const std::map<unsigned long, T>& tx, unsigned int n)
{
    std::vector<T> items;

    auto it = tx.begin();
    size_t counter = 0;

    // Pegar primeiros N itens
    while (it != tx.end() && counter < n)
    {
        items.push_back(it->second);
        ++it;
        ++counter;
    }

    return items;
}
