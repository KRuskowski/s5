# s5 — Real-parts BOM (JLCPCB assembly)

Selection rule (per Karl): **availability/longevity first, cost second.**
Committed parts (ICs, LDOs, connectors, specific-footprint FETs, relay, opto)
= name-brand + AEC-Q where possible, stocked on **JLC and Digikey/Mouser**.
Jellybean passives = JLC Basic, interchangeable (EOL risk ≈ 0).

Status legend: ✅ confirmed on JLC · 🔎 verify C# / live stock on JLC · ✏️ needs footprint download · ⚠️ design decision below.

## Committed actives / ICs
| Ref | Function | Recommended part | LCSC | Package | Notes |
|-----|----------|------------------|------|---------|-------|
| U18 | standby 3V3 LDO (24→3.3V) | **TI TPS7B6933QDCYRQ1** | C108471 | SOT-223-4 | ✅ AEC-Q100, 40V, 150mA, low-Iq. No EN (always-on). |
| U20 | PoE-logic 3V3 LDO (24→3.3V) | **TI TPS7B6933QDCYRQ1** | C108471 | SOT-223-4 | ✅ same P/N as U18. ⚠️ PoE-shed moves to PSE SHTDWN (see notes) so no EN needed. |
| Q50 | reverse-polarity P-FET | **Infineon BSP315P** (alt onsemi NVF2955) | 🔎 | SOT-223 | -60V, ~55mΩ. Matches current footprint. |
| Q60 | relay-coil driver | **2N7002** (Nexperia/onsemi) | JLC Basic | SOT-23 | logic-level, 60V/300mA; coil is ~6mA. |
| U21 | DI opto-isolator | **EL357N(C)** or LTV-357T | JLC Basic | SOP-4 (SMD) | ✏️ was PC817 DIP — use SMD for JLC. |
| K1 | fault relay | **Hongfa HFD4/24-S** | C190593 | SMD, DPDT | ✅ 24V coil (~6mA), 2A Form-C; use one pole. ✏️ redefine pins to footprint. |
| U10 | STM watchdog | TI **TPS3823-33** | 🔎 | SOT-23-5 | already real. |
| U15/U16 | temp sensors | ADI **DS18B20** | 🔎 | TO-92 or SOIC-8 | already real. Pick SMD (SOIC) for JLC if hot-spot allows. |
| U17 | humidity | Sensirion **SHT40-AD1B** | 🔎 | DFN-4 | already real (extended). |
| U19 | supervisor MCU | ST **STM32G071CBT6** | 🔎 | LQFP-48 | already real. |
| (U1/U6/U8/U9/U11/U12/U14/U7) | KSZ9477 / TPS54560 / W25N02JW / TPS23861 / TLV62569 / AP2112 / LM5122 / T113-S3 | already real MPNs | 🔎 | — | verify each is JLC-stocked (some may be hand-add). |

## Connectors
| Ref | Function | Recommended part | LCSC | Notes |
|-----|----------|------------------|------|-------|
| J1 | power in (bottom) | Phoenix **MSTB 2,5/3-GF-5,08** | 🔎✏️ | industrial standard; may be JLC hand-add (THT). |
| J11 | signal (top) | Phoenix **MC 1,5/5-GF-3.5** | 🔎✏️ | 3.5mm — can't cross-plug with power. |
| J8 | microSD (non-edge) | **Molex 5033981892** (push-push) | C428492 | ✅ self-contained, not edge. ✏️ swap from 104031, remap pads. |
| J_USB | USB-C (vertical, internal) | **vertical 16P USB-C** | C9900273287 | ✅ upright, USB2. ✏️ swap from horizontal GCT, remap pads. |
| J2–J6 | 5× RJ45 PoE (front) | **LinkPP LPJG0926HENL** | 🔎 | already chosen. |
| J10 | SWD header | 2×3 1.27mm | JLC Basic | internal. |

## Protection / passives
| Ref | Function | Recommended | Notes |
|-----|----------|-------------|-------|
| F1 | input fuse | 2A slow-blow 1206 (Bel/Littelfuse) | 🔎 or PPTC. |
| RV1 | MOV | ⚠️ **DNP** unless cert demands — the TVS covers normal surge at 24V. |
| RT1 | inrush NTC | ⚠️ likely **DELETE** — TPS54560/LM5122 soft-start, 24V inrush is modest. |
| FB1 | ferrite bead | Sunlord/Murata 1206, 600Ω@100MHz, ≥2A | JLC Basic. |
| D50 | VIN TVS (uni) | **SMBJ30A** (28.9V standoff, clamps <48V) | JLC. |
| D55/D57 | signal TVS (bidir) | **SMBJ33CA** | JLC. |
| D51 | Vgs clamp zener | **BZX84C12** | JLC Basic. |
| D54/D56 | flyback/reverse | **1N4148W** (SOD-123) | JLC Basic. |
| D52/D53 | watchdog Schottky | **BAT54** | JLC Basic. |
| TS1 | over-temp thermal switch | **KSD9700-class, ~70°C NO** | ⚠️ mechanical/THT — likely hand-add, not JLC SMD. |

## Design decisions to confirm
1. **LDO thermal → SOT-223, PoE-shed via SHTDWN.** 24→3.3V @ ~0.4W is too hot for SOT-23-5 in a sealed box; SOT-223 (with copper) is fine. TPS7B69 only has EN in the SOT-23-5 body, so instead of gating U20's EN for PoE load-shed, the STM drives the **TPS23861 SHTDWN pins** (they're currently strapped high). Cleaner, cooler, one LDO P/N for U18+U20.
2. **Cap voltage derating:** VIN_24V caps ≥ 50V, VBUS_48V caps ≥ 100V. Set these when picking the bulk caps.
3. **Drop RT1 (NTC)** and **DNP RV1 (MOV)** unless you specifically want them.
4. **THT/mechanical parts** (Phoenix terminals, fuse, thermostat) — confirm JLC assembles them or plan to fit them yourself.

## Workflow
Netlist part-defs (pin numbering) get finalized **once footprints are downloaded**,
because the Part() pin numbers must match each footprint's actual pads. Download
order of priority: microSD (Molex 5033981892), vertical USB-C, relay (HFD4/24-S),
LDO SOT-223-4, P-FET SOT-223 (standard, may already be in KiCad).
