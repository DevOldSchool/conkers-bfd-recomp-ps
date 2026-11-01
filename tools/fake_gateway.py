#!/usr/bin/env python3
"""A stand-in router for testing the online host's port mapping without touching a real one.

It answers UPnP IGD (a root description and the WANIPConnection actions the game uses:
AddPortMapping, GetExternalIPAddress, DeletePortMapping) over HTTP on 127.0.0.1, and NAT-PMP on
UDP 127.0.0.1:5351 (the protocol's fixed port). Every request is printed, so a test can see the
port opened and closed again. Point the game at it with

    CONKER_NET_UPNP_URL=http://127.0.0.1:<http port>/rootDesc.xml
    CONKER_NET_NATPMP_GATEWAY=127.0.0.1

(with either set, the game tries only the methods set; see src/net/net_portmap.hpp).

    tools/fake_gateway.py [--http-port N] [--external 203.0.113.7] [--upnp fail|conflict|off]
                          [--natpmp fail|off]
"""

import argparse
import re
import socket
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SERVICE = "urn:schemas-upnp-org:service:WANIPConnection:1"

ROOT_DESC = """<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
<specVersion><major>1</major><minor>0</minor></specVersion>
<URLBase>http://127.0.0.1:{port}/</URLBase>
<device>
<deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
<friendlyName>Stand-in gateway</friendlyName>
<deviceList><device>
<deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
<serviceList><service>
<serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1</serviceType>
<serviceId>urn:upnp-org:serviceId:WANCommonIFC1</serviceId>
<controlURL>/ctl/CmnIfCfg</controlURL><eventSubURL>/evt/CmnIfCfg</eventSubURL><SCPDURL>/WANCfg.xml</SCPDURL>
</service></serviceList>
<deviceList><device>
<deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
<serviceList><service>
<serviceType>{service}</serviceType>
<serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>
<controlURL>/ctl/IPConn</controlURL><eventSubURL>/evt/IPConn</eventSubURL><SCPDURL>/WANIPCn.xml</SCPDURL>
</service></serviceList>
</device></deviceList>
</device></deviceList>
</device>
</root>
"""

ENVELOPE = (
    '<?xml version="1.0"?>\r\n'
    '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
    's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>{body}</s:Body></s:Envelope>\r\n'
)

FAULT = (
    "<s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>"
    '<UPnPError xmlns="urn:schemas-upnp-org:control-1-0"><errorCode>{code}</errorCode>'
    "<errorDescription>{text}</errorDescription></UPnPError></detail></s:Fault>"
)

LOCK = threading.Lock()
MAPPINGS = {}


def log(text):
    with LOCK:
        print(f"{time.strftime('%H:%M:%S')} {text}", flush=True)


def field(body, name):
    match = re.search(rf"<{name}>([^<]*)</{name}>", body)
    return match.group(1) if match else ""


def make_handler(args):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def reply(self, status, text, kind="text/xml"):
            data = text.encode()
            self.send_response(status)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path == "/rootDesc.xml":
                log("upnp: root description read")
                self.reply(200, ROOT_DESC.format(port=args.http_port, service=SERVICE))
            else:
                self.reply(404, "not here", "text/plain")

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length).decode(errors="replace")
            action = self.headers.get("SOAPAction", "").strip('"').split("#")[-1]
            if args.upnp == "fail":
                log(f"upnp: {action} refused (--upnp fail)")
                self.reply(500, ENVELOPE.format(body=FAULT.format(code=501, text="Action Failed")))
                return
            if action == "GetExternalIPAddress":
                log(f"upnp: GetExternalIPAddress -> {args.external}")
                body = (f'<u:GetExternalIPAddressResponse xmlns:u="{SERVICE}">'
                        f"<NewExternalIPAddress>{args.external}</NewExternalIPAddress>"
                        "</u:GetExternalIPAddressResponse>")
                self.reply(200, ENVELOPE.format(body=body))
            elif action == "AddPortMapping":
                port = field(body, "NewExternalPort")
                proto = field(body, "NewProtocol")
                client = field(body, "NewInternalClient")
                lease = field(body, "NewLeaseDuration")
                if args.upnp == "conflict":
                    log(f"upnp: AddPortMapping {proto} {port} refused: taken by another machine (--upnp conflict)")
                    self.reply(500, ENVELOPE.format(body=FAULT.format(code=718, text="ConflictInMappingEntry")))
                    return
                MAPPINGS[("upnp", proto, port)] = client
                log(f"upnp: AddPortMapping {proto} {port} -> {client}:{field(body, 'NewInternalPort')} "
                    f"lease {lease} ({field(body, 'NewPortMappingDescription')})")
                self.reply(200, ENVELOPE.format(body=f'<u:AddPortMappingResponse xmlns:u="{SERVICE}"/>'))
            elif action == "DeletePortMapping":
                port = field(body, "NewExternalPort")
                proto = field(body, "NewProtocol")
                had = MAPPINGS.pop(("upnp", proto, port), None)
                log(f"upnp: DeletePortMapping {proto} {port} ({'removed' if had else 'there was none'})")
                self.reply(200, ENVELOPE.format(body=f'<u:DeletePortMappingResponse xmlns:u="{SERVICE}"/>'))
            else:
                log(f"upnp: {action} not known")
                self.reply(500, ENVELOPE.format(body=FAULT.format(code=401, text="Invalid Action")))

    return Handler


def natpmp(args):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 5351))
    start = time.time()
    external = socket.inet_aton(args.external)
    while True:
        data, peer = sock.recvfrom(64)
        if len(data) < 2 or data[0] != 0:
            continue
        epoch = int(time.time() - start)
        opcode = data[1]
        if args.natpmp == "fail":
            log(f"natpmp: opcode {opcode} refused (--natpmp fail)")
            sock.sendto(struct.pack("!BBHI", 0, 128 + opcode, 3, epoch), peer)  # 3: network failure
        elif opcode == 0:
            log(f"natpmp: public address -> {args.external}")
            sock.sendto(struct.pack("!BBHI", 0, 128, 0, epoch) + external, peer)
        elif opcode in (1, 2) and len(data) >= 12:
            _, _, inside, outside, lifetime = struct.unpack("!HHHHI", data[:12])
            proto = "UDP" if opcode == 1 else "TCP"
            if lifetime == 0:
                had = MAPPINGS.pop(("natpmp", proto, inside), None)
                log(f"natpmp: remove {proto} {inside} ({'removed' if had else 'there was none'})")
            else:
                MAPPINGS[("natpmp", proto, inside)] = peer[0]
                log(f"natpmp: map {proto} {outside} -> {peer[0]}:{inside} lifetime {lifetime}")
            sock.sendto(struct.pack("!BBHIHHI", 0, 128 + opcode, 0, epoch, inside, outside or inside, lifetime), peer)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--http-port", type=int, default=0)
    parser.add_argument("--external", default="203.0.113.7")
    parser.add_argument("--upnp", choices=["ok", "fail", "conflict", "off"], default="ok")
    parser.add_argument("--natpmp", choices=["ok", "fail", "off"], default="ok")
    args = parser.parse_args()
    if args.natpmp != "off":
        threading.Thread(target=natpmp, args=(args,), daemon=True).start()
        log("natpmp: on udp 127.0.0.1:5351")
    if args.upnp != "off":
        server = ThreadingHTTPServer(("127.0.0.1", args.http_port), None)
        args.http_port = server.server_address[1]
        server.RequestHandlerClass = make_handler(args)
        log(f"upnp: CONKER_NET_UPNP_URL=http://127.0.0.1:{args.http_port}/rootDesc.xml")
        server.serve_forever()
    else:
        while True:
            time.sleep(3600)


if __name__ == "__main__":
    sys.exit(main())
