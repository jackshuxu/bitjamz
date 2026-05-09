#include "network.h"
#include "session.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace {

std::atomic<bool>          g_stop{false};
int                        g_listener_fd = -1;
std::thread                g_listener_thread;
std::mutex                 g_peers_mutex;
std::vector<std::thread>   g_peer_threads;

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

void per_peer_handler(int sock, std::shared_ptr<SessionState> state) {
    uint8_t  type = 0;
    uint16_t room_n = 0;
    if (!recv_all(sock, &type, 1))                 { ::close(sock); return; }
    if (type != MSG_HANDSHAKE)                     { ::close(sock); return; }
    if (!recv_all(sock, &room_n, sizeof(room_n)))  { ::close(sock); return; }
    uint16_t room = ntohs(room_n);

    if (room != state->session_id) {
        uint8_t fail = MSG_HANDSHAKE_FAIL;
        send_all(sock, &fail, 1);
        ::close(sock);
        return;
    }

    MsgState m = build_msg_state(*state);
    uint8_t tag = MSG_STATE;
    send_all(sock, &tag, 1);
    send_all(sock, &m,   sizeof(m));
    ::close(sock);
}

void listener_loop(std::shared_ptr<SessionState> state) {
    while (!g_stop.load()) {
        sockaddr_in peer{};
        socklen_t   peer_len = sizeof(peer);
        int sock = ::accept(g_listener_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (sock < 0) {
            if (g_stop.load()) break;
            // Transient accept error: avoid a hot spin loop on EBADF/ECONNABORTED/etc.
            if (errno == EINTR) continue;
            break;
        }
        std::lock_guard<std::mutex> lock(g_peers_mutex);
        g_peer_threads.emplace_back(per_peer_handler, sock, state);
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

    g_listener_thread = std::thread(listener_loop, state);
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
    ::close(sock);

    auto s = std::make_shared<SessionState>();
    apply_msg_state(*s, m);
    return s;
}

void Network::stop() {
    g_stop.store(true);
    if (g_listener_fd >= 0) {
        ::shutdown(g_listener_fd, SHUT_RDWR);
        ::close(g_listener_fd);
        g_listener_fd = -1;
    }
    if (g_listener_thread.joinable()) g_listener_thread.join();
    {
        std::lock_guard<std::mutex> lock(g_peers_mutex);
        for (auto& th : g_peer_threads) if (th.joinable()) th.join();
        g_peer_threads.clear();
    }
}
