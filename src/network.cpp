#include <chrono>
#include <cmath>
#include <thread>
#include <set>
#include <iostream>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include "session.h"
#include "network.h"

int Network::connect_to_server(uint32_t address_num, uint16_t room) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    
    if (sock < 0) {
        std::perror("Socket creation failed");
        return -1;
    }

    struct sockaddr_in serv_addr;
    std::memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(App::GLOBAL_PORT);
    serv_addr.sin_addr.s_addr = htonl(address_num);

    // This is a blocking call
    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::perror("Connection failed");
        close(sock);
        return -1;
    }

    std::cout << "Connected! Sending join request for room " << room << "...\n";

    // Expected to get response back from host as to whether a valid room was selected
    send(sock, &room, sizeof(room), 0);

    uint8_t response;
    recv(sock, &response, 1, 0);

    if (response == App::STATUS_FAILURE) {
        std::cerr << "Error: Room does not exist!" << std::endl;
    }

    uint8_t success_code = App::STATUS_SUCCESS;
    send(sock, &success_code, 1, 0);
    std::cout << "Connected to room!" << std::endl;

    // Need to add loop for receiving information
    // Perhaps we also kick off a thread here that handles sending updates

    return 0;
}

void Network::client_handler(int client_sock) {
    uint16_t requested_room = 0;
    int bytes_received = recv(client_sock, &requested_room, sizeof(requested_room), 0);
    requested_room = ntohs(requested_room);

    if (bytes_received <= 0) {
        std::cout << "Client disconnected before sending room ID.\n";
        close(client_sock);

        return;
    }

    bool room_exists = false;

    for (const auto& [key, value] : SessionState::active_rooms) {
        if (key == requested_room) {
            room_exists = true;
            break;
        }
    }

    if (!room_exists) {
        uint8_t error_code = 0xFF; // Define 0xFF as "Room doesn't exist"
        send(client_sock, &error_code, 1, 0);

        std::cout << "Client's desired room does not exist; closing connection\n";
        close(client_sock);

        return;
    }

    // Room exists, send notif
    uint8_t success_code = App::STATUS_SUCCESS;
    send(client_sock, &success_code, 1, 0);
    
    // Verifying client acks new connection
    uint8_t client_response;
    recv(client_sock, &client_response, sizeof(client_response), 0);

    if (client_response == App::STATUS_FAILURE) {
        std::cout << std::format("Client joined room {}\n", requested_room);
    }
    
    SessionState* state = SessionState::active_rooms[requested_room];
    // Sending initial state
    send(client_sock, &(state->state_struct), sizeof(MsgState), 0);

    auto interval = std::chrono::milliseconds(NET_FLUSH_MS);
    auto next_wakeup = std::chrono::steady_clock::now() + interval;

    // Loop sending updates
    while (true) {
        if (state->has_updates.load()) {
            std::cout << "has updates" << std::endl;
            MsgDiff diff = state->diff;
            ssize_t sent = send(client_sock, &diff, sizeof(MsgDiff), 0);
            
            if (sent <= 0) break;
            
            state->has_updates.store(false);
        } else {
            std::cout << "has no updates" << std::endl;
        }

        std::this_thread::sleep_until(next_wakeup);
        next_wakeup += interval;
    }

    close(client_sock);
}

void Network::receive_connections() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    // Allow immediate reuse of the port after restart
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // Listen on all network cards
    address.sin_port = htons(5000);

    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 5); // Queue up to 5 pending connections

    while (true) {
        int client_sock = accept(server_fd, nullptr, nullptr);

        if (client_sock >= 0) {
            // New user connected! Spawn a thread to handle them
            std::thread([client_sock]() {
                client_handler(client_sock);
            }).detach();
        }
    }
}