# T113-S3 Pin Map — verified from datasheet v1.6

Extracted from the Allwinner T113-S3 datasheet v1.6 (§4.2 Pin
Characteristics, §4.3 GPIO Multiplex Function). Package eLQFP128,
14×14×1.4 mm. Use these ball numbers for the netlist SoC block —
they are real, not placeholders.

**Critical:** only ONE dedicated ground pin (AGND = 91). The part
relies on the **exposed thermal pad = GND** for the main ground
return. The EP→GND connection is not optional.

## Power / ground / clock / reset

| Function | Signal | Ball# | Rail |
|---|---|---|---|
| Core | VDD-CORE0 / VDD-CORE1 | 116, 117 | 0.9 V (DCDC) |
| System | VDD-SYS0/1/2 | 46, 51, 81 | 3.3 V |
| IO | VCC-IO | 83 | 3.3 V |
| GPIO bank PE | VCC-PE | 34 | 3.3 V |
| GPIO bank PD | VCC-PD | 66 | 3.3 V |
| GPIO bank PG | VCC-PG | 128 | 3.3 V |
| PLL | VCC-PLL | 20 | 3.3 V |
| RTC | VCC-RTC | 26 | 3.3 V |
| DRAM I/O | VCC-DRAM0/1, VDD18-DRAM | 48, 49, 50 | 1.8 V |
| DRAM ZQ cal | DZQ | 47 | (240 Ω to GND) |
| Analog | AVCC | 89 | 3.3 V (filtered) |
| Internal LDO | LDO-IN / LDOA-OUT / LDOB-OUT | 29 / 28 / 30 | in 3.3 V, outs bypass |
| Ground | AGND | 91 | + **EP = GND** |
| 24 MHz osc | DXIN / DXOUT | 23 / 22 | DCXO |
| (ref clk out) | REFCLK-OUT | 21 | |
| 32.768 kHz | X32KIN / X32KOUT | 25 / 24 | RTC |
| Reset | RESET (I/OD) | 27 | VCC-RTC domain |
| Boot select | BOOT-SEL0 (PC4) / BOOT-SEL1 (PC5) | 17 / 16 | strap |
| NC | NC0 | 106 | leave open |

Unused analog/video rails (audio codec, TV out/in, LVDS) — HPVCC
(97), VCC-TVOUT (77), VCC-TVIN (107), VCC-LVDS (65): confirm
per-module whether they may be left unpowered when the module is
unused (most can; verify in §5.3 Recommended Operating Conditions).

## RGMII → KSZ9477 port 6 (PG group, mux Function4)

Full RGMII in one contiguous bank — this is the CPU/management link
that frees all 5 copper PHYs for users.

| RGMII signal | Pin | Ball# |
|---|---|---|
| RXCTL | PG0 | 120 |
| RXD0 | PG1 | 118 |
| RXD1 | PG2 | 119 |
| TXCK | PG3 | 121 |
| TXD0 | PG4 | 123 |
| TXD1 | PG5 | 122 |
| TXD2 | PG6 | 1 |
| TXD3 | PG7 | 2 |
| RXD2 | PG8 | 3 |
| RXD3 | PG9 | 4 |
| RXCK | PG10 | 5 |
| TXCTL | PG12 | 124 |
| CLKIN (RX clk in) | PG13 | 125 |
| MDC | PG14 | 126 |
| MDIO | PG15 | 127 |

(RMII is also available on this bank if a 100 M link is ever
preferred: PG0=CRS-DV, PG1/2=RXD0/1, PG3=TXCK, PG4/5=TXD0/1,
PG12=TXEN, PG13=RXER.)

## SPI0 → KSZ9477 management + boot flash (PC group, Function2)

| SPI0 signal | Pin | Ball# |
|---|---|---|
| CLK | PC2 | 19 |
| CS0 | PC3 | 18 |
| MOSI | PC4 | 17 |
| MISO | PC5 | 16 |
| WP | PC6 | 15 |
| HOLD | PC7 | 14 |

Shared bus: SPI0-CS0 (PC3) → boot flash. The KSZ9477 needs its own
chip-select — assign a spare GPIO (e.g. a PB or PD pin) as KSZ_CS,
same pattern as v3.

## TWI0 → TPS23861 PoE controller (PB group, Function4)

| TWI0 signal | Pin | Ball# |
|---|---|---|
| TWI0-SDA | PB2 | 86 |
| TWI0-SCK | PB3 | 85 |

## UART0 debug console (PE group, Function6)

| UART0 signal | Pin | Ball# |
|---|---|---|
| UART0-TX | PE2 | 35 |
| UART0-RX | PE3 | 33 |

## Notes for the netlist

- Footprint: reuse v3's `LQFP-128_14x14mm_P0.4mm` + add the exposed
  pad (EP → GND). Confirm body/pitch matches eLQFP128 from §7.2.
- Power tree changes from v4 (H3): drop the external DDR3 (+1.5 V
  rail) and the SY8106A DVFS buck. The T113 core is a 0.9 V DCDC
  (VDD-CORE 116/117); add a 1.8 V rail for VDD18-DRAM; reuse v4's
  3.3 V for the SYS/IO/PE/PD/PG/PLL/RTC domains.
- Source: Allwinner T113-S3 datasheet v1.6, §4.2 / §4.3.
