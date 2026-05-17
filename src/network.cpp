#include "network.h"
#include "net_compat.h"
#include "session.h"

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
std::atomic<int>           g_listener_fd{-1};
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
        int n = ::send(sock, reinterpret_cast<const char*>(p), static_cast<int>(len), 0);
        if (n <= 0) return false;
        p   += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool recv_all(int sock, void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);
    while (len > 0) {
        int n = ::recv(sock, reinterpret_cast<char*>(p), static_cast<int>(len), 0);
        if (n <= 0) return false;
        p   += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

} // anonymous namespace

namespace bitjams_net_internal {

// ---- MSG_STATE (variable length) ----

bool send_msg_state(int sock, const SessionState& s) {
    // Header is bounded; melodic body is up to 4 × (1 + 64*4) = 1028.
    // 46 + 1028 = 1074 max body. Plus 1 tag byte.
    uint8_t buf[1100];
    uint8_t* p = buf;

    *p++ = MSG_STATE;

    uint16_t sid_n = htons(s.session_id);
    std::memcpy(p, &sid_n, 2); p += 2;

    int32_t bpm_n = htonl(s.bpm);
    std::memcpy(p, &bpm_n, 4); p += 4;

    for (int t = 0; t < TRACKS; ++t) *p++ = s.track_root_midi[t];

    for (int k = 0; k < DRUM_KINDS; ++k) {
        uint16_t mask = 0;
        for (int st = 0; st < STEPS; ++st) {
            if (s.patterns[0]->drum_grid[k][0][st]) mask |= static_cast<uint16_t>(1) << st;
        }
        uint16_t mn = htons(mask);
        std::memcpy(p, &mn, 2); p += 2;
    }

    for (int m = 0; m < MELODIC_VOICES; ++m) {
        const auto& notes = s.patterns[0]->melodic_notes[m];
        uint8_t count = static_cast<uint8_t>(notes.size());
        *p++ = count;
        for (uint8_t i = 0; i < count; ++i) {
            *p++ = notes[i].start_step;
            *p++ = notes[i].duration_steps;
            *p++ = notes[i].pitch_midi;
            *p++ = notes[i].velocity;
        }
    }

    return send_all(sock, buf, static_cast<size_t>(p - buf));
}

bool recv_and_apply_msg_state(int sock, SessionState& s) {
    // Caller has already consumed the MSG_STATE tag byte.
    uint16_t sid_n = 0;
    if (!recv_all(sock, &sid_n, 2)) return false;
    s.session_id = ntohs(sid_n);

    int32_t bpm_n = 0;
    if (!recv_all(sock, &bpm_n, 4)) return false;
    s.bpm = ntohl(bpm_n);

    uint8_t roots[TRACKS];
    if (!recv_all(sock, roots, TRACKS)) return false;
    for (int t = 0; t < TRACKS; ++t) s.track_root_midi[t] = roots[t];

    for (int k = 0; k < DRUM_KINDS; ++k) {
        uint16_t mn = 0;
        if (!recv_all(sock, &mn, 2)) return false;
        uint16_t mask = ntohs(mn);
        for (int st = 0; st < STEPS; ++st) {
            s.patterns[0]->drum_grid[k][0][st] = (mask & (static_cast<uint16_t>(1) << st)) != 0;
        }
    }

    for (int m = 0; m < MELODIC_VOICES; ++m) {
        uint8_t count = 0;
        if (!recv_all(sock, &count, 1)) return false;
        s.patterns[0]->melodic_notes[m].clear();
        s.patterns[0]->melodic_notes[m].reserve(count);
        for (uint8_t i = 0; i < count; ++i) {
            Note n{};
            uint8_t four[4];
            if (!recv_all(sock, four, 4)) return false;
            n.start_step     = four[0];
            n.duration_steps = four[1];
            n.pitch_midi     = four[2];
            n.velocity       = four[3];
            s.patterns[0]->melodic_notes[m].push_back(n);
        }
    }
    return true;
}

// ---- MSG_EDIT (drum-only) ----

size_t build_msg_edit(SessionState& s, uint8_t* buf, size_t buf_cap) {
    // Layout written: [count][cells...][bpm_present][bpm32][ta_present][ta_mask16]
    if (buf_cap < 9) return 0;

    uint8_t* p = buf;
    uint8_t* count_ptr = p++;
    uint8_t cell_count = 0;

    bool joiner_path = s.network_alive.load() && s.is_joiner;

    for (int t = 0; t < TRACKS; ++t) {
        if (TRACK_DEFS[t].type != TrackType::DRUM) {
            // Defensive: melodic tracks should never set bits here, but if
            // they do, drop them so we don't ship garbage.
            s.dirty[t].exchange(0);
            continue;
        }
        uint16_t claimed = s.dirty[t].exchange(0);
        if (joiner_path && claimed) {
            s.in_flight[t].fetch_or(claimed);
        }
        uint16_t bits = claimed;
        int dk = TRACK_DEFS[t].drum_kind;
        while (bits) {
            int step = bitjams_ctz(bits);
            *p++ = static_cast<uint8_t>(t);
            *p++ = static_cast<uint8_t>(step);
            *p++ = s.patterns[0]->drum_grid[dk][0][step] ? 1 : 0;
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

    bool any_change = (cell_count > 0) || bpm_changed;
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
        if (TRACK_DEFS[t].type != TrackType::DRUM) continue;  // drum-only path
        int dk = TRACK_DEFS[t].drum_kind;

        uint16_t bit = static_cast<uint16_t>(1) << step;
        if (is_joiner) {
            if (s.dirty[t].load() & bit) continue;
            if (s.in_flight[t].load() & bit) {
                s.patterns[0]->drum_grid[dk][0][step] = (v != 0);
                s.in_flight[t].fetch_and(static_cast<uint16_t>(~bit));
                continue;
            }
            s.patterns[0]->drum_grid[dk][0][step] = (v != 0);
        } else {
            s.patterns[0]->drum_grid[dk][0][step] = (v != 0);
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
                if (s.bpm_in_flight.load()) s.bpm_in_flight.store(false);
            }
        } else {
            s.bpm = bpm;
            s.bpm_dirty.store(true);
        }
    }

    return true;
}

// ---- MSG_MELODIC_TRACK ----

size_t build_msg_melodic_track(int melodic_idx, const std::vector<Note>& notes,
                               uint8_t* buf, size_t buf_cap) {
    // tag(1) + track_id(1) + note_count(1) + notes(4*N)
    size_t need = 3 + 4 * notes.size();
    if (buf_cap < need) return 0;
    uint8_t* p = buf;
    *p++ = MSG_MELODIC_TRACK;
    // Wire uses the track index 8..11 (matches TRACKS layout), not melodic_idx.
    int track_id = -1;
    for (int t = 0; t < TRACKS; ++t) {
        if (TRACK_DEFS[t].type == TrackType::MELODIC &&
            TRACK_DEFS[t].melodic_idx == melodic_idx) {
            track_id = t; break;
        }
    }
    if (track_id < 0) return 0;
    *p++ = static_cast<uint8_t>(track_id);
    *p++ = static_cast<uint8_t>(notes.size());
    for (const Note& n : notes) {
        *p++ = n.start_step;
        *p++ = n.duration_steps;
        *p++ = n.pitch_midi;
        *p++ = n.velocity;
    }
    return static_cast<size_t>(p - buf);
}

bool recv_and_apply_msg_melodic_track(int sock, SessionState& s, bool is_joiner) {
    uint8_t track_id = 0;
    uint8_t count = 0;
    if (!recv_all(sock, &track_id, 1)) return false;
    if (!recv_all(sock, &count, 1))    return false;

    std::vector<Note> incoming;
    incoming.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t four[4];
        if (!recv_all(sock, four, 4)) return false;
        // Wire format is still 4-byte (start, dur, pitch, vel); bar is
        // implicit at 0 until Phase 5 widens the schema.
        Note n{};
        n.bar            = 0;
        n.start_step     = four[0];
        n.duration_steps = four[1];
        n.pitch_midi     = four[2];
        n.velocity       = four[3];
        incoming.push_back(n);
    }

    if (track_id >= TRACKS) return true;
    if (TRACK_DEFS[track_id].type != TrackType::MELODIC) return true;
    int idx = TRACK_DEFS[track_id].melodic_idx;

    // Network-applied move overwrites the vector wholesale; restore the
    // reserve() guarantee so the timing thread's concurrent iteration of
    // melodic_notes can't race a future reallocating push_back.
    std::lock_guard<std::mutex> lk(s.patterns[0]->melodic_mutex);
    if (is_joiner) {
        // Rule Y: drop inbound if we have an unsent local edit (dirty);
        // accept if it's our own echo (in_flight); otherwise apply.
        if (s.melodic_dirty[idx].load()) return true;
        if (s.melodic_in_flight[idx].load()) {
            s.patterns[0]->melodic_notes[idx] = std::move(incoming);
            s.patterns[0]->melodic_notes[idx].reserve(MAX_NOTES_PER_MELODIC);
            s.melodic_in_flight[idx].store(false);
            return true;
        }
        s.patterns[0]->melodic_notes[idx] = std::move(incoming);
        s.patterns[0]->melodic_notes[idx].reserve(MAX_NOTES_PER_MELODIC);
    } else {
        s.patterns[0]->melodic_notes[idx] = std::move(incoming);
        s.patterns[0]->melodic_notes[idx].reserve(MAX_NOTES_PER_MELODIC);
        s.melodic_dirty[idx].store(true);  // relay to other peers
    }
    return true;
}

// ---- MSG_TRACK_ROOT_MIDI ----

bool recv_and_apply_msg_track_root_midi(int sock, SessionState& s, bool is_joiner) {
    uint8_t pair[2];
    if (!recv_all(sock, pair, 2)) return false;
    uint8_t track_id = pair[0];
    uint8_t midi     = pair[1];
    if (track_id >= TRACKS) return true;

    uint16_t bit = static_cast<uint16_t>(1) << track_id;
    if (is_joiner) {
        if (s.track_root_dirty.load() & bit) return true;
        if (s.track_root_in_flight.load() & bit) {
            s.track_root_midi[track_id] = midi;
            s.track_root_in_flight.fetch_and(static_cast<uint16_t>(~bit));
            return true;
        }
        s.track_root_midi[track_id] = midi;
    } else {
        s.track_root_midi[track_id] = midi;
        s.track_root_dirty.fetch_or(bit);
    }
    return true;
}

// Returns the number of bytes written to buf (tag + payload), or 0 if
// nothing dirty. Sends at most one MSG_MELODIC_TRACK or MSG_TRACK_ROOT_MIDI
// per call; caller invokes repeatedly to drain.
size_t build_pending_aux(SessionState& s, uint8_t* buf, size_t buf_cap) {
    bool joiner_path = s.network_alive.load() && s.is_joiner;

    for (int m = 0; m < MELODIC_VOICES; ++m) {
        bool expected = true;
        if (s.melodic_dirty[m].compare_exchange_strong(expected, false)) {
            if (joiner_path) s.melodic_in_flight[m].store(true);
            std::lock_guard<std::mutex> lk(s.patterns[0]->melodic_mutex);
            return build_msg_melodic_track(m, s.patterns[0]->melodic_notes[m], buf, buf_cap);
        }
    }

    uint16_t roots = s.track_root_dirty.exchange(0);
    if (roots) {
        if (joiner_path) s.track_root_in_flight.fetch_or(roots);
        // Send one per call; re-dirty the rest.
        int t = bitjams_ctz(roots);
        uint16_t rest = roots & ~(static_cast<uint16_t>(1) << t);
        if (rest) s.track_root_dirty.fetch_or(rest);
        if (buf_cap < 3) return 0;
        buf[0] = MSG_TRACK_ROOT_MIDI;
        buf[1] = static_cast<uint8_t>(t);
        buf[2] = s.track_root_midi[t];
        return 3;
    }
    return 0;
}

} // namespace bitjams_net_internal

namespace {

using bitjams_net_internal::send_msg_state;
using bitjams_net_internal::recv_and_apply_msg_state;
using bitjams_net_internal::build_msg_edit;
using bitjams_net_internal::recv_and_apply_msg_edit;
using bitjams_net_internal::recv_and_apply_msg_melodic_track;
using bitjams_net_internal::recv_and_apply_msg_track_root_midi;
using bitjams_net_internal::build_pending_aux;

bool dispatch_inbound(int sock, SessionState& state, uint8_t tag, bool is_joiner) {
    switch (tag) {
        case MSG_EDIT:
            return recv_and_apply_msg_edit(sock, state, is_joiner);
        case MSG_MELODIC_TRACK:
            return recv_and_apply_msg_melodic_track(sock, state, is_joiner);
        case MSG_TRACK_ROOT_MIDI:
            return recv_and_apply_msg_track_root_midi(sock, state, is_joiner);
        default:
            return false;
    }
}

void flush_one_round(int sock, SessionState& state, std::atomic<bool>& alive) {
    // Drum cells + bpm + ta in one MSG_EDIT, then drain melodic + roots one
    // message at a time. Each gets a fresh tag byte; build_msg_edit writes
    // tag separately because we share the buffer scheme.
    uint8_t buf[2048];
    size_t payload = build_msg_edit(state, buf + 1, sizeof(buf) - 1);
    if (payload > 0) {
        buf[0] = MSG_EDIT;
        if (!send_all(sock, buf, payload + 1)) { alive.store(false); return; }
    }

    for (;;) {
        size_t n = build_pending_aux(state, buf, sizeof(buf));
        if (n == 0) break;
        if (!send_all(sock, buf, n)) { alive.store(false); return; }
    }
}

void per_peer_handler(int sock, std::shared_ptr<SessionState> state, std::shared_ptr<Peer> peer) {
    uint8_t  type = 0;
    uint16_t room_n = 0;
    if (!recv_all(sock, &type, 1))                 { peer->alive.store(false); close_socket(sock); return; }
    if (type != MSG_HANDSHAKE)                     { peer->alive.store(false); close_socket(sock); return; }
    if (!recv_all(sock, &room_n, sizeof(room_n)))  { peer->alive.store(false); close_socket(sock); return; }
    uint16_t room = ntohs(room_n);

    if (room != state->session_id) {
        uint8_t fail = MSG_HANDSHAKE_FAIL;
        send_all(sock, &fail, 1);
        peer->alive.store(false);
        close_socket(sock);
        return;
    }

    if (!send_msg_state(sock, *state)) {
        peer->alive.store(false);
        close_socket(sock);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_peers_mutex);
        peer->sock = sock;
        g_peers.push_back(peer);
    }

    while (!g_stop.load() && peer->alive.load()) {
        uint8_t tag = 0;
        if (!recv_all(sock, &tag, 1)) break;
        if (!dispatch_inbound(sock, *state, tag, /*is_joiner=*/false)) break;
    }

    peer->alive.store(false);
    ::shutdown(sock, SHUT_RDWR);
    close_socket(sock);
}

void listener_loop(std::shared_ptr<SessionState> state) {
    while (!g_stop.load()) {
        sockaddr_in peer_addr{};
        socklen_t   peer_len = sizeof(peer_addr);
        int sock = accept_socket(g_listener_fd.load(), reinterpret_cast<sockaddr*>(&peer_addr), &peer_len);
        if (sock < 0) {
            if (g_stop.load()) break;
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            break;
        }
        auto peer = std::make_shared<Peer>();
        std::lock_guard<std::mutex> lock(g_peers_mutex);
        g_peer_threads.emplace_back(per_peer_handler, sock, state, peer);
    }
}

void host_flush_loop(std::shared_ptr<SessionState> state) {
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(NET_FLUSH_MS));
        if (g_stop.load()) break;

        // Build once, send to every peer. Splitting per-peer would let one
        // dead socket block the others; per-round shared buf is fine.
        uint8_t edit_buf[2048];
        size_t edit_payload = build_msg_edit(*state, edit_buf + 1, sizeof(edit_buf) - 1);
        std::vector<std::vector<uint8_t>> aux_packets;
        for (;;) {
            uint8_t scratch[2048];
            size_t n = build_pending_aux(*state, scratch, sizeof(scratch));
            if (n == 0) break;
            aux_packets.emplace_back(scratch, scratch + n);
        }

        std::vector<std::shared_ptr<Peer>> snapshot;
        {
            std::lock_guard<std::mutex> lk(g_peers_mutex);
            snapshot = g_peers;
        }
        for (auto& p : snapshot) {
            if (!p->alive.load()) continue;
            if (edit_payload > 0) {
                edit_buf[0] = MSG_EDIT;
                if (!send_all(p->sock, edit_buf, edit_payload + 1)) {
                    p->alive.store(false); continue;
                }
            }
            for (auto& pkt : aux_packets) {
                if (!send_all(p->sock, pkt.data(), pkt.size())) {
                    p->alive.store(false); break;
                }
            }
        }

        std::lock_guard<std::mutex> lk(g_peers_mutex);
        std::erase_if(g_peers, [](auto& p){ return !p->alive.load(); });
    }
}

void joiner_recv_loop(int sock, std::shared_ptr<SessionState> state) {
    while (state->network_alive.load()) {
        uint8_t tag = 0;
        if (!recv_all(sock, &tag, 1)) break;
        if (!dispatch_inbound(sock, *state, tag, /*is_joiner=*/true)) break;
    }
    state->network_alive.store(false);
}

void joiner_flush_loop(int sock, std::shared_ptr<SessionState> state) {
    while (state->running.load() && state->network_alive.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(NET_FLUSH_MS));
        if (!state->network_alive.load()) break;
        flush_one_round(sock, *state, state->network_alive);
    }
}

} // anonymous namespace

void Network::host(std::shared_ptr<SessionState> state) {
    g_stop.store(false);

    g_listener_fd.store(create_socket());
    int opt = 1;
    ::setsockopt(g_listener_fd.load(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(NET_PORT);
    if (::bind(g_listener_fd.load(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::perror("bind");
        close_socket(g_listener_fd.load());
        g_listener_fd.store(-1);
        return;
    }
    if (::listen(g_listener_fd.load(), 5) < 0) {
        ::perror("listen");
        close_socket(g_listener_fd.load());
        g_listener_fd.store(-1);
        return;
    }

    g_listener_thread   = std::thread(listener_loop, state);
    g_host_flush_thread = std::thread(host_flush_loop, state);
}

std::shared_ptr<SessionState> Network::join(const char* ip, uint16_t room) {
    int sock = create_socket();
    if (sock < 0) return nullptr;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(NET_PORT);
    if (::inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close_socket(sock);
        return nullptr;
    }
    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close_socket(sock);
        return nullptr;
    }

    uint8_t  hs = MSG_HANDSHAKE;
    uint16_t rn = htons(room);
    if (!send_all(sock, &hs, 1) ||
        !send_all(sock, &rn, sizeof(rn))) {
        close_socket(sock);
        return nullptr;
    }

    uint8_t resp = 0;
    if (!recv_all(sock, &resp, 1))  { close_socket(sock); return nullptr; }
    if (resp == MSG_HANDSHAKE_FAIL) { close_socket(sock); return nullptr; }
    if (resp != MSG_STATE)          { close_socket(sock); return nullptr; }

    auto s = std::make_shared<SessionState>();
    if (!recv_and_apply_msg_state(sock, *s)) { close_socket(sock); return nullptr; }
    s->is_joiner = true;
    s->network_alive.store(true);

    g_joiner_sock         = sock;
    g_joiner_recv_thread  = std::thread(joiner_recv_loop,  sock, s);
    g_joiner_flush_thread = std::thread(joiner_flush_loop, sock, s);
    return s;
}

void Network::stop() {
    g_stop.store(true);

    if (g_listener_fd.load() >= 0) {
        ::shutdown(g_listener_fd.load(), SHUT_RDWR);
        close_socket(g_listener_fd.load());
        g_listener_fd.store(-1);
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

    if (g_joiner_sock >= 0) {
        ::shutdown(g_joiner_sock, SHUT_RDWR);
    }
    if (g_joiner_recv_thread.joinable())  g_joiner_recv_thread.join();
    if (g_joiner_flush_thread.joinable()) g_joiner_flush_thread.join();
    if (g_joiner_sock >= 0) {
        close_socket(g_joiner_sock);
        g_joiner_sock = -1;
    }

    g_stop.store(false);
}
