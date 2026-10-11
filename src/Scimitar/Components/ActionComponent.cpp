#include "ActionComponent.h"

int __fastcall ActionComponent::SetTargetAction(int actionAlias, int a3_seems_always_0, int ActionTransition, int a5_seems_always_0xFFFF, int ActionTransitionLayer)
{
	return Hook::CallClassMethod<int, ActionComponent*, int, int, int, int, int>(0x006625D0, this, actionAlias, a3_seems_always_0, ActionTransition, a5_seems_always_0xFFFF, ActionTransitionLayer);
}

void ActionComponent::SetActionKit(int* handle)
{
	Hook::CallClassMethod<void, ActionComponent*, int*>(0x00671280, this, handle);
}

void* ActionComponent::GetCurrentAction(unsigned char* actionIsMirrored, int channelID, int layer)
{
	return Hook::CallClassMethod<void*, ActionComponent*, unsigned char*, int, int>(0x00666C80, this, actionIsMirrored, channelID, layer);
}

void ActionComponent::ActivateCharacterActionSet(void* characterActionSetRef)
{
	Hook::CallClassMethod<void, ActionComponent*, void*>(0x00651840, this, characterActionSetRef);
}

void ActionComponent::SetBlendFactor(unsigned char index, float value, int channelID, int layer)
{
	Hook::CallClassMethod<void, ActionComponent*, unsigned char, float, int, int>(0x00652B10, this, index, value, channelID, layer);
}

float ActionComponent::GetBlendFactor(unsigned char index, int channelID, int layer)
{
	return Hook::CallClassMethod<float, ActionComponent*, unsigned char, int, int>(0x00667590, this, index, channelID, layer);
}

int ActionComponent::GetBlendFactorCount(int channelID, int layer)
{
	char* controller = Hook::CallClassMethod<char*, ActionComponent*, int, int>(0x00666BB0, this, channelID, layer);
	return controller ? *(unsigned short*)(controller + 0x32) : 0;
}

void* ActionComponent::GetCurrentEvaluator(int channelID, int layer)
{
	char* controller = Hook::CallClassMethod<char*, ActionComponent*, int, int>(0x00666BB0, this, channelID, layer);
	return controller ? *(void**)(controller + 0x28) : nullptr;
}

float ActionComponent::GetCurrentActionRatio(int channelID, int layer)
{
	return Hook::CallClassMethod<float, ActionComponent*, int, int>(0x00667170, this, channelID, layer);
}

void ActionComponent::SetCurrentActionRatio(float ratio, int channelID, bool processDisplacement, bool processEvents, int layer)
{
	Hook::CallClassMethod<void, ActionComponent*, float, int, bool, bool, int>(0x00652640, this, ratio, channelID, processDisplacement, processEvents, layer);
}

void ActionComponent::SetBlendFactorPersistence(unsigned char slotIndex, int persistType, int channelID, int layer)
{
	Hook::CallClassMethod<void, ActionComponent*, unsigned char, int, int, int>(0x00652D60, this, slotIndex, persistType, channelID, layer);
}

float ActionComponent::GetTimeScale(int channelID, int layer)
{
	return Hook::CallClassMethod<float, ActionComponent*, int, int>(0x00667540, this, channelID, layer);
}

void ActionComponent::SetTimeScale(float value, int channelID, int layer, bool onlyIfChanged)
{
	Hook::CallClassMethod<void, ActionComponent*, float, int, int, bool>(0x006528B0, this, value, channelID, layer, onlyIfChanged);
}

void ActionComponent::SetTimeScalePersistence(int persistType, int channelID, int layer)
{
	Hook::CallClassMethod<void, ActionComponent*, int, int, int>(0x00652A40, this, persistType, channelID, layer);
}