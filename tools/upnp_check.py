#!/usr/bin/env python3
"""upnp_check.py - read-only look at this network's UPnP gateway (lobby slice 4, docs/design/lobby-code.md).

  py tools\\upnp_check.py                 discover the gateway, print its control URL and external IP
  py tools\\upnp_check.py --port 47801    also ask whether a UDP mapping for that port exists, and to whom

It only sends SSDP M-SEARCH and the two READ actions GetExternalIPAddress / GetSpecificPortMappingEntry.
It never adds or deletes a mapping. Standard library only.
Exit 0 = gateway found (and, with --port, the answer printed); 1 = no UPnP gateway answered; 2 = usage.
"""
from __future__ import annotations

import argparse
import re
import socket
import sys
import time
import urllib.parse
import urllib.request

SSDP = ("239.255.255.250", 1900)
TARGETS = (
    "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
    "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
    "urn:schemas-upnp-org:service:WANIPConnection:1",
    "urn:schemas-upnp-org:service:WANIPConnection:2",
    "urn:schemas-upnp-org:service:WANPPPConnection:1",
)


def lan_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 53))  # no packet is sent for a UDP connect
        return s.getsockname()[0]
    finally:
        s.close()


def discover(timeout: float = 3.0) -> list[str]:
    # Pin the multicast to the default-route (LAN) interface. Measured 2026-11-12 on the owner's PC:
    # without IP_MULTICAST_IF the M-SEARCH left through a VPN adapter (ZeroTier/Radmin/Tailscale are
    # installed) and NO gateway answered; bound to the Ethernet address, 192.168.68.1 answered at once.
    ip = lan_ip()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    s.bind((ip, 0))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(ip))
    s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
    s.settimeout(0.25)
    for st in TARGETS:
        msg = ("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
               f"MX: 2\r\nST: {st}\r\n\r\n").encode()
        s.sendto(msg, SSDP)
    found: list[str] = []
    t_end = time.monotonic() + timeout
    while time.monotonic() < t_end:
        try:
            data, _ = s.recvfrom(4096)
        except socket.timeout:
            continue
        m = re.search(rb"(?im)^location:\s*(\S+)", data)
        if m:
            loc = m.group(1).decode(errors="replace")
            if loc not in found:
                found.append(loc)
    s.close()
    return found


def control_url(location: str):
    xml = urllib.request.urlopen(location, timeout=4).read().decode(errors="replace")
    base = re.search(r"<URLBase>\s*([^<]+?)\s*</URLBase>", xml)
    base = base.group(1) if base else location
    for svc in re.findall(r"<service>(.*?)</service>", xml, re.S):
        st = re.search(r"<serviceType>\s*([^<]+?)\s*</serviceType>", svc)
        cu = re.search(r"<controlURL>\s*([^<]+?)\s*</controlURL>", svc)
        if st and cu and ("WANIPConnection" in st.group(1) or "WANPPPConnection" in st.group(1)):
            return st.group(1), urllib.parse.urljoin(base, cu.group(1))
    return None


def soap(url: str, service: str, action: str, args: dict[str, str]):
    body = "".join(f"<{k}>{v}</{k}>" for k, v in args.items())
    env = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
           's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
           f'<u:{action} xmlns:u="{service}">{body}</u:{action}></s:Body></s:Envelope>')
    req = urllib.request.Request(url, data=env.encode(), method="POST", headers={
        "Content-Type": 'text/xml; charset="utf-8"', "SOAPAction": f'"{service}#{action}"'})
    try:
        return 200, urllib.request.urlopen(req, timeout=4).read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode(errors="replace")


def tag(xml: str, name: str) -> str | None:
    m = re.search(rf"<(?:\w+:)?{name}>\s*([^<]*?)\s*</(?:\w+:)?{name}>", xml)
    return m.group(1) if m else None


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="read-only UPnP gateway check")
    ap.add_argument("--port", type=int, default=None)
    a = ap.parse_args(argv)
    locs = discover()
    if not locs:
        print("[upnp] RESULT no UPnP gateway answered SSDP in 3 s (UPnP off on the router, or no IGD)")
        return 1
    for loc in locs:
        try:
            cu = control_url(loc)
        except Exception as e:  # noqa: BLE001 - report and try the next device
            print(f"[upnp] {loc}: description fetch failed: {e}")
            continue
        if not cu:
            print(f"[upnp] {loc}: no WANIPConnection/WANPPPConnection service")
            continue
        service, url = cu
        print(f"[upnp] gateway {loc}\n[upnp] service {service}\n[upnp] control {url}")
        code, xml = soap(url, service, "GetExternalIPAddress", {})
        print(f"[upnp] external IP: {tag(xml, 'NewExternalIPAddress') if code == 200 else f'HTTP {code} {tag(xml, 'errorDescription')}'}")
        if a.port is not None:
            code, xml = soap(url, service, "GetSpecificPortMappingEntry",
                             {"NewRemoteHost": "", "NewExternalPort": str(a.port), "NewProtocol": "UDP"})
            if code == 200:
                print(f"[upnp] RESULT mapping UDP {a.port} -> {tag(xml, 'NewInternalClient')}:{tag(xml, 'NewInternalPort')} "
                      f"desc={tag(xml, 'NewPortMappingDescription')!r} lease={tag(xml, 'NewLeaseDuration')} "
                      f"enabled={tag(xml, 'NewEnabled')}")
            else:
                print(f"[upnp] RESULT no mapping for UDP {a.port} (HTTP {code} {tag(xml, 'errorCode')} {tag(xml, 'errorDescription')})")
        else:
            print("[upnp] RESULT gateway OK")
        return 0
    print("[upnp] RESULT devices answered but none offers a WAN connection service")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
