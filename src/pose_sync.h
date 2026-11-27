#pragma once
// [Coop] PoseSync (default 0) / PoseSyncApply (default 0): F3 pose parity. The local player's LOCAL pose
// (SkeletonComponent+0x134) is captured at the compose call 0x0045349C, sent on the proto-17 pose lane
// ('AC4P'), resolved by bone name on the puppet's rig, measured against the puppet's own pose, and (Apply=1)
// written into the puppet's LOCAL pose at the same instruction, before the engine composes it.
// See docs/design/pose-sync-impl.md.
class Entity;
namespace PoseSync
{
    void Init();                                   // dllmain: reads the flags, installs the call redirect when PoseSync=1
    void Tick(Entity* player, Entity* puppet);     // main thread, once per local-player update
    void ForgetPuppet();                           // the puppet is being removed: stop applying to it now
}
