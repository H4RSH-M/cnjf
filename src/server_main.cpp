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

#include "protocol.h" 

#define PORT 8080
#define TICK_RATE 60 // target 60 updates per sec

void handle_openssl_error() {
    ERR_print_errors_fp(stderr);
    exit(EXIT_FAILURE);
}

int main() {
    std::cout << "[INFO] Booting Jackfruit Game Engine..." << std::endl;

    // dtls context setup
    OpenSSL_add_ssl_algorithms();
    SSL_load_error_strings();
    SSL_CTX *ctx = SSL_CTX_new(DTLS_server_method());
    
    if (SSL_CTX_use_certificate_file(ctx, "../server.crt", SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx, "../server.key", SSL_FILETYPE_PEM) <= 0) {
        handle_openssl_error();
    }

    // raw udp socket
    int server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    bind(server_fd, (const struct sockaddr *)&server_addr, sizeof(server_addr));

    std::cout << "[INFO] Waiting for first player to connect..." << std::endl;

    BIO *bio = BIO_new_dgram(server_fd, BIO_NOCLOSE);
    SSL *ssl = SSL_new(ctx);
    SSL_set_bio(ssl, bio, bio);

    // block until handshake completes
    if (SSL_accept(ssl) <= 0) {
        std::cerr << "[ERROR] Handshake failed." << std::endl;
        return 1;
    }
    std::cout << "[SUCCESS] Player Connected! Starting Game Loop..." << std::endl;

    // switch to non-blocking for the main loop
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    const std::chrono::milliseconds target_frame_time(1000 / TICK_RATE);
    uint32_t current_tick = 0;
    uint32_t highest_seq_received = 0; // track udp ordering

    // main server loop
    while (true) {
        auto frame_start = std::chrono::steady_clock::now();
        current_tick++;

        GamePacket incoming_packet;
        int bytes_read = SSL_read(ssl, &incoming_packet, sizeof(GamePacket));
        
        if (bytes_read == sizeof(GamePacket)) {
            if (incoming_packet.type == CLIENT_INPUT) {
                // only process fresh packets. drop delayed udp junk.
                if (incoming_packet.sequence_number >= highest_seq_received) {
                    highest_seq_received = incoming_packet.sequence_number;
                    
                    std::cout << "[Tick " << current_tick << "] "
                              << "Seq: " << incoming_packet.sequence_number 
                              << " | Pos: (" << incoming_packet.x << ", " << incoming_packet.y << ")" 
                              << std::endl;
                } else {
                    std::cout << "[WARNING] Dropped out-of-order packet: " 
                              << incoming_packet.sequence_number << std::endl;
                }
            }
        } else if (bytes_read > 0) {
            std::cerr << "[WARNING] Bad packet size." << std::endl;
        } else {
            // normal behavior if no data this tick. break only on actual errors.
            int err = SSL_get_error(ssl, bytes_read);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                std::cerr << "[ERROR] Connection dropped." << std::endl;
                break; 
            }
        }

        // sleep to maintain tick rate
        auto frame_end = std::chrono::steady_clock::now();
        auto elapsed_time = std::chrono::duration_cast<std::chrono::milliseconds>(frame_end - frame_start);
        
        if (elapsed_time < target_frame_time) {
            std::this_thread::sleep_for(target_frame_time - elapsed_time);
        }
    }

    // teardown
    SSL_free(ssl);
    close(server_fd);
    SSL_CTX_free(ctx);
    return 0;
}