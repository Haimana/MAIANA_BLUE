# MAIANA Blue AIS firmware — VS Code / PlatformIO

This directory is the **PlatformIO** build of the MAIANA-class AIS transponder firmware. It lives alongside the **original CubeIDE / Eclipse** tree in this repository:

| Directory | Toolchain | Notes |
|-----------|-----------|--------|
| **`../Maiana_Blue_AIS_Firmware/`** | STM32CubeIDE (Eclipse CDT), STM32CubeMX `.ioc`, OpenOCD scripts | Reference “original” layout for this repo. |
| **`./` (this folder)** | **VS Code + PlatformIO** (`platformio.ini`) | Same application family, newer source line and multi-environment builds. |

Upstream concept and license remain those of the **MAIANA** project (GPL v3, Peter Antypas).

---

## Current repository layout (this tree)

- **`platformio.ini`** — defines multiple `[env:...]` targets (L432 / L412, legacy vs new breakout, with or without bootloader offset `0x4800`). In the stock file, **only one** environment is active (`L432-NewBreakouts-Bootloader`); the others are present but **commented out** — uncomment the block you need before building.
- **`Src/`**, **`Inc/`**, **`Drivers/`**, **`startup/`**, linker scripts — same overall structure as the CubeIDE project; build filters select BSP and startup per environment.
- **`Python/`** — `fwupdate-py3.py` and bootloader helpers for field updates.
- **`Binaries/`** — used locally for post-build **`.bin`** copies (see below). **`Binaries/*.bin`** is listed in **`.gitignore`** so generated images are not committed. Prebuilt binaries were removed from version control earlier.
- **`.gitignore`** — ignores **`.pio/`**, `.cursor/`, and similar generated content.

Firmware images are produced under **`.pio/build/<env>/`** after a successful build.

### Named copy in `Binaries/` (post-build)

After each successful `pio run`, **`scripts/pio_post_copy_firmware.py`** copies `firmware.bin` into **`Binaries/`** with a generated name, for example:

`maiana_l432kb_L432-NewBreakouts-Bootloader_fw4_5_0_newBo_boot0x4800.bin`

Tokens include: **MCU** (`board_build.mcu`), **PlatformIO environment** name, **`FW_REV`** from `Inc/config.h`, **legacy vs new breakout** (`LEGACY_BREAKOUTS`), **full flash vs bootloader layout** (`VECT_TAB_OFFSET` → `boot0x4800` or `fullflash`), and **`BOARD_REV`** when defined.

Disable the copy step:

```bash
PIO_COPY_FIRMWARE_BIN=0 pio run -e L432-NewBreakouts-Bootloader
```

---

## Differences vs `../Maiana_Blue_AIS_Firmware/` (original in this repo)

The following summarizes **this PlatformIO tree** relative to the sibling **CubeIDE** project **as checked into this repository** (not every historical upstream tag).

### Build system and project files

- **PlatformIO** (`platformio.ini`, `syscalls.c` for newlib) instead of Eclipse **`.cproject` / `.project`**, STM32CubeMX **`.ioc`**, and bundled **OpenOCD** flash configs.
- Environments in `platformio.ini` are toggled with comments; **uncomment** an `[env:...]` section to use `pio run -e <name>`. The CubeIDE project uses separate build configurations inside the IDE.

### Application / firmware version

- **`FW_REV`** in `Inc/config.h` is **4.5.0** here vs **4.4.1** in `../Maiana_Blue_AIS_Firmware/Inc/config.h` (reported in `$PAISYS` and related output).

### RF transmit power

- **Configurable PA level** stored in flash (`TXConfig` / BSP `bsp_*_tx_config`), CLI commands **`pa_level`** / **`pa_level?`**, and proprietary sentence **`$PAITXC`**.
- The CubeIDE sibling in this repo uses **fixed** PA parameters in `Transceiver.cpp` and **does not** expose those CLI commands.

### Global radio state

- **`gRadioState`** is declared **`extern`** in `Inc/RadioState.hpp` and defined once in **`Src/Receiver.cpp`**, so all translation units share a single RX/TX state variable.
- The CubeIDE tree in this repo keeps **`static volatile RadioState gRadioState`** in the header, which can create **separate copies** per `.cpp` that includes the header — a known pitfall for receiver/transmitter coordination.

### NMEA and configuration behaviour

- **`$PAIOTP`** includes an extra field: **`BSP_HW_REV`** (board support package hardware revision string).
- **`factory reset`** in `Configuration.cpp` **does not** erase XO trim in this tree (call to `bsp_erase_xo_trim()` commented out); behaviour may differ from the CubeIDE project.

### Default PA tuning (when no stored TX config)

- When no valid stored PA configuration is present, default `pa_mode` / `pa_level` values in **`Transceiver.cpp`** differ from the older fixed table in the 4.4.1 tree (intended for updated hardware power targets; always validate on your hardware).

### Optional / work-in-progress sources

- **`Inc/bsp/bsp_11_9.hpp`** and **`Src/bsp/bsp_11_9.cpp`** exist here but are **not** wired into `Inc/bsp/bsp.hpp` in the stock configuration — treat as **WIP** unless you integrate them and add a matching `BOARD_REV` / build environment.

### What is intentionally **not** different

- Core AIS **encode/decode** logic (`AISMessages`, `NMEAEncoder`, `RXPacket`, `TXPacket`, channel tables) matches the same design as the CubeIDE tree for the same source generation.
- **Hourly `bsp_reboot()`** workaround in `RadioManager` (RX long-run behaviour) is present in both lines where that source exists.

---

## Building

From this directory:

```bash
pio run -e L432-NewBreakouts-Bootloader
```

Pick the `[env:...]` that matches your MCU (L432 vs L412), breakout type (legacy vs new), and whether the image must sit **after** the MAIANA bootloader (`*Bootloader*` environments and `VECT_TAB_OFFSET`). Ensure that environment’s block is **uncommented** in `platformio.ini` (only one active `[env:]` should be uncommented at a time unless you know your PlatformIO workflow supports multiple defaults).

---

## Flashing / DFU

Use the scripts in **`Python/`** (e.g. `fwupdate-py3.py`) with a serial port and the **`firmware.bin`** from `.pio/build/<env>/`, after placing the device in bootloader/DFU as required by your hardware.

---

## License

Firmware sources remain under **GNU GPL v3** (see file headers). This README describes only layout and behaviour differences for maintainers; it does not replace the upstream license text.
