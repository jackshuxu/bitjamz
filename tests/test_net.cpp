#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>

#include "net_compat.h"

#include "session.h"
#include "network.h"

// Internal helpers exposed by network.cpp so tests can drive a second joiner
// over a raw socket without going through Network::join's single-slot globals.
namespace bitjams_net_internal {
    bool   send_msg_state(int sock, const SessionState& s);
    bool   recv_and_apply_msg_state(int sock, SessionState& s);
    size_t build_msg_edit(SessionState& s, uint8_t* buf, size_t buf_cap);
    bool   recv_and_apply_msg_edit(int sock, SessionState& s, bool is_joiner);
    size_t build_msg_melodic_track(int melodic_idx, const std::vector<Note>& notes,
                                   uint8_t* buf, size_t buf_cap);
    bool   recv_and_apply_msg_melodic_track(int sock, SessionState& s, bool is_joiner);
    size_t build_pending_aux(SessionState& s, uint8_t* buf, size_t buf_cap);
}

// Creates a connected socket pair. On POSIX uses AF_UNIX socketpair; on
// Windows uses a TCP loopback pair (AF_UNIX socketpair is unavailable).
static bool make_socket_pair(int sv[2]) {
#ifndef _WIN32
    return ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0;
#else
    int listener = create_socket();
    if (listener < 0) return false;
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close_socket(listener); return false;
    }
    if (::listen(listener, 1) < 0) { close_socket(listener); return false; }
    socklen_t len = sizeof(addr);
    ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
    sv[0] = create_socket();
    if (sv[0] < 0) { close_socket(listener); return false; }
    if (::connect(sv[0], reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close_socket(sv[0]); close_socket(listener); return false;
    }
    sv[1] = accept_socket(listener, nullptr, nullptr);
    close_socket(listener);
    return sv[1] >= 0;
#endif
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
        int s = create_socket();
        if (s < 0) return nullptr;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(NET_PORT);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_socket(s); return nullptr;
        }
        uint8_t  hs = MSG_HANDSHAKE;
        uint16_t rn = htons(room);
        if (::send(s, reinterpret_cast<const char*>(&hs), 1, 0) != 1) {
            close_socket(s); return nullptr;
        }
        if (::send(s, reinterpret_cast<const char*>(&rn), sizeof(rn), 0) != static_cast<int>(sizeof(rn))) {
            close_socket(s); return nullptr;
        }
        uint8_t resp = 0;
        if (::recv(s, reinterpret_cast<char*>(&resp), 1, MSG_WAITALL) != 1) { close_socket(s); return nullptr; }
        if (resp != MSG_STATE)                      { close_socket(s); return nullptr; }

        auto j = std::unique_ptr<TestJoiner>(new TestJoiner());
        j->sock  = s;
        j->state = std::make_shared<SessionState>();
        if (!bitjams_net_internal::recv_and_apply_msg_state(s, *j->state)) {
            close_socket(s); return nullptr;
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
            close_socket(sock);
            sock = -1;
        }
    }

private:
    TestJoiner() = default;

    void recv_loop() {
        while (state->network_alive.load()) {
            uint8_t tag = 0;
            int n = ::recv(sock, reinterpret_cast<char*>(&tag), 1, MSG_WAITALL);
            if (n != 1) break;
            if (tag != MSG_EDIT) break;
            if (!bitjams_net_internal::recv_and_apply_msg_edit(sock, *state, /*is_joiner=*/true)) break;
        }
        state->network_alive.store(false);
    }

    void send_buf(const uint8_t* buf, size_t total) {
        size_t sent = 0;
        while (sent < total) {
            int n = ::send(sock, reinterpret_cast<const char*>(buf + sent), static_cast<int>(total - sent), 0);
            if (n <= 0) { state->network_alive.store(false); return; }
            sent += static_cast<size_t>(n);
        }
    }

    void flush_loop() {
        uint8_t buf[2048];
        while (state->network_alive.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(NET_FLUSH_MS));
            if (!state->network_alive.load()) break;
            size_t payload_len = bitjams_net_internal::build_msg_edit(*state, buf + 1, sizeof(buf) - 1);
            if (payload_len > 0) {
                buf[0] = MSG_EDIT;
                send_buf(buf, payload_len + 1);
                if (!state->network_alive.load()) return;
            }
            for (;;) {
                size_t n = bitjams_net_internal::build_pending_aux(*state, buf, sizeof(buf));
                if (n == 0) break;
                send_buf(buf, n);
                if (!state->network_alive.load()) return;
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
    host_state->patterns[0]->drum_grid[DK_CLAP][5]   = true;
    host_state->patterns[0]->drum_grid[DK_CYMBAL][9] = true;
    // A melodic note on the drone track must also propagate.
    host_state->patterns[0]->melodic_notes[3 /*drone*/].push_back({15, 1, 60, 127});
    host_state->track_root_midi[9] = 55;  // bass: change from default

    Network::host(host_state);
    wait_for_listener();

    auto joiner_state = Network::join("127.0.0.1", 4242);
    assert(joiner_state != nullptr);
    assert(joiner_state->session_id == 4242);
    assert(joiner_state->bpm == 137);
    assert(joiner_state->patterns[0]->drum_grid[DK_CLAP][5]   == true);
    assert(joiner_state->patterns[0]->drum_grid[DK_CYMBAL][9] == true);
    assert(joiner_state->patterns[0]->drum_grid[DK_KICK][0]   == false);
    assert(joiner_state->patterns[0]->melodic_notes[3].size() == 1);
    assert(joiner_state->patterns[0]->melodic_notes[3][0].start_step == 15);
    assert(joiner_state->patterns[0]->melodic_notes[3][0].pitch_midi == 60);
    assert(joiner_state->track_root_midi[9] == 55);

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
    assert(joiner_state->patterns[0]->drum_grid[DK_CLOSED_HAT][7] == false);

    // Simulate a host UI mutation. Track 3 = chat = closed_hat drum_kind.
    host_state->patterns[0]->drum_grid[DK_CLOSED_HAT][7] = true;
    host_state->dirty[3].fetch_or(static_cast<uint16_t>(1) << 7);

    wait_flush_windows();

    assert(joiner_state->patterns[0]->drum_grid[DK_CLOSED_HAT][7] == true);

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

    // Simulate joiner UI mutation. Track 5 = cowb = DK_COWBELL drum_kind.
    joiner_state->patterns[0]->drum_grid[DK_COWBELL][2] = true;
    joiner_state->dirty[5].fetch_or(static_cast<uint16_t>(1) << 2);

    wait_flush_windows();

    // Host received and applied the edit.
    assert(host_state->patterns[0]->drum_grid[DK_COWBELL][2] == true);

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

    // Different drum cells, no conflict. Track 1 = snare, track 6 = tom.
    joiner_a->patterns[0]->drum_grid[DK_SNARE][4] = true;
    joiner_a->dirty[1].fetch_or(static_cast<uint16_t>(1) << 4);

    joiner_b->state->patterns[0]->drum_grid[DK_TOM][12] = true;
    joiner_b->state->dirty[6].fetch_or(static_cast<uint16_t>(1) << 12);

    wait_flush_windows();

    assert(host_state->patterns[0]->drum_grid[DK_SNARE][4]    == true);
    assert(host_state->patterns[0]->drum_grid[DK_TOM][12]     == true);
    assert(joiner_a->patterns[0]->drum_grid[DK_SNARE][4]      == true);
    assert(joiner_a->patterns[0]->drum_grid[DK_TOM][12]       == true);
    assert(joiner_b->state->patterns[0]->drum_grid[DK_SNARE][4]  == true);
    assert(joiner_b->state->patterns[0]->drum_grid[DK_TOM][12]   == true);

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
    // Same drum cell (chat track). Joiner A wants true, joiner B wants false.
    joiner_a->patterns[0]->drum_grid[DK_CLOSED_HAT][5] = true;
    joiner_a->dirty[3].fetch_or(static_cast<uint16_t>(1) << 5);

    joiner_b->state->patterns[0]->drum_grid[DK_CLOSED_HAT][5] = false;
    joiner_b->state->dirty[3].fetch_or(static_cast<uint16_t>(1) << 5);

    // Extra window — gives the conflict-flicker time to resolve.
    wait_flush_windows(4);

    bool host_v = host_state->patterns[0]->drum_grid[DK_CLOSED_HAT][5];
    assert(joiner_a->patterns[0]->drum_grid[DK_CLOSED_HAT][5]        == host_v);
    assert(joiner_b->state->patterns[0]->drum_grid[DK_CLOSED_HAT][5] == host_v);

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
    // Track 2 = clap = DK_CLAP.
    joiner_b->patterns[0]->drum_grid[DK_CLAP][2] = true;
    joiner_b->dirty[2].fetch_or(static_cast<uint16_t>(1) << 2);

    wait_flush_windows();

    assert(host_state->patterns[0]->drum_grid[DK_CLAP][2] == true);
    assert(joiner_b->patterns[0]->drum_grid[DK_CLAP][2]   == true);

    joiner_a.reset();
    Network::stop();
    std::cout << "test_joiner_disconnect_does_not_crash_host PASSED\n";
}

static void test_projection_lowest_wins() {
    std::vector<Note> notes;
    notes.push_back({0, 4, 69, 127});  // A4
    notes.push_back({0, 4, 72, 127});  // C5
    auto proj = project_row(notes, 16);
    // Both notes start at cell 0; lowest-pitch wins -> A4 (idx 0).
    assert(proj[0].note_idx == 0);
    assert(proj[0].is_start);
    // Continuation cell 1: active = {A4, C5}; tie on most-recent (both 0);
    // tie-break lowest -> A4 still.
    assert(proj[1].note_idx == 0);
    assert(!proj[1].is_start);
    std::cout << "test_projection_lowest_wins PASSED\n";
}

static void test_projection_stack_preempt_and_return() {
    std::vector<Note> notes;
    notes.push_back({0, 16, 69, 127});  // A4 covers entire loop
    notes.push_back({0,  1, 71, 127});  // B4 at cell 0 only
    notes.push_back({1,  2, 60, 127});  // C4 at cells 1..2
    auto proj = project_row(notes, 16);
    // cell 0: starters {A4, B4} -> A4 (lower).
    assert(proj[0].note_idx == 0);
    // cell 1: starter C4 -> C4.
    assert(proj[1].note_idx == 2);
    assert(proj[1].is_start);
    // cell 2: no starter; active {A4, C4}; most-recent = C4 (start 1).
    assert(proj[2].note_idx == 2);
    // cell 3: only A4 active.
    assert(proj[3].note_idx == 0);
    assert(proj[15].note_idx == 0);
    std::cout << "test_projection_stack_preempt_and_return PASSED\n";
}

static void test_collision_truncate() {
    std::vector<Note> notes;
    notes.push_back({0, 4, 60, 127});  // C4 [0..3]
    clear_or_truncate_at(notes, /*pitch_midi=*/60, /*at_step=*/2);
    // A < C -> truncate to duration = C - A = 2 (covers cells 0..1).
    assert(notes.size() == 1);
    assert(notes[0].start_step == 0);
    assert(notes[0].duration_steps == 2);
    std::cout << "test_collision_truncate PASSED\n";
}

static void test_collision_delete_on_exact_start() {
    std::vector<Note> notes;
    notes.push_back({0, 4, 60, 127});  // C4 [0..3]
    clear_or_truncate_at(notes, 60, 0);
    // A == C -> delete entirely.
    assert(notes.empty());
    std::cout << "test_collision_delete_on_exact_start PASSED\n";
}

static void test_collision_different_pitch_unaffected() {
    std::vector<Note> notes;
    notes.push_back({0, 4, 60, 127});  // C4
    notes.push_back({0, 4, 64, 127});  // E4 same range, different row
    clear_or_truncate_at(notes, /*pitch=*/64, /*at_step=*/0);
    // Only the E4 row collides; C4 is untouched.
    assert(notes.size() == 1);
    assert(notes[0].pitch_midi == 60);
    assert(notes[0].duration_steps == 4);
    std::cout << "test_collision_different_pitch_unaffected PASSED\n";
}

static void test_note_cap() {
    std::vector<Note> notes;
    for (int i = 0; i < MAX_NOTES_PER_MELODIC; ++i) {
        Note n{0, 1, static_cast<uint8_t>(60), 127};
        assert(add_note(notes, n));
    }
    Note overflow{0, 1, 60, 127};
    assert(!add_note(notes, overflow));
    assert(notes.size() == MAX_NOTES_PER_MELODIC);
    std::cout << "test_note_cap PASSED\n";
}

static void test_midi_to_hz() {
    // A4 = MIDI 69 = 440 Hz exactly.
    float a4 = midi_to_hz(69);
    assert(std::fabs(a4 - 440.f) < 1e-3f);
    // C4 = MIDI 60 ≈ 261.6256
    float c4 = midi_to_hz(60);
    assert(std::fabs(c4 - 261.6256f) < 0.01f);
    // Octave up doubles frequency.
    float a5 = midi_to_hz(81);
    assert(std::fabs(a5 - 880.f) < 1e-2f);
    std::cout << "test_midi_to_hz PASSED\n";
}

// Use a socketpair to exercise serialize/deserialize round trips without
// involving Network::host. We send through one end and read from the other
// using the production parsers.
static void test_msg_state_roundtrip() {
    int sv[2];
    if (!make_socket_pair(sv)) std::abort();

    SessionState src;
    src.session_id = 12345;
    src.bpm = 144;
    src.patterns[0]->drum_grid[DK_KICK][0] = true;
    src.patterns[0]->drum_grid[DK_KICK][8] = true;
    src.patterns[0]->drum_grid[DK_SNARE][4] = true;
    src.track_root_midi[8] = 64;  // lead -> E4
    src.patterns[0]->melodic_notes[0].push_back({0, 4, 67, 127});
    src.patterns[0]->melodic_notes[0].push_back({4, 2, 60, 100});
    src.patterns[0]->melodic_notes[3].push_back({0, 16, 36, 127});

    assert(bitjams_net_internal::send_msg_state(sv[0], src));

    // Consume the leading tag byte to match the protocol prefix.
    uint8_t tag = 0;
    int n = ::recv(sv[1], reinterpret_cast<char*>(&tag), 1, MSG_WAITALL);
    assert(n == 1);
    assert(tag == MSG_STATE);

    SessionState dst;
    assert(bitjams_net_internal::recv_and_apply_msg_state(sv[1], dst));
    assert(dst.session_id == 12345);
    assert(dst.bpm == 144);
    assert(dst.patterns[0]->drum_grid[DK_KICK][0] == true);
    assert(dst.patterns[0]->drum_grid[DK_KICK][8] == true);
    assert(dst.patterns[0]->drum_grid[DK_SNARE][4] == true);
    assert(dst.patterns[0]->drum_grid[DK_SNARE][0] == false);
    assert(dst.track_root_midi[8] == 64);
    assert(dst.patterns[0]->melodic_notes[0].size() == 2);
    assert(dst.patterns[0]->melodic_notes[0][0].start_step == 0);
    assert(dst.patterns[0]->melodic_notes[0][0].duration_steps == 4);
    assert(dst.patterns[0]->melodic_notes[0][0].pitch_midi == 67);
    assert(dst.patterns[0]->melodic_notes[0][1].velocity == 100);
    assert(dst.patterns[0]->melodic_notes[3].size() == 1);
    assert(dst.patterns[0]->melodic_notes[3][0].duration_steps == 16);
    assert(dst.patterns[0]->melodic_notes[1].empty());

    close_socket(sv[0]);
    close_socket(sv[1]);
    std::cout << "test_msg_state_roundtrip PASSED\n";
}

static void test_msg_melodic_track_roundtrip() {
    int sv[2];
    if (!make_socket_pair(sv)) std::abort();

    std::vector<Note> notes = {
        {0, 1, 60, 127},
        {2, 4, 64, 90},
        {7, 9, 67, 127},
    };
    uint8_t buf[1024];
    size_t n = bitjams_net_internal::build_msg_melodic_track(
        /*melodic_idx=*/1, notes, buf, sizeof(buf));
    assert(n > 0);
    int s = ::send(sv[0], reinterpret_cast<const char*>(buf), static_cast<int>(n), 0);
    assert(s == static_cast<int>(n));

    uint8_t tag = 0;
    ::recv(sv[1], reinterpret_cast<char*>(&tag), 1, MSG_WAITALL);
    assert(tag == MSG_MELODIC_TRACK);

    SessionState dst;
    assert(bitjams_net_internal::recv_and_apply_msg_melodic_track(
        sv[1], dst, /*is_joiner=*/false));
    // melodic_idx=1 -> track 9 (bass).
    assert(dst.patterns[0]->melodic_notes[1].size() == 3);
    assert(dst.patterns[0]->melodic_notes[1][0].pitch_midi == 60);
    assert(dst.patterns[0]->melodic_notes[1][2].pitch_midi == 67);
    assert(dst.patterns[0]->melodic_notes[1][1].velocity == 90);

    close_socket(sv[0]);
    close_socket(sv[1]);
    std::cout << "test_msg_melodic_track_roundtrip PASSED\n";
}

static void test_melodic_track_propagates_host_to_joiner() {
    auto host_state = std::make_shared<SessionState>();
    host_state->session_id = 6060;

    Network::host(host_state);
    wait_for_listener();

    auto joiner_state = Network::join("127.0.0.1", 6060);
    assert(joiner_state != nullptr);
    assert(joiner_state->patterns[0]->melodic_notes[0].empty());

    // Host writes a note on lead (melodic_idx=0) and dirties.
    { std::lock_guard<std::mutex> lk(host_state->patterns[0]->melodic_mutex);
      host_state->patterns[0]->melodic_notes[0].push_back({3, 2, 72, 127}); }
    host_state->melodic_dirty[0].store(true);

    wait_flush_windows();

    { std::lock_guard<std::mutex> lk(joiner_state->patterns[0]->melodic_mutex);
      assert(joiner_state->patterns[0]->melodic_notes[0].size() == 1);
      assert(joiner_state->patterns[0]->melodic_notes[0][0].start_step == 3);
      assert(joiner_state->patterns[0]->melodic_notes[0][0].pitch_midi == 72); }

    Network::stop();
    std::cout << "test_melodic_track_propagates_host_to_joiner PASSED\n";
}

static void test_melodic_track_propagates_joiner_to_host() {
    auto host_state = std::make_shared<SessionState>();
    host_state->session_id = 6161;

    Network::host(host_state);
    wait_for_listener();

    auto joiner_state = Network::join("127.0.0.1", 6161);
    assert(joiner_state != nullptr);

    { std::lock_guard<std::mutex> lk(joiner_state->patterns[0]->melodic_mutex);
      joiner_state->patterns[0]->melodic_notes[2].push_back({0, 8, 60, 127});
      joiner_state->patterns[0]->melodic_notes[2].push_back({8, 8, 67, 127}); }
    joiner_state->melodic_dirty[2].store(true);

    wait_flush_windows();

    { std::lock_guard<std::mutex> lk(host_state->patterns[0]->melodic_mutex);
      assert(host_state->patterns[0]->melodic_notes[2].size() == 2);
      assert(host_state->patterns[0]->melodic_notes[2][1].pitch_midi == 67); }
    // After the host echo, in_flight should clear.
    assert(joiner_state->melodic_in_flight[2].load() == false);

    Network::stop();
    std::cout << "test_melodic_track_propagates_joiner_to_host PASSED\n";
}

int main() {
    net_init();
    test_midi_to_hz();
    test_note_cap();
    test_projection_lowest_wins();
    test_projection_stack_preempt_and_return();
    test_collision_truncate();
    test_collision_delete_on_exact_start();
    test_collision_different_pitch_unaffected();
    test_msg_state_roundtrip();
    test_msg_melodic_track_roundtrip();
    test_state_sync();
    test_wrong_room_rejected();
    test_edit_propagates_host_to_joiner();
    test_edit_propagates_joiner_to_host();
    test_two_joiners_non_conflicting();
    test_two_joiners_conflicting_converge();
    test_joiner_disconnect_does_not_crash_host();
    test_melodic_track_propagates_host_to_joiner();
    test_melodic_track_propagates_joiner_to_host();
    std::cout << "All net tests passed.\n";
    return 0;
}
