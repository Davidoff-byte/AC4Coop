// UPnP-IGD client (lobby slice 4). See upnp.h. Pure Winsock + WinHTTP; no engine calls.
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <iphlpapi.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include "upnp.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace {

void SetErr(char* err, size_t n, const char* fmt, ...) {
    if (!err || !n) return;
    va_list ap; va_start(ap, fmt);
    _vsnprintf(err, n, fmt, ap);
    va_end(ap);
    err[n - 1] = 0;
}

bool IEquals(const char* a, const char* b, size_t n) { return _strnicmp(a, b, n) == 0; }

// Case-insensitive header value from an SSDP answer ("LOCATION: http://...").
bool HeaderValue(const char* msg, const char* name, char* out, size_t outN) {
    const size_t nl = strlen(name);
    for (const char* p = msg; *p; ) {
        if (IEquals(p, name, nl) && p[nl] == ':') {
            p += nl + 1;
            while (*p == ' ' || *p == '\t') ++p;
            size_t i = 0;
            while (*p && *p != '\r' && *p != '\n' && i + 1 < outN) out[i++] = *p++;
            out[i] = 0;
            return i > 0;
        }
        const char* e = strchr(p, '\n');
        if (!e) break;
        p = e + 1;
    }
    return false;
}

// Text of the first element whose local name is `name` (namespace prefixes ignored) at or after `from`.
bool Tag(const std::string& xml, const char* name, std::string* out, size_t from = 0, size_t* endPos = nullptr) {
    const std::string n(name);
    size_t p = from;
    for (;;) {
        p = xml.find(n, p);
        if (p == std::string::npos) return false;
        const bool openOk = p > 0 && (xml[p - 1] == '<' || xml[p - 1] == ':');
        const size_t after = p + n.size();
        if (openOk && after < xml.size() && xml[after] == '>') {
            // make sure this is an opening tag, not "</name>"
            size_t lt = p - 1; while (lt > 0 && xml[lt] != '<') --lt;
            if (lt + 1 < xml.size() && xml[lt + 1] != '/') {
                const size_t vs = after + 1;
                const size_t ve = xml.find('<', vs);
                if (ve == std::string::npos) return false;
                std::string v = xml.substr(vs, ve - vs);
                size_t a = v.find_first_not_of(" \t\r\n"), b = v.find_last_not_of(" \t\r\n");
                *out = (a == std::string::npos) ? std::string() : v.substr(a, b - a + 1);
                if (endPos) *endPos = ve;
                return true;
            }
        }
        p = after;
    }
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// One HTTP request through WinHTTP (handles chunked bodies). Gateways answer within milliseconds on a
// LAN; 4 s timeouts keep a dead one from stalling the caller.
bool Http(const std::string& url, const char* method, const std::string& soapAction, const std::string& body,
          std::string* resp, int* status, char* err, size_t errN) {
    std::wstring wurl = Widen(url);
    URL_COMPONENTS uc{}; uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[1024] = {0};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 1024;
    wchar_t extra[512] = {0}; uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 512;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { SetErr(err, errN, "bad URL %s", url.c_str()); return false; }
    std::wstring fullPath = std::wstring(path) + extra;
    if (fullPath.empty()) fullPath = L"/";
    HINTERNET ses = WinHttpOpen(L"AC4Coop-lobby/1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { SetErr(err, errN, "WinHttpOpen err=%lu", GetLastError()); return false; }
    WinHttpSetTimeouts(ses, 4000, 4000, 4000, 4000);
    bool ok = false;
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, Widen(method).c_str(), fullPath.c_str(), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0) : nullptr;
    if (req) {
        std::wstring hdr;
        if (!soapAction.empty())
            hdr = L"Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + Widen(soapAction) + L"\"\r\n";
        const BOOL sent = WinHttpSendRequest(req, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(),
                                             hdr.empty() ? 0 : (DWORD)-1L,
                                             body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                             (DWORD)body.size(), (DWORD)body.size(), 0);
        if (sent && WinHttpReceiveResponse(req, nullptr)) {
            DWORD code = 0, sz = sizeof(code);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &code, &sz, WINHTTP_NO_HEADER_INDEX);
            *status = (int)code;
            resp->clear();
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                if (resp->size() + avail > 256 * 1024) break;   // a description is a few KB; cap a runaway
                std::string chunk(avail, '\0'); DWORD got = 0;
                if (!WinHttpReadData(req, &chunk[0], avail, &got) || got == 0) break;
                resp->append(chunk.data(), got);
            }
            ok = true;
        } else {
            SetErr(err, errN, "HTTP %s %s failed err=%lu", method, url.c_str(), GetLastError());
        }
    } else {
        SetErr(err, errN, "HTTP connect %s failed err=%lu", url.c_str(), GetLastError());
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}

// Resolve a controlURL against URLBase (or the description URL).
std::string JoinUrl(const std::string& base, const std::string& rel) {
    if (_strnicmp(rel.c_str(), "http://", 7) == 0 || _strnicmp(rel.c_str(), "https://", 8) == 0) return rel;
    const size_t schemeEnd = base.find("://");
    const size_t hostEnd = (schemeEnd == std::string::npos) ? std::string::npos : base.find('/', schemeEnd + 3);
    const std::string origin = (hostEnd == std::string::npos) ? base : base.substr(0, hostEnd);
    if (!rel.empty() && rel[0] == '/') return origin + rel;
    const size_t lastSlash = base.rfind('/');
    const std::string dir = (lastSlash == std::string::npos || lastSlash < schemeEnd + 3) ? origin + "/" : base.substr(0, lastSlash + 1);
    return dir + rel;
}

bool Soap(const Upnp::Gateway& gw, const char* action, const std::string& args, std::string* resp, int* status,
          char* err, size_t errN) {
    std::string env =
        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:";
    env += action; env += " xmlns:u=\""; env += gw.service; env += "\">"; env += args;
    env += "</u:"; env += action; env += "></s:Body></s:Envelope>";
    std::string sa = std::string(gw.service) + "#" + action;
    return Http(gw.controlUrl, "POST", sa, env, resp, status, err, errN);
}

int SoapErrorCode(const std::string& resp) {
    std::string v;
    return Tag(resp, "errorCode", &v) ? atoi(v.c_str()) : 0;
}

bool ParseIPv4(const std::string& s, uint32_t* ipHost) {
    in_addr a{};
    if (inet_pton(AF_INET, s.c_str(), &a) != 1) return false;
    *ipHost = ntohl(a.s_addr);
    return true;
}

std::string IpStr(uint32_t ipHost) {
    char b[32]; _snprintf(b, sizeof(b), "%u.%u.%u.%u", ipHost >> 24, (ipHost >> 16) & 0xFF, (ipHost >> 8) & 0xFF, ipHost & 0xFF);
    b[sizeof(b) - 1] = 0;
    return b;
}

}  // namespace

namespace Upnp {

// The real adapter: asked of the system routing table (GetBestInterface towards 8.8.8.8), not of a socket, so
// a per-application VPN/split-tunnel filter on this process cannot hide it. The chosen adapter must be up and
// have an IPv4 unicast address; if the best-route adapter is down (e.g. a disconnected VPN adapter left in the
// table), the first up adapter that has an IPv4 default gateway is used instead.
// Reported 2026-11-12: on a friend's PC the game ended up on NordLynx's 10.5.0.2 with no internet path while
// the launcher on the same PC worked (net_log: "no default IPv4 route", STUN unanswered).
bool PrimaryIPv4(uint32_t* ipHost, char* name, size_t nameN) {
    if (name && nameN) name[0] = 0;
    DWORD best = 0;
    const bool haveBest = GetBestInterface(htonl(0x08080808u), &best) == NO_ERROR;
    ULONG len = 16 * 1024;
    std::string buf(len, '\0');
    ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS,
                                    nullptr, (IP_ADAPTER_ADDRESSES*)&buf[0], &len);
    if (rc == ERROR_BUFFER_OVERFLOW) { buf.assign(len, '\0'); rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS, nullptr, (IP_ADAPTER_ADDRESSES*)&buf[0], &len); }
    if (rc != NO_ERROR) return false;
    auto firstV4 = [](IP_ADAPTER_ADDRESSES* a, uint32_t* out) {
        for (IP_ADAPTER_UNICAST_ADDRESS* u = a->FirstUnicastAddress; u; u = u->Next)
            if (u->Address.lpSockaddr && u->Address.lpSockaddr->sa_family == AF_INET) {
                const uint32_t ip = ntohl(((sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr);
                if ((ip >> 24) != 127 && (ip >> 16) != 0xA9FE) { *out = ip; return true; }
            }
        return false;
    };
    IP_ADAPTER_ADDRESSES* pick = nullptr; uint32_t ip = 0;
    for (auto* a = (IP_ADAPTER_ADDRESSES*)&buf[0]; a && !pick; a = a->Next)
        if (haveBest && a->IfIndex == best && a->OperStatus == IfOperStatusUp && firstV4(a, &ip)) pick = a;
    for (auto* a = (IP_ADAPTER_ADDRESSES*)&buf[0]; a && !pick; a = a->Next)
        if (a->OperStatus == IfOperStatusUp && a->FirstGatewayAddress && a->IfType != IF_TYPE_SOFTWARE_LOOPBACK && firstV4(a, &ip)) pick = a;
    if (!pick) return false;
    *ipHost = ip;
    if (name && nameN) WideCharToMultiByte(CP_UTF8, 0, pick->FriendlyName, -1, name, (int)nameN, nullptr, nullptr);
    return true;
}

// Names of adapters that are up, have an IPv4 address and look like a VPN / virtual tunnel: interface type
// tunnel (131), PPP (23) or proprietary-virtual (53), or a description naming a known tunnel driver. A hint for
// the player and for remote logs only (a friend's game sat on NordLynx 10.5.0.2 with no internet path,
// 2026-11-12); nothing is decided from it.
int VpnLikeAdapters(char* out, size_t n) {
    if (out && n) out[0] = 0;
    ULONG len = 16 * 1024;
    std::string buf(len, '\0');
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)&buf[0], &len);
    if (rc == ERROR_BUFFER_OVERFLOW) { buf.assign(len, '\0'); rc = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES*)&buf[0], &len); }
    if (rc != NO_ERROR) return 0;
    static const wchar_t* kWords[] = { L"wireguard", L"nordlynx", L"wintun", L"tap-windows", L"openvpn", L"vpn", L"zerotier",
                                       L"tailscale", L"hamachi", L"warp", L"proton" };
    int count = 0; std::string names;
    for (auto* a = (IP_ADAPTER_ADDRESSES*)&buf[0]; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || !a->FirstUnicastAddress) continue;
        bool vpn = a->IfType == IF_TYPE_TUNNEL || a->IfType == IF_TYPE_PPP || a->IfType == IF_TYPE_PROP_VIRTUAL;
        std::wstring d = std::wstring(a->Description ? a->Description : L"") + L" " + (a->FriendlyName ? a->FriendlyName : L"");
        for (auto& ch : d) ch = (wchar_t)towlower(ch);
        for (const wchar_t* w : kWords) if (d.find(w) != std::wstring::npos) vpn = true;
        if (!vpn) continue;
        char nm[128] = {0}; WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, nm, sizeof(nm), nullptr, nullptr);
        if (count++) names += ", ";
        names += nm;
    }
    if (out && n) lstrcpynA(out, names.c_str(), (int)n);
    return count;
}

bool DefaultRouteIPv4(uint32_t* ipHost) {
    if (PrimaryIPv4(ipHost, nullptr, 0)) return true;   // routing table first (see PrimaryIPv4)
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in dst{}; dst.sin_family = AF_INET; dst.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);
    bool ok = false;
    if (connect(s, (sockaddr*)&dst, sizeof(dst)) == 0) {
        sockaddr_in me{}; int ml = sizeof(me);
        if (getsockname(s, (sockaddr*)&me, &ml) == 0 && me.sin_addr.s_addr != 0) { *ipHost = ntohl(me.sin_addr.s_addr); ok = true; }
    }
    closesocket(s);
    return ok;
}

int AddressClass(uint32_t ip) {
    const uint32_t a = ip >> 24, b = (ip >> 16) & 0xFF;
    if (a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168)) return 1;
    if (a == 100 && b >= 64 && b <= 127) return 2;
    if (a == 0 || a == 127 || (a == 169 && b == 254)) return 3;
    return 0;
}

bool Discover(uint32_t lanIpHost, Gateway* gw, char* err, size_t errN) {
    std::memset(gw, 0, sizeof(*gw));
    gw->lanIp = lanIpHost;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { SetErr(err, errN, "socket err=%d", WSAGetLastError()); return false; }
    sockaddr_in me{}; me.sin_family = AF_INET; me.sin_addr.s_addr = htonl(lanIpHost);
    bind(s, (sockaddr*)&me, sizeof(me));
    in_addr ifa{}; ifa.s_addr = htonl(lanIpHost);
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&ifa, sizeof(ifa));
    DWORD ttl = 2; setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
    DWORD rto = 250; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rto, sizeof(rto));
    sockaddr_in mc{}; mc.sin_family = AF_INET; mc.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &mc.sin_addr);
    static const char* kTargets[] = {
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
        "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANIPConnection:2",
        "urn:schemas-upnp-org:service:WANPPPConnection:1",
    };
    for (const char* st : kTargets) {
        char msg[256];
        _snprintf(msg, sizeof(msg), "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
                                    "MX: 2\r\nST: %s\r\n\r\n", st);
        msg[sizeof(msg) - 1] = 0;
        sendto(s, msg, (int)strlen(msg), 0, (sockaddr*)&mc, sizeof(mc));
    }
    // Collect distinct LOCATIONs for 3 s, then try each until one has a WAN connection service.
    char locs[4][256] = {{0}}; int nloc = 0;
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < 3000 && nloc < 4) {
        char buf[2048]; sockaddr_in from{}; int fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fl);
        if (n <= 0) continue;
        buf[n] = 0;
        char loc[256];
        if (!HeaderValue(buf, "LOCATION", loc, sizeof(loc))) continue;
        bool dup = false;
        for (int i = 0; i < nloc; ++i) if (strcmp(locs[i], loc) == 0) dup = true;
        if (!dup) { strcpy(locs[nloc++], loc); }
        // The first IGD answer is enough once we have one; keep listening briefly for siblings only.
        if (nloc >= 1 && GetTickCount64() - t0 > 1500) break;
    }
    closesocket(s);
    if (nloc == 0) { SetErr(err, errN, "no UPnP gateway answered SSDP in 3 s (UPnP off on the router?)"); return false; }
    char lastErr[160] = "no WANIPConnection/WANPPPConnection service";
    for (int i = 0; i < nloc; ++i) {
        std::string xml; int st = 0;
        if (!Http(locs[i], "GET", "", "", &xml, &st, lastErr, sizeof(lastErr)) || st != 200) {
            if (st) SetErr(lastErr, sizeof(lastErr), "description %s HTTP %d", locs[i], st);
            continue;
        }
        std::string base;
        if (!Tag(xml, "URLBase", &base) || base.empty()) base = locs[i];
        size_t pos = 0;
        for (;;) {
            size_t svc = xml.find("<service>", pos);
            if (svc == std::string::npos) break;
            size_t svcEnd = xml.find("</service>", svc);
            if (svcEnd == std::string::npos) break;
            const std::string block = xml.substr(svc, svcEnd - svc);
            std::string type, ctl;
            if (Tag(block, "serviceType", &type) && Tag(block, "controlURL", &ctl) &&
                (type.find("WANIPConnection") != std::string::npos || type.find("WANPPPConnection") != std::string::npos)) {
                lstrcpynA(gw->location, locs[i], sizeof(gw->location));
                lstrcpynA(gw->service, type.c_str(), sizeof(gw->service));
                lstrcpynA(gw->controlUrl, JoinUrl(base, ctl).c_str(), sizeof(gw->controlUrl));
                return true;
            }
            pos = svcEnd;
        }
    }
    SetErr(err, errN, "%s", lastErr);
    return false;
}

bool GetExternalIp(const Gateway& gw, uint32_t* ipHost, char* err, size_t errN) {
    std::string r; int st = 0;
    if (!Soap(gw, "GetExternalIPAddress", "", &r, &st, err, errN)) return false;
    std::string v;
    if (st != 200 || !Tag(r, "NewExternalIPAddress", &v) || !ParseIPv4(v, ipHost)) {
        SetErr(err, errN, "GetExternalIPAddress HTTP %d upnpErr=%d value=\"%s\"", st, SoapErrorCode(r), v.c_str());
        return false;
    }
    return true;
}

bool AddUdpMapping(const Gateway& gw, uint16_t extPort, uint16_t intPort, uint32_t leaseSec, const char* desc,
                   int* upnpErr, uint32_t* leaseUsed, char* err, size_t errN) {
    *upnpErr = 0; *leaseUsed = leaseSec;
    for (int attempt = 0; attempt < 2; ++attempt) {
        char args[640];
        _snprintf(args, sizeof(args),
                  "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>"
                  "<NewInternalPort>%u</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
                  "<NewPortMappingDescription>%s</NewPortMappingDescription><NewLeaseDuration>%u</NewLeaseDuration>",
                  (unsigned)extPort, (unsigned)intPort, IpStr(gw.lanIp).c_str(), desc, (unsigned)*leaseUsed);
        args[sizeof(args) - 1] = 0;
        std::string r; int st = 0;
        if (!Soap(gw, "AddPortMapping", args, &r, &st, err, errN)) return false;
        if (st == 200) { *upnpErr = 0; return true; }
        *upnpErr = SoapErrorCode(r);
        if (*upnpErr == 725 && *leaseUsed != 0) { *leaseUsed = 0; continue; }   // OnlyPermanentLeasesSupported
        std::string d; Tag(r, "errorDescription", &d);
        SetErr(err, errN, "AddPortMapping HTTP %d upnpErr=%d %s", st, *upnpErr, d.c_str());
        return false;
    }
    return false;
}

bool AddUdpMappingAny(const Gateway& gw, uint16_t intPort, uint32_t leaseSec, const char* desc,
                      uint16_t* extPortUsed, int* upnpErr, uint32_t* leaseUsed, char* err, size_t errN) {
    for (int k = 0; k < 10; ++k) {
        const uint16_t ext = (uint16_t)(intPort + k);
        if (AddUdpMapping(gw, ext, intPort, leaseSec, desc, upnpErr, leaseUsed, err, errN)) { *extPortUsed = ext; return true; }
        if (*upnpErr != 718) return false;   // only a port conflict is worth another port
    }
    return false;
}

bool GetUdpMapping(const Gateway& gw, uint16_t extPort, uint32_t* client, uint16_t* intPort,
                   char* desc, size_t descN, char* err, size_t errN) {
    char args[256];
    _snprintf(args, sizeof(args), "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>",
              (unsigned)extPort);
    args[sizeof(args) - 1] = 0;
    std::string r; int st = 0;
    if (!Soap(gw, "GetSpecificPortMappingEntry", args, &r, &st, err, errN)) return false;
    if (st != 200) {
        std::string d; Tag(r, "errorDescription", &d);
        SetErr(err, errN, "no mapping (HTTP %d upnpErr=%d %s)", st, SoapErrorCode(r), d.c_str());
        return false;
    }
    std::string c, p, d;
    Tag(r, "NewInternalClient", &c); Tag(r, "NewInternalPort", &p); Tag(r, "NewPortMappingDescription", &d);
    *client = 0; ParseIPv4(c, client);
    *intPort = (uint16_t)atoi(p.c_str());
    if (desc && descN) lstrcpynA(desc, d.c_str(), (int)descN);
    return true;
}

bool DeleteUdpMapping(const Gateway& gw, uint16_t extPort, char* err, size_t errN) {
    char args[256];
    _snprintf(args, sizeof(args), "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>",
              (unsigned)extPort);
    args[sizeof(args) - 1] = 0;
    std::string r; int st = 0;
    if (!Soap(gw, "DeletePortMapping", args, &r, &st, err, errN)) return false;
    if (st != 200) {
        std::string d; Tag(r, "errorDescription", &d);
        SetErr(err, errN, "DeletePortMapping HTTP %d upnpErr=%d %s", st, SoapErrorCode(r), d.c_str());
        return false;
    }
    return true;
}

}  // namespace Upnp
