// windows header collision hack
#if defined(_WIN32)
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
#include <iomanip>
#include <sstream>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include "protocol.h"

// windows needs this wsa init nonsense
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
        if (drop_dice(rng) < loss_rate) return; 

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
    if (client_fd == INVALID_SOCKET) return 1;
    
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
        ERR_print_errors_fp(stderr);
        return 1;
    }
    
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

    // Custom Theme Colors
    Color bgDark = { 11, 19, 43, 255 };
    Color gridColor = { 28, 37, 65, 255 };
    Color playerCore = { 0, 200, 255, 255 };
    Color playerGlow = { 0, 100, 255, 150 };
    Color enemyCore = { 255, 50, 80, 255 };
    Color enemyGlow = { 200, 0, 50, 150 };
    Color textNeon = { 0, 255, 200, 255 };
    Color bottomBarBg = { 0, 0, 0, 200 };

    uint32_t current_sequence = 0;
    int latest_ack = 0;
    
    // spawn mid screen
    float player_x = 400.0f;
    float player_y = 300.0f;
    const float MOVEMENT_SPEED = 4.0f;
    
    LagSim bad_wifi(150, 50, 0.15f);

    std::vector<GamePacket> history_buffer;
    std::vector<OtherPlayer> remote_players;
    
    auto last_send_time = std::chrono::steady_clock::now();

    // main loop
    while(!WindowShouldClose() && engine_running) {
        bool moved = false;

        // get wasd inputs
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) { player_y -= MOVEMENT_SPEED; moved = true; }
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) { player_y += MOVEMENT_SPEED; moved = true; }
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT)) { player_x -= MOVEMENT_SPEED; moved = true; }
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) { player_x += MOVEMENT_SPEED; moved = true; }

        auto now = std::chrono::steady_clock::now();
        bool is_heartbeat = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_send_time).count() > 1000;

        // heartbeat packet so server doesnt kick us
        if (moved || is_heartbeat) {
            GamePacket packet;
            packet.type = CLIENT_INPUT;
            packet.sequence_number = current_sequence++;
            packet.x = player_x;
            packet.y = player_y;
            
            history_buffer.push_back(packet);
            
            // dump into lag sim instead of sending straight out
            bad_wifi.Add(packet);
            last_send_time = now;
        }
        
        // read auth state from server
        GamePacket server_reply;
        while (SSL_read(ssl, &server_reply, sizeof(GamePacket)) == sizeof(GamePacket)) {
            if (server_reply.type == SERVER_STATE) {
                latest_ack = server_reply.sequence_number;

                history_buffer.erase(
                    std::remove_if(history_buffer.begin(), history_buffer.end(),
                        [&](const GamePacket& p) { return p.sequence_number <= server_reply.sequence_number; }),
                    history_buffer.end()
                );

                remote_players.clear();
                for(int i = 0; i < server_reply.num_other_players; i++) {
                    remote_players.push_back(server_reply.other_players[i]);
                }
            }
        }

        // push packets that finished waiting
        bad_wifi.Update(ssl);

        BeginDrawing();
        ClearBackground(bgDark);
        
        // Draw Cyber Grid
        for(int i = 0; i < 800; i += 40) DrawLine(i, 0, i, 600, gridColor);
        for(int i = 0; i < 600; i += 40) DrawLine(0, i, 800, i, gridColor);
        
        // Draw Remote Players (Sharp Red)
        for (const auto& other : remote_players) {
            DrawRectangle((int)other.x - 15, (int)other.y - 15, 30, 30, enemyGlow);
            DrawRectangleLines((int)other.x - 15, (int)other.y - 15, 30, 30, enemyCore);
            DrawText(TextFormat("ID:%u", other.id % 1000), (int)other.x - 15, (int)other.y - 30, 10, WHITE);
        }

        // Draw Local Player (Sharp Neon Blue)
        DrawRectangle((int)player_x - 15, (int)player_y - 15, 30, 30, playerGlow);
        DrawRectangleLines((int)player_x - 15, (int)player_y - 15, 30, 30, playerCore);
        DrawText("YOU", (int)player_x - 10, (int)player_y - 30, 10, textNeon);

        // Top UI Overlay
        DrawRectangle(5, 5, 250, 45, ColorAlpha(BLACK, 0.7f));
        DrawRectangleLines(5, 5, 250, 45, textNeon);
        DrawText("WASD/Arrows to Move", 15, 12, 10, GRAY);
        DrawText(TextFormat("Active Connections: %d", remote_players.size() + 1), 15, 27, 10, WHITE);

        // Bottom Bar UI (Jitter, Ping, Loss, Sequences)
        DrawRectangle(0, 560, 800, 40, bottomBarBg);
        DrawLine(0, 560, 800, 560, textNeon);
        DrawText(TextFormat("PING: %dms", 150), 20, 575, 10, WHITE);
        DrawText(TextFormat("JITTER: +/- %dms", 50), 120, 575, 10, WHITE);
        DrawText(TextFormat("LOSS: %.0f%%", 0.15f * 100), 250, 575, 10, RED);
        
        DrawText(TextFormat("SEQ SENT: %d", current_sequence), 450, 575, 10, GRAY);
        DrawText(TextFormat("LATEST ACK: %d", latest_ack), 570, 575, 10, textNeon);
        DrawText(TextFormat("UNACKED BUFFER: %d", history_buffer.size()), 690, 575, 10, history_buffer.size() > 5 ? RED : GREEN);

        EndDrawing();
    }
    
    CloseWindow();
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