#pragma once

#include "Component.h"

// Animation/action driver of a human. Everything below acts on the per-(channel, layer) controller,
// reached through 0x666BB0 (returns 0 when there is no controller for that pair).
class ActionComponent : Component
{
public:
	Entity* Owner;
	int* ActionKitHandle;

public:
	inline static ClassDescriptor* GetStaticClassDescriptor()
	{
		return Hook::CallCdeclMethod<ClassDescriptor*>(0x00648BA0);
	}

	int __fastcall SetTargetAction(int actionAlias, int a3_seems_always_0, int ActionTransition, int a5_seems_always_0xFFFF, int ActionTransitionLayer);
	void SetActionKit(int* handle);

	// Currently playing action on (channel, layer); the file-defined alias sits at action+0x08. We use it to
	// learn which clip the peer is playing so the puppet can be told to play the same one.
	void* GetCurrentAction(unsigned char* actionIsMirrored, int channelID, int layer);

	// Per-slot blend factor (the walk/jog/run blend inside one locomotion action). index selects the slot,
	// value is clamped to [0,1]. GetBlendFactor is 0x667590: it resolves the controller and reads the slot
	// from it, returning 0.0 when there is none.
	void SetBlendFactor(unsigned char index, float value, int channelID, int layer);
	float GetBlendFactor(unsigned char index, int channelID, int layer);

	// Slot count of the controller's blend tree: u16 at controller+0x32 with the data at +0x2C. It is sized
	// per action (the ground move cycle uses 46 slots), and writes past it are dropped silently.
	int GetBlendFactorCount(int channelID, int layer);

	// ACTPersistType of a slot. Transient is the default and means the next action update resets the slot
	// before the clip is sampled; ACTPERSIST_ALWAYS (=4) survives action/transition resets. That is what we
	// need on a puppet whose own statechart is disabled, and it is what the engine's locomotion does too:
	// set persistence once, then write the factor every frame. Engine 0x652D60.
	void SetBlendFactorPersistence(unsigned char slotIndex, int persistType, int channelID, int layer);

	// Playback time scale of the channel's clip. Speed-warps the loop so the feet match the ground speed;
	// a puppet with the right action and blend factors still looks slow if this is wrong. Get returns 1.0
	// (not 0, which would freeze the clip) when there is no controller. Set writes controller+0 and
	// propagates to every clip evaluator (+0x40); onlyIfChanged skips writes within 5e-4.
	// Get 0x667540, Set 0x6528B0, persistence 0x652A40 (ACTPERSIST_ALWAYS to survive resets).
	float GetTimeScale(int channelID, int layer);
	void  SetTimeScale(float value, int channelID, int layer, bool onlyIfChanged);
	void  SetTimeScalePersistence(int persistType, int channelID, int layer);

	// Current ActionEvaluator of (channel, layer) at controller+0x28, or null. Layout of the evaluator:
	//   +0x0D4 u8  m_EvaluatorType (4 = ActionEvaluator)
	//   +0x0E0     m_BlendInEvaluator: +0x0E0 m_Time, +0x0E4 m_Length (runtime copies of
	//              m_ActionBlend.m_Length, made in Start), +0x1C8 m_ActionBlend.m_Length
	//   +0x258 u8  m_ContextData.m_IsTransitionAction
	//   +0x2A8     m_Action
	void* GetCurrentEvaluator(int channelID, int layer);

	// Normalized position [0,1] of the current clip. Set seeks every evaluator on the channel's controllers,
	// and the game calls it right after SetTargetAction to start the matching clip at the peer's phase.
	// Get 0x667170, Set 0x652640.
	float GetCurrentActionRatio(int channelID, int layer);
	void  SetCurrentActionRatio(float ratio, int channelID, bool processDisplacement, bool processEvents, int layer);

	// Swaps in a character action set: takes a Reference<CharacterActionSet>* (a pointer to the handle slot,
	// e.g. the peer component's +0x18), ref-counts it, stores it in this component's CAS slot (this+0x18)
	// and adds its dynamic action blocks. Handing the puppet the local player's set gives its actions the
	// right character clips instead of the generic base.
	void ActivateCharacterActionSet(void* characterActionSetRef);

	inline int SetEnabled(bool enabled)
	{
		return Component::SetEnabled(enabled);
	}
};
