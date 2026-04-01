#pragma once
#include <cstdint>

#pragma pack(push, 1)

enum PacketType : uint8_t {
    CLIENT_INPUT = 1,
    SERVER_STATE = 2
};

//struct to hold other players' data
struct OtherPlayer {
    uint32_t id;
    float x;
    float y;
};

struct GamePacket {
    PacketType type;
    uint32_t sequence_number;
    float x;
    float y;
    
    // Deliverable 2 :Multi-client broadcasting payload
    uint8_t num_other_players;
    OtherPlayer other_players[8]; //limited to max 8 players currently
};

#pragma pack(pop)