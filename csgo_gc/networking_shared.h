#pragma once

#include <steam/steam_api.h>

constexpr EP2PSend NetMessageSendFlags = k_EP2PSendReliable;
constexpr int NetMessageChannel = 7;

// Compatibility wrapper that mimics the bits of SteamNetworkingMessage_t we use.
// The caller owns this object and must call Release().
struct SteamNetworkingMessage_t
{
    CSteamID m_steamIDPeer{};
    std::vector<uint8_t> m_buffer;

    const void *GetData() const
    {
        return m_buffer.data();
    }

    uint32_t GetSize() const
    {
        return static_cast<uint32_t>(m_buffer.size());
    }

    void Release()
    {
        delete this;
    }
};

// NOTE: these are used as gc message types!
// if they overlap with the game's gc messages, we're doomed
enum ENetworkMsg : uint32_t
{
    // sent by the server to client when they connect, data is the auth ticket
    k_EMsgNetworkConnect = (1u << 31) - 1,
};
