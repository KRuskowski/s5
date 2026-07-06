# s5 Hardware — 5-Port Managed Switch Board

KiCad + SKiDL hardware for the s5 (S501) switch.

## Generating the netlist

```sh
export KICAD_SYMBOL_DIR=/usr/share/kicad/symbols
export KICAD8_SYMBOL_DIR=/usr/share/kicad/symbols
python netlistscript          # writes netlist_s5.net
```

Import the result into KiCad: File -> Import -> Netlist.

## What's on the board

| Block | Part | Notes |
|---|---|---|
| Management SoC | **Allwinner T113-S3** (eLQFP128, SiP 128MB DDR3) | No external DRAM routing. Real pin numbers from datasheet v1.6 (see `T113S3_PINMAP.md`). |
| Switch ASIC | KSZ9477S (128-TQFP) | 5 copper GbE PHY ports + RGMII CPU port (port 6). Mainline DSA (`microchip,ksz9477`). |
| CPU-port link | T113 EMAC (RGMII, PG bank) -> KSZ port 6 | MAC-to-MAC, no magnetics, frees all 5 copper ports for users. |
| 5x GbE ports | Discrete GbE magnetics (YDS 30F-51NL) + plain RJ45 | Full 1000BASE-T (4 pairs/port). |
| PoE PSE | 2x TPS23861 (4ch each), all 5 ports | 48V DC in, N-FET + sense per port. |
| Power input | 48V DC -> TPS54560 buck (5V) | LDO chain: 3.3/2.5/1.2V (KSZ) + 0.9V buck (T113 core) + 1.8V LDO (DRAM). |
| Boot flash | W25Q128 SPI NOR (SOIC-8) | Shared SPI0 bus; flash CS on PC3, KSZ CS via GPIO. |
| Watchdog | TPS3823-33 (SOT-23-5) | RST -> T113 RESET, WDI from T113 GPIO. |
| Console | 3-pin UART header | T113 UART0 (PE2/PE3). |
| USB | USB-C (GCT USB4110) | Device mode for debug/FEL. |
| Stackup | **4-layer** | No external DDR3 = no high-speed signal layer needed. |

## Key design files

| File | Description |
|---|---|
| `netlistscript` | SKiDL connection script (the source of truth) |
| `netlist_s5.net` | Generated netlist (0 errors) |
| `T113S3_PINMAP.md` | T113-S3 pin map extracted from datasheet v1.6 |

## Open TODOs (marked in-script)

1. T113 eLQFP128 **exposed pad** -> GND is mandatory (only one
   dedicated GND pin: AGND=91). Add EP in the footprint.
2. RGMII CLKIN 25MHz reference source (KSZ SYNCLKO or crystal).
3. Unused analog rails (VCC-LVDS/HPVCC/TVOUT/TVIN) — confirm
   per datasheet §5.3 whether they may be left unpowered.
4. AVCC filtering (ferrite bead recommended per datasheet).
5. T113 package footprint (`eLQFP128_14x14x1.4mm`) — confirm
   or create.
