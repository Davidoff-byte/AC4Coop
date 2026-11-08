#pragma once
#include "netstate.h"

// Host-IP peer-to-peer UDP link. Both sides send their local PlayerState and receive the peer's.
// Config from AC4Coop.ini [Net]: Enabled, LocalPort, PeerIP, PeerPort.
namespace Net {
    bool Init();                        // read ini, open socket, start recv thread; returns Enabled()
    bool Enabled();
    bool IsSecondInstance();            // true if LocalPort was taken (this is the same-PC 2nd instance)
    void Send(const PlayerState& s);    // sendto peer (no-op if disabled)
    bool GetRemote(PlayerState& out);   // copy latest received state; false if none yet
    int  DrainRemote(PlayerState* out, int max); // pop up to max queued player packets, arrival order

    // Entity sync (host-authoritative "synced worlds"): a separate packet type on the same socket.
    void SendEntities(const EntityPacket& p);   // sendto peer (no-op if disabled)
    bool GetEntities(EntityPacket& out);        // copy latest received entity list; false if none yet,
                                                // or if the last one is older than 3 s (dead peer -> expired)
    // Drain the received death entries (kind=3) that were queued by the recv thread. Returns the number
    // of entries copied into `out` (0 = none). Deaths are queued separately because the single
    // "latest entity list" slot is last-writer-wins and a one-entry death packet would otherwise be
    // overwritten by the next snapshot before the frame could read it.
    uint32_t GetKillEntities(EntityPacket& out, uint32_t max);

    // Session-lifecycle hardening (Feature #3, ini [Coop] NetStrictPeer):
    void     SetStrictPeer(bool strict); // accept datagrams only from the configured PeerIP (default true)
    // X5: the local world id (World::GetObjectID()), set by phase1.cpp each frame. The entity encoder
    // stamps it into the wire header; the decoder drops snapshots whose world differs from the local one.
    void     SetLocalWorldId(uint64_t id);
    uint64_t LastRecvMs();               // GetTickCount64() of the last ACCEPTED packet (0 = none yet)
    uint32_t MillisSinceRecv();          // ms since the last accepted packet; 0xFFFFFFFF if none yet

    // Counters for the [Net] stats line. txBytes counts what sendto() was given (the full packet
    // struct), so the number is the real wire cost of this protocol, not an estimate.
    // rejects = datagrams from the peer that were dropped without applying state; protoMismatch is
    // the subset dropped because protoVer != NET_PROTO_VER. sizeMismatch is the player lane's
    // anonymous case: the leading magic was ours but the datagram length was not
    // sizeof(PlayerState), i.e. the peer runs a build with a different struct. That path is checked
    // BEFORE the version gate, so it can never be counted as a protoMismatch (see net.cpp).
    // All of them are 0 on a well-formed link.
    struct Stats { uint32_t txPackets, txBytes, txEnts, rxPackets, rxBytes, rejects, protoMismatch, sizeMismatch; };
    void GetStats(Stats& out);
    void ResetStats();
}
