# einheit s5 — 5-Port Managed Switch (product SKU: S501)

The simplest member of the `s1xx` Linux-switch family. A 5-port
gigabit managed switch: an Allwinner T113-S3 runs the Linux management
plane and drives a switch ASIC over the kernel DSA subsystem; the
ASIC does line-rate L2 switching. Sibling to `s100` — same software
pattern, cheaper hardware, trimmed to the simplest 5-port config.

## Where it sits in the einheit switch line

| Project | CPU | ASIC | Framework | Role |
|---|---|---|---|---|
| `s` | RP2040 (bare metal) | KSZ8795 (SPI) | `cli/core` (C11) | Tiny embedded switch, serial CLI only |
| `s100` | Linux | KSZ9567 (DSA) | full `einheit-cli` | Linux switch CLI, 7-port |
| **`s5`** | **T113-S3 (Linux)** | **KSZ9477 (DSA)** | **full `einheit-cli` + UI** | **Simplest 5-port managed switch** |

s5 is essentially `s100`'s software retargeted: same DSA-based
switch adapter, same einheit-cli framework, on cheaper T113-S3 hardware
with the 5-port KSZ9477 instead of the 7-port KSZ9567. Much of
`s100/src/` (the DSA driver, switch adapter, sys/util) ports
directly.

## Hardware (v1)

| Part | Choice | Notes |
|---|---|---|
| Management SoC | Allwinner T113-S3 | SiP 128MB DDR3 (no external DRAM routing). RGMII EMAC for the CPU-port link. Not in the data path. |
| Switch ASIC | KSZ9477 (5-port GbE) | Mainline DSA support (`microchip,ksz9477`). Does all line-rate switching. |
| Storage | SPI NOR flash (W25Q128) | On SPI0 bus (shared with KSZ, separate CS). |
| Board | 4-layer | T113-S3's integrated DDR is what makes 4-layer viable (no DDR fly-out). |

### CPU ↔ ASIC link
The T113-S3 configures the KSZ9477 over SPI (register access) and
connects to the switch CPU port (port 6) over RGMII — a MAC-to-MAC
link with no magnetics. The kernel DSA subsystem exposes each switch
port as a netdev (`lan1`..`lan5`). The management plane reads/writes
switch state through DSA — the same path `s100` uses for the KSZ9567.

## Software

Reuses the einheit stack:
- **CLI** — `einheit-cli` adapter (`switch_adapter.cc`) driving the
  switch via DSA. Ports directly from `s100`.
- **Web UI** — `einheit-ui` adapter for port status, VLAN config,
  statistics. (s100 has no UI yet; s5 adds it — the T113-S3 has the
  headroom.)
- **No ZMQ** — local transport, the management binary reads switch
  state in-process (`EINHEIT_NO_ZMQ=ON`, like s100).

### Rootfs strategy
SPI flash is too small for Debian. s5 uses a **Buildroot/OpenWrt
minimal rootfs** (squashfs in flash) — a different image pipeline
from the firewall (T527 + eMMC + Debian). The einheit-cli C++ stack
+ UI assets set the flash floor.

## Open decisions

1. **Flash size + type.** SPI NOR 32-64 MB or SPI NAND 128 MB to
   run the full C++ CLI + web UI comfortably. 16 MB NOR would force
   the C11 core (`cli/core`) and a thin UI — the `s` vs `s100`
   fork, decided by flash size. **Recommendation: SPI NAND 128 MB**
   for headroom + the web UI.
2. **Switch ASIC confirm.** KSZ9477 is the natural 5-port GbE pick
   with mainline DSA. Alternative: RTL8367 (cheaper, weaker DSA
   support). KSZ9477 keeps the s100 software reuse.
3. **T113-S3 flash boot path.** Confirm the T113-S3 BSP boots from SPI NAND
   (vs NOR) — affects the bootloader (U-Boot) config.
4. **Management port.** Management is in-band via the RGMII DSA
   CPU port (port 6). UART console provides out-of-band rescue.

## v1 scope

A managed 5-port gigabit switch an operator configures via CLI and
web UI:
- Per-port: enable/disable, link status, speed/duplex, statistics
- VLAN: 802.1Q port-based and tagged VLANs
- Port mirroring
- MAC address table view
- Basic STP (if the KSZ9477 + DSA expose it)

Out of scope for v1: LACP/bonding, advanced QoS, SNMP, routing
(it's a switch, not a router — that's the firewall's job).

## Status

Project charter. No code yet. Next: confirm the open hardware
decisions (flash, ASIC), then port `s100`'s DSA adapter and add the
einheit-ui adapter.
