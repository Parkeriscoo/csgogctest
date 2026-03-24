#include "stdafx.h"
#include "networking_server.h"
#include "gc_message.h"

NetworkingServer::NetworkingServer(ISteamNetworking *networkingMessages)
    : m_networkingMessages{ networkingMessages }
    , m_sessionRequest{ this, &NetworkingServer::OnSessionRequest }
    , m_sessionFailed{ this, &NetworkingServer::OnSessionFailed }
{
}

bool NetworkingServer::ReceiveMessage(SteamNetworkingMessage_t *&message)
{
    uint32_t size;
    if (!m_networkingMessages->IsP2PPacketAvailable(&size, NetMessageChannel))
    {
        return false;
    }

    message = new SteamNetworkingMessage_t{};
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
        return false;
    }

    if (readSize != message->m_buffer.size())
    {
        message->m_buffer.resize(readSize);
    }

    uint64_t steamId = message->m_steamIDPeer.ConvertToUint64();

    // see if we have a session
    if (!m_clients.Has(steamId))
    {
        Platform::Print("NetworkingServer: ignored message from %llu (no session)\n", steamId);
        message->Release();
        return false;
    }

    return true;
}

// helper for SteamNetworkingMessages::SendMessageToUser that attempts to do some kind of error handling
static void SendMessageToUser(ISteamNetworking *networkingMessages, uint64_t steamId, const void *data, uint32_t size)
{
    CSteamID identity{ steamId };

    bool result = networkingMessages->SendP2PPacket(
        identity,
        data,
        size,
        NetMessageSendFlags,
        NetMessageChannel);

    if (!result)
    {
        Platform::Print("SendMessageToUser failed for %llu, closing session and trying again\n", steamId);

        networkingMessages->CloseP2PChannelWithUser(identity, NetMessageChannel);

        result = networkingMessages->SendP2PPacket(
            identity,
            data,
            size,
            NetMessageSendFlags,
            NetMessageChannel);

        if (!result)
        {
            // not much we can do in this situation i guess
            Platform::Print("SendMessageToUser failed for %llu\n", steamId);
        }
    }
}

void NetworkingServer::ClientConnected(uint64_t steamId, const void *ticket, uint32_t ticketSize)
{
    if (!m_clients.Add(steamId))
    {
        Platform::Print("got ClientConnected for %llu but they're already on the list! ignoring\n", steamId);
        return;
    }

    // send a message, if the client has csgo_gc installed they will
    // reply with their so cache and we'll add them to our list
    GCMessageWrite messageWrite{ k_EMsgNetworkConnect };
    messageWrite.WriteUint32(ticketSize);
    messageWrite.WriteData(ticket, ticketSize);

    // FIXME: this gets sent when the client is connecting to the server, it's not uncommon for
    // the connection to time out, in which case the player's socache never gets to the server
    SendMessageToUser(m_networkingMessages, steamId, messageWrite.Data(), messageWrite.Size());
}

void NetworkingServer::ClientDisconnected(uint64_t steamId)
{
    if (!m_clients.Remove(steamId))
    {
        Platform::Print("got ClientDisconnected for %llu but they're not on the list! ignoring\n", steamId);
        return;
    }

    m_networkingMessages->CloseP2PChannelWithUser(CSteamID{ steamId }, NetMessageChannel);
}

void NetworkingServer::SendMessage(uint64_t steamId, const void *data, uint32_t size)
{
    if (!m_clients.Has(steamId))
    {
        Platform::Print("No csgo_gc session with %llu, not sending message!!!\n");
        return;
    }

    SendMessageToUser(m_networkingMessages, steamId, data, size);
}

void NetworkingServer::OnSessionRequest(P2PSessionRequest_t *param)
{
    uint64_t steamId = param->m_steamIDRemote.ConvertToUint64();

    if (!m_clients.Has(steamId))
    {
        Platform::Print("%llu sent a session request, we don't have a csgo_gc session, ignoring...\n");
        return;
    }

    Platform::Print("%llu sent a session request, we were playing GC with them so accept\n");

    if (!m_networkingMessages->AcceptP2PSessionWithUser(param->m_steamIDRemote))
    {
        Platform::Print("AcceptP2PSessionWithUser with %llu failed???\n", steamId);
    }
}

void NetworkingServer::OnSessionFailed(P2PSessionConnectFail_t *param)
{
    // don't do anything, rely on the auth session
    Platform::Print("OnSessionFailed: %llu, error %u\n",
        param->m_steamIDRemote.ConvertToUint64(),
        static_cast<uint32_t>(param->m_eP2PSessionError));
}
