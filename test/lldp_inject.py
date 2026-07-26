#!/usr/bin/env python3
"""Inject one LLDPDU into a switch port, as a cabled neighbour would.

Runs on the HYPERVISOR, not on the target. LLDP is nearest-bridge:
its frames are consumed by the first bridge that sees them and never
forwarded, so a neighbour has to be on the other end of the actual
cable. Inside a VM there is no "other end" to stand on — the tap
device on the host is it.

    sudo test/lldp_inject.py vnet1730          # the tap behind lan1

Then, on the target:

    show neighbors

The frame it sends is deliberately hand-built rather than produced by
our own encoder: a test that encodes and decodes with the same code
proves only that the code agrees with itself. This one is written from
802.1AB, so it is an independent check that we parse what a real
neighbour sends.
"""

import socket
import struct
import sys


def tlv(type_code, body):
  """One TLV: 7-bit type, 9-bit length, body."""
  return struct.pack("!H", (type_code << 9) | len(body)) + body


def build(chassis_mac, port_id, system_name, mgmt_ip):
  chassis = bytes.fromhex(chassis_mac.replace(":", ""))
  pdu = b""
  pdu += tlv(1, b"\x04" + chassis)                    # chassis id (MAC)
  pdu += tlv(2, b"\x05" + port_id.encode())           # port id (ifname)
  pdu += tlv(3, struct.pack("!H", 120))               # ttl
  pdu += tlv(4, b"uplink to the s5")                  # port description
  pdu += tlv(5, system_name.encode())                 # system name
  pdu += tlv(6, b"test neighbour")                    # system description
  pdu += tlv(7, struct.pack("!HH", 0x0014, 0x0014))   # bridge + router
  pdu += tlv(8, b"\x05\x01" + socket.inet_aton(mgmt_ip)
             + b"\x02" + struct.pack("!I", 3) + b"\x00")
  pdu += tlv(0, b"")                                  # end
  frame = bytes.fromhex("0180c200000e") + chassis + b"\x88\xcc" + pdu
  return frame + b"\x00" * max(0, 60 - len(frame))


def main():
  if len(sys.argv) < 2:
    print(__doc__)
    return 2
  iface = sys.argv[1]
  frame = build("de:ad:be:ef:00:42", "ge-0/0/7", "corridor-sw",
                "10.90.0.254")
  sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW)
  sock.bind((iface, 0))
  # Three of them: one can race the daemon's socket coming up after a
  # reload, and a neighbour that only ever spoke once is not a case
  # worth encoding into the test.
  for _ in range(3):
    sock.send(frame)
  print(f"injected {len(frame)} bytes on {iface} "
        f"(expect 'corridor-sw' in `show neighbors`)")
  return 0


if __name__ == "__main__":
  sys.exit(main())
