# MAIANA BLUE — Version 12 (MBv12)

Firmware and hardware design files for the MAIANA-class AIS transponder ecosystem. This project builds upon the open-source [MAIANA project](https://github.com/peterantypas/maiana) (GPL v3, Peter Antypas).

## Branch Context

The **`MBv12`** branch forms the foundation for **Version 12** of MAIANA Blue:
- Branch base: branched from **`cleanup/drop-legacy-bsp`** (inheriting firmware `FW 4.5.16` with N2K true heading support, GNSS autobaud, and consolidated `bsp_11_10` hardware support).
- Structure: streamlined workspace containing exclusively the active transponder firmware and the accompanying KiCad hardware design.

## Repository Layout

- **`Maiana_Blue_Ais_Firmware/`** — AIS transponder firmware configured for VS Code / PlatformIO (STM32L432 target).
- **`Maiana_Blue_Schematic/`** — KiCad project containing the complete schematic, PCB layout, and manufacturing files for MAIANA Blue v12.
