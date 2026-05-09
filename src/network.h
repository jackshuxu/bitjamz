#pragma once
#include <cstdint>
#include <memory>
#include "session.h"

inline constexpr uint16_t NET_PORT     = 47474;
inline constexpr int      NET_FLUSH_MS = 50;

enum MsgType : uint8_t {
    MSG_HANDSHAKE      = 0x00,
    MSG_HANDSHAKE_FAIL = 0x01,
    MSG_STATE          = 0x02,
    MSG_EDIT           = 0x03,
};

class Network {
public:
    // Start listener on NET_PORT and a flush thread that broadcasts MSG_EDIT
    // packets to every connected peer at NET_FLUSH_MS cadence. Per-peer
    // recv threads apply inbound edits to `state` and dirty them so the
    // flush relays back to all peers (the original sender included).
    static void host(std::shared_ptr<SessionState> state);

    // Synchronous: connect, handshake, recv MSG_STATE. On success, leaves
    // the socket open, spawns a recv thread + a flush thread, and returns
    // a fresh SessionState with `network_alive == true`. Returns nullptr
    // on any failure (host unreachable, wrong room, socket error).
    static std::shared_ptr<SessionState> join(const char* ip, uint16_t room);

    // Signal everything to stop, shutdown sockets to unblock blocking
    // recv/accept, join all background threads. Safe to call when no host
    // or join was issued, and safe to call multiple times.
    static void stop();
};
