#pragma once
#include <ctsdint>

#pragma pack(push, 1)

enum PacketType : uint8_t{
	CLIENT_INPUT = 1;
	SERVER_STATE = 2;
};

struct GamePacket{
	PacketType type;
	uint32_t sequence_number;
	float x;
	float y;
};