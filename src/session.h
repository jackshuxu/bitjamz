#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

// Anything used by more than one module (audio, ui, net, main) lives here.
// Per-module helpers live in that module's own .cpp as file-locals.

inline constexpr int TRACKS         = 12;
inline constexpr int STEPS          = 16;
inline constexpr int MELODIC_VOICES = 4;
inline constexpr int DRUM_KINDS     = 8;

enum class TrackType { DRUM, MELODIC };

// Recording state. OFF = passive playback. COUNTDOWN = 4-beat count-in
// before recording begins; play_step is held at 0. RECORDING = live capture
// is active; input keypaths additionally write into the underlying track
// data at the current play_step.
enum class RecordState { OFF, COUNTDOWN, RECORDING };

// 64-note cap per melodic track. Wire-format note_count is uint8_t but a
// 64-cap keeps packets small and matches the UI's "1-bar" mental model.
inline constexpr int MAX_NOTES_PER_MELODIC = 64;

// One note in a melodic track. start_step + duration_steps may exceed
// loop_len; the audio side only fires the onset.
struct Note {
    uint8_t start_step;
    uint8_t duration_steps;
    uint8_t pitch_midi;
    uint8_t velocity;
};

inline float midi_to_hz(uint8_t m) {
    return 440.0f * std::exp2((static_cast<int>(m) - 69) / 12.0f);
}

// Append n to notes unless the 64-note cap is reached. Returns true on
// success, false on no-op overflow.
inline bool add_note(std::vector<Note>& notes, const Note& n) {
    if (notes.size() >= MAX_NOTES_PER_MELODIC) return false;
    notes.push_back(n);
    return true;
}

// Same-row collision rule applied before placing a new note at (at_step) on
// pitch (pitch_midi). For each existing note on the same pitch row whose
// span [A, A+D) contains at_step:
//   A == at_step -> deleted
//   A <  at_step -> duration shrunk to (at_step - A)
inline void clear_or_truncate_at(std::vector<Note>& notes,
                                 uint8_t pitch_midi, uint8_t at_step) {
    for (auto it = notes.begin(); it != notes.end();) {
        if (it->pitch_midi != pitch_midi) { ++it; continue; }
        int a = it->start_step;
        int b = a + it->duration_steps - 1;
        if (a <= static_cast<int>(at_step) && static_cast<int>(at_step) <= b) {
            if (a == static_cast<int>(at_step)) {
                it = notes.erase(it);
                continue;
            }
            it->duration_steps = static_cast<uint8_t>(at_step - a);
        }
        ++it;
    }
}

// One projected cell from a melodic track for the single-row sequencer view.
struct ProjectedCell {
    int  note_idx;   // -1 if cell is empty
    bool is_start;
    bool is_end;
};

// Walk a melodic track's notes and produce per-cell display info under the
// "starters win (lowest-pitch tiebreak); continuation cells pick the
// most-recently-started still-sustaining (lowest-pitch tiebreak)" stack
// rule. Cells past loop_len are returned as { -1, false, false }.
inline std::array<ProjectedCell, STEPS> project_row(
    const std::vector<Note>& notes, int loop_len) {
    std::array<ProjectedCell, STEPS> out{};
    for (auto& c : out) c = { -1, false, false };

    for (int c = 0; c < loop_len; ++c) {
        int best = -1;
        bool best_is_starter = false;
        // First pass: starters on this cell.
        for (size_t i = 0; i < notes.size(); ++i) {
            const Note& n = notes[i];
            if (n.start_step != c) continue;
            int end = n.start_step + n.duration_steps;
            if (c >= end) continue;
            if (best < 0) { best = static_cast<int>(i); best_is_starter = true; continue; }
            if (n.pitch_midi < notes[best].pitch_midi) best = static_cast<int>(i);
        }
        // No starter: pick the most-recently-started still-sustaining note
        // (lowest-pitch on tie).
        if (best < 0) {
            for (size_t i = 0; i < notes.size(); ++i) {
                const Note& n = notes[i];
                int end = n.start_step + n.duration_steps;
                if (c < n.start_step || c >= end) continue;
                if (best < 0) { best = static_cast<int>(i); continue; }
                const Note& cur = notes[best];
                if (n.start_step > cur.start_step) best = static_cast<int>(i);
                else if (n.start_step == cur.start_step && n.pitch_midi < cur.pitch_midi)
                    best = static_cast<int>(i);
            }
        }
        out[c].note_idx = best;
        if (best >= 0) {
            const Note& n = notes[best];
            int end_step = n.start_step + n.duration_steps - 1;
            if (end_step >= loop_len) end_step = loop_len - 1;
            out[c].is_start = (c == n.start_step) && best_is_starter;
            out[c].is_end   = (c == end_step);
        }
    }
    return out;
}

// drum_kind values when type == DRUM (index into audio drum voice + DRUM_PARAMS)
enum DrumKind {
    DK_KICK = 0,
    DK_SNARE,
    DK_CLAP,
    DK_CLOSED_HAT,
    DK_OPEN_HAT,
    DK_COWBELL,
    DK_TOM,
    DK_CYMBAL,
};

struct TrackDef {
    const char* name;        // <= 5 chars
    char        key;         // MPC trigger key
    TrackType   type;
    int         drum_kind;   // 0..7 if DRUM, else -1
    int         melodic_idx; // 0..3 if MELODIC, else -1
};

// Static configuration. Not part of SessionState; never serialized.
// MPC pad layout: core kit on top, extended drums, melodic on bottom.
inline constexpr TrackDef TRACK_DEFS[TRACKS] = {
    // core drums: v b n m
    { "kick",  'v', TrackType::DRUM, DK_KICK,       -1 },
    { "snare", 'b', TrackType::DRUM, DK_SNARE,      -1 },
    { "clap",  'n', TrackType::DRUM, DK_CLAP,       -1 },
    { "chat",  'm', TrackType::DRUM, DK_CLOSED_HAT, -1 },
    // extended drums: f g h j
    { "ohat",  'f', TrackType::DRUM, DK_OPEN_HAT,   -1 },
    { "cowb",  'g', TrackType::DRUM, DK_COWBELL,    -1 },
    { "tom",   'h', TrackType::DRUM, DK_TOM,        -1 },
    { "cymb",  'j', TrackType::DRUM, DK_CYMBAL,     -1 },
    // melodic: r t y u
    { "lead",  'r', TrackType::MELODIC, -1, 0 },
    { "bass",  't', TrackType::MELODIC, -1, 1 },
    { "chord", 'y', TrackType::MELODIC, -1, 2 },
    { "drone", 'u', TrackType::MELODIC, -1, 3 },
};

// One bitjams session.
//
// A "session" is the collaborative unit: what a host hosts, what a joiner
// joins, what gets wiped on session switch. Shared by every thread —
// UI, timing thread, audio callback, and (future) network flush thread.
//
// Three logical groups of fields:
//   1. Shared creative state — synced across the network:
//        drum_grid, melodic_notes, bpm, track_root_midi
//   2. Per-peer runtime — never synced, each peer runs its own:
//        play_step, playing, running, trig, loop_len, track_muted, track_solo
//   3. Networking bookkeeping — only meaningful in multi-peer mode:
//        session_id, dirty, melodic_dirty
class SessionState {
public:
    SessionState();                          // default solo state
    ~SessionState() = default;

    // --- networking bookkeeping ---
    uint16_t session_id = 0;

    // --- shared creative state (synced) ---
    // Drum tracks indexed by TRACK_DEFS[t].drum_kind (0..7). Melodic tracks
    // indexed by TRACK_DEFS[t].melodic_idx (0..3).
    bool              drum_grid[DRUM_KINDS][STEPS] = {};
    std::vector<Note> melodic_notes[MELODIC_VOICES];
    // Per-track default root pitch in MIDI. Drum tracks use it as the
    // synthesized voice's base pitch; melodic tracks use it for grid-mode
    // space-entry and live-pad trigger.
    uint8_t track_root_midi[TRACKS] = {
        36, 38, 40, 42, 44, 46, 48, 50,   // drums
        60, 48, 60, 36,                   // lead, bass, chord, drone
    };
    int   bpm = 120;

    // --- per-peer mix (not synced) ---
    // track_muted: if true, this peer skips sequencer triggers for the track.
    // track_solo : if any track in the session is soloed, all non-solo tracks
    // are silenced for this peer. Mute wins over solo (a muted+soloed track
    // is silent).
    bool  track_muted[TRACKS] = {};
    bool  track_solo[TRACKS]  = {};

    // --- per-peer runtime (not synced) ---
    int               loop_len = STEPS;
    std::atomic<int>  play_step{0};
    std::atomic<bool> running{true};
    std::atomic<bool> playing{true};

    // Recording state machine. Per-peer; never synced.
    std::atomic<RecordState> rec_state{RecordState::OFF};
    // Displayed count-in beat number (1..4) during COUNTDOWN; 0 otherwise.
    std::atomic<int>         countdown_beat{0};
    // Position within the current count-in beat (0..3). Advanced by the
    // timing thread; rolls over to advance countdown_beat.
    std::atomic<int>         countdown_subtick{0};

    // Metronome. Per-peer; never synced.
    std::atomic<bool> metronome_enabled{false};
    std::atomic<bool> metronome_trig{false};
    std::atomic<int>  metronome_pitch_hz{800};

    // sequencer -> audio: one-shot triggers
    std::atomic<bool> trig[TRACKS] = {};

    // --- networking bookkeeping ---
    // Drum-only: one bitmask per drum track, one bit per step. UI sets bits
    // via fetch_or when it edits a drum cell; flush thread claims them via
    // exchange(0) every NET_FLUSH_MS to construct outbound MSG_EDIT packets.
    // Melodic tracks use whole-list resends (see melodic_dirty).
    std::atomic<uint16_t> dirty[TRACKS] = {};

    // Melodic-track whole-list dirty flag. Set by the UI after any edit to
    // melodic_notes[i] is durable (note placed, released, deleted). The
    // flush thread resends the entire note list as MSG_MELODIC_TRACK.
    std::atomic<bool> melodic_dirty[MELODIC_VOICES] = {};
    std::atomic<bool> melodic_in_flight[MELODIC_VOICES] = {};

    // track_root_midi sync: bit per track, same Rule Y semantics.
    std::atomic<uint16_t> track_root_dirty{0};
    std::atomic<uint16_t> track_root_in_flight{0};

    // True iff this peer is a connected joiner. Set by Network::join() after
    // a successful handshake. Cleared by the joiner recv thread on EOF /
    // socket error; the joiner flush thread checks it and exits when it
    // falls. Always false on host and in solo mode.
    std::atomic<bool> network_alive{false};

    // Joiner-side optimistic-edit guard. Bit set when the flush thread has
    // claimed an outbound cell from `dirty` but the host's echo hasn't come
    // back yet. recv path uses Rule Y: if bit set, accept inbound and clear;
    // if not set, normal apply.
    std::atomic<uint16_t> in_flight[TRACKS] = {};

    // Scalar sync (bpm). Same Rule Y semantics as cells.
    std::atomic<bool>     bpm_dirty{false};
    std::atomic<bool>     bpm_in_flight{false};

    // Joiner-only label: lets the UI render "solo (host left)" once
    // network_alive falls. Set true in Network::join().
    bool is_joiner = false;
};

// UI/timing → audio onset trigger pool. Each melodic track has TRIG_POOL_SIZE
// atomic slots; producers CAS a freq into a free slot (-1.f). Audio callback
// drains the pool each block and claims a polyphony voice per trigger.
//
// Multi-producer safe (timing thread + UI key paths can both fire on the
// same audio frame, e.g. a chord onset). Drops the trigger silently if the
// pool is full — only happens if more than TRIG_POOL_SIZE simultaneous
// onsets land between audio callbacks (~10ms), which is well past chord
// territory.
inline constexpr int TRIG_POOL_SIZE = 8;
extern std::atomic<float> synth_trig_pool[MELODIC_VOICES][TRIG_POOL_SIZE];

void enqueue_synth_trig(int melodic_idx, float freq_hz);

// Solo / host default constructor wrapper. Builds a SessionState with the
// default drum pattern stamped onto kick / snare / chat tracks.
std::shared_ptr<SessionState> make_solo_session_state();

// The sequencer clock: walks the drum_grid / melodic_notes step by step at
// the session's bpm, writing drum trigs and melodic synth_trig_freqs.
void timing_thread(SessionState& s);

// Overdub capture path. Called from every input source that produces audible
// output (MPC pad, keyboard-mode pitch key, piano-roll pitch key). No-op if
// rec_state != RECORDING. Writes at the current play_step:
//   - drum track    : sets drum_grid[drum_kind][play_step] + dirty bit
//   - melodic track : appends Note{play_step, 1, pitch_midi, 127} to the
//                     track's note list (dedup'd by (start_step,pitch)),
//                     bounded by the 64-note cap
// `pitch_midi` is unused for drum tracks.
void record_input(SessionState& s, int track, uint8_t pitch_midi);

// Drive the record state machine on a `q` keypress.
//   OFF (playing)  → RECORDING : no transport changes
//   OFF (stopped)  → COUNTDOWN : playing←true, play_step←0, countdown_beat←1
//   COUNTDOWN      → OFF       : cancels (playing←false, play_step stays 0)
//   RECORDING      → OFF       : stops capture, transport keeps playing
void toggle_record(SessionState& s);

// Drive the transport-stop key `p` while COUNTDOWN or RECORDING is active.
// Hard halt: rec_state←OFF, playing←false, play_step←0. No-op if rec_state
// is already OFF (the caller handles plain transport-stop separately).
void hard_halt_transport(SessionState& s);

// One step-tick of countdown progress. No-op if rec_state != COUNTDOWN.
// Behavior: on each beat boundary (every 4 ticks) sets metronome_trig and
// metronome_pitch_hz (1000 Hz on beat 1, 800 Hz on beats 2..4). After the
// 16th call (i.e. the 4th completed beat) flips rec_state to RECORDING with
// play_step still at 0 so the loop's step 0 fires on the next tick.
void countdown_tick(SessionState& s);

// One step-tick of normal metronome firing. Sets metronome_trig and
// metronome_pitch_hz on beat boundaries (every 4 steps) iff
// metronome_enabled is true. `step` is the play_step about to fire. Step 0
// gets the accented (1000 Hz) blip; other beat boundaries get 800 Hz.
void metronome_step_tick(SessionState& s, int step);
