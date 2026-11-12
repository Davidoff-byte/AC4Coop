#pragma once
// UPnP-IGD client for lobby host mode (docs/design/lobby-code.md, slice 4). Blocking calls: run them on a
// background thread, never the game thread. No engine calls; Winsock + WinHTTP only.
#include <cstdint>
#include <cstddef>

namespace Upnp {

struct Gateway {
    char     location[256];    // device description URL from the SSDP answer
    char     controlUrl[256];  // absolute control URL of the WAN*Connection service
    char     service[96];      // urn:schemas-upnp-org:service:WANIPConnection:1 (or :2, or WANPPPConnection:1)
    uint32_t lanIp;            // the local address the search was sent from (host order)
};

// SSDP M-SEARCH from `lanIpHost` (pinned with IP_MULTICAST_IF: measured 2026-11-12, an unpinned search
// left through a VPN adapter and no gateway answered), then fetch the description and pick the first
// WANIPConnection/WANPPPConnection service. ~3 s worst case.
bool Discover(uint32_t lanIpHost, Gateway* gw, char* err, size_t errN);

bool GetExternalIp(const Gateway& gw, uint32_t* ipHost, char* err, size_t errN);

// AddPortMapping UDP extPort -> gw.lanIp:intPort. Tries `leaseSec` first; on UPnP error 725
// (OnlyPermanentLeasesSupported) retries with 0. *upnpErr = the gateway's errorCode (0 on success).
// *leaseUsed = the lease that was accepted.
bool AddUdpMapping(const Gateway& gw, uint16_t extPort, uint16_t intPort, uint32_t leaseSec, const char* desc,
                   int* upnpErr, uint32_t* leaseUsed, char* err, size_t errN);

// AddUdpMapping for intPort, trying external ports intPort, intPort+1 .. intPort+9 while the gateway answers
// 718 ConflictInMappingEntry. Measured 2026-11-12 on the owner's router: UDP 47801 -> 718 although no UPnP
// entry lists it (GetGenericPortMappingEntry 0..21), while 47811 mapped and deleted cleanly. The lobby code
// carries the external port, so it need not equal the local one. *extPortUsed = the port that worked.
bool AddUdpMappingAny(const Gateway& gw, uint16_t intPort, uint32_t leaseSec, const char* desc,
                      uint16_t* extPortUsed, int* upnpErr, uint32_t* leaseUsed, char* err, size_t errN);

// GetSpecificPortMappingEntry: *client = internal client (host order), *intPort, desc copied (may be "").
bool GetUdpMapping(const Gateway& gw, uint16_t extPort, uint32_t* client, uint16_t* intPort,
                   char* desc, size_t descN, char* err, size_t errN);

bool DeleteUdpMapping(const Gateway& gw, uint16_t extPort, char* err, size_t errN);

// The local IPv4 of the default route (host order): PrimaryIPv4 first, then a connected UDP socket (sends nothing).
bool DefaultRouteIPv4(uint32_t* ipHost);

// The real adapter's IPv4 from the system routing table (best route to 8.8.8.8; the adapter must be up), or the
// first up adapter with an IPv4 default gateway. `name` gets the adapter's friendly name. Unaffected by
// per-application socket filtering (VPN split tunnelling) on the calling process.
bool PrimaryIPv4(uint32_t* ipHost, char* name, size_t nameN);

// Address class used by the verdicts: 0 public, 1 private (RFC 1918), 2 carrier-grade NAT (100.64/10),
// 3 other non-routable (0/8, 127/8, 169.254/16).
int AddressClass(uint32_t ipHost);

}  // namespace Upnp
