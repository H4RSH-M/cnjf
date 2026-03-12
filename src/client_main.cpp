#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <protocol.h>

#define SERVER_IP "127.0.0.1"
#define PORT 8080

int main() {
    std::cout << "[INFO] Jackfruit Client booting up..." << std::endl;

    OpenSSL_add_ssl_algorithms();
    SSL_load_error_strings();
    SSL_CTX *ctx = SSL_CTX_new(DTLS_client_method());

    // udp raww 
    int client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    
    connect(client_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));

    // openssl utilisation
    SSL *ssl = SSL_new(ctx);
    BIO *bio = BIO_new_dgram(client_fd, BIO_NOCLOSE);
    
    BIO_ctrl(bio, BIO_CTRL_DGRAM_SET_CONNECTED, 0, &server_addr);
    SSL_set_bio(ssl, bio, bio);

    std::cout << "[INFO] Initiating DTLS Handshake with server at " << SERVER_IP << ":" << PORT << "..." << std::endl;
    
    // handshake attempt
    if (SSL_connect(ssl) <= 0) {
        std::cerr << "[ERROR] DTLS Handshake failed or timed out!" << std::endl;
        ERR_print_errors_fp(stderr);
    } else {
        std::cout << "[SUCCESS] DTLS Connection Established! Entering Game Loop..." << std::endl;
        
        uint32_t current_sequence = 0;
        float player_x = 0.0f;
        
        while(true) {
            GamePacket packet;
            packet.type = CLIENT_INPUT;
            packet.sequence_number = current_sequence++;
            packet.x = player_x;
            packet.y = 0.0f;
            
            SSL_write(ssl, &packet, sizeof(GamePacket));
            
            player_x += 1.5f; 
            
            usleep(100000);
        }
    }

    SSL_free(ssl);
    close(client_fd);
    SSL_CTX_free(ctx);
    return 0;
}