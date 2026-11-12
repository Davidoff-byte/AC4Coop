#pragma once
// [Coop] AutoContinue=1 (default 0): boot straight into the world. Accepts the title screen's "press start" and
// picks the save slot the way the game's own code does (see auto_continue.cpp for the evidence). Never starts a new
// game: an empty slot is not selected. [Coop] AutoContinueSlot=-1 (default: the highlighted slot, else the first slot
// that has a save) or 0..2. Acts once per process. Log: auto_continue_log.txt.
namespace AutoContinue {
    void Init();   // DllMain attach, after the build gate
    bool Enabled();
}
