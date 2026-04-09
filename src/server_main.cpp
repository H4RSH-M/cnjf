#include <cstring>
#include <iostream>
#include <fstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <chrono>
#include <thread>
#include <map>
#include <string>
#include <functional>
#include <iomanip>

#include "protocol.h"

#define PORT 8080
#define TICK_RATE 60

void handle_openssl_error() {
    ERR_print_errors_fp(stderr);
    exit(EXIT_FAILURE);
}

struct PlayerState {
    SSL* ssl;
    uint32_t highest_seq_received;
    std::chrono::steady_clock::time_point last_active;
    std::chrono::steady_clock::time_point last_packet_time;
    
    // Stats for Dashboard
    float x;
    float y;
    uint32_t total_packets_received;
    uint32_t total_packets_lost;
    int latest_arrival_gap_ms;
};

// Timestamp helper for the log file
static std::string ts() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()).count() % 1000;
    std::ostringstream ss;
    ss << std::put_time(std::localtime(&t), "%H:%M:%S")
       << "." << std::setw(3) << std::setfill('0') << ms;
    return ss.str();
}

int main() {
    // Open the log file in append mode. All events go here so they don't break the dashboard.
    std::ofstream logger("server_events.log", std::ios::out | std::ios::app);
    logger << "\n[" << ts() << "] === SERVER BOOT SEQUENCED ===" << std::endl;

    OpenSSL_add_ssl_algorithms();
    SSL_load_error_strings();
    SSL_CTX *ctx = SSL_CTX_new(DTLS_server_method());
    
    if (SSL_CTX_use_certificate_file(ctx, "../server.crt", SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx, "../server.key", SSL_FILETYPE_PEM) <= 0) {
        handle_openssl_error();
    }

    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    
    // note to self: The main socket must have reuse flags, otherwise Linux 
    // won't route follow-up packets to the individual player sockets.
    // I already paid the price for this
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    bind(server_fd, (const struct sockaddr *)&server_addr, sizeof(server_addr));

    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    std::map<std::string, PlayerState> active_players;
    const std::chrono::milliseconds target_frame_time(1000 / TICK_RATE);
    uint32_t current_tick = 0;
    
    auto last_dashboard_draw = std::chrono::steady_clock::now();
    logger << "[" << ts() << "] Server fully initialized on port " << PORT << " at " << TICK_RATE << "Hz." << std::endl;

    while (true) {
        auto frame_start = std::chrono::steady_clock::now();
        current_tick++;

        struct sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        char discard_buf[4096];
        
        // Normal recvfrom instead of PEEK. Actually consume the packet 
        // from the master socket so there is no inf deadlock.
        if (recvfrom(server_fd, discard_buf, sizeof(discard_buf), MSG_DONTWAIT, (struct sockaddr*)&peer_addr, &peer_len) > 0) {
            std::string ip(inet_ntoa(peer_addr.sin_addr));
            std::string port = std::to_string(ntohs(peer_addr.sin_port));
            std::string player_id = ip + ":" + port;

            if (active_players.find(player_id) == active_players.end()) {
                logger << "[" << ts() << "] [CONNECT] Connection attempt from: " << player_id << std::endl;
                
                int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
                setsockopt(client_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                setsockopt(client_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
                bind(client_fd, (const struct sockaddr *)&server_addr, sizeof(server_addr));
                connect(client_fd, (struct sockaddr*)&peer_addr, peer_len);
                fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL, 0) | O_NONBLOCK);

                SSL *client_ssl = SSL_new(ctx);
                BIO *bio = BIO_new_dgram(client_fd, BIO_NOCLOSE);
                SSL_set_bio(client_ssl, bio, bio);
                SSL_set_accept_state(client_ssl);

                // Starts the players roughly in the center of the screen
                active_players[player_id] = {
                    client_ssl, 0, std::chrono::steady_clock::now(), std::chrono::steady_clock::now(),
                    400.0f, 300.0f, 0, 0, 0
                };
            }
        }

        auto now = std::chrono::steady_clock::now();

        // Processing the active players
        for (auto it = active_players.begin(); it != active_players.end(); ) {
            std::string player_id = it->first;
            SSL* ssl = it->second.ssl;
            bool player_dropped = false;

            if (!SSL_is_init_finished(ssl)) {
                if (SSL_do_handshake(ssl) <= 0) {
                    int err = SSL_get_error(ssl, -1);
                    // Ignore EAGAIN/WANT_READ.
                    if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE && err != SSL_ERROR_SYSCALL) {
                        logger << "[" << ts() << "] [ERROR] Handshake failed for " << player_id << std::endl;
                        player_dropped = true;
                    }
                } else {
                    logger << "[" << ts() << "] [SUCCESS] Handshake complete for " << player_id << ". Encrypted tunnel established." << std::endl;
                    it->second.last_active = now;
                }
            } else {
                GamePacket incoming_packet;
                int bytes_read;
                
                while ((bytes_read = SSL_read(ssl, &incoming_packet, sizeof(GamePacket))) > 0) {
                    it->second.last_active = now;
                    
                    if (bytes_read == sizeof(GamePacket) && incoming_packet.type == CLIENT_INPUT) {
                        
                        // Network Math: Sequence gaps indicate dropped packets
                        if (it->second.highest_seq_received > 0 && incoming_packet.sequence_number > it->second.highest_seq_received + 1) {
                            it->second.total_packets_lost += (incoming_packet.sequence_number - it->second.highest_seq_received - 1);
                        }
                        
                        // Measure arrival gap (Jitter proxy)
                        it->second.latest_arrival_gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.last_packet_time).count();
                        it->second.last_packet_time = now;
                        it->second.total_packets_received++;

                        if (incoming_packet.sequence_number >= it->second.highest_seq_received) {
                            it->second.highest_seq_received = incoming_packet.sequence_number;
                            
                            // Updating the authoritative server position
                            it->second.x = incoming_packet.x;
                            it->second.y = incoming_packet.y;

                            // bounce the confirmed state back to the client
                            GamePacket ack;
                            ack.type = SERVER_STATE;
                            ack.sequence_number = incoming_packet.sequence_number;
                            ack.x = incoming_packet.x; 
                            ack.y = incoming_packet.y;
                            
                            // Pack all OTHER players into the broadcast payload
                            ack.num_other_players = 0;
                            for (const auto& other : active_players) {
                                if (other.first != player_id && ack.num_other_players < 8) {
                                    // Hash the IP string to create a unique color/ID for rendering
                                    ack.other_players[ack.num_other_players].id = (uint32_t)std::hash<std::string>{}(other.first);
                                    ack.other_players[ack.num_other_players].x = other.second.x;
                                    ack.other_players[ack.num_other_players].y = other.second.y;
                                    ack.num_other_players++;
                                }
                            }
                            SSL_write(ssl, &ack, sizeof(GamePacket));
                        }
                    }
                }
                
                // Non-blocking SSL_read returns WANT_READ when no data - that's fine
                if (bytes_read <= 0) {
                    int err = SSL_get_error(ssl, bytes_read);
                    if (err == SSL_ERROR_ZERO_RETURN || (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE && err != SSL_ERROR_SYSCALL)) {
                        logger << "[" << ts() << "] [DISCONNECT] Player explicitly disconnected: " << player_id << std::endl;
                        player_dropped = true;
                    }
                }
            }

            if (!player_dropped && std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_active).count() > 3) {
                logger << "[" << ts() << "] [TIMEOUT] Player timed out (Force Quit / No heartbeat): " << player_id << std::endl;
                player_dropped = true;
            }

            if (player_dropped) {
                SSL_free(ssl);
                it = active_players.erase(it);
            } else {
                ++it;
            }
        }

        // TUI Dashboard Drawing Logic (Updates twice a second to prevent flickering)
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_dashboard_draw).count() > 500) {
            std::cout << "\033[2J\033[H"; // ANSI Escape: Clear screen and move cursor to top-left
            std::cout << "+----------------------------------------------------------------------+\n";
            std::cout << "| G.N.E. SERVER DIAGNOSTICS | PORT: " << PORT << " | TICK: " << std::setw(15) << std::left << current_tick << " |\n";
            std::cout << "+----------------------------------------------------------------------+\n";
            std::cout << std::left << "| " << std::setw(20) << "PLAYER ID" 
                      << std::setw(15) << "| POS (X, Y)" 
                      << std::setw(15) << "| PKT LOSS" 
                      << std::setw(15) << "| ARRIVAL GAP" << "|\n";
            std::cout << "+----------------------------------------------------------------------+\n";
            
            if (active_players.empty()) {
                std::cout << "| Waiting for connections...                                           |\n";
            } else {
                for (const auto& pair : active_players) {
                    float loss_pct = 0.0f;
                    if (pair.second.total_packets_received > 0) {
                        loss_pct = ((float)pair.second.total_packets_lost / (pair.second.total_packets_received + pair.second.total_packets_lost)) * 100.0f;
                    }

                    std::string pos_str = std::to_string((int)pair.second.x) + ", " + std::to_string((int)pair.second.y);
                    std::string loss_str = std::to_string((int)loss_pct) + "% (" + std::to_string(pair.second.total_packets_lost) + ")";
                    std::string gap_str = std::to_string(pair.second.latest_arrival_gap_ms) + " ms";

                    std::cout << "| " << std::left << std::setw(19) << pair.first
                              << "| " << std::setw(13) << pos_str
                              << "| " << std::setw(13) << loss_str
                              << "| " << std::setw(13) << gap_str << "|\n";
                }
            }
            std::cout << "+----------------------------------------------------------------------+\n";
            std::cout << "  (View server_events.log for connection and handshake history)\n";
            last_dashboard_draw = now;
        }

        auto frame_end = std::chrono::steady_clock::now();
        auto elapsed_time = std::chrono::duration_cast<std::chrono::milliseconds>(frame_end - frame_start);
        if (elapsed_time < target_frame_time) {
            std::this_thread::sleep_for(target_frame_time - elapsed_time);
        }
    }

    SSL_CTX_free(ctx);
    close(server_fd);
    logger.close();
    return 0;
}