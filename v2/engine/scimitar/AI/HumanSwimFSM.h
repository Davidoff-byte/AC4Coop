#pragma once

#include <cstdint>
#include "..\..\HookMath.h"

// Swim locomotion input path.
//
// The swim FSM has no resident data in the human data bundle: it is lazily instantiated and reached from
// the human's swim context, so the chain we drive is
//
//   Human::m_HumanContextBundle.m_Swim   (HumanSwim*)
//     +0x30  Lazy<HumanSwimFSM>          (a Lazy is just { T* m_Object })
//   HumanSwimFSM +0x0C  m_HumanSwimScope (HumanSwimScope*)
//   HumanSwimScope +0x1100  m_DestDirection
//   HumanSwimScope +0x1110  m_DestSpeedRatio   (the field HumanSwim::SetSpeedRatio writes)
//
// Writing the speed ratio (and direction) is what makes the FSM leave its idle sub-state, play the
// moving-swim animation and travel; the authoritative position still comes from the caller's
// SetGlobalMatrixInternal(updatePhysics=true) capsule push. When the puppet is already in the swim context
// the engine has instantiated both objects, so we only read the two pointers and poke the fields (no
// Instantiate / CreateOrGetHumanSwimScopeFSM call).
//
// Provenance of the offsets: 0x30 and 0x0C are interface/base layout; the deep scope fields were located
// with Cheat Engine on PC (m_CurSpeedProportion, m_Sprint, m_AISwim). phase1 logs the chain and the speed
// it reads, so a wrong hop shows up as a speed outside 0..1 while swimming before any write is trusted.

class HumanSwimScope
{
public:
	char              _pad_0000[0xA50];
	float             m_CurSpeedProportion;   // 0xA50  eased current max-speed proportion (0..1); drives the
	                                          //        stroke blend. Dest fields alone do not advance it on
	                                          //        a teleported puppet.
	float             m_Sprint;               // 0xA54  sprint-swim flag (float 0 or 1)
	char              _pad_0A58[0xDD8 - 0xA58];
	// m_AI_Swim_* action-request floats: Wait, Turn, MoveVerySlow, MoveSlow, MoveFast, CatchBreath,
	// UnderWater, DivingBell. These select/blend which swim animation the FSM plays.
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
