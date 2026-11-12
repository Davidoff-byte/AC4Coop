#pragma once
// Boot-video hook (boot straight into the world, part A). AC4BFSP.exe statically imports _BinkOpen@8 from
// bink2w32.dll (dumpbin /imports, 2026-11-12), so every video it plays is opened through that one import slot.
//   [Coop] VideoLog=1        log every BinkOpen (file, flags, caller, ms since process start) to video_log.txt
//   [Coop] SkipIntroVideos=1 (part A step 2, not implemented yet)
// Both default 0. The import slot is found by NAME in the exe's import descriptors (not a hardcoded RVA) and
// swapped for a stub that calls the saved original: no instruction is rewritten.
namespace VideoHook {
    void Install();   // call from the poll thread start (not DllMain: the slot must already be resolved)
}
