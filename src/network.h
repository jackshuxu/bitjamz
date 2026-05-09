#pragma once
#include <cstdint>
#include <memory>
#include "session.h"

inline constexpr uint16_t NET_PORT     = 47474;
inline constexpr int      NET_FLUSH_MS = 50;  // unused this milestone

enum MsgType : uint8_t {
    MSG_HANDSHAKE      = 0x00,
    MSG_HANDSHAKE_FAIL = 0x01,
    MSG_STATE          = 0x02,
    // MSG_DIFF        = 0x03,  // milestone-2
};

class Network {
public:
    // Start listener on NET_PORT. Background thread accepts peers; each
    // accepted peer gets a short-lived thread that does handshake + STATE
    // send + close. Idempotent: calling again with no prior stop is UB —
    // call stop() first.
    static void host(std::shared_ptr<SessionState> state);

    // Synchronous: connect, handshake, recv STATE, close. Returns a fresh
    // SessionState built from the received bytes. Returns nullptr on any
    // failure (host unreachable, wrong room, socket error).
    static std::shared_ptr<SessionState> join(const char* ip, uint16_t room);

    // Signal the listener to stop, shutdown its socket to unblock accept,
    // join all peer threads. Safe to call when no host was started.
    static void stop();
};
