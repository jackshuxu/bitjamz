#include "network.h"
#include "session.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

std::atomic<bool>          g_stop{false};

// --- host-side state ---
int                        g_listener_fd = -1;
std::thread                g_listener_thread;
std::thread                g_host_flush_thread;

struct Peer {
    int               sock = -1;
    std::atomic<bool> alive{true};
};

std::mutex                            g_peers_mutex;
std::vector<std::shared_ptr<Peer>>    g_peers;
std::vector<std::thread>              g_peer_threads;

// --- joiner-side state ---
int                        g_joiner_sock = -1;
std::thread                g_joiner_recv_thread;
std::thread                g_joiner_flush_thread;

bool send_all(int sock, const void* buf, size_t len) {
    const auto* p = static_cast<const uint8_t*>(buf);
    while (len > 0) {
        ssize_t n = ::send(sock, p, len, 0);
        if (n <= 0) return false;
        p   += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool recv_all(int sock, void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);
    while (len > 0) {
        ssize_t n = ::recv(sock, p, len, 0);
        if (n <= 0) return false;
        p   += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

MsgState build_msg_state(const SessionState& s) {
    MsgState m{};
    m.session_id = htons(s.session_id);
    m.bpm        = htonl(s.bpm);
    for (int t = 0; t < TRACKS; ++t) {
        m.track_active[t] = s.track_active[t] ? 1 : 0;
        for (int st = 0; st < STEPS; ++st) {
            m.grid[t][st] = s.grid[t][st] ? 1 : 0;
        }
    }
    return m;
}

void apply_msg_state(SessionState& s, const MsgState& m) {
    s.session_id = ntohs(m.session_id);
    s.bpm        = ntohl(m.bpm);
    for (int t = 0; t < TRACKS; ++t) {
        s.track_active[t] = (m.track_active[t] != 0);
        for (int st = 0; st < STEPS; ++st) {
            s.grid[t][st] = (m.grid[t][st] != 0);
        }
    }
}

} // anonymous namespace

// MSG_EDIT helpers — placed in a non-anonymous namespace so the test binary
// can drive a second joiner over a raw socket without going through the
// global single-joiner path.
namespace bitjams_net_internal {

size_t build_msg_edit(SessionState& s, uint8_t* buf, size_t buf_cap) {
    // Layout written: [count][cells...][bpm_present][bpm32][ta_present][ta_mask16]
    // Min: 1 + 0 + 1 + 4 + 1 + 2 = 9 bytes.
    // Max: 1 + TRACKS*STEPS*3 + 1 + 4 + 1 + 2 = 585 bytes.
    if (buf_cap < 9) return 0;

    uint8_t* p = buf;
    uint8_t* count_ptr = p++;
    uint8_t cell_count = 0;

    bool joiner_path = s.network_alive.load() && s.is_joiner;

    for (int t = 0; t < TRACKS; ++t) {
        uint16_t claimed = s.dirty[t].exchange(0);
        if (joiner_path && claimed) {
            s.in_flight[t].fetch_or(claimed);
        }
        uint16_t bits = claimed;
        while (bits) {
            int step = __builtin_ctz(bits);
            *p++ = static_cast<uint8_t>(t);
            *p++ = static_cast<uint8_t>(step);
            *p++ = s.grid[t][step] ? 1 : 0;
            ++cell_count;
            bits &= bits - 1;
        }
    }
    *count_ptr = cell_count;

    bool bpm_changed = s.bpm_dirty.exchange(false);
    *p++ = bpm_changed ? 1 : 0;
    int32_t bpm_n = htonl(s.bpm);
    std::memcpy(p, &bpm_n, sizeof(bpm_n));
    p += sizeof(bpm_n);
    if (bpm_changed && joiner_path) {
        s.bpm_in_flight.store(true);
    }

    uint16_t ta_dirty = s.track_active_dirty.exchange(0);
    bool ta_changed = (ta_dirty != 0);
    *p++ = ta_changed ? 1 : 0;
    uint16_t ta_mask = 0;
    for (int t = 0; t < TRACKS; ++t) {
        if (s.track_active[t]) ta_mask |= static_cast<uint16_t>(1) << t;
    }
    uint16_t ta_mask_n = htons(ta_mask);
    std::memcpy(p, &ta_mask_n, sizeof(ta_mask_n));
    p += sizeof(ta_mask_n);
    if (ta_changed && joiner_path) {
        s.track_active_in_flight.fetch_or(ta_dirty);
    }

    bool any_change = (cell_count > 0) || bpm_changed || ta_changed;
    return any_change ? static_cast<size_t>(p - buf) : 0;
}

bool recv_and_apply_msg_edit(int sock, SessionState& s, bool is_joiner) {
    uint8_t cell_count = 0;
    if (!recv_all(sock, &cell_count, 1)) return false;

    for (int i = 0; i < cell_count; ++i) {
        uint8_t triple[3];
        if (!recv_all(sock, triple, 3)) return false;
        uint8_t t = triple[0];
        uint8_t step = triple[1];
        uint8_t v = triple[2];
        if (t >= TRACKS || step >= STEPS) continue;

        uint16_t bit = static_cast<uint16_t>(1) << step;
        if (is_joiner) {
            if (s.dirty[t].load() & bit) {
                continue;
            }
            if (s.in_flight[t].load() & bit) {
                s.grid[t][step] = (v != 0);
                s.in_flight[t].fetch_and(static_cast<uint16_t>(~bit));
                continue;
            }
            s.grid[t][step] = (v != 0);
        } else {
            s.grid[t][step] = (v != 0);
            s.dirty[t].fetch_or(bit);
        }
    }

    uint8_t bpm_present = 0;
    int32_t bpm_n = 0;
    if (!recv_all(sock, &bpm_present, 1)) return false;
    if (!recv_all(sock, &bpm_n, sizeof(bpm_n))) return false;
    if (bpm_present) {
        int32_t bpm = ntohl(bpm_n);
        if (is_joiner) {
            if (!s.bpm_dirty.load()) {
                s.bpm = bpm;
                if (s.bpm_in_flight.load()) {
                    s.bpm_in_flight.store(false);
                }
            }
        } else {
            s.bpm = bpm;
            s.bpm_dirty.store(true);
        }
    }

    uint8_t  ta_present = 0;
    uint16_t ta_mask_n  = 0;
    if (!recv_all(sock, &ta_present, 1)) return false;
    if (!recv_all(sock, &ta_mask_n, sizeof(ta_mask_n))) return false;
    if (ta_present) {
        uint16_t ta_mask = ntohs(ta_mask_n);
        for (int t = 0; t < TRACKS; ++t) {
            uint16_t tb = static_cast<uint16_t>(1) << t;
            bool incoming = (ta_mask & tb) != 0;
            if (is_joiner) {
                if (s.track_active_dirty.load() & tb) continue;
                if (s.track_active_in_flight.load() & tb) {
                    s.track_active[t] = incoming;
                    s.track_active_in_flight.fetch_and(static_cast<uint16_t>(~tb));
                    continue;
                }
                s.track_active[t] = incoming;
            } else {
                s.track_active[t] = incoming;
                s.track_active_dirty.fetch_or(tb);
            }
        }
    }

    return true;
}

} // namespace bitjams_net_internal

namespace {

using bitjams_net_internal::build_msg_edit;
using bitjams_net_internal::recv_and_apply_msg_edit;

void per_peer_handler(int sock, std::shared_ptr<SessionState> state, std::shared_ptr<Peer> peer) {
    uint8_t  type = 0;
    uint16_t room_n = 0;
    if (!recv_all(sock, &type, 1))                 { peer->alive.store(false); ::close(sock); return; }
    if (type != MSG_HANDSHAKE)                     { peer->alive.store(false); ::close(sock); return; }
    if (!recv_all(sock, &room_n, sizeof(room_n)))  { peer->alive.store(false); ::close(sock); return; }
    uint16_t room = ntohs(room_n);

    if (room != state->session_id) {
        uint8_t fail = MSG_HANDSHAKE_FAIL;
        send_all(sock, &fail, 1);
        peer->alive.store(false);
        ::close(sock);
        return;
    }

    MsgState m = build_msg_state(*state);
    uint8_t tag = MSG_STATE;
    if (!send_all(sock, &tag, 1) || !send_all(sock, &m, sizeof(m))) {
        peer->alive.store(false);
        ::close(sock);
        return;
    }

    // Register this peer for the host flush thread to broadcast to.
    {
        std::lock_guard<std::mutex> lk(g_peers_mutex);
        peer->sock = sock;
        g_peers.push_back(peer);
    }

    // Long-lived recv loop. Inbound MSG_EDIT packets get applied to host
    // state and dirtied for relay via the host flush thread.
    while (!g_stop.load() && peer->alive.load()) {
        uint8_t edit_tag = 0;
        if (!recv_all(sock, &edit_tag, 1)) break;
        if (edit_tag != MSG_EDIT) break;
        if (!recv_and_apply_msg_edit(sock, *state, /*is_joiner=*/false)) break;
    }

    peer->alive.store(false);
    ::shutdown(sock, SHUT_RDWR);
    ::close(sock);
    // Reaper inside host_flush_loop drops dead peers from g_peers.
}

void listener_loop(std::shared_ptr<SessionState> state) {
    while (!g_stop.load()) {
        sockaddr_in peer_addr{};
        socklen_t   peer_len = sizeof(peer_addr);
        int sock = ::accept(g_listener_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
        if (sock < 0) {
            if (g_stop.load()) break;
            if (errno == EINTR) continue;
            break;
        }
        auto peer = std::make_shared<Peer>();
        std::lock_guard<std::mutex> lock(g_peers_mutex);
        g_peer_threads.emplace_back(per_peer_handler, sock, state, peer);
    }
}

void host_flush_loop(std::shared_ptr<SessionState> state) {
    uint8_t buf[1024];
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(NET_FLUSH_MS));
        if (g_stop.load()) break;

        size_t payload_len = build_msg_edit(*state, buf + 1, sizeof(buf) - 1);
        if (payload_len > 0) {
            buf[0] = MSG_EDIT;

            std::vector<std::shared_ptr<Peer>> snapshot;
            {
                std::lock_guard<std::mutex> lk(g_peers_mutex);
                snapshot = g_peers;
            }
            for (auto& p : snapshot) {
                if (!p->alive.load()) continue;
                if (!send_all(p->sock, buf, payload_len + 1)) {
                    p->alive.store(false);
                }
            }
        }

        // Reap dead peers regardless of whether we sent.
        std::lock_guard<std::mutex> lk(g_peers_mutex);
        std::erase_if(g_peers, [](auto& p){ return !p->alive.load(); });
    }
}

void joiner_recv_loop(int sock, std::shared_ptr<SessionState> state) {
    while (state->network_alive.load()) {
        uint8_t tag = 0;
        if (!recv_all(sock, &tag, 1)) break;
        if (tag != MSG_EDIT) break;
        if (!recv_and_apply_msg_edit(sock, *state, /*is_joiner=*/true)) break;
    }
    state->network_alive.store(false);
    // fd lifecycle owned by Network::stop().
}

void joiner_flush_loop(int sock, std::shared_ptr<SessionState> state) {
    uint8_t buf[1024];
    while (state->running.load() && state->network_alive.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(NET_FLUSH_MS));
        if (!state->network_alive.load()) break;
        size_t payload_len = build_msg_edit(*state, buf + 1, sizeof(buf) - 1);
        if (payload_len == 0) continue;
        buf[0] = MSG_EDIT;
        if (!send_all(sock, buf, payload_len + 1)) {
            state->network_alive.store(false);
            break;
        }
    }
}

} // anonymous namespace

void Network::host(std::shared_ptr<SessionState> state) {
    g_stop.store(false);

    g_listener_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    ::setsockopt(g_listener_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(NET_PORT);
    if (::bind(g_listener_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::perror("bind");
        ::close(g_listener_fd);
        g_listener_fd = -1;
        return;
    }
    if (::listen(g_listener_fd, 5) < 0) {
        ::perror("listen");
        ::close(g_listener_fd);
        g_listener_fd = -1;
        return;
    }

    g_listener_thread   = std::thread(listener_loop, state);
    g_host_flush_thread = std::thread(host_flush_loop, state);
}

std::shared_ptr<SessionState> Network::join(const char* ip, uint16_t room) {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return nullptr;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(NET_PORT);
    if (::inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        ::close(sock);
        return nullptr;
    }
    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(sock);
        return nullptr;
    }

    uint8_t  hs = MSG_HANDSHAKE;
    uint16_t rn = htons(room);
    if (!send_all(sock, &hs, 1) ||
        !send_all(sock, &rn, sizeof(rn))) {
        ::close(sock);
        return nullptr;
    }

    uint8_t resp = 0;
    if (!recv_all(sock, &resp, 1))  { ::close(sock); return nullptr; }
    if (resp == MSG_HANDSHAKE_FAIL) { ::close(sock); return nullptr; }
    if (resp != MSG_STATE)          { ::close(sock); return nullptr; }

    MsgState m{};
    if (!recv_all(sock, &m, sizeof(m))) { ::close(sock); return nullptr; }

    auto s = std::make_shared<SessionState>();
    apply_msg_state(*s, m);
    s->is_joiner = true;
    s->network_alive.store(true);

    g_joiner_sock         = sock;
    g_joiner_recv_thread  = std::thread(joiner_recv_loop,  sock, s);
    g_joiner_flush_thread = std::thread(joiner_flush_loop, sock, s);
    return s;
}

void Network::stop() {
    g_stop.store(true);

    // --- host teardown ---
    if (g_listener_fd >= 0) {
        ::shutdown(g_listener_fd, SHUT_RDWR);
        ::close(g_listener_fd);
        g_listener_fd = -1;
    }
    if (g_listener_thread.joinable())   g_listener_thread.join();
    if (g_host_flush_thread.joinable()) g_host_flush_thread.join();
    {
        std::lock_guard<std::mutex> lk(g_peers_mutex);
        for (auto& p : g_peers) {
            p->alive.store(false);
            if (p->sock >= 0) ::shutdown(p->sock, SHUT_RDWR);
        }
    }
    {
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> lk(g_peers_mutex);
            threads.swap(g_peer_threads);
        }
        for (auto& th : threads) if (th.joinable()) th.join();
    }
    {
        std::lock_guard<std::mutex> lk(g_peers_mutex);
        g_peers.clear();
    }

    // --- joiner teardown ---
    if (g_joiner_sock >= 0) {
        ::shutdown(g_joiner_sock, SHUT_RDWR);
    }
    if (g_joiner_recv_thread.joinable())  g_joiner_recv_thread.join();
    if (g_joiner_flush_thread.joinable()) g_joiner_flush_thread.join();
    if (g_joiner_sock >= 0) {
        ::close(g_joiner_sock);
        g_joiner_sock = -1;
    }

    g_stop.store(false);
}
