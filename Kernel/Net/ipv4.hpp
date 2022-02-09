#ifndef IPV4_HPP
#define IPV4_HPP

#include "../Mem/mm.hpp"
#include <LibC/types.hpp>

typedef struct ipv4_packet {
    uint8_t header_length : 4;
    uint8_t version : 4;
    uint8_t tos;
    uint16_t length;

    uint16_t ident;
    uint16_t flags;

    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;

    uint32_t source_ip;
    uint32_t destination_ip;
} __attribute__((packed)) ipv4_packet_t;

namespace IPV4 {
uint16_t calculate_checksum(ipv4_packet_t* packet, uint32_t size = sizeof(ipv4_packet_t));
bool handle_packet(ipv4_packet_t* ipv4, uint32_t size);
void send_packet(uint32_t destination_ip, uint8_t* buffer, uint32_t size);
}

#endif