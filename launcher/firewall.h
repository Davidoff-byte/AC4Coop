#pragma once
// Windows Firewall and the game exe (launcher, M0). A host must receive an unsolicited UDP packet; Windows blocks
// that unless the program has an inbound allow rule, and a dismissed "Allow access?" prompt leaves Block rules.
// Query is read-only and needs no elevation. Apply needs elevation: the launcher relaunches itself elevated with
// --firewall-apply "<exe>" (one UAC prompt, only when the player clicks the button).
#include <cstddef>

namespace Firewall {

enum Status { UNKNOWN = 0, FIREWALL_OFF, ALLOWED, NO_RULE, BLOCKED };

// Inbound rules whose program is `exePath` (case-insensitive): any enabled Block -> BLOCKED (a block wins over an
// allow in Windows Firewall); else an enabled Allow for UDP or any protocol -> ALLOWED; else NO_RULE.
// `detail` gets one plain line.
Status Query(const char* exePath, char* detail, size_t n);

// Disables (does not delete) enabled inbound Block rules for `exePath` and adds one inbound UDP allow rule
// "AC4Coop - Black Flag (UDP in)" for all profiles, unless it exists. Must run elevated.
bool Apply(const char* exePath, char* msg, size_t n);

// Starts this exe elevated with --firewall-apply "<exePath>" and waits for it. False if the prompt was declined.
bool RunElevated(const char* exePath, char* msg, size_t n);

}  // namespace Firewall
