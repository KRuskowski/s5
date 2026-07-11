# einheit s5 — 5-Port Managed PoE Switch (product SKU: S501)

A 5-port gigabit **managed switch with PoE PSE**, built for an industrial
DIN-rail box. An Allwinner T113-S3 runs the Linux management plane and drives a
KSZ9477 switch ASIC over the kernel DSA subsystem; the ASIC does line-rate L2
switching, and two TPS23861 controllers source PoE on all five ports. The
simplest member of the `s1xx` Linux-switch family — `s100`'s software retargeted
onto cheaper hardware.

## Where it sits in the einheit switch line

| Project | CPU | ASIC | Framework | Role |
|---|---|---|---|---|
| `s` | RP2040 (bare metal) | KSZ8795 (SPI) | `cli/core` (C11) | Tiny embedded switch, serial CLI only |
| `s100` | Linux | KSZ9567 (DSA) | full `einheit-cli` | Linux switch CLI, 7-port |
| **`s5`** | **T113-S3 (Linux)** | **KSZ9477 (DSA)** | **`einheit-cli` + PoE** | **5-port managed PoE switch** |

s5 is essentially `s100`'s software (DSA switch adapter, einheit-cli) on cheaper
T113-S3 hardware with the 5-port KSZ9477, plus PoE PSE that `s100` doesn't have.

## Hardware — two boards

Split across a **core** board and a **power** board, joined by a coplanar
**Samtec PowerStrip** board-to-board interconnect (54 V PoE bus + MAIN_EN + logic;
no cable). Both on the SKiDL → KiCad netlist flow. **Both are essentially routed.**

### Core board — `hw/core` (4-layer, ~235 parts)
The switch + front I/O. Routed: ~2000 tracks / ~500 vias + copper zones.

| Block | Part |
|---|---|
| Management SoC | Allwinner **T113-S3** (eLQFP128, SiP 128 MB DDR3 — no external DDR routing, which is what makes 4-layer viable) |
| Switch ASIC | **KSZ9477S** (128-TQFP) — 5× GbE PHY + RGMII CPU port (port 6, MAC-to-MAC, no magnetics) |
| PoE PSE | 2× **TPS23861** (4-ch each) — all 5 ports |
| Magnetics + jacks | 5× PoE magnetics (LinkPP LPJG0926HENL) + RJ45 |
| Boot flash | **W25N02JW SPI NAND** (shared SPI0 bus) |
| Misc | watchdog (TPS3823), USB-C (FEL/debug), UART console |

Detail: `hw/core/README.md`, `hw/core/T113S3_PINMAP.md`, `hw/core/BOM_REAL_PARTS.md`.

### Power board — `hw/power` (~60 parts)
Industrial **24 V DC in → PoE + logic rails**. Routed mostly as copper pours.

- **24 V → LM5122 boost → 54 V** (VBUS_54V, the PoE bus)
- **TPS54560 buck → 5 V**, then LDO chain (3.3 / 2.5 / 1.2 / 1.8 / 0.9 V)
- High-current commutation loops laid out per `hw/power/CAP_PLACEMENT.md` (the
  boost output loop and buck input loop are the layout-critical ones).

## Software (`src/`, v0.1.0)

The einheit-cli stack on the **confd/engine management plane**:
- `switch_adapter.cc` — KSZ9477 via kernel DSA (ports directly from `s100`)
- `backend.cc` / `service.cc` — the management binary
- `poe.cc` — PoE port control
- `dsa.cc`, `sys.cc`, `util.cc` — DSA + platform glue

Builds `einheit_s5` (static) + `einheit_launch`. In-process transport, **no ZMQ**
(`EINHEIT_NO_ZMQ=ON`, like s100). Web UI (einheit-ui) planned — the T113-S3 has
the headroom.

### Rootfs
SPI NAND is too small for Debian → **Buildroot/OpenWrt minimal rootfs** (squashfs
in NAND). Different image pipeline from the firewall (T527 + eMMC + Debian).

## Status

- **Software:** v0.1.0, building. Switch adapter (KSZ9477/DSA), backend/service,
  PoE control; migrated to the confd/engine management plane.
- **Hardware:** core board **routed and DRC-clean**; power routed via copper
  pours (16 zones). 3D models aligned for the enclosure. Power board being
  redone (connector change moved the outline).
- **Power-tree audit (datasheet-verified):** all rails checked against
  datasheets — VDD-CORE/VDD-SYS 0.9 V, KSZ core 1.2 V / analog 2.5 V / I/O
  3.3 V, T113 VCC-DRAM fed by internal LDOB (pin 30 → 48/49), DZQ = 240 Ω,
  both PoE controllers' A3 address straps intentional, buck EN/FB/PG correct.
  **No board-killers.**
- **Resolved decisions:** ASIC = KSZ9477 · flash = SPI NAND (W25N02JW) ·
  board interconnect = Samtec PowerStrip (replaced the cable) · mounting M2.5.

### Open hardware items
1. **RGMII 25 MHz clock reference — OPEN.** T113 `RGMII_CLKIN_25M` (pin 125 /
   PG13) is floating; KSZ `SYNCLKO` (pin 95) is only pulled up, not routed to
   it. Needs a source wired (SYNCLKO → CLKIN, or external osc) — under review.
2. **KSZ analog-supply ferrite — OPEN.** AVDDH (2.5 V) / AVDDL (1.2 V) are fed
   straight from the regulators with no ferrite isolation — under review.

### Resolved by the power-tree audit
- T113 exposed pad → GND — **done** (pad 129 = 6.4 mm EP, grounded).
- Unused analog rails (VCC-LVDS/HPVCC/TVOUT/TVIN) — **confirmed safe to leave
  unpowered** per datasheet.

## v1 scope

A managed 5-port gigabit PoE switch configured via CLI (and later web UI):
- Per-port: enable/disable, link status, speed/duplex, statistics, **PoE control**
- VLAN: 802.1Q port-based and tagged
- Port mirroring, MAC table view, basic STP (if KSZ9477 + DSA expose it)

Out of scope for v1: LACP/bonding, advanced QoS, SNMP, routing (that's the
firewall's job).
