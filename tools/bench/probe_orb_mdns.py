"""Send one PTR question for _orb._tcp.local three ways and report which reach
the orb. This separates "the responder is deaf" from "this box cannot originate
multicast", which is a WSL fact rather than a firmware one.

The reply is parsed by zeroconf, not by anything in this file: an independent
implementation reading the announcement is the evidence, my parser agreeing with
my encoder is not.
"""
import socket, struct, sys
from zeroconf import DNSIncoming

ROOT = "_orb._tcp.local"


def question(name, qtype=12, qclass=1):
    out = b""
    for label in name.split("."):
        out += bytes([len(label)]) + label.encode()
    return out + b"\x00" + struct.pack("!HH", qtype, qclass)


def probe(dest, tid):
    payload = struct.pack("!HHHHHH", tid, 0, 1, 0, 0, 0) + question(ROOT)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 0))
    s.settimeout(4)
    try:
        s.sendto(payload, (dest, 5353))
        data, frm = s.recvfrom(2048)
    except socket.timeout:
        print(f"  {dest:15s} NO REPLY")
        return None
    finally:
        s.close()
    m = DNSIncoming(data)
    print(f"  {dest:15s} {len(data)} B from {frm[0]}  zeroconf: valid={m.valid} "
          f"qd={m.num_questions} an={m.num_answers} ns={m.num_authorities} "
          f"ar={m.num_additionals} qu={m.has_qu_question}")
    return m


if __name__ == "__main__":
    ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.2.34"
    net = ".".join(ip.split(".")[:3]) + ".255"
    for i, dest in enumerate((ip, net, "224.0.0.251")):
        kind = {0: "unicast  ", 1: "broadcast", 2: "multicast "}[i]
        print(kind, end="")
        m = probe(dest, 0xC0FF + i)
        if m is None:
            continue
        for r in list(m.answers()):
            print("      AN " + str(r.name) + " type=" + str(r.type) +
                  " ttl=" + str(getattr(r, "ttl", None)) + " class=" + str(r.class_))
        if i == 0:
            # A unicast round trip is proven working; the interesting question
            # is whether the other two ever reach the orb at all, so keep going.
            pass
