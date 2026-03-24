#include "stdafx.h"
#include "networking_client.h"
#include "gc_client.h"

NetworkingClient::NetworkingClient(ISteamNetworking *networkingMessages)
    : m_networkingMessages{ networkingMessages }
    , m_sessionRequest{ this, &NetworkingClient::OnSessionRequest }
    , m_sessionFailed{ this, &NetworkingClient::OnSessionFailed }
{
}

void NetworkingClient::Update(ClientGC *gc)
{
    uint32_t size;
    while (m_networkingMessages->IsP2PPacketAvailable(&size, NetMessageChannel))
    {
        auto *message = new SteamNetworkingMessage_t{};
        message->m_buffer.resize(size);

        uint32_t readSize = 0;
        if (!m_networkingMessages->ReadP2PPacket(
                message->m_buffer.data(),
                static_cast<uint32_t>(message->m_buffer.size()),
                &readSize,
                &message->m_steamIDPeer,
                NetMessageChannel))
        {
            message->Release();
            continue;
        }

        if (readSize != message->m_buffer.size())
        {
            message->m_buffer.resize(readSize);
        }

        uint64_t steamId = message->m_steamIDPeer.ConvertToUint64();

        // pass 0 as type so it gets parsed from the message
        GCMessageRead messageRead{ 0, message->GetData(), message->GetSize() };
        if (!messageRead.IsValid())
        {
            assert(false);
            message->Release();
            continue;
        }

        if (HandleMessage(gc, steamId, messageRead))
        {
            // that was an internal message
            message->Release();
            continue;
        }

        // don't pass messages to the gc unless it's our gameserver
        if (!m_serverSteamId || steamId != m_serverSteamId)
        {
            Platform::Print("NetworkingClient: ignored message from %llu (not our gs %llu)\n", steamId, m_serverSteamId);
            message->Release();
            continue;
        }

        // let the gc have a whack at it
        gc->PostToGC(GCEvent::NetMessage, 0, message->GetData(), message->GetSize());

        message->Release();
    }
}

static bool ValidateTicket(std::unordered_map<uint32_t, AuthTicket> &tickets, uint64_t steamId, const void *data, uint32_t size)
{
    for (auto &pair : tickets)
    {
        if (pair.second.buffer.size() == size && !memcmp(pair.second.buffer.data(), data, size))
        {
            pair.second.steamId = steamId;
            return true;
        }
    }

    return false;
}

bool NetworkingClient::HandleMessage(ClientGC *gc, uint64_t steamId, GCMessageRead &message)
{
    if (message.IsProtobuf())
    {
        // internal messages are not protobuf based
        return false;
    }

    uint32_t typeUnmasked = message.TypeUnmasked();
    if (typeUnmasked == k_EMsgNetworkConnect)
    {
        uint32_t ticketSize = message.ReadUint32();
        const void *ticket = message.ReadData(ticketSize);
        if (!message.IsValid())
        {
            Platform::Print("NetworkingClient: ignored connection from %llu (malfored message)\n", steamId);
            return true;
        }

        if (!ValidateTicket(m_tickets, steamId, ticket, ticketSize))
        {
            Platform::Print("NetworkingClient: ignored connection from %llu (ticket mismatch)\n", steamId);
            return true;
        }

        Platform::Print("NetworkingClient: sending socache to %llu\n", steamId);
        m_serverSteamId = steamId;
        gc->PostToGC(GCEvent::SOCacheRequest, 0, nullptr, 0);

        return true;
    }

    return false;
}

void NetworkingClient::SendMessage(const void *data, uint32_t size)
{
    if (!m_serverSteamId)
    {
        // not connected to a server
        return;
    }

    [[maybe_unused]] bool result = m_networkingMessages->SendP2PPacket(
        CSteamID{ m_serverSteamId },
        data,
        size,
        NetMessageSendFlags,
        NetMessageChannel);

    assert(result);
}

void NetworkingClient::SetAuthTicket(uint32_t handle, const void *data, uint32_t size)
{
    AuthTicket &ticket = m_tickets[handle];
    ticket.steamId = 0;
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(data);
    ticket.buffer.assign(bytes, bytes + size);
}

void NetworkingClient::ClearAuthTicket(uint32_t handle)
{
    auto it = m_tickets.find(handle);
    if (it == m_tickets.end())
    {
        assert(false);
        Platform::Print("NetworkingClient: tried to clear a nonexistent auth ticket???\n");
        return;
    }

    if (it->second.steamId)
    {
        Platform::Print("NetworkingClient: closing p2p session with %llu\n", it->second.steamId);

        // we had a session so close the connection
        m_networkingMessages->CloseP2PChannelWithUser(CSteamID{ it->second.steamId }, NetMessageChannel);

        // was this our current gameserver? if it was, clear it
        if (it->second.steamId == m_serverSteamId)
        {
            Platform::Print("NetworkingClient: clearing gs identity\n");
            m_serverSteamId = 0;
        }
    }

    m_tickets.erase(it);
}

void NetworkingClient::OnSessionRequest(P2PSessionRequest_t *param)
{
    if (!param->m_steamIDRemote.BGameServerAccount())
    {
        // csgo_gc related connections come from gameservers
        return;
    }

    // accept the connection, we should receive the k_EMsgNetworkConnect message
    m_networkingMessages->AcceptP2PSessionWithUser(param->m_steamIDRemote);
}

void NetworkingClient::OnSessionFailed(P2PSessionConnectFail_t *param)
{
    Platform::Print("NetworkingClient::OnSessionFailed: %llu, error %u\n",
        param->m_steamIDRemote.ConvertToUint64(),
        static_cast<uint32_t>(param->m_eP2PSessionError));
}
