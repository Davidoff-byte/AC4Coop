// Offline link harness (tools/multiguest_test.py): the real Net:: link from src/net.cpp in a console process, no game.
// Uses only the public net.h API that already existed on feat/integration-1121 (d9b463a), so the SAME source builds
// against the base tree and against feat/multi-guest: that is how the golden datagrams in
// tools/testdata/multiguest_golden.json were captured from the base build's real send path.
//
//   link_main.exe <seconds>     reads AC4Coop.ini next to the exe ([Net] Mode=host|join ...), then at 20 Hz:
//                               Net::Send(recipe PlayerState), Net::SendEntities(recipe snapshot), Net::DrainRemote.
// stdout: one "[harness] ..." summary line per second and a final "[harness] done ..." line.
// Never part of the ASI build.
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "net.h"

// Deterministic PlayerState: zeroed like Phase1::CaptureLocalState (F15 memset), header fields, then a byte pattern
// over +20..+564 (everything between worldId and hostTimeMs), hostTimeMs fixed. Tail pad +569..+575 stays 0.
static void Recipe(PlayerState& s, uint32_t seq) {
    std::memset(&s, 0, sizeof(s));
    uint8_t* b = (uint8_t*)&s;
    for (int i = 20; i < 565; ++i) b[i] = (uint8_t)(i * 31 + 7);
    s.magic = NET_MAGIC;
    s.protoVer = NET_PROTO_VER;
    s.seq = seq;
    const uint64_t world = 0x1122334455667788ull;
    std::memcpy(b + 12, &world, 8);
    s.hostTimeMs = 0x12345678u;
}

static EntityPacket g_ep;
static void EntRecipe(EntityPacket& p, uint32_t seq) {
    std::memset(&p, 0, sizeof(p));
    p.magic = NET_ENTITY_MAGIC; p.protoVer = NET_PROTO_VER; p.seq = seq; p.hostMs = 0x0BADF00Du + seq; p.count = 3;
    for (uint32_t i = 0; i < 3; ++i) {
        EntityState& e = p.ents[i];
        e.key = 10 + i; e.kind = EK_GUARD; e.stage = 1;
        e.pos[0] = 100.0f + (float)i; e.pos[1] = 200.0f; e.pos[2] = 3.0f;   // constant: the delta stream is deterministic
        e.quat[3] = 1.0f; e.actionAlias = 0x1000u + i; e.objId = 0x0000700000000000ull + i; e.hp = 100; e.arch = 7;
    }
}

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? atoi(argv[1]) : 10;
    if (!Net::Init()) { printf("[harness] Net::Init returned false\n"); fflush(stdout); return 2; }
    Net::SetLocalWorldId(0x1122334455667788ull);
    const DWORD t0 = GetTickCount();
    uint32_t seq = 0, rx = 0, lastSeq = 0, ticks = 0;
    DWORD nextLog = t0 + 1000;
    while (GetTickCount() - t0 < (DWORD)seconds * 1000u) {
        PlayerState s; Recipe(s, ++seq);
        Net::Send(s);
        EntRecipe(g_ep, seq);
        Net::SendEntities(g_ep);
        PlayerState b[8];
        const int n = Net::DrainRemote(b, 8);
        if (n > 0) { rx += (uint32_t)n; lastSeq = b[n - 1].seq; }
        ++ticks;
        if (GetTickCount() >= nextLog) {
            nextLog += 1000;
            Net::Stats st; Net::GetStats(st);
            printf("[harness] t=%u tx=%u rxPkts=%u rxPlayer=%u lastSeq=%u rejects=%u proto=%u size=%u msSinceRecv=%u\n",
                   (unsigned)(GetTickCount() - t0), st.txPackets, st.rxPackets, rx, lastSeq, st.rejects, st.protoMismatch,
                   st.sizeMismatch, Net::MillisSinceRecv());
            fflush(stdout);
        }
        Sleep(50);
    }
    Net::Stats st; Net::GetStats(st);
    printf("[harness] done ticks=%u tx=%u rxPlayer=%u rejects=%u protoMismatch=%u sizeMismatch=%u\n", ticks, st.txPackets, rx,
           st.rejects, st.protoMismatch, st.sizeMismatch);
    fflush(stdout);
    ExitProcess(0);   // the recv thread blocks in recvfrom; do not wait for it
}
