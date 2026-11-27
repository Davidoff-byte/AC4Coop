// One-off: prints offsetof(PlayerState, field) and sizeof(PlayerState) so peer9.py's wire packer
// never hand-counts the layout. Produced tools/oneoff/playerstate_offsets_proto16.txt.
// Build (repo root, x86 toolset matching the plugin):
//   call vcvarsall.bat x86 && cl /nologo /std:c++17 /EHsc /I src tools\oneoff\playerstate_offsets.cpp
#include <cstdio>
#include <cstddef>
#include "netstate.h"

#define O(f) std::printf("%-38s %4zu\n", #f, offsetof(PlayerState, f))
#define S(t) std::printf("sizeof(%-31s %4zu  alignof %2zu\n", #t ")", sizeof(t), alignof(t))

int main()
{
    S(HookMath::Transform);
    S(HookMath::Vector4);
    S(SwimBlock);
    S(ClimbBlock);
    S(ContextBlock);
    std::printf("sizeof(PlayerState)                     %4zu  alignof %2zu\n",
                sizeof(PlayerState), alignof(PlayerState));
    std::printf("NET_PROTO_VER=%u NET_MAGIC=0x%08X BLEND_MAX=%d\n",
                (unsigned)NET_PROTO_VER, (unsigned)NET_MAGIC, BLEND_MAX);
    std::printf("----\n");

    O(magic);
    O(protoVer);
    O(seq);
    O(worldId);
    O(sceneKey);
    O(sceneCellId);
    O(matrix);
    O(curSpeed);
    O(destSpeed);
    O(heading);
    O(isSet);
    O(sprint);
    O(breakHardOnEdge);
    O(breakHard);
    O(curMvt);
    O(destMvt);
    O(contextId);
    O(subState);
    O(humanGroundStage);
    O(weaponTotallyOut);
    O(isInTree);
    O(isOnShip);
    O(isOnOcean);
    O(doJumpToVoid);
    O(stalkingZoneCount);
    O(stalkingZoneBlendedCount);
    O(stalkingClipCount);
    O(stalkingClipBlendedCount);
    O(stalkingClipAllowFastStalkingCount);
    O(disturbedZoneCount);
    O(actionAlias);
    O(actionChannel);
    O(actionLayer);
    O(blendCount);
    O(blendFactors);
    O(actionRatio);
    O(blendInLen);
    O(blendInStartFromEnd);
    O(blendInTargetStart);
    O(blendInFlags);
    O(isTransitionAction);
    O(timeScale);
    O(ctx);
    O(shipValid);
    O(shipPos);
    O(shipQuat);
    O(shipHP);
    O(shipMaxHP);
    O(shipClass);
    O(valid);
    O(hostTimeMs);
    return 0;
}
