// Windows Firewall rules for the game exe. See firewall.h. COM: INetFwPolicy2 (netfw.h).
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shellapi.h>
#include <netfw.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "firewall.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace {

const wchar_t* kRuleName = L"AC4Coop - Black Flag (UDP in)";

std::wstring Widen(const char* s) {
    const int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    return w;
}

struct Com {
    HRESULT hr; INetFwPolicy2* pol = nullptr; INetFwRules* rules = nullptr;
    Com() {
        hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        init = SUCCEEDED(hr);
        if (init || hr == RPC_E_CHANGED_MODE) {
            hr = CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwPolicy2), (void**)&pol);
            if (SUCCEEDED(hr)) hr = pol->get_Rules(&rules);
        }
    }
    ~Com() { if (rules) rules->Release(); if (pol) pol->Release(); if (init) CoUninitialize(); }
    bool ok() const { return SUCCEEDED(hr) && pol && rules; }
    bool init;
};

struct Found { INetFwRule* rule; bool block; bool udpOrAny; bool ours; };

// Enabled inbound rules for `exe`. The caller releases each rule.
std::vector<Found> RulesFor(Com& c, const std::wstring& exe) {
    std::vector<Found> out;
    IUnknown* unk = nullptr;
    if (FAILED(c.rules->get__NewEnum(&unk)) || !unk) return out;
    IEnumVARIANT* en = nullptr;
    unk->QueryInterface(__uuidof(IEnumVARIANT), (void**)&en); unk->Release();
    if (!en) return out;
    VARIANT v; VariantInit(&v); ULONG got = 0;
    while (en->Next(1, &v, &got) == S_OK && got == 1) {
        INetFwRule* r = nullptr;
        if (v.vt == VT_DISPATCH && v.pdispVal) v.pdispVal->QueryInterface(__uuidof(INetFwRule), (void**)&r);
        VariantClear(&v);
        if (!r) continue;
        BSTR app = nullptr; NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_OUT; VARIANT_BOOL enabled = VARIANT_FALSE;
        r->get_ApplicationName(&app); r->get_Direction(&dir); r->get_Enabled(&enabled);
        const bool match = app && _wcsicmp(app, exe.c_str()) == 0 && dir == NET_FW_RULE_DIR_IN && enabled == VARIANT_TRUE;
        if (app) SysFreeString(app);
        if (!match) { r->Release(); continue; }
        NET_FW_ACTION act = NET_FW_ACTION_BLOCK; long proto = 256; BSTR name = nullptr;
        r->get_Action(&act); r->get_Protocol(&proto); r->get_Name(&name);
        out.push_back({ r, act == NET_FW_ACTION_BLOCK, proto == NET_FW_IP_PROTOCOL_UDP || proto == NET_FW_IP_PROTOCOL_ANY,
                        name && wcscmp(name, kRuleName) == 0 });
        if (name) SysFreeString(name);
    }
    en->Release();
    return out;
}

bool FirewallOn(Com& c) {   // on for at least one profile that is active now
    long types = 0;
    if (FAILED(c.pol->get_CurrentProfileTypes(&types))) return true;
    for (long p : { (long)NET_FW_PROFILE2_DOMAIN, (long)NET_FW_PROFILE2_PRIVATE, (long)NET_FW_PROFILE2_PUBLIC }) {
        VARIANT_BOOL on = VARIANT_TRUE;
        if ((types & p) && SUCCEEDED(c.pol->get_FirewallEnabled((NET_FW_PROFILE_TYPE2)p, &on)) && on == VARIANT_TRUE) return true;
    }
    return false;
}

}  // namespace

namespace Firewall {

Status Query(const char* exePath, char* detail, size_t n) {
    Com c;
    if (!c.ok()) { _snprintf(detail, n, "Windows Firewall could not be read (0x%08lx)", (unsigned long)c.hr); detail[n - 1] = 0; return UNKNOWN; }
    if (!FirewallOn(c)) { _snprintf(detail, n, "Windows Firewall is off"); detail[n - 1] = 0; return FIREWALL_OFF; }
    auto rules = RulesFor(c, Widen(exePath));
    int blocks = 0, allows = 0;
    for (auto& f : rules) { if (f.block) ++blocks; else if (f.udpOrAny) ++allows; f.rule->Release(); }
    Status st = blocks ? BLOCKED : allows ? ALLOWED : NO_RULE;
    _snprintf(detail, n, st == BLOCKED ? "Windows Firewall BLOCKS the game (%d block rule%s)" :
                         st == ALLOWED ? "Windows Firewall allows the game" :
                                         "Windows Firewall has no rule for the game yet - friends may not reach you when you host",
              blocks, blocks == 1 ? "" : "s");
    detail[n - 1] = 0;
    return st;
}

bool Apply(const char* exePath, char* msg, size_t n) {
    Com c;
    if (!c.ok()) { _snprintf(msg, n, "firewall COM failed 0x%08lx", (unsigned long)c.hr); msg[n - 1] = 0; return false; }
    const std::wstring exe = Widen(exePath);
    auto rules = RulesFor(c, exe);
    int disabled = 0; bool have = false; HRESULT hr = S_OK;
    for (auto& f : rules) {
        if (f.block) { const HRESULT h = f.rule->put_Enabled(VARIANT_FALSE); if (SUCCEEDED(h)) ++disabled; else hr = h; }
        else if (f.ours) have = true;
        f.rule->Release();
    }
    if (!have) {
        INetFwRule* r = nullptr;
        hr = CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwRule), (void**)&r);
        if (SUCCEEDED(hr)) {
            BSTR name = SysAllocString(kRuleName), app = SysAllocString(exe.c_str());
            BSTR desc = SysAllocString(L"Lets co-op players reach this game (added by the AC4Coop launcher). Safe to delete.");
            r->put_Name(name); r->put_Description(desc); r->put_ApplicationName(app);
            r->put_Protocol(NET_FW_IP_PROTOCOL_UDP); r->put_Direction(NET_FW_RULE_DIR_IN); r->put_Action(NET_FW_ACTION_ALLOW);
            r->put_Profiles(NET_FW_PROFILE2_ALL); r->put_Enabled(VARIANT_TRUE);
            hr = c.rules->Add(r);
            SysFreeString(name); SysFreeString(app); SysFreeString(desc); r->Release();
        }
    }
    _snprintf(msg, n, "firewall: %d block rule(s) disabled, allow rule %s (hr=0x%08lx)", disabled,
              have ? "already present" : SUCCEEDED(hr) ? "added" : "NOT added", (unsigned long)hr);
    msg[n - 1] = 0;
    return SUCCEEDED(hr);
}

bool RunElevated(const char* exePath, char* msg, size_t n) {
    char self[MAX_PATH]; GetModuleFileNameA(nullptr, self, MAX_PATH);
    std::string args = std::string("--firewall-apply \"") + exePath + "\"";
    SHELLEXECUTEINFOA se{ sizeof(se) };
    se.fMask = SEE_MASK_NOCLOSEPROCESS; se.lpVerb = "runas"; se.lpFile = self; se.lpParameters = args.c_str(); se.nShow = SW_HIDE;
    if (!ShellExecuteExA(&se) || !se.hProcess) {
        _snprintf(msg, n, GetLastError() == ERROR_CANCELLED ? "Not changed: the administrator prompt was declined" : "Could not start the elevated step (error %lu)", GetLastError());
        msg[n - 1] = 0; return false;
    }
    WaitForSingleObject(se.hProcess, 30000);
    DWORD code = 1; GetExitCodeProcess(se.hProcess, &code); CloseHandle(se.hProcess);
    _snprintf(msg, n, code == 0 ? "Firewall updated" : "The firewall change failed (see launcher_log.txt)"); msg[n - 1] = 0;
    return code == 0;
}

}  // namespace Firewall
