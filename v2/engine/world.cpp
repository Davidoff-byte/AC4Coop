#include "world.h"

#include "scimitar/World.h"
#include "scimitar/Entity.h"
#include "scimitar/AI/AIEntityHelper.h"

namespace v2::engine {

void* CurrentWorld() { return World::GetCurrentWorld(); }

void* LocalPlayerEntity() {
    World* w = World::GetCurrentWorld();
    return w ? (void*)w->GetPlayer() : nullptr;
}

void* LocalPlayerHuman() {
    Entity* p = (Entity*)LocalPlayerEntity();
    return p ? (void*)AIEntityHelper::GetHuman(p) : nullptr;
}

bool IsLocalPlayerHuman(void* human) { return human && LocalPlayerHuman() == human; }

}  // namespace v2::engine
