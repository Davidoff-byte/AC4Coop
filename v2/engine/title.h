#pragma once
// Engine layer: the title menu (v1 auto_continue.cpp, live-proven). Receipts: decomp/units.json 00F91B20 (type check),
// 014ACD10 (press start), 018DAC60 (slot chosen). Called from the engine's own UI update/callback threads, where the engine
// makes the same calls itself (ARCHITECTURE.md R1, documented exception).
#include <cstdint>

namespace v2::engine::title {

// sub_17B4840's component: when its state is 2/3 and the entity's +0xD8 is 2, issue "press start" once per 700 ms (max 3).
void TryPressStart(void* component);
// After sub_18E2390 ran: choose the save once per process (never a new game). wantSlot -1 = the highlighted/first save.
void TryContinue(void* menu, uint32_t highlighted, int wantSlot);

}  // namespace v2::engine::title
