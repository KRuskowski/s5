# s5-core — decoupling / bulk cap placement guide

Where every capacitor on `s5.kicad_pcb` goes, **named**. Refs match
`netlist_s5.net` (regenerate before cross-checking: `python netlistscript`).

**The one rule:** a *decoupling* cap goes **next to the power pin it serves**,
one per pin, shortest via to the plane — not clustered at the regulator. Groups
below with many caps on one rail = **one per pin of that IC.** *Bulk* caps (the
bigger 10 µF) go at the **rail source** (converter output / interconnect blade).

Cap ref ranges are from the netlist; the KSZ-vs-T113 split on the shared 3.3 V
rail is by creation order (KSZ section builds before T113).

---

## 1. Interconnect entry — at the J9 PowerStrip blades

| Rail | Caps | Value | Place |
|---|---|---|---|
| **VBUS_54V** | **C1** | 10 µF / 1210 **≥100 V** | J9 blade 1. ⚠️ must be a 100 V part |
| VIN_24V | C2, C10 | 10 µF | J9 blade 3 |
| VDD_5V | C3, C5, C11 | 10/22 µF | J9 blade 4 / 5 V input |
| VDD_3V3 (entry) | C4, C12 | 10 µF | J9 blade 5 |

## 2. Point-of-load converters — in/out caps tight to each

| Converter | Caps | Place |
|---|---|---|
| 0.9 V buck **U11** (TLV62569) | **C6, C7** (22 µF out) · **C15, C90** (10 µF) + inductor | all four sit AT U11 (top-right strip, X≈186-192 / Y≈72), NOT at the T113. Tightest loop — Cin at VIN, SW→L→Cout small |
| U3 AP2112K-2.5 (KSZ 2.5 V) | **C13** (10 µF out) | at U3 |
| U4 AP2112K-1.2 (KSZ+T113 1.2 V) | **C14** (10 µF out) | at U4 |
| AP2112K-1.8 (T113 1.8 V) | **C8, C9, C16** (10 µF) | at the LDO |
| 3.3 V rail bulk | **C91** (10 µF) | bulk on VDD_3V3 near the LDO cluster (U3/U4/U12, X≈171/Y≈87) / where VDD_3V3 lands from J9 — not pin-critical |
| microSD VDD | **C95** (100 nF) | at **J8** microSD VDD pin (X≈175/Y≈104) — SD-card 3.3 V decoupling |
| flash VCC | **C96** (100 nF) | at **U8** W25N02 flash pin 8 VCC (X≈162/Y≈124) — tight |

## 3. KSZ9477S (U1) — one cap per power pin

| Rail | Caps | Value | Place |
|---|---|---|---|
| **VDD_1V2** (core) | **C26–C45** (20) | 100 nF | **one next to each KSZ 1.2 V pin** — the big one |
| VDD_2V5 (analog) | **C17–C25** (9) | 100 nF | one per KSZ 2.5 V (AVDDH/VDDHS) pin |
| VDD_3V3 (I/O) | **C46, C47, C48** | 100 nF | one per KSZ VDDIO pin |
| 25 MHz crystal | **C49, C50** | 15 pF | XI / XO legs, tight to the crystal |
| Reset RC | **C51** | 10 µF | at the KSZ reset pin |

## 4. T113-S3 (U7) — one cap per power pin

| Rail | Caps | Value | Place |
|---|---|---|---|
| **VDD_3V3** (I/O banks) | **C68–C79** (12) | 100 nF | one per T113 VCC-IO 3.3 V pin |
| **VDD_CORE 0.9 V** (VDD-CORE + VDD-SYS) | **C80–C85** (6) | 100 nF | one per core pin. ⚠️ VDD-SYS is 0.9 V, not 3.3 |
| VDD_1V8 (PLL/RTC/AVCC/DDR-ctrl) | **C86–C89** (4) | 100 nF | one per 1.8 V analog pin. ⚠️ not 3.3 V |
| Internal LDO out | **C66, C67** | 1 µF | at T113 LDO-A / LDO-B output pins |
| 24 MHz crystal | **C92, C93** | 18 pF | XIN / XOUT, tight to the crystal |
| Reset | **C94** | 100 nF | at the T113 reset pin |

## 5. TPS23861 PoE PSE (U11 / U12)

| Rail | Caps | Place |
|---|---|---|
| **VDD_3V3_PSE** (vpse33, always-on) | **C62, C64, C100, C101** | 2 per controller at the VDD pin (1) |
| **VBUS_54V** (VPWR) | **C63, C65** (100 nF) | near each controller's VPWR pin (28). ⚠️ 100 nF but on the 54 V rail — must be **≥100 V** parts |

## 6. Standby / PoE-logic 3.3 V (always-on)

| Rail | Caps | Place |
|---|---|---|
| VDD_3V3_SB | **C97–C99, C102–C105, C107–C109** (10) | at the standby 3V3 LDO (U18/U20) + the PoE-logic loads |

## 7. RJ45 MagJacks (J2–J6) — center-tap caps

| Port | Caps | Place |
|---|---|---|
| P1 | C52, C53 | at MagJack 1 center-tap pins (Bob-Smith) |
| P2 | C54, C55 | at MagJack 2 |
| P3 | C56, C57 | at MagJack 3 |
| P4 | C58, C59 | at MagJack 4 |
| P5 | C60, C61 | at MagJack 5 |

## 8. Misc

| Cap | Net | Place |
|---|---|---|
| C106 | MCU_NRST | watchdog / reset RC |
| C110 | PE | earth Y-cap (protective earth) |

---

## Priority order when placing

1. **0.9 V buck loop** (C6/C7/C15 + L) — fires or fails EMI if loose.
2. **VBUS_54V bulk C1** — 100 V part, right at the blade.
3. **KSZ 1.2 V (C26–C45)** and **T113 core (C80–C85) / 1.8 V (C86–C89)** — one per pin, shortest vias.
4. Everything else (2.5 V, 3.3 V decoupling, magjack CT caps) — one per pin, less critical.
