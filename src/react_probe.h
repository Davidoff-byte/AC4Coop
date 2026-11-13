#pragma once
// Detection-reaction probe (host): where CRLDetectParams CanReact (0x1076110) stops for the co-op puppet, and the
// R5 seam. See react_probe.cpp. [Coop] ReactProbe (default 0, read at load) installs both hooks;
// [Coop] AlertQueryAsPlayer (live, default 0) makes the R5 alert-state query use the local player's handle when
// the stimulus target is the puppet.

namespace ReactProbe
{
    void Init();
    int  FormatReport(char* out, int cap);   // one [React] line per window (counters reset); 0 when not installed
}
