#include "modules.h"
#include "core/config.h"
#include "core/log.h"
#include "core/tick.h"
#ifdef AC4COOP_DEVTOOLS
#include "dev/pilot.h"
#include <windows.h>
#include <process.h>
#endif

namespace v2::modules {
namespace {
#ifdef AC4COOP_DEVTOOLS
// DevTools: the pilot bridge (tools/pilot.py). Its 20 Hz poll retries the import patch / window subclass / Present patch.
void PilotTick(void* human, uint64_t) { Pilot::OnPlayerFrame(human); }
unsigned __stdcall PilotPoll(void*) { for (;;) { Pilot::Poll(); Sleep(50); } }
#endif
}  // namespace

void Init() {
    // Phase 0: no gameplay modules yet. Tick order bands: 100 net in, 200 sync apply, 300 capture, 900 dev.
#ifdef AC4COOP_DEVTOOLS
    if (Cfg().pilot) {
        Pilot::Init();
        tick::Register("pilot", 900, &PilotTick);
        _beginthreadex(nullptr, 0, PilotPoll, nullptr, 0, nullptr);
    }
#endif
}

}  // namespace v2::modules
