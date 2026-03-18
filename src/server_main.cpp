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

// client tracker
struct PlayerState {
    SSL* ssl;
    uint32_t highest_seq_received;
};

int main() {
    std::cout << "[INFO] Booting Jackfruit Game Engine (Multi-Client)..." << std::endl;

    OpenSSL_add_ssl_algorithms();
    SSL_load_error_strings();
    SSL_CTX *ctx = SSL_CTX_new(DTLS_server_method());
    
    // dtls requires cookie exchange for multi-client to prevent udp spoofing
    SSL_CTX_set_options(ctx, SSL_OP_COOKIE_EXCHANGE);

    if (SSL_CTX_use_certificate_file(ctx, "../server.crt", SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx, "../server.key", SSL_FILETYPE_PEM) <= 0) {
        handle_openssl_error();
    }

    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
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

    // main server loop
    while (true) {
        auto frame_start = std::chrono::steady_clock::now();
        current_tick++;

        // 1. check for new connections knocking on the raw socket
        struct sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        char peek_buf[1];
        
        // peek the socket without removing the packet to see who it is
        if (recvfrom(server_fd, peek_buf, 1, MSG_PEEK | MSG_DONTWAIT, (struct sockaddr*)&peer_addr, &peer_len) > 0) {
            std::string ip(inet_ntoa(peer_addr.sin_addr));
            std::string port = std::to_string(ntohs(peer_addr.sin_port));
            std::string player_id = ip + ":" + port;

            if (active_players.find(player_id) == active_players.end()) {
                std::cout << "[INFO] New connection attempt from: " << player_id << std::endl;
                
                // linux udp hack: create a dedicated connected socket for this specific client
                int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
                int opt = 1;
                setsockopt(client_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                setsockopt(client_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
                bind(client_fd, (const struct sockaddr *)&server_addr, sizeof(server_addr));
                connect(client_fd, (struct sockaddr*)&peer_addr, peer_len);
                
                fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL, 0) | O_NONBLOCK);

                SSL *client_ssl = SSL_new(ctx);
                BIO *bio = BIO_new_dgram(client_fd, BIO_NOCLOSE);
                SSL_set_bio(client_ssl, bio, bio);
                SSL_set_accept_state(client_ssl);

                active_players[player_id] = {client_ssl, 0};
                
                // flush the peeking packet from the main socket so we don't loop infinitely
                recvfrom(server_fd, peek_buf, 1, MSG_DONTWAIT, NULL, NULL);
            }
        }

        // 2. processing the incoming data for all established client/players
        for (auto it = active_players.begin(); it != active_players.end(); ) {
            std::string player_id = it->first;
            SSL* ssl = it->second.ssl;
            uint32_t& highest_seq = it->second.highest_seq_received;

            if (!SSL_is_init_finished(ssl)) {
                if (SSL_do_handshake(ssl) <= 0) {
                    int err = SSL_get_error(ssl, -1);
                    if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                        std::cerr << "[ERROR] Handshake failed for " << player_id << std::endl;
                        SSL_free(ssl);
                        it = active_players.erase(it);
                        continue;
                    }
                } else {
                    std::cout << "[SUCCESS] Handshake complete for " << player_id << std::endl;
                }
            } else {
                GamePacket incoming_packet;
                int bytes_read = SSL_read(ssl, &incoming_packet, sizeof(GamePacket));
                
                if (bytes_read == sizeof(GamePacket) && incoming_packet.type == CLIENT_INPUT) {
                    if (incoming_packet.sequence_number >= highest_seq) {
                        highest_seq = incoming_packet.sequence_number;
                        std::cout << "[Tick " << current_tick << " | " << player_id << "] "
                                  << "Seq: " << incoming_packet.sequence_number 
                                  << " | Pos: (" << incoming_packet.x << ", " << incoming_packet.y << ")" 
                                  << std::endl;
                    }
                } else if (bytes_read < 0) {
                    int err = SSL_get_error(ssl, bytes_read);
                    if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                        std::cout << "[INFO] Player disconnected: " << player_id << std::endl;
                        SSL_free(ssl);
                        it = active_players.erase(it);
                        continue;
                    }
                }
            }
            ++it;
        }

        // 60 tick sleep
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