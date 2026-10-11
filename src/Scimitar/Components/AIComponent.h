#pragma once

#include "..\BaseTypes.h"
#include "Component.h"

class AIComponent : public Component
{
public:
};

class CharacterBehavior : public AIComponent
{
public:
};

class BhvAssassin : public CharacterBehavior
{
public:
	void SwitchActorToDebugMode(bool forceTeleportToGround);
	void SwitchActorToGameMode();
};