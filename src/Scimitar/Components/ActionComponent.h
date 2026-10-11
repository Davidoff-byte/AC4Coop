#pragma once

#include "Component.h"

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

	// Returns the currently-playing scimitar::Action* on (channelID, layer); its file-defined alias is at
	// Action+0x08. Used to capture which clip the peer is playing so the puppet can be told to play it.
	void* GetCurrentAction(unsigned char* actionIsMirrored, int channelID, int layer);

	// Per-channel animation blend factor (e.g. the walk/jog/run speed blend within one locomotion action).
	// index = blend slot, value clamped [0,1]. PC GetBlendFactor = 0x667590 (returns float via st0; resolves
	// the channel controller for (channel,layer) then reads slot `index`, 0.0 if no controller).
	void SetBlendFactor(unsigned char index, float value, int channelID, int layer);
	float GetBlendFactor(unsigned char index, int channelID, int layer);

	// Number of blend-factor slots on the (channel,layer) controller = ActionController::m_BlendFactors size
	// (u16 @ controller+0x32, data @ +0x2C). Sized per action: the ground locomotion move cycle uses a 46-slot
	// blend tree (HumanGroundScope::UpdateGroundBlendTree -> SimpleAnimBlender<46> -> SetTargetBlendFactors),
	// so 4 slots is nowhere near enough. Writes past this count are silently dropped by the engine.
	// Controller lookup = PC 0x666BB0 (GetFirstChannelControllerUpdateSafe); 0 if no controller.
	int GetBlendFactorCount(int channelID, int layer);

	// Per-slot blend-factor persistence (ACTPersistType). The DEFAULT is transient — a blend factor set via
	// SetBlendFactor is reset by the next action update before the clip is sampled. ACTPERSIST_ALWAYS (=4)
	// makes the slot survive action/transition resets, so our externally-driven blend actually sticks on the
	// puppet (whose own statechart, which would normally re-set it every frame, is disabled). The engine's own
	// locomotion does the same: SetBlendFactorPersistence(ALWAYS) once, then SetBlendFactors each frame.
	// PC 0x652D60, thiscall(this, u8 slotIndex, ACTPersistType, u32 channelID, ACTLayer).
	void SetBlendFactorPersistence(unsigned char slotIndex, int persistType, int channelID, int layer);

	// Playback TIME SCALE (clip speed multiplier) on a channel controller. Locomotion speed-warps its loop
	// clip through this so foot-speed matches ground speed — so a teleported puppet that has the RIGHT action
	// and the RIGHT blend factors STILL plays slow if its time scale isn't synced (confirmed via read-back:
	// action+blend matched the peer yet the gait was slow). Get returns 1.0 when no controller (never 0, which
	// would freeze the anim). SetTimeScale writes controller+0 and propagates to every clip evaluator (+0x40);
	// onlyIfChanged skips the write if within 5e-4. PC GetTimeScale 0x667540, SetTimeScale 0x6528B0,
	// SetTimeScalePersistence 0x652A40 (persistType ACTPERSIST_ALWAYS=4 to keep it across resets).
	float GetTimeScale(int channelID, int layer);
	void  SetTimeScale(float value, int channelID, int layer, bool onlyIfChanged);
	void  SetTimeScalePersistence(int persistType, int channelID, int layer);

	// Current ActionEvaluator on the (channel,layer) controller (ActionController+0x28, m_CurrentActionEvaluatorCache)
	// or null. Layout (PS3-symbol names, PC-verified via GetCurrentAction/sub_495700):
	//   +0x0D4 u8  m_EvaluatorType (4 = ActionEvaluator)
	//   +0x0E0     m_BlendInEvaluator (ActionBlendEvaluator): +0x0E0 m_Time, +0x0E4 m_Length (runtime, copied from
	//              m_ActionBlend.m_Length in ActionBlendEvaluator::Start), +0x1C8 m_ActionBlend.m_Length
	//   +0x258 u8  m_ContextData.m_IsTransitionAction
	//   +0x2A8     m_Action
	void* GetCurrentEvaluator(int channelID, int layer);

	// Normalized playback position [0,1] of the current action on (channel,layer). GetCurrentActionRatio PC
	// 0x667170 (0.0 if no controller). SetCurrentActionRatio PC 0x652640 seeks every evaluator on the channel's
	// controllers; the game calls it right after SetTargetAction to start the new clip at a matched phase
	// (e.g. Exec_Move_To_JumpImpulsion: SetCurrentActionRatio(r, 0, processDisplacement=0, processEvents=1, ly)).
	float GetCurrentActionRatio(int channelID, int layer);
	void  SetCurrentActionRatio(float ratio, int channelID, bool processDisplacement, bool processEvents, int layer);

	// Sets/activates the character action-set (per-character clip overrides on top of the generic kit).
	// Takes a Reference<CharacterActionSet>* (points to the handle slot, e.g. &otherActionComponent+0x18 on
	// PC): it ref-counts the handle, swaps it into this component's CAS slot (this+0x18), and adds its dynamic
	// action blocks. We hand the puppet the local player's CAS (same character, already active) so its actions
	// resolve to the right character's clips instead of the generic base.
	void ActivateCharacterActionSet(void* characterActionSetRef);

	inline int SetEnabled(bool enabled)
	{
		return Component::SetEnabled(enabled);
	}
};
