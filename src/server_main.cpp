#include <iostream>
#include <cstring>
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
};

int main() {
    std::cout << "[INFO] Booting Game Networking Engine (Multi-Client)..." << std::endl;

    OpenSSL_add_ssl_algorithms();
    SSL_load_error_strings();
    SSL_CTX *ctx = SSL_CTX_new(DTLS_server_method());
    
    if (SSL_CTX_use_certificate_file(ctx, "../server.crt", SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx, "../server.key", SSL_FILETYPE_PEM) <= 0) {
        handle_openssl_error();
    }

    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    
    // The main socket MUST have reuse flags, otherwise Linux 
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

    std::cout << "[INFO] Server running on port " << PORT << ". Ticking at " << TICK_RATE << "Hz." << std::endl;

    std::map<std::string, PlayerState> active_players;
    const std::chrono::milliseconds target_frame_time(1000 / TICK_RATE);
    uint32_t current_tick = 0;

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
                std::cout << "[INFO] New connection attempt from: " << player_id << std::endl;
                
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

                active_players[player_id] = {client_ssl, 0, std::chrono::steady_clock::now()};
            }
        }

        auto now = std::chrono::steady_clock::now();

        // Processing the active players
        for (auto it = active_players.begin(); it != active_players.end(); ) {
            std::string player_id = it->first;
            SSL* ssl = it->second.ssl;
            uint32_t& highest_seq = it->second.highest_seq_received;
            bool player_dropped = false;

            if (!SSL_is_init_finished(ssl)) {
                if (SSL_do_handshake(ssl) <= 0) {
                    int err = SSL_get_error(ssl, -1);
                    // Ignore EAGAIN/WANT_READ.
                    if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE && err != SSL_ERROR_SYSCALL) {
                        std::cerr << "[ERROR] Handshake failed for " << player_id << std::endl;
                        player_dropped = true;
                    }
                } else {
                    std::cout << "[SUCCESS] Handshake complete for " << player_id << std::endl;
                    it->second.last_active = now;
                }
            } else {
                GamePacket incoming_packet;
                int bytes_read;
                
                while ((bytes_read = SSL_read(ssl, &incoming_packet, sizeof(GamePacket))) > 0) {
                    it->second.last_active = now;
                    
                    if (bytes_read == sizeof(GamePacket) && incoming_packet.type == CLIENT_INPUT) {
                        if (incoming_packet.sequence_number >= highest_seq) {
                            highest_seq = incoming_packet.sequence_number;
                            std::cout << "[Tick " << current_tick << " | " << player_id << "] "
                                      << "Seq: " << incoming_packet.sequence_number 
                                      << " | Pos: (" << incoming_packet.x << ", " << incoming_packet.y << ")" 
                                      << std::endl;

                            // bounce confirmed state back to the client
                            GamePacket ack;
                            ack.type = SERVER_STATE;
                            ack.sequence_number = incoming_packet.sequence_number;
                            ack.x = incoming_packet.x; 
                            ack.y = incoming_packet.y;
                            SSL_write(ssl, &ack, sizeof(GamePacket));
                        }
                    }
                }
                
                if (bytes_read <= 0) {
                    int err = SSL_get_error(ssl, bytes_read);
                    if (err == SSL_ERROR_ZERO_RETURN || (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE && err != SSL_ERROR_SYSCALL)) {
                        std::cout << "[INFO] Player explicitly disconnected: " << player_id << std::endl;
                        player_dropped = true;
                    }
                }
            }

            if (!player_dropped && std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_active).count() > 3) {
                std::cout << "[WARNING] Player timed out (Force Quit): " << player_id << std::endl;
                player_dropped = true;
            }

            if (player_dropped) {
                SSL_free(ssl);
                it = active_players.erase(it);
            } else {
                ++it;
            }
        }

        auto frame_end = std::chrono::steady_clock::now();
        auto elapsed_time = std::chrono::duration_cast<std::chrono::milliseconds>(frame_end - frame_start);
        if (elapsed_time < target_frame_time) {
            std::this_thread::sleep_for(target_frame_time - elapsed_time);
        }
    }

    SSL_CTX_free(ctx);
    close(server_fd);
    return 0;
}