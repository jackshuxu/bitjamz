#pragma once

#include <cstdint>
#include <memory>

#include "session.h"

// Single compile-time port for all session connections.
inline constexpr uint16_t NET_PORT = 47474;

// Flush window for outbound diff aggregation.
inline constexpr int NET_FLUSH_MS = 50;

// Wire message type tags. Each message on the socket is a 1-byte type
// prefix followed by a fixed-size packed payload.
enum MsgType : uint8_t {
    MSG_HANDSHAKE = 0x00, // joiner -> host: session_id only
    MSG_STATE     = 0x01, // host -> peer: full SessionState snapshot
    MSG_DIFF      = 0x02, // peer <-> host: incremental edits
};

// Public net interface. Implementations live in net.cpp (future work).
void net_host(std::shared_ptr<SessionState> state, uint16_t port);
void net_join(std::shared_ptr<SessionState> state, const char* ip, uint16_t port);
void net_stop();

class Network {
public:
    static int connect_to_server(uint32_t address_num, uint16_t room);
    static void receive_connections();
    static void client_handler(int client_sock);
};