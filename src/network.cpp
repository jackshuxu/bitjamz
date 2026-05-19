#include "network.h"
#include "net_compat.h"
#include "session.h"
#include "sync_protocol.h"

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
//
// Phase 5 schema. Carries the full patterns vector + song timeline. Body
// size grows with the number of patterns and notes. We size the send
// buffer dynamically (std::vector) to avoid the prior fixed-cap risk.

namespace {
// Helper: write a uint16_t in network byte order, advancing the cursor.
void put_u16(std::vector<uint8_t>& buf, uint16_t v) {
    uint16_t n = htons(v);
    buf.insert(buf.end(),
               reinterpret_cast<uint8_t*>(&n),
               reinterpret_cast<uint8_t*>(&n) + 2);
}
void put_u8(std::vector<uint8_t>& buf, uint8_t v) { buf.push_back(v); }
void put_i32(std::vector<uint8_t>& buf, int32_t v) {
    int32_t n = htonl(v);
    buf.insert(buf.end(),
               reinterpret_cast<uint8_t*>(&n),
               reinterpret_cast<uint8_t*>(&n) + 4);
}
}  // namespace

bool send_msg_state(int sock, const SessionState& s) {
    std::vector<uint8_t> buf;
    buf.reserve(2048);
    buf.push_back(MSG_STATE);
    put_u16(buf, s.session_id);
    put_i32(buf, s.bpm);
    for (int t = 0; t < TRACKS; ++t) buf.push_back(s.track_root_midi[t]);

    put_u16(buf, static_cast<uint16_t>(s.patterns.size()));
    for (const auto& pp : s.patterns) {
        const Pattern& p = *pp;
        put_u16(buf, p.id);
        put_u8(buf, p.length_bars);
        put_u8(buf, p.time_sig_num);
        // drum_grid_packed: 8 × 4 × 4 = 128 bytes (bit per step, packed LSB-first).
        for (int k = 0; k < DRUM_KINDS; ++k) {
            for (int b = 0; b < MAX_BARS_PER_PATTERN; ++b) {
                for (int byte = 0; byte < MAX_STEPS_PER_BAR / 8; ++byte) {
                    uint8_t v = 0;
                    for (int bit = 0; bit < 8; ++bit) {
                        int st = byte * 8 + bit;
                        if (p.drum_grid[k][b][st].load()) v |= (1u << bit);
                    }
                    buf.push_back(v);
                }
            }
        }
        for (int m = 0; m < MELODIC_VOICES; ++m) {
            std::lock_guard<std::mutex> lk(p.melodic_mutex);
            const auto& notes = p.melodic_notes[m];
            put_u16(buf, static_cast<uint16_t>(notes.size()));
            for (const Note& n : notes) {
                buf.push_back(n.bar);
                buf.push_back(n.start_step);
                buf.push_back(n.duration_steps);
                buf.push_back(n.pitch_midi);
                buf.push_back(n.velocity);
            }
        }
    }
    put_u16(buf, static_cast<uint16_t>(s.song.size()));
    for (uint16_t pid : s.song) put_u16(buf, pid);

    return send_all(sock, buf.data(), buf.size());
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

    uint16_t pc_n = 0;
    if (!recv_all(sock, &pc_n, 2)) return false;
    uint16_t pc = ntohs(pc_n);

    // Reset patterns; the host's snapshot is authoritative.
    s.patterns.clear();
    s.patterns.reserve(pc);
    for (uint16_t i = 0; i < pc; ++i) {
        auto np = std::make_unique<Pattern>();
        uint16_t id_n;
        if (!recv_all(sock, &id_n, 2)) return false;
        np->id = ntohs(id_n);
        uint8_t lb = 0, tsn = 0;
        if (!recv_all(sock, &lb, 1))  return false;
        if (!recv_all(sock, &tsn, 1)) return false;
        np->length_bars  = lb;
        np->time_sig_num = tsn;
        for (int k = 0; k < DRUM_KINDS; ++k) {
            for (int b = 0; b < MAX_BARS_PER_PATTERN; ++b) {
                for (int byte = 0; byte < MAX_STEPS_PER_BAR / 8; ++byte) {
                    uint8_t v = 0;
                    if (!recv_all(sock, &v, 1)) return false;
                    for (int bit = 0; bit < 8; ++bit) {
                        int st = byte * 8 + bit;
                        np->drum_grid[k][b][st].store((v >> bit) & 1u);
                    }
                }
            }
        }
        for (int m = 0; m < MELODIC_VOICES; ++m) {
            uint16_t nc_n = 0;
            if (!recv_all(sock, &nc_n, 2)) return false;
            uint16_t nc = ntohs(nc_n);
            np->melodic_notes[m].reserve(std::max<size_t>(MAX_NOTES_PER_MELODIC, nc));
            for (uint16_t j = 0; j < nc; ++j) {
                Note n{};
                uint8_t five[5];
                if (!recv_all(sock, five, 5)) return false;
                n.bar            = five[0];
                n.start_step     = five[1];
                n.duration_steps = five[2];
                n.pitch_midi     = five[3];
                n.velocity       = five[4];
                np->melodic_notes[m].push_back(n);
            }
        }
        s.patterns.push_back(std::move(np));
    }
    // If host sent zero patterns (shouldn't happen) ensure a stub exists.
    if (s.patterns.empty()) {
        auto np = std::make_unique<Pattern>();
        np->id = 1;
        s.patterns.push_back(std::move(np));
    }

    uint16_t sl_n = 0;
    if (!recv_all(sock, &sl_n, 2)) return false;
    uint16_t sl = ntohs(sl_n);
    s.song.clear();
    s.song.reserve(sl);
    for (uint16_t i = 0; i < sl; ++i) {
        uint16_t pid_n = 0;
        if (!recv_all(sock, &pid_n, 2)) return false;
        s.song.push_back(ntohs(pid_n));
    }
    if (s.song.empty()) s.song.push_back(s.patterns[0]->id);
    return true;
}

// ---- MSG_EDIT (drum-only) ----
//
// Phase 5 wire layout per cell: pattern_id (u16 NBO) + track (u8) + bar (u8)
// + step (u8) + value (u8) = 6 bytes/cell. The dirty bitmask plumbing still
// tracks one bit per (track, step) — meaning a single flush window can only
// emit edits for one (pattern, bar) tuple. For solo / single-pattern jam
// the active pattern is always the edit pattern; multi-pattern simultaneous
// edits are sequenced across flush windows.

size_t build_msg_edit(SessionState& s, uint8_t* buf, size_t buf_cap) {
    // Header: [count][cells...][bpm_present][bpm32]
    // Each cell now occupies 6 bytes (was 3). Safety check: 16 tracks * 32
    // bits * 6 bytes = 3072 max payload, plus header.
    if (buf_cap < 9) return 0;

    uint8_t* p = buf;
    uint8_t* count_ptr = p++;
    uint8_t cell_count = 0;

    bool joiner_path = s.network_alive.load() && s.is_joiner;

    // The pattern + bar context for these dirty bits is the user's current
    // edit focus when the dirty bit was set. We snapshot once at flush time.
    uint16_t pid = s.current_edit_pattern_id.load();
    uint8_t  bar = static_cast<uint8_t>(s.edit_bar.load());
    Pattern* pat = find_pattern(s, pid);
    if (!pat && !s.patterns.empty()) {
        pat = s.patterns[0].get();
        pid = pat->id;
    }
    if (!pat) {
        *count_ptr = 0;
        *p++ = 0;
        int32_t zero = 0;
        std::memcpy(p, &zero, 4); p += 4;
        return 0;
    }

    for (int t = 0; t < TRACKS; ++t) {
        if (TRACK_DEFS[t].type != TrackType::DRUM) {
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
            uint16_t pid_n = htons(pid);
            std::memcpy(p, &pid_n, 2); p += 2;
            *p++ = static_cast<uint8_t>(t);
            *p++ = bar;
            *p++ = static_cast<uint8_t>(step);
            *p++ = pat->drum_grid[dk][bar][step].load() ? 1 : 0;
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
        uint8_t hdr[6];
        if (!recv_all(sock, hdr, 6)) return false;
        uint16_t pid = static_cast<uint16_t>(hdr[0] << 8 | hdr[1]);
        uint8_t  t   = hdr[2];
        uint8_t  bar = hdr[3];
        uint8_t  step = hdr[4];
        uint8_t  v   = hdr[5];
        if (t >= TRACKS || step >= MAX_STEPS_PER_BAR
            || bar >= MAX_BARS_PER_PATTERN) continue;
        if (TRACK_DEFS[t].type != TrackType::DRUM) continue;
        Pattern* pat = find_pattern(s, pid);
        if (!pat) continue;
        int dk = TRACK_DEFS[t].drum_kind;

        uint16_t bit = static_cast<uint16_t>(1) << step;
        // Only track in_flight against the local edit-focus pattern/bar; for
        // other patterns we just apply the inbound edit (Rule Y degrades to
        // last-writer-wins for non-current-focus edits in this Phase 5).
        bool current_focus = (pid == s.current_edit_pattern_id.load()
                              && bar == s.edit_bar.load());
        if (is_joiner && current_focus) {
            if (s.dirty[t].load() & bit) continue;
            if (s.in_flight[t].load() & bit) {
                pat->drum_grid[dk][bar][step] = (v != 0);
                s.in_flight[t].fetch_and(static_cast<uint16_t>(~bit));
                continue;
            }
            pat->drum_grid[dk][bar][step] = (v != 0);
        } else if (is_joiner) {
            pat->drum_grid[dk][bar][step] = (v != 0);
        } else {
            // Host: apply + redirty so we relay to peers.
            pat->drum_grid[dk][bar][step] = (v != 0);
            if (current_focus) s.dirty[t].fetch_or(bit);
        }
    }

    uint8_t bpm_present = 0;
    int32_t bpm_n = 0;
    if (!recv_all(sock, &bpm_present, 1)) return false;
    if (!recv_all(sock, &bpm_n, sizeof(bpm_n))) return false;
    if (bpm_present) {
        int32_t bpm = ntohl(bpm_n);
        apply_optimistic_local_edit_scalar(s.bpm, s.bpm_dirty, s.bpm_in_flight,
                                           static_cast<int>(bpm), is_joiner);
    }

    return true;
}

// ---- MSG_MELODIC_TRACK ----

// Phase 5: melodic track resend now carries pattern_id (u16 NBO) + u16
// note_count + 5-byte notes (bar prepended). Caller passes the pattern_id
// to associate the resend with.
size_t build_msg_melodic_track(uint16_t pattern_id, int melodic_idx,
                               const std::vector<Note>& notes,
                               uint8_t* buf, size_t buf_cap) {
    // tag(1) + pid(2) + track_id(1) + count(2) + notes(5*N)
    size_t need = 6 + 5 * notes.size();
    if (buf_cap < need) return 0;
    uint8_t* p = buf;
    *p++ = MSG_MELODIC_TRACK;
    uint16_t pid_n = htons(pattern_id);
    std::memcpy(p, &pid_n, 2); p += 2;
    int track_id = -1;
    for (int t = 0; t < TRACKS; ++t) {
        if (TRACK_DEFS[t].type == TrackType::MELODIC &&
            TRACK_DEFS[t].melodic_idx == melodic_idx) {
            track_id = t; break;
        }
    }
    if (track_id < 0) return 0;
    *p++ = static_cast<uint8_t>(track_id);
    uint16_t cnt_n = htons(static_cast<uint16_t>(notes.size()));
    std::memcpy(p, &cnt_n, 2); p += 2;
    for (const Note& n : notes) {
        *p++ = n.bar;
        *p++ = n.start_step;
        *p++ = n.duration_steps;
        *p++ = n.pitch_midi;
        *p++ = n.velocity;
    }
    return static_cast<size_t>(p - buf);
}

bool recv_and_apply_msg_melodic_track(int sock, SessionState& s, bool is_joiner) {
    uint16_t pid_n = 0;
    if (!recv_all(sock, &pid_n, 2)) return false;
    uint16_t pid = ntohs(pid_n);
    uint8_t  track_id = 0;
    if (!recv_all(sock, &track_id, 1)) return false;
    uint16_t cnt_n = 0;
    if (!recv_all(sock, &cnt_n, 2)) return false;
    uint16_t count = ntohs(cnt_n);

    std::vector<Note> incoming;
    incoming.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        uint8_t five[5];
        if (!recv_all(sock, five, 5)) return false;
        Note n{};
        n.bar            = five[0];
        n.start_step     = five[1];
        n.duration_steps = five[2];
        n.pitch_midi     = five[3];
        n.velocity       = five[4];
        incoming.push_back(n);
    }

    if (track_id >= TRACKS) return true;
    if (TRACK_DEFS[track_id].type != TrackType::MELODIC) return true;
    int idx = TRACK_DEFS[track_id].melodic_idx;

    Pattern* pat = find_pattern(s, pid);
    if (!pat && !s.patterns.empty()) pat = s.patterns[0].get();
    if (!pat) return true;

    std::lock_guard<std::mutex> lk(pat->melodic_mutex);
    // Phase 5 simplification: melodic_dirty/in_flight remains a single
    // global bit per melodic-idx (not per-pattern). The optimistic-edit
    // guard therefore only protects edits to the currently-focused pattern.
    bool current_focus = (pid == s.current_edit_pattern_id.load());
    if (is_joiner && current_focus) {
        if (s.melodic_dirty[idx].load()) return true;
        if (s.melodic_in_flight[idx].load()) {
            pat->melodic_notes[idx] = std::move(incoming);
            pat->melodic_notes[idx].reserve(MAX_NOTES_PER_MELODIC);
            s.melodic_in_flight[idx].store(false);
            return true;
        }
        pat->melodic_notes[idx] = std::move(incoming);
        pat->melodic_notes[idx].reserve(MAX_NOTES_PER_MELODIC);
    } else if (is_joiner) {
        pat->melodic_notes[idx] = std::move(incoming);
        pat->melodic_notes[idx].reserve(MAX_NOTES_PER_MELODIC);
    } else {
        pat->melodic_notes[idx] = std::move(incoming);
        pat->melodic_notes[idx].reserve(MAX_NOTES_PER_MELODIC);
        if (current_focus) s.melodic_dirty[idx].store(true);
    }
    return true;
}

// ---- MSG_PATTERN_NEW ----

bool recv_and_apply_msg_pattern_new(int sock, SessionState& s, bool is_joiner) {
    (void)is_joiner;
    uint16_t pid_n = 0;
    if (!recv_all(sock, &pid_n, 2)) return false;
    uint16_t pid = ntohs(pid_n);
    uint8_t lb = 0, tsn = 0;
    if (!recv_all(sock, &lb, 1))  return false;
    if (!recv_all(sock, &tsn, 1)) return false;

    std::lock_guard<std::mutex> lk(s.patterns_mutex);
    if (find_pattern(s, pid)) return true;  // dedup
    auto np = std::make_unique<Pattern>();
    np->id = pid;
    np->length_bars = lb;
    np->time_sig_num = tsn;
    s.patterns.push_back(std::move(np));
    return true;
}

// ---- MSG_PATTERN_META ----

bool recv_and_apply_msg_pattern_meta(int sock, SessionState& s, bool is_joiner) {
    (void)is_joiner;
    uint16_t pid_n = 0;
    if (!recv_all(sock, &pid_n, 2)) return false;
    uint16_t pid = ntohs(pid_n);
    uint8_t lb = 0, tsn = 0;
    if (!recv_all(sock, &lb, 1))  return false;
    if (!recv_all(sock, &tsn, 1)) return false;
    Pattern* pat = find_pattern(s, pid);
    if (!pat) return true;
    // Route length through the funnel so all four invariants fire (clamp,
    // pattern_meta_dirty, song-run resize, cursor clamp on shrink). The
    // pattern_meta_dirty bit it sets is harmless here — the flush builder
    // re-emits a packet identical to the one we just received, which the
    // peer will discard as a no-op via the same in_flight gate used for
    // edits. time_sig_num stays a direct write; it has its own (separate)
    // invariant set and isn't part of this PRD.
    set_pattern_length(s, *pat, static_cast<int>(lb));
    pat->time_sig_num = tsn;
    return true;
}

// ---- MSG_SONG_EDIT ----

bool recv_and_apply_msg_song_edit(int sock, SessionState& s, bool is_joiner) {
    (void)is_joiner;
    uint16_t bar_n = 0, pid_n = 0;
    if (!recv_all(sock, &bar_n, 2)) return false;
    if (!recv_all(sock, &pid_n, 2)) return false;
    uint16_t bar = ntohs(bar_n);
    uint16_t pid = ntohs(pid_n);
    if (bar >= MAX_SONG_BARS) return true;
    std::lock_guard<std::mutex> lk(s.patterns_mutex);
    while (s.song.size() <= bar) {
        if (s.song.size() >= MAX_SONG_BARS) return true;
        s.song.push_back(0);
    }
    s.song[bar] = pid;
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
    apply_optimistic_local_edit_bitmask(s.track_root_midi[track_id],
                                        s.track_root_dirty,
                                        s.track_root_in_flight,
                                        bit, midi, is_joiner);
    return true;
}

// Claim the lowest-set bit from any of `mask_lanes` 64-bit atomic words,
// clearing it. Returns the global bit index (lane*64 + position), or -1 if
// no bit was set.
static int claim_lowest_bit(std::atomic<uint64_t>* mask, int lanes) {
    for (int l = 0; l < lanes; ++l) {
        uint64_t v = mask[l].load();
        while (v) {
            // Find lowest set bit.
#ifdef _MSC_VER
            unsigned long pos = 0;
            _BitScanForward64(&pos, v);
            int b = static_cast<int>(pos);
#else
            int b = __builtin_ctzll(v);
#endif
            uint64_t bit = static_cast<uint64_t>(1) << b;
            uint64_t prev = v;
            if (mask[l].compare_exchange_weak(prev, v & ~bit)) {
                return l * 64 + b;
            }
            v = mask[l].load();
        }
    }
    return -1;
}

// Returns the number of bytes written to buf (tag + payload), or 0 if
// nothing dirty. Sends at most one of: MSG_MELODIC_TRACK, MSG_TRACK_ROOT_MIDI,
// MSG_PATTERN_NEW, MSG_PATTERN_META, MSG_SONG_EDIT per call; caller invokes
// repeatedly to drain.
size_t build_pending_aux(SessionState& s, uint8_t* buf, size_t buf_cap) {
    bool joiner_path = s.network_alive.load() && s.is_joiner;

    for (int m = 0; m < MELODIC_VOICES; ++m) {
        bool expected = true;
        if (s.melodic_dirty[m].compare_exchange_strong(expected, false)) {
            if (joiner_path) s.melodic_in_flight[m].store(true);
            uint16_t pid = s.current_edit_pattern_id.load();
            Pattern* pat = find_pattern(s, pid);
            if (!pat && !s.patterns.empty()) {
                pat = s.patterns[0].get(); pid = pat->id;
            }
            if (!pat) return 0;
            std::lock_guard<std::mutex> lk(pat->melodic_mutex);
            return build_msg_melodic_track(pid, m, pat->melodic_notes[m],
                                           buf, buf_cap);
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

    // MSG_PATTERN_NEW: tag(1) + pid(2) + length_bars(1) + time_sig_num(1) = 5
    int pn = claim_lowest_bit(s.pattern_new_dirty, 4);
    if (pn >= 0) {
        Pattern* pat = find_pattern(s, static_cast<uint16_t>(pn));
        if (pat && buf_cap >= 5) {
            buf[0] = MSG_PATTERN_NEW;
            uint16_t pid_n = htons(static_cast<uint16_t>(pn));
            std::memcpy(buf + 1, &pid_n, 2);
            buf[3] = pat->length_bars;
            buf[4] = pat->time_sig_num;
            return 5;
        }
        // Pattern vanished (shouldn't happen); drop the bit and continue.
    }

    // MSG_PATTERN_META: same shape as MSG_PATTERN_NEW.
    int pm = claim_lowest_bit(s.pattern_meta_dirty, 4);
    if (pm >= 0) {
        Pattern* pat = find_pattern(s, static_cast<uint16_t>(pm));
        if (pat && buf_cap >= 5) {
            buf[0] = MSG_PATTERN_META;
            uint16_t pid_n = htons(static_cast<uint16_t>(pm));
            std::memcpy(buf + 1, &pid_n, 2);
            buf[3] = pat->length_bars;
            buf[4] = pat->time_sig_num;
            return 5;
        }
    }

    // MSG_SONG_EDIT: tag(1) + bar(2) + pid(2) = 5
    int sb = claim_lowest_bit(s.song_dirty, 2);
    if (sb >= 0) {
        uint16_t pid = 0;
        if (sb < static_cast<int>(s.song.size())) pid = s.song[sb];
        if (buf_cap >= 5) {
            buf[0] = MSG_SONG_EDIT;
            uint16_t bar_n = htons(static_cast<uint16_t>(sb));
            uint16_t pid_n = htons(pid);
            std::memcpy(buf + 1, &bar_n, 2);
            std::memcpy(buf + 3, &pid_n, 2);
            return 5;
        }
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
        case MSG_PATTERN_NEW:
            return bitjams_net_internal::recv_and_apply_msg_pattern_new(sock, state, is_joiner);
        case MSG_PATTERN_META:
            return bitjams_net_internal::recv_and_apply_msg_pattern_meta(sock, state, is_joiner);
        case MSG_SONG_EDIT:
            return bitjams_net_internal::recv_and_apply_msg_song_edit(sock, state, is_joiner);
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
