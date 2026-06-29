# s105 Hardware — 5-Port Managed Switch Board

KiCad + SkiDL hardware for the s105 (S501) switch. Ported from
`f.appliance` (the firewall carrier), reusing the KSZ9477, the 5 GbE
MagJacks, the power tree, and SPI management.

## Generating the netlist

```sh
export KICAD_SYMBOL_DIR=/usr/share/kicad/symbols
export KICAD8_SYMBOL_DIR=/usr/share/kicad/symbols
python netlistscript          # writes netlist_s105.net
```

(Uses the f.appliance SkiDL venv, or any venv with `skidl`
installed. Import the result into KiCad: File → Import → Netlist.)

## What's on the board

| Block | Part | Source |
|---|---|---|
| Switch ASIC | KSZ9477S (128-TQFP) | reused from f.appliance |
| 5× GbE ports | HR911130C MagJacks | reused |
| Power tree | 5V → 3V3 → 2V5 → 1V2 (AMS1117) | reused |
| Power input | 5V barrel jack | new (replaces f.appliance USB-C PD) |
| Management SoC | Allwinner V3c | **new — pinout TODO** |
| Rootfs storage | SPI NAND 128 MB (W25N01-class) | new |
| CPU-port link | V3c EMAC ↔ KSZ9477 CPU port (RMII, 100M) | new |
| Console | 3-pin UART header | new |

## Removed from the f.appliance design

- **LAN7431** PCIe-to-RGMII NIC — the firewall used it to bridge the
  switch into the host's data path. The s105 switch has no host data
  path; the V3c is management-only.
- **PCIe FPC connector** — went with the LAN7431.
- **Cubie A7S mount** — replaced by the on-board V3c.
- **USB-C PD (STUSB4500)** — a switch is adapter-powered; barrel
  jack is simpler.

## ⚠️ Before this netlist is trustworthy

The **V3c pin numbers are placeholders** (`P_xxx`). The net wiring is
complete and correct — SPI0 to the KSZ9477, SPI1 to the flash, RMII
to the CPU port, crystal, reset, UART, power — but the V3c-side pin
assignments are NOT real. They were deliberately not invented;
fabricating pin numbers is how a board silently fails at assembly.

To finish:
1. Get the V3c datasheet pinout. Replace every `P_xxx` in the
   `### NEEDS V3c DATASHEET` section with real ball/pin designators.
2. Confirm the V3c core voltage and the integrated-DDR rail voltage;
   add the missing core + DRAM regulators (only 3V3/2V5/1V2 for the
   switch exist today).
3. Add the V3c package footprint (`footprint='TODO:V3c_QFP'`).
4. Confirm the KSZ9477 CPU-port RMII pin map against DS00002392C
   (port-6 MAC group) — the RMII pins on U1 are a best-effort from
   the port-6 group and should be checked.
5. Confirm the SPI NAND part + the V3c BSP boots from SPI NAND
   (vs NOR) — affects U-Boot.

Everything except the V3c-side pins is production-shaped and matches
the proven f.appliance blocks.
