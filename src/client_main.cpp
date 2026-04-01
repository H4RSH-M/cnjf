// windows header collision hack
#if defined(_WIN32)
    // rename windows functions before they load
    #define Rectangle WinRectangle
    #define CloseWindow WinCloseWindow
    #define ShowCursor WinShowCursor
    #define DrawText WinDrawText
    #define DrawTextEx WinDrawTextEx
    #define LoadImage WinLoadImage
    #define PlaySound WinPlaySound
#endif

#include <iostream>
#include <cstring>
#include <queue>
#include <random>
#include <chrono>
#include <vector>
#include <algorithm>
#include <csignal>
#include <thread>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include "protocol.h"

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")

    // undo the renames so raylib works
    #undef Rectangle
    #undef CloseWindow
    #undef ShowCursor
    #undef DrawText
    #undef DrawTextEx
    #undef LoadImage
    #undef PlaySound
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #define SOCKET int
    #define INVALID_SOCKET -1
    #define closesocket close
#endif

#include "raylib.h"

#define SERVER_IP "127.0.0.1" // change if testing over actual network
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

    // rng stuff
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
    
    // windows needs this wsa init nonsense
#if defined(_WIN32)
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return 1;
    }
#endif

    std::cout << "[INFO] Game Networking Engine Client booting up..." << std::endl;

    // dtls setup
    OpenSSL_add_ssl_algorithms();
    SSL_load_error_strings();
    SSL_CTX *ctx = SSL_CTX_new(DTLS_client_method());

    SOCKET client_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (client_fd == INVALID_SOCKET) {
        std::cerr << "Socket creation failed." << std::endl;
        return 1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    connect(client_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));

    SSL *ssl = SSL_new(ctx);
    BIO *bio = BIO_new_dgram(client_fd, BIO_NOCLOSE);
    
    BIO_ctrl(bio, BIO_CTRL_DGRAM_SET_CONNECTED, 0, &server_addr);
    SSL_set_bio(ssl, bio, bio);

    std::cout << "[INFO] Initiating DTLS Handshake with server at " << SERVER_IP << ":" << PORT << "..." << std::endl;
    
    if (SSL_connect(ssl) <= 0) {
        std::cerr << "[ERROR] DTLS Handshake failed or timed out!" << std::endl;
        ERR_print_errors_fp(stderr);
    } else {
        std::cout << "[SUCCESS] DTLS Connection Established! Booting GUI..." << std::endl;
        
        // set non blocking based on os
#if defined(_WIN32)
        u_long mode = 1;
        ioctlsocket(client_fd, FIONBIO, &mode);
#else
        int flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
#endif

        // fire up raylib
        InitWindow(800, 600, "Game Networking Engine");
        SetTargetFPS(60);

        uint32_t current_sequence = 0;
        
        // spawn mid screen
        float player_x = 400.0f;
        float player_y = 300.0f;
        const float MOVEMENT_SPEED = 4.0f;
        
        // 150ms base ping, 50ms random jitter, 15% drop
        LagSim bad_wifi(150, 50, 0.15f);

        std::vector<GamePacket> history_buffer;
        std::vector<OtherPlayer> remote_players;

        // main loop
        while(!WindowShouldClose() && engine_running) {
            
            // get wasd inputs
            if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) { player_y -= MOVEMENT_SPEED; }
            if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) { player_y += MOVEMENT_SPEED; }
            if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) { player_x -= MOVEMENT_SPEED; }
            if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) { player_x += MOVEMENT_SPEED; }

            // heartbeat packet so server doesnt kick us
            GamePacket packet;
            packet.type = CLIENT_INPUT;
            packet.sequence_number = current_sequence++;
            packet.x = player_x;
            packet.y = player_y;
            
            history_buffer.push_back(packet);

            // dump into lag sim instead of sending straight out
            bad_wifi.Add(packet);
            
            // read auth state from server
            GamePacket server_reply;
            while (SSL_read(ssl, &server_reply, sizeof(GamePacket)) == sizeof(GamePacket)) {
                if (server_reply.type == SERVER_STATE) {
                    
                    // dump confirmed history
                    history_buffer.erase(
                        std::remove_if(history_buffer.begin(), history_buffer.end(),
                            [&](const GamePacket& p) { return p.sequence_number <= server_reply.sequence_number; }),
                        history_buffer.end()
                    );

                    // update the remote guys for rendering
                    remote_players.clear();
                    for(int i = 0; i < server_reply.num_other_players; i++) {
                        remote_players.push_back(server_reply.other_players[i]);
                    }
                }
            }

            // push packets that finished waiting
            bad_wifi.Update(ssl);

            BeginDrawing();
            ClearBackground(RAYWHITE);
            
            // draw other players (red)
            for (const auto& other : remote_players) {
                DrawCircle((int)other.x, (int)other.y, 20, RED);
                DrawText(TextFormat("ID: %u", other.id % 1000), (int)other.x - 20, (int)other.y - 35, 10, DARKGRAY);
            }

            // draw us on top (blue)
            DrawCircle((int)player_x, (int)player_y, 20, BLUE);
            DrawText("YOU", (int)player_x - 12, (int)player_y - 35, 10, DARKBLUE);

            // text overlay stuff
            DrawText("WASD/Arrows to Move", 10, 10, 20, DARKGRAY);
            DrawText(TextFormat("Ping Sim: %d ms", 150), 10, 30, 20, GRAY);
            DrawText(TextFormat("Connected Players: %d", remote_players.size() + 1), 10, 50, 20, GRAY);

            EndDrawing();
        }
        
        CloseWindow();
    }

    std::cout << "\n[INFO] Graceful shutdown initiated. Sending disconnect signal..." << std::endl;
    SSL_shutdown(ssl);

    SSL_free(ssl);
    closesocket(client_fd);
    
#if defined(_WIN32)
    WSACleanup();
#endif
    
    SSL_CTX_free(ctx);
    return 0;
}