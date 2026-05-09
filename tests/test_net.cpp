#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>

#include "session.h"
#include "network.h"

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

int main() {
    test_state_sync();
    test_wrong_room_rejected();
    std::cout << "All net tests passed.\n";
    return 0;
}
