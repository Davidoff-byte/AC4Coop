#pragma once

// [Coop] FortProbe (default 0, read-only): which forts and towers exist on this machine and their state, so fort
// sync can name them. Nothing is written. See fort_probe.cpp for the field map and the enumeration sources.
void FortProbeTick();
