#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "session.h"
#include "network.h"

// Internal helpers exposed by network.cpp so tests can drive a second joiner
// over a raw socket without going through Network::join's single-slot globals.
namespace bitjams_net_internal {
    size_t build_msg_edit(SessionState& s, uint8_t* buf, size_t buf_cap);
    bool   recv_and_apply_msg_edit(int sock, SessionState& s, bool is_joiner);
}

// Hand-rolled secondary joiner. First joiner uses Network::join; this one
// connects over a raw socket and runs its own recv + flush threads. Lets a
// single-process test exercise multi-joiner scenarios.
class TestJoiner {
public:
    std::shared_ptr<SessionState> state;
    int                           sock = -1;
    std::thread                   recv_th;
    std::thread                   flush_th;

    static std::unique_ptr<TestJoiner> connect(uint16_t room) {
        int s = ::socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return nullptr;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(NET_PORT);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(s); return nullptr;
        }
        uint8_t  hs = MSG_HANDSHAKE;
        uint16_t rn = htons(room);
        if (::send(s, &hs, 1, 0) != 1)             { ::close(s); return nullptr; }
        if (::send(s, &rn, sizeof(rn), 0) != (ssize_t)sizeof(rn)) {
            ::close(s); return nullptr;
        }
        uint8_t resp = 0;
        if (::recv(s, &resp, 1, MSG_WAITALL) != 1) { ::close(s); return nullptr; }
        if (resp != MSG_STATE)                      { ::close(s); return nullptr; }

        MsgState m{};
        size_t got = 0;
        while (got < sizeof(m)) {
            ssize_t n = ::recv(s, reinterpret_cast<uint8_t*>(&m) + got, sizeof(m) - got, 0);
            if (n <= 0) { ::close(s); return nullptr; }
            got += static_cast<size_t>(n);
        }

        auto j = std::unique_ptr<TestJoiner>(new TestJoiner());
        j->sock  = s;
        j->state = std::make_shared<SessionState>();
        j->state->session_id = ntohs(m.session_id);
        j->state->bpm        = ntohl(m.bpm);
        for (int t = 0; t < TRACKS; ++t) {
            j->state->track_active[t] = (m.track_active[t] != 0);
            for (int st = 0; st < STEPS; ++st) {
                j->state->grid[t][st] = (m.grid[t][st] != 0);
            }
        }
        j->state->is_joiner = true;
        j->state->network_alive.store(true);

        j->recv_th  = std::thread(&TestJoiner::recv_loop,  j.get());
        j->flush_th = std::thread(&TestJoiner::flush_loop, j.get());
        return j;
    }

    void close_socket_force() {
        // Simulates an abrupt disconnect from outside the joiner's flush
        // path — used by the disconnect test.
        state->network_alive.store(false);
        if (sock >= 0) {
            ::shutdown(sock, SHUT_RDWR);
        }
    }

    ~TestJoiner() {
        state->network_alive.store(false);
        if (sock >= 0) {
            ::shutdown(sock, SHUT_RDWR);
        }
        if (recv_th.joinable())  recv_th.join();
        if (flush_th.joinable()) flush_th.join();
        if (sock >= 0) {
            ::close(sock);
            sock = -1;
        }
    }

private:
    TestJoiner() = default;

    void recv_loop() {
        while (state->network_alive.load()) {
            uint8_t tag = 0;
            ssize_t n = ::recv(sock, &tag, 1, MSG_WAITALL);
            if (n != 1) break;
            if (tag != MSG_EDIT) break;
            if (!bitjams_net_internal::recv_and_apply_msg_edit(sock, *state, /*is_joiner=*/true)) break;
        }
        state->network_alive.store(false);
    }

    void flush_loop() {
        uint8_t buf[1024];
        while (state->network_alive.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(NET_FLUSH_MS));
            if (!state->network_alive.load()) break;
            size_t payload_len = bitjams_net_internal::build_msg_edit(*state, buf + 1, sizeof(buf) - 1);
            if (payload_len == 0) continue;
            buf[0] = MSG_EDIT;
            size_t total = payload_len + 1;
            size_t sent = 0;
            const uint8_t* p = buf;
            while (sent < total) {
                ssize_t n = ::send(sock, p + sent, total - sent, 0);
                if (n <= 0) { state->network_alive.store(false); return; }
                sent += static_cast<size_t>(n);
            }
        }
    }
};

static void wait_for_listener() {
    // Listener bind happens before host() returns; small grace for accept().
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

static void test_state_sync() {
    auto host_state = std::make_shared<SessionState>();
    host_state->session_id      = 4242;
    host_state->bpm             = 137;
    host_state->grid[2][5]      = true;
    host_state->grid[7][9]      = true;
    host_state->grid[11][15]    = true;
    host_state->track_active[3] = false;
    host_state->track_active[8] = false;

    Network::host(host_state);
    wait_for_listener();

    auto joiner_state = Network::join("127.0.0.1", 4242);
    assert(joiner_state != nullptr);
    assert(joiner_state->session_id == 4242);
    assert(joiner_state->bpm == 137);
    assert(joiner_state->grid[2][5] == true);
    assert(joiner_state->grid[7][9] == true);
    assert(joiner_state->grid[11][15] == true);
    assert(joiner_state->grid[0][0] == false);
    assert(joiner_state->track_active[3] == false);
    assert(joiner_state->track_active[8] == false);
    assert(joiner_state->track_active[0] == true);

    Network::stop();
    std::cout << "test_state_sync PASSED\n";
}

static void test_wrong_room_rejected() {
    auto host_state = std::make_shared<SessionState>();
    host_state->session_id = 4242;

    Network::host(host_state);
    wait_for_listener();

    auto joiner_state = Network::join("127.0.0.1", 9999);
    assert(joiner_state == nullptr);

    // Host's state must be unchanged — no crash, no mutation.
    assert(host_state->session_id == 4242);

    Network::stop();
    std::cout << "test_wrong_room_rejected PASSED\n";
}

static void wait_flush_windows(int n = 3) {
    // 50ms flush window; n full windows + a small buffer for scheduling jitter.
    std::this_thread::sleep_for(std::chrono::milliseconds(50 * n + 30));
}

static void test_edit_propagates_host_to_joiner() {
    auto host_state = std::make_shared<SessionState>();
    host_state->session_id = 5050;

    Network::host(host_state);
    wait_for_listener();

    auto joiner_state = Network::join("127.0.0.1", 5050);
    assert(joiner_state != nullptr);
    assert(joiner_state->grid[3][7] == false);

    // Simulate a host UI mutation.
    host_state->grid[3][7] = true;
    host_state->dirty[3].fetch_or(static_cast<uint16_t>(1) << 7);

    wait_flush_windows();

    assert(joiner_state->grid[3][7] == true);

    Network::stop();
    std::cout << "test_edit_propagates_host_to_joiner PASSED\n";
}

static void test_edit_propagates_joiner_to_host() {
    auto host_state = std::make_shared<SessionState>();
    host_state->session_id = 5151;

    Network::host(host_state);
    wait_for_listener();

    auto joiner_state = Network::join("127.0.0.1", 5151);
    assert(joiner_state != nullptr);

    // Simulate joiner UI mutation.
    joiner_state->grid[5][2] = true;
    joiner_state->dirty[5].fetch_or(static_cast<uint16_t>(1) << 2);

    wait_flush_windows();

    // Host received and applied the edit.
    assert(host_state->grid[5][2] == true);

    // Host's relay echo arrived back at the joiner and cleared the in-flight
    // guard.
    uint16_t bit = static_cast<uint16_t>(1) << 2;
    assert((joiner_state->in_flight[5].load() & bit) == 0);

    Network::stop();
    std::cout << "test_edit_propagates_joiner_to_host PASSED\n";
}

static void test_two_joiners_non_conflicting() {
    auto host_state = std::make_shared<SessionState>();
    host_state->session_id = 5252;

    Network::host(host_state);
    wait_for_listener();

    auto joiner_a = Network::join("127.0.0.1", 5252);
    assert(joiner_a != nullptr);

    auto joiner_b = TestJoiner::connect(5252);
    assert(joiner_b != nullptr);

    // Different cells, no conflict.
    joiner_a->grid[1][4] = true;
    joiner_a->dirty[1].fetch_or(static_cast<uint16_t>(1) << 4);

    joiner_b->state->grid[9][12] = true;
    joiner_b->state->dirty[9].fetch_or(static_cast<uint16_t>(1) << 12);

    wait_flush_windows();

    assert(host_state->grid[1][4]   == true);
    assert(host_state->grid[9][12]  == true);
    assert(joiner_a->grid[1][4]     == true);
    assert(joiner_a->grid[9][12]    == true);
    assert(joiner_b->state->grid[1][4]  == true);
    assert(joiner_b->state->grid[9][12] == true);

    joiner_b.reset();
    Network::stop();
    std::cout << "test_two_joiners_non_conflicting PASSED\n";
}

static void test_two_joiners_conflicting_converge() {
    auto host_state = std::make_shared<SessionState>();
    host_state->session_id = 5353;

    Network::host(host_state);
    wait_for_listener();

    auto joiner_a = Network::join("127.0.0.1", 5353);
    assert(joiner_a != nullptr);

    auto joiner_b = TestJoiner::connect(5353);
    assert(joiner_b != nullptr);

    // Same cell. Joiner A wants true, joiner B wants false. Both flush at
    // ~the same time. The host's relay is authoritative.
    joiner_a->grid[3][5] = true;
    joiner_a->dirty[3].fetch_or(static_cast<uint16_t>(1) << 5);

    joiner_b->state->grid[3][5] = false;
    joiner_b->state->dirty[3].fetch_or(static_cast<uint16_t>(1) << 5);

    // Extra window — gives the conflict-flicker time to resolve.
    wait_flush_windows(4);

    bool host_v = host_state->grid[3][5];
    assert(joiner_a->grid[3][5]            == host_v);
    assert(joiner_b->state->grid[3][5]     == host_v);

    joiner_b.reset();
    Network::stop();
    std::cout << "test_two_joiners_conflicting_converge PASSED\n";
}

static void test_joiner_disconnect_does_not_crash_host() {
    auto host_state = std::make_shared<SessionState>();
    host_state->session_id = 5454;

    Network::host(host_state);
    wait_for_listener();

    // Joiner A: raw-socket so we can yank its connection abruptly.
    auto joiner_a = TestJoiner::connect(5454);
    assert(joiner_a != nullptr);

    // Joiner B: the survivor.
    auto joiner_b = Network::join("127.0.0.1", 5454);
    assert(joiner_b != nullptr);

    // Force-close joiner A.
    joiner_a->close_socket_force();
    wait_flush_windows();

    // Surviving joiner B can still send edits and the host applies + relays.
    joiner_b->grid[2][2] = true;
    joiner_b->dirty[2].fetch_or(static_cast<uint16_t>(1) << 2);

    wait_flush_windows();

    assert(host_state->grid[2][2] == true);
    assert(joiner_b->grid[2][2]   == true);

    joiner_a.reset();
    Network::stop();
    std::cout << "test_joiner_disconnect_does_not_crash_host PASSED\n";
}

int main() {
    test_state_sync();
    test_wrong_room_rejected();
    test_edit_propagates_host_to_joiner();
    test_edit_propagates_joiner_to_host();
    test_two_joiners_non_conflicting();
    test_two_joiners_conflicting_converge();
    test_joiner_disconnect_does_not_crash_host();
    std::cout << "All net tests passed.\n";
    return 0;
}
