# s5 — JLCPCB assembly cross-reference (both boards)

Cross-referenced against the JLC/LCSC assembly library on **2026-07-12**.
**Stock is point-in-time — re-confirm every C# in the JLC cart before ordering.**
Selection rule (per Karl): availability/longevity first, cost second.

Two buckets: **JLC must place** (fine-pitch / leadless — infeasible by hand) and
**hand-add** (connectors, magnetics, THT — Karl self-sources & solders).

---

## ⚠️ Silent killers — verify these BEFORE ordering

| Item | Board | Problem |
|---|---|---|
| **W25N02JW flash U8** | core | Footprint is `SOIC-8_3.9x4.9mm` (150-mil narrow), but the stocked `W25N02JWZEIF` (C2962014) is **WSON-8-EP** — and standard 2 Gb SPI-NAND SOIC is 208-mil *wide* anyway. **Footprint almost certainly wrong.** Also OOS / pre-order. |
| **PMEG10010 D4** | power | Footprint `SOD-123F`, but stocked part (C179426) is **SOD-123W**. Verify land pattern. |
| **Osram M676 LEDs D3–D6** | core | Not in JLC library. Everlight 19-21x substitutes use a **smaller 0603 land that won't match** the Mini-TOPLED footprint → hand-solder Osram, or change footprint to a JLC LED. |
| **Q1–Q5 N-FET** | core | Netlist has a **generic placeholder** ("NMOS", SOT-223). Pick a real part: **DMN3032LE-13 = C156338** (30 V, logic-level, Extended). |

---

## CORE board

### JLC must place (SMT — don't hand-solder)

| Ref | Part | LCSC# | Lib | Note |
|---|---|---|---|---|
| U1 | KSZ9477STXI | **C631685** | Ext | stock ~26 — pre-order |
| U7 | T113-S3 | **C5197687** | Ext | in library ✓ (~2758) |
| U9,U13 | TPS23861PW | **C2872514** | Ext | OOS at LCSC; need ×6 — pre-order |
| U11 | TLV62569PDDCR | **C398365** | Ext | clean |
| U19 | STM32G071CBT6 | **C432212** | Ext | clean |
| U8 | W25N02JWZEIF | **C2962014** | Ext | ⚠️ package/footprint (above) + OOS |
| U10 | TPS3823-33DBVR | **C7719** | Ext | clean |
| U18,U20 | TPS7B6933QDCYRQ1 | **C108471** | Ext | verified ✓ |
| U3 | AP2112K-2.5 | **C176945** | Ext | stock ~230 |
| U4 | AP2112K-1.2 | **C460310** | Ext | clean |
| U12 | AP2112K-1.8 | **C176944** | Ext | clean |
| U17 | SHT40-AD1B-R3 | **C2848306** | Ext | DFN, confirm stock |
| U21 | EL357N(C) | **C29981** | Basic | confirm live stock |
| D54,D56 | 1N4148W (SOD-123) | **C81598** | Basic | huge stock ✓ |
| D2 | 1N4148 (SOD-323) | **C2128** | Basic | LCSC OOS → alt C9900014362 |
| D52,D53 | BAT54 (SOD-323) | **C22629** | Ext | no Basic in SOD-323 |
| D55,D57 | SMBJ33CA (SMB) | **C83325** | Ext | |
| FB1,FB2 | Ferrite 220Ω@100MHz (Murata BLM18SG221) | **C88988** | Pref | **use this, not a JLC-Basic ferrite** (those are ~200 mA/380 mΩ = fail spec). Alt TDK C76815 |
| Y1 | 25 MHz xtal (NDK NX2016SA) | **C843258** | Pref | stock ~15 — pre-order/alt |
| Y2 | 24 MHz xtal | **C2682776** | Pref | clean |
| Y3 | 32.768 kHz xtal | **C97606** | Pref | 12.5 pF; lower-CL sib C97605/C97604 |
| Q60 | 2N7002 | **C8545** | Basic | std-threshold — use 2N7002K if driven at 3.3 V |
| Q1–Q5 | N-FET SOT-223 → **DMN3032LE-13** | **C156338** | Ext | replaces placeholder (above) |
| SW2 | tactile KMR231GLFS | **C99271** | Basic | optional-JLC |
| J8 | microSD 104031-0811 | **C585350** | Ext | optional-JLC |

### Hand-add (Karl self-sources)

| Ref | Part | Note |
|---|---|---|
| J9 | Samtec MPT-06-01 PowerStrip | LCSC-catalog only (C3684105), THT — not assemblable |
| J11 | Phoenix MC 1,5/5-GF-3,5 | THT, not in placement lib |
| J12 | Molex USB-C 2171820001 | not in LCSC/JLC at all; sibling 2171800001 = C3197940 if you want JLC to place |
| J2–J6 | LinkPP LPJG0926HENL magjacks | JLC *can* wave-solder (C2874230, Ext) if desired; otherwise hand |
| U15,U16 | DS18B20 (TO-92) | THT; SMT alt = DS18B20Z SOIC-8 **C97190** for a clean line |
| K1 | Hongfa HFD23-024-1ZS relay | **C32381** (THT; BOM's old C190593 was wrong). OOS at check |
| D3–D6 | Osram M676 LEDs | not in JLC (footprint issue above) |
| J7,J10 | pin headers (2.54 / 1.27 SWD) | J7=C49257; J10 1.27 2x3 SMD SKU unverified |
| LP1, MH1–6, TS1 | light guide / mounts / thermostat | mechanical, not placed |

---

## POWER board

### JLC must place (SMT)

| Ref | Part | LCSC# | Lib | Note |
|---|---|---|---|---|
| U1 | LM5122MHX | **C77241** | Ext | HTSSOP-20-EP |
| U2 | TPS54560DDAR | **C31966** | Ext | SOIC-8-EP |
| U3 | AMS1117-3.3 | **C6186** | **Basic** | only Basic part on this board |
| Q2,Q3 | BSC057N08NS3G | **C534354** | Ext | PowerPAK SO-8, 80 V |
| Q1 | SQJ409EP | **C727776** | Ext | verified ✓ PowerPAK SO-8L |
| D2 | BZX84C12 | **C112551** | Ext | |
| D4 | PMEG10010ELRX | **C179426** | Ext | ⚠️ SOD-123W vs footprint (above) |
| D1 | SMBJ33A (SMB) | **C78419** | Ext | |
| D3 | SS56 (SMC) | **C2848695** | Ext | note SS56 is often SMA; this is the SMC listing |
| FB1 | Coilcraft XAL1010-222MED | **C5125746** | Ext | verified ✓; stock ~287 — pre-order |

### Needs a decision / hand-add

| Ref | Part | Note |
|---|---|---|
| L1 | Bourns SRP1245A-220M | **not in JLC** → swap to SRP1265A-220M **C2041465** (bigger 13.5×12.5 footprint) or hand-solder |
| F1 | Littelfuse 0452012 (Slo-Blo 12 A) | **not in JLC** → hand-solder, or fast-acting 0451012 **C99549** (changes inrush) |
| RV1 | MOV S14K30 | **DNP** (JLC skips); THT if ever populated (C210626) |
| J2 | Samtec MPS-06-01 PowerStrip | not in lib, THT — hand-solder |
| J1 | Phoenix MSTB 2,5/3-GF-5,08 | not in lib, THT — hand-solder |
| MH1–4 | M2.5 mounts | mechanical |

---

## Order-time checklist
1. **Fix the 3 footprints** (flash, PMEG, LEDs) or move those parts to hand-add.
2. **Pick Q1–Q5** (DMN3032LE-13 C156338) in the netlist.
3. **Pre-order thin stock:** KSZ (26), TPS23861 (OOS), W25N02 (OOS), AP2112K-2.5 (230), 25 MHz xtal (15), XAL1010 (287).
4. **Ferrites = C88988** (not a Basic ferrite).
5. Budget **per-feeder Extended fees** — almost the whole BOM is Extended; only AMS1117, a few diodes, 2N7002, EL357N, KMR2 button are Basic.
6. Everything in the hand-add tables: self-source & solder as planned.
