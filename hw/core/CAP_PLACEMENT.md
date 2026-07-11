# s5-core — decoupling / bulk cap placement guide

Where every capacitor on `s5.kicad_pcb` must sit. Refs match `netlistscript`
(regenerate before cross-checking: `python netlistscript` → `netlist_s5.net`).

**The one rule that covers 80% of these:** a *decoupling* cap belongs **next to
the power pin it serves, on the same side, with the shortest possible via to the
plane** — not clustered near the regulator. The `decouple(rail, gnd, N)` calls in
the script make **N identical caps on a rail on purpose: one per power pin.**
Select all caps on a net in KiCad and spread them one-per-pin around the IC.

*Bulk* caps (the bigger 10/22/100 µF) sit at the **rail source** (the converter
output or the interconnect entry), not at the loads.

---

## 1. Interconnect entry — at the J9 PowerStrip blades

The core board is fed from the power board over the 6-blade J9. Put one entry
bulk cap right at each incoming blade:

| Cap | Value | Place at |
|---|---|---|
| **54 V bulk** | **10 µF / 1210, ≥100 V** | J9 blade 1 (**VBUS_54V**). ⚠️ **Must be a 100 V part** — do NOT substitute a 0805/50 V cap. |
| VIN_24V | 10 µF / 1206 | J9 blade 3 |
| VDD_5V | 10 µF / 1206 | J9 blade 4 |
| VDD_3V3 | 10 µF / 1206 | J9 blade 5 |

---

## 2. Point-of-load converters — tight in/out caps at each

Hug these to the regulator; the buck loop is the layout-sensitive one.

| Converter | Caps | Place |
|---|---|---|
| **0.9 V buck (TLV62569)** — T113 VDD-CORE | Cin, Cout, + the inductor | **Tightest.** Cin across VIN(5)+GND(2); Cout at the output; keep the SW→L→Cout loop small. |
| **U3 AP2112K-2.5** (3.3→2.5, KSZ analog) | Cin + Cout | Cin at VIN, Cout at VOUT, both next to U3. |
| **U4 AP2112K-1.2** (3.3→1.2, KSZ + T113) | Cin + Cout | at U4. (Combined 1.2 V load is near the 600 mA limit — watch it.) |
| **AP2112K-1.8** (3.3→1.8, T113 PLL/RTC/DRAM) | Cin + Cout | at that LDO. |
| Per-rail bulk | 10 µF / 0805 each | one at each rail's source |
| VDD_5V | 10 µF + 22 µF / 0805 | at the 5 V entry / buck input |

---

## 3. KSZ9477S (U1) — one cap per power pin

The 128-TQFP has power pins on all four sides. **Do not cluster** — one cap per pin.

| Group | Count | Place |
|---|---|---|
| **VDD_1V2 (core)** | **20 × 100 nF** | **one next to each KSZ 1.2 V pin.** Most numerous + most important — this is the switch core. |
| VDD_2V5 (analog) | 9 × 100 nF | one per KSZ 2.5 V (AVDDH/VDDHS) pin |
| VDD_3V3 (I/O) | 3 × 100 nF | one per KSZ VDDIO pin |
| 25 MHz crystal | 2 × 15 pF | one per XI/XO leg, next to the crystal; keep the crystal loop tiny |
| Reset RC | 10 µF / 0805 | at the KSZ reset pin (RC delay) |

---

## 4. T113-S3 (U7) — one cap per power pin

The eLQFP128 also has power pins on all sides + the exposed pad → GND.

| Group | Count | Place |
|---|---|---|
| **VDD_3V3 (I/O banks)** | **12 × 100 nF** | one per T113 VCC-IO 3.3 V pin |
| **VCORE 0.9 V** (VDD-CORE + VDD-SYS) | 6 × 100 nF | one per core pin. ⚠️ VDD-SYS is 0.9 V, **not** 3.3 V (the old s5 had it on 3.3 = dead chip) |
| VDD_1V8 (PLL/RTC/AVCC/DDR-ctrl) | 4 × 100 nF | one per 1.8 V analog pin. ⚠️ these are 1.8 V, **not** 3.3 V |
| Internal LDO out | 2 × 1 µF | at the T113 LDO-A / LDO-B output pins |
| 24 MHz crystal | 2 × 18 pF | one per XIN/XOUT, next to the crystal |
| 32.768 kHz RTC | its load caps | next to the RTC crystal |
| Bulk | 10 µF / 0805 | near the T113 power entry |

---

## 5. TPS23861 PoE PSE (2×, U11/U12) — TSSOP-28

| Cap | Place |
|---|---|
| VDD decouple (100 nF + 1× on **vpse33**, always-on PoE rail) | at pin 1 (VDD) of each controller |
| VPWR bulk (**VBUS_54V**) | near pin 28 (VPWR) — the 54 V feed to the port FETs |

---

## 6. RJ45 MagJacks (5×, J2–J6, LPJG0926HENL)

| Cap | Count | Place |
|---|---|---|
| Center-tap caps (100 nF) | **2 per port** (CT1, CT2) | at each magjack's center-tap pins — the Bob-Smith termination. VC1/VC2 (pins 11/12) tie to the 54 V PSE feed. |

---

## Priority order when placing

1. **0.9 V buck loop** (Cin/Cout/L) — fires or fails EMI if loose.
2. **VBUS_54V 100 V entry bulk** — right part, right at the blade.
3. **KSZ 1.2 V** and **T113 core/1.8 V** decoupling — one-per-pin, shortest vias.
4. Everything else (2.5 V, 3.3 V decoupling, magjack CT caps) — one-per-pin, but less critical.
