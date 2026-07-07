# s5-power — decoupling / bulk cap placement guide

Which component each capacitor must sit next to when laying out `power.kicad_pcb`.
Refs match `netlistscript` (regenerate before cross-checking: `netlist_s5power.net`).

**Two things that bite:**

- **Several caps share the `VIN_24V` net but belong in different places** — do not cluster them. C4 bypasses the boost controller, C13/C14 feed the boost power stage, C17/C18 feed the buck input. Same net, three physical locations, each next to its own consumer.
- **The loop caps (⚠) are the ones that cause fires / fail conducted-EMI if placed loosely.** Place them first and tightest; everything else is secondary.

## ⚠ Critical high-current loops — tightest, place first

| Cap | Value | Place across / next to |
|-----|-------|------------------------|
| **C10, C11, C12** | 10µF/100V | Boost **output** loop: across **Q3 (QH) drain pins 5–8 (VBUS_54V)** and **Q2 (QL) source pins 1–3 (PGND)**. This is the boost commutation loop — keep the Cout→Q3→Q2 area physically tiny. |
| **C17, C18** | 2.2µF | Buck **input** loop: hug **U2 pin 2 (VIN)** and **U2 pin 7 + EP (pin 9, GND)**, forming a tight loop with catch diode **D3**. Single most layout-sensitive cap on the buck. |
| **C13, C14** | 22µF/63V | Boost power-stage **input reservoir** (feeds the inductor). Straddle VIN_24V→GND right at **RS pin 1** (24V side of the sense resistor, = L1's input end); ground into the **PGND pour by Q2 (QL) source**. Closes the loop C13/14→RS→L1→QL→PGND. NOT at the LM5122 chip (that's C4) and NOT at the board entry (that's C2/C3). Boost input current is continuous, so "near RS/L1, grounded to PGND" is enough — not as critical as the C10–C12 output loop. |

## LM5122 boost controller (U1) — housekeeping, hug the pin

| Cap | Value | Near |
|-----|-------|------|
| **C4** | 1µF | U1 **pin 5 (VIN)** → return to pin 9 (AGND) |
| **C5** | 4.7µF | U1 **pin 17 (VCC)** → AGND |
| **C9** | 100nF | Bootstrap: U1 **pin 20 (BST)** → **pin 18 (SW)**; keep the loop to the Q2/Q3 gate area short |
| **C6** | 100nF | U1 **pin 7 (SS)** |
| **C7** | 10nF | U1 **pin 11 (COMP)** |
| **C8** | 100pF | Across U1 **pins 3/4 (CSN/CSP)** — put it right at the pins; route it and the two 100R sense resistors as a Kelvin pair back across **RS**. |

## TPS54560 buck (U2)

| Cap | Value | Near |
|-----|-------|------|
| **C15** | 100nF | Bootstrap: U2 **pin 1 (BOOT)** → **pin 8 (PH)** |
| **C16** | 15nF | U2 **pin 6 (COMP)** |
| **C19, C20** | 22µF | Buck **output**: at **L2 output (VDD_5V)**; return near U2 GND |

## Input front-end & LDO

| Cap | Value | Near |
|-----|-------|------|
| **C1** | 10µF | **Before** ferrite FB1 (VIN_PROT) — by Q1 drain |
| **C2** | 10µF | **After** ferrite FB1 (VIN_24V) |
| **C3** | 100µF | Bulk at input — near FB1 output / J1 |
| **C21** | 10µF | AMS1117 **U3 pin 2 (VO / VDD_3V3)** output |

## General rule

Every GND-return cap wants its own stitching via straight down to the ground plane, right at the cap pad — never a long shared ground trace back to the IC. Loop-critical caps go on the same layer as their IC, hugging the pin. Bulk caps (C3, C10–C12) may sit a little further out.

## Open item

**U3 (AMS1117) has no dedicated input cap** on pin 3 (VI / VDD_5V) — it currently relies on the buck output caps C19/C20 upstream. If U3 lands any distance from them, add a 1µF at U3 pin 3.
