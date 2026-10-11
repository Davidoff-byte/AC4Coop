#pragma once

#include <cstdint>
#include "..\..\HookMath.h"

// Replicated from PS3 symbols (scimitar::fsm_humanswim). Minimal chain to reach the swim locomotion
// INPUT fields — the swim equivalent of driving HumanGroundData for ground. Swim has no resident data in
// the HumanDataBundle; its state lives in a lazily-instantiated FSM scope reached from the swim context:
//
//   Human::m_HumanContextBundle.m_Swim  (HumanSwim*)
//     + 0x30  Lazy<HumanSwimFSM>   -> the FSM pointer sits at that offset (Lazy = { T* m_Object })
//   HumanSwimFSM + 0x0C  m_HumanSwimScope (HumanSwimScope*)
//   HumanSwimScope + 0x0FF0  m_DestDirection  (travel direction)
//   HumanSwimScope + 0x1000  m_DestSpeedRatio (exactly what HumanSwim::SetSpeedRatio writes)
//
// Driving m_DestSpeedRatio (+ m_DestDirection) makes the swim FSM leave its idle sub-state, play the
// moving-swim animation and travel — position is then corrected authoritatively by the caller's
// SetGlobalMatrixInternal(updatePhysics=true) capsule push. When the puppet is already IN the Swim
// context the FSM + scope are engine-instantiated, so we only read the pointers and poke the fields
// (no need to call the lazy Instantiate / CreateOrGetHumanSwimScopeFSM).
//
// !!! PC VERIFICATION !!! These are PS3 offsets. The first two hops (HumanSwim m_FSM @ 0x30,
// HumanSwimFSM m_HumanSwimScope @ 0x0C) are simple layouts and very likely identical on PC (32-bit
// pointers, same interface/base counts). The deep HumanSwimScope field offset (0x1000) sits past large
// nested types and COULD shift on PC — phase1 logs the chain pointers + read speed so a wrong offset is
// visible (speed should read ~0..1 while swimming) before the write path is trusted.

class HumanSwimScope
{
public:
	char              _pad_0000[0xA50];
	float             m_CurSpeedProportion;   // 0xA50  eased current max-speed proportion (0..1); drives the
	                                          //        swim stroke anim blend (CE-found). DEST fields alone
	                                          //        don't advance it on a teleported puppet.
	float             m_Sprint;               // 0xA54  shift-held / sprint-swim flag, float 0 or 1 (CE-found)
	char              _pad_0A58[0xDD8 - 0xA58];
	// m_AI_Swim_* action-request floats (CE-found): Wait, Turn, MoveVerySlow, MoveSlow, MoveFast,
	// CatchBreath, UnderWater, DivingBell — these select/blend which swim animation the FSM plays.
	float             m_AISwim[8];            // 0xDD8..0xDF7
	char              _pad_0DF8[0x1100 - 0xDF8];
	HookMath::Vector4 m_DestDirection;        // 0x1100
	float             m_DestSpeedRatio;       // 0x1110
};

class HumanSwimFSM
{
public:
	void*           m_Owner;             // 0x00  HumanSwim*
	uint16_t        m_TargetState;       // 0x04
	void*           m_TransitPtr;        // 0x08
	HumanSwimScope* m_HumanSwimScope;    // 0x0C
};

class HumanSwim
{
public:
	char          _pad_0000[0x34];
	HumanSwimFSM* m_FSM;                 // 0x30  Lazy<HumanSwimFSM>::m_Object
};
