#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <protocol.h>
#include <queue>
#include <random>
#include <chrono>
#include <vector>
#include <algorithm>
#include <signal.h>

#define SERVER_IP "127.0.0.1"
#define PORT 8080

volatile sig_atomic_t engine_running = 1;

void handle_sigint(int sig) {
    engine_running = 0;
}

// lag sim structs
struct StuckPacket {
    GamePacket data;
    std::chrono::steady_clock::time_point release_time;
};

class LagSim {
private:
    std::queue<StuckPacket> q;
    int ping;
    int jitter;
    float loss_rate; 

    // rng tools
    std::mt19937 rng;
    std::uniform_real_distribution<float> drop_dice;
    std::uniform_int_distribution<int> jitter_dice;

public:
    LagSim(int p, int j, float l) 
        : ping(p), jitter(j), loss_rate(l),
          rng(std::random_device{}()), drop_dice(0.0f, 1.0f), jitter_dice(-j, j) {}

    // hijack the packet instead of sending it
    void Add(const GamePacket& pkt) {
        // roll the dice to see if we delete it
        if (drop_dice(rng) < loss_rate) {
            std::cout << "[SIM] Yeeted packet " << pkt.sequence_number << " into the void.\n";
            return; 
        }

        // how long to hold it hostage
        int delay = ping + jitter_dice(rng);
        if (delay < 0) delay = 0;

        auto send_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
        q.push({pkt, send_time});
    }

    void Update(SSL* ssl) {
        auto now = std::chrono::steady_clock::now();

        while (!q.empty()) {
            if (now >= q.front().release_time) {
                // buffering done, ab sending the packet
                GamePacket p = q.front().data;
                SSL_write(ssl, &p, sizeof(GamePacket));
                q.pop();
            } else {
                // packet at the front isn't ready, so stop checking
                break; 
            }
        }
    }
};

int main() {
    signal(SIGINT, handle_sigint);
    std::cout << "[INFO] Game Networking Engine Client booting up..." << std::endl;

    OpenSSL_add_ssl_algorithms();
    SSL_load_error_strings();
    SSL_CTX *ctx = SSL_CTX_new(DTLS_client_method());

    // udp raw 
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
        
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

        uint32_t current_sequence = 0;
        float player_x = 0.0f;
        
        // lag simulator 
        // 150ms base ping, 50ms random jitter, 15% packet drop chance
        LagSim bad_wifi(150, 50, 0.15f);

        std::vector<GamePacket> history_buffer;

        while(engine_running) {
            GamePacket packet;
            packet.type = CLIENT_INPUT;
            packet.sequence_number = current_sequence++;
            packet.x = player_x;
            packet.y = 0.0f;
            
            history_buffer.push_back(packet);

            // feed packet to lsgsim insteas of socket
            bad_wifi.Add(packet);
            
            player_x += 1.5f; 
            
            GamePacket server_reply;
            while (SSL_read(ssl, &server_reply, sizeof(GamePacket)) == sizeof(GamePacket)) {
                if (server_reply.type == SERVER_STATE) {
                    std::cout << "[RECONCILE] Server confirmed Seq " << server_reply.sequence_number << ". Clearing old history." << std::endl;
                    
                    history_buffer.erase(
                        std::remove_if(history_buffer.begin(), history_buffer.end(),
                            [&](const GamePacket& p) { return p.sequence_number <= server_reply.sequence_number; }),
                        history_buffer.end()
                    );
                }
            }

            // push any packets that are done waiting
            bad_wifi.Update(ssl);

            usleep(16666);
        }
    }

    std::cout << "\n[INFO] Graceful shutdown initiated. Sending disconnect signal..." << std::endl;
    SSL_shutdown(ssl);

    SSL_free(ssl);
    close(client_fd);
    SSL_CTX_free(ctx);
    return 0;
}