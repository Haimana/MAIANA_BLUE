# MAIANA Blue AIS firmware — VS Code / PlatformIO (board rev 12.1)

This directory is the **PlatformIO** build (VS Code + PlatformIO, `platformio.ini`) of the MAIANA-class AIS transponder firmware for the **MAIANA Blue board revision 12.1** (`BSP_HW_REV` `12.1.0`). It was ported from the original **STM32CubeIDE / Eclipse** project, which has been removed from this repository; this tree is now the only AIS firmware source.

Upstream concept and license remain those of the **MAIANA** project (GPL v3, Peter Antypas).

- **Firmware version:** `FW_REV` **4.6.0** (`Inc/config.h`), reported in `$PAISYS`.
- **MCU:** STM32L432KB (bootloader layout, application at `0x08004800`).
- **Radios:** 2 × Si4463 (U1 = transceiver on channel A, U2 = receiver on channel B), sharing `SPI1`.
- **GNSS:** Quectel LC76G on `USART2`.
- **Non-volatile storage:** ST **M95040** 512-byte **SPI** EEPROM on `SPI1` (no I2C anywhere on this board).

---

## Hardware rev 12.1 vs 11.x (what the BSP changed)

| MCU pin | Board 11.x | Board 12.1 |
|---|---|---|
| `PA2` | GNSS `1PPS` input | `USART2_TX` → LC76G `RXD` (MCU → GNSS line now wired) |
| `PB6` | I2C `SCL` (EEPROM) | **M95040 chip select** (SPI EEPROM on `SPI1`, shared with both Si4463) |
| `PB7` | I2C `SDA` (EEPROM) | GNSS `1PPS` input (EXTI) |

All other pins are unchanged (see `Inc/bsp/bsp_12_1.hpp`).

**GNSS TX line:** `BSP_GNSS_HAS_TX` is `1` and the BSP implements `bsp_gnss_send()`, but **no GNSS module configuration is sent yet**. `GPS::configureModule()` is an empty extension point (see the TODO in `Src/GPS.cpp`). The firmware still only listens: it probes 9600 / 115200 baud, locks on valid NMEA and remembers the last working rate as a hint in the config flags. `PA2` is kept in analog Hi-Z while the GNSS is powered off so it cannot back-power the module.

---

## Repository layout (this tree)

- **`platformio.ini`** — one active environment, **`L432-NewBreakouts-Bootloader`** (STM32L432KB, `VECT_TAB_OFFSET=0x4800`, `LEGACY_BREAKOUTS=0`, `BOARD_REV=121`). Other L432 variants are present but **commented out**; uncomment the block you need before building.
  > [!NOTE]
  > **STM32L412 is unsupported**: the binary is ~135 KB, far beyond the 64 KB of the L412. Commented-out L412 / L422 blocks in `platformio.ini` are obsolete stubs.
- **`Src/`**, **`Inc/`**, **`Drivers/`**, **`startup/`**, linker scripts (`STM32L4xx_FLASH-Bootloader.ld` for the active env).
- **`Src/bsp/bsp_12_1.cpp`**, **`Inc/bsp/bsp_12_1.hpp`** — the only BSP. `bsp_12_1.cpp` refuses to compile (`#error`) unless `BOARD_REV` is `121`.
- **`bootloader/maiana-bootloader-l432.bin`** — MAIANA bootloader used to build the combined image.
- **`Python/`** — `fwupdate-py3.py` (field update through the MAIANA bootloader) and ST ROM bootloader scripts (see `Python/README.md`).
- **`scripts/pio_post_copy_firmware.py`** — post-build copy / naming / combined image (below).
- **`Binaries/`** — post-build **`.bin`** copies. Release images in this folder are **tracked in git**; add or update them when you publish a new build.
- **`.gitignore`** — ignores **`.pio/`**, `.cursor/`, and similar generated content.

---

## Building

From this directory:

```bash
pio run -e L432-NewBreakouts-Bootloader
```

The raw image is produced under **`.pio/build/L432-NewBreakouts-Bootloader/firmware.bin`**.

### Named copies in `Binaries/` (post-build)

After each successful `pio run`, **`scripts/pio_post_copy_firmware.py`** copies `firmware.bin` into **`Binaries/`** as:

- `maiana_l432kb_hw12_1_fw4_6_0_boot0x4800.bin` — application only (starts at `0x08004800`);
- `maiana_l432kb_hw12_1_fw4_6_0_boot0x4800_combined.bin` — **bootloader + metadata + application** in one image starting at `0x08000000`: `bootloader/maiana-bootloader-l432.bin`, `0xFF` padding, a 16-byte metadata record at `0x4000` (magic `0xABADBABE`, application size, CRC32, flags) and the application at `0x4800`.

Name tokens: **MCU** (`board_build.mcu`), **hardware revision** from the `-DBOARD_REV=` build flag (`121` → `hw12_1`; `hwUnknown` plus a warning if the flag is missing), **`FW_REV`** from `Inc/config.h` (dots → underscores), and **layout** (`VECT_TAB_OFFSET` → `boot0x4800`, otherwise `fullflash`). `Inc/bsp/bsp.hpp` uses the same `BOARD_REV` (default `121`).

Environment switches:

```bash
PIO_COPY_FIRMWARE_BIN=0 pio run -e L432-NewBreakouts-Bootloader   # no copy to Binaries/
PIO_SKIP_COMBINED=1     pio run -e L432-NewBreakouts-Bootloader   # skip only the combined image
```

---

## Flashing / DFU

- **Full image over SWD:** program `..._combined.bin` at `0x08000000` (it already contains the bootloader and the metadata record at `0x4000`).
- **`dfu` command:** the application sets `DFU_FLAG_MAGIC` and reboots; early in `main()` it then jumps to the STM32 **ROM bootloader** (system memory, `0x1FFF0000`).
  - `Python/stm32-bootloader-py3.py <port> <image>` talks to the ROM bootloader and writes from `FLASH_BASE` = `0x08000000`, so use it with the **combined** image (details in `Python/README.md`; disconnect your serial terminal first).
- **`Python/fwupdate-py3.py <port> <imagefile>`** talks to the **MAIANA bootloader** (waits for the `MAIANA bootloader` banner, then `load <size> <crc32>` → `READY`) and sends the **application** image (`maiana_l432kb_hw12_1_fw4_6_0_boot0x4800.bin`).

---

## EEPROM (M95040 on SPI1)

512 bytes, 16-byte pages. Each record starts with a 32-bit magic and is at most 64 bytes:

| Address | Record | Content |
|---|---|---|
| `0x000` | Station data | MMSI, name, call sign, type, dimensions (`STATION_DATA_MAGIC`) |
| `0x040` | Config flags | `flags[0]` bit0 = TX inhibit, bit1 = hourly-reboot inhibit, bits2-3 = last GNSS baud code (9600 / 115200) |
| `0x060` | XO trim | Si4463 crystal trim value |
| `0x070` | TX config | `pa_level` (0–127) |

Behaviour (`Src/bsp/bsp_12_1.cpp`):

- `SPI1` is shared with both Si4463. Every EEPROM access runs in a short critical section (interrupts of priority ≥ 1 masked, SPI at 5 MHz) moving **at most 8 data bytes**, so AIS bit timing is not disturbed.
- Writes are **power-loss safe**: magic invalidated first, payload written, magic written last, then read back and compared. Identical records are not rewritten. On failure the CLI prints **`ERROR: EEPROM write failed`** and the RAM copy is re-read from the EEPROM.
- At boot the BSP checks that the EEPROM answers (status register bits 7..4 always read `1` on the M95040) and clears block protection (BP1/BP0) if it is set. The result is reported at every boot as **`$PAIEEP,<status>,<SR>*hh`**:
  - `0` = OK, `1` = block protection found and cleared, `2` = block protection could not be cleared, `3` = EEPROM not responding;
  - `SR` = status register in hex (`F0` on a healthy, unprotected M95040).
- Missing records fall back to defaults: TX enabled, hourly reboot on, default PA level, no station data.

---

## Serial CLI (USART1)

Commands are matched by prefix (`help` prints the list). Replies are NMEA-style `$PAI...*hh` sentences unless shown otherwise.

| Command | Action | Reply |
|---|---|---|
| `help` | List commands | text |
| `station <mmsi>,<name>,<callsign>,<type>,<len>,<beam>,<portoffset>,<bowoffset>` | Save station data (types 30, 34, 36, 37 accepted) | `$PAISTN,...` |
| `station?` | Report station data | `$PAISTN,<mmsi>,<name>,<callsign>,<type>,<len>,<beam>,<port>,<bow>` |
| `erase station` | Erase station data | `$PAISTN` (zeroed fields) |
| `factory reset` | Erase station data and config flags (XO trim and TX config are kept) | `$PAISTN`, `$PAIHRB` |
| `sys?` | System data | `$PAISYS,<hwrev>,<fwrev>,<serial>,<mcu>,<breakout>,<bootloader 0/1>` |
| `hourlyreboot on` / `off` / `?` | Hourly self-reboot setting | `$PAIHRB,<0/1>,<uptime s>` |
| `tx on` / `tx off` / `tx?` | Soft TX switch | `$PAITXCFG,<hardwired>,<hw switch on>,<soft on>,<station set>,<TX allowed>` |
| `tx test` | Send a 1-second test packet (9600 random bits) on a random channel — use a dummy load | `$PAITX,<A/B>,...` when done |
| `stoptx` | Return the transceiver to receive | — |
| `msg24` | Queue message 24 | `$PAISCHTX,...` |
| `gps on` / `gps off` | Power-cycle / switch off the GNSS | — |
| `gnss?` | GNSS link | `$PAIGNSS,<baud>,<link state>,<hint baud>,<probe switches>,<rx errors>,<has TX 0/1>` |
| `heading <0-360>` / `heading off` / `heading?` | True heading for message 18 | `$PAIHDG,<deg>,...` or `$PAIHDG,,511` |
| `xotrim <n>` | Apply and save XO trim | `$PAIXOTR,<n>` (preceded by `ERROR: EEPROM write failed` on failure) |
| `xotrim?` | Report XO trim | `$PAIXOTR,<n>` |
| `pa_level <0-127>` | Save and apply Si4463 `PA_PWR_LVL` | `OK, saved and applied.` / `OK, saved, applying after current TX.` / `ERROR: EEPROM write failed` / `Invalid level (0-127)` |
| `pa_level?` | Report PA level | `$PAITXC,<level>,<stored 0/1>` |
| `otp?` / `otp <hwrev> [serial]` | One-time-programmable data | `$PAIOTP,<serial>,<hwrev>,<BSP_HW_REV>` |
| `reboot` / `cli` / `dfu` | Reboot / reboot into CLI mode / bootloader | — |
| `nmea?` | NMEA path diagnostics (only with `NMEA_DIAG=1`) | `$PAINMEA,...` |

Unsolicited sentences: `$PAIEEP` (boot), `$PAITXCFG` (boot), `$PAITX,<A/B>,<msg>` after each transmission, `$PAISCHTX,<msg>,<code>` (scheduling: 0 = OK, 1 = alloc error, 2 = queue full, 3 = packet too old), `$PAINF,<A/B>,0x..` (noise floor), `$PAIERR,RFIC,<id>` (radio init error).

### PA level details

- `pa_level` is the Si4463 `PA_PWR_LVL` (7-bit, **0–127**). Input must be a plain decimal integer; anything else is rejected and **not** saved.
- After a **verified** EEPROM write the level is applied live (`Transceiver::setTXPower`); if a transmission is active or imminent it is applied right after it.
- `$PAITXC`: first field = stored level, or the default if nothing is stored; second field = `1` if a stored TX config exists, else `0`.
- Defaults (`Transceiver::paDefaults()`): Si4463 `pa_mode` 0x48 / `pa_level` 0x28 (40). `pa_mode` and `pa_bias_clkduty` always follow the chip; only `pa_level` is stored. The output stage is an external LDMOS (Q1) driven by the Si4463, so the level is not linear in dBm — measure on your hardware.

---

## Notable changes in 4.6.0 (this tree)

- **BSP 12.1**: M95040 SPI EEPROM driver (replaces I2C), power-loss safe records, `$PAIEEP` boot report, I2C HAL module disabled.
- **EEPROM write results are reported**: all write/erase paths return success and the CLI prints `ERROR: EEPROM write failed` instead of failing silently.
- **AIS TX Gaussian filter fix**: the BT = 0.4 TX shaping coefficients are now written to the Si4463 **MODEM** group (`MODEM_TX_FILTER_COEFF_8..0`, properties `0x200F–0x2017`). Earlier firmware wrote them to the PA group (`0x22`), which has no such properties, so the chip kept its default BT = 0.5 filter. Verify the TX spectrum after updating.
- **`xotrim <n>`** now answers with `$PAIXOTR,<n>`.
- **Hardware-tagged binary names** (`hw12_1`).

### Other behaviour inherited from earlier PlatformIO versions

- **`gRadioState`** is declared `extern` in `Inc/RadioState.hpp` and defined once in `Src/Receiver.cpp` (the original tree had a `static` copy per translation unit).
- **`$PAIOTP`** includes `BSP_HW_REV` as an extra field.
- **`NMEA_DIAG`** (`Inc/config.h`, default **0**) gates `nmea?` and its counters. Enable with `-DNMEA_DIAG=1` in `build_flags`.
- **Hourly `bsp_reboot()`** workaround in `RadioManager` (can be disabled with `hourlyreboot off`).
- Core AIS encode/decode (`AISMessages`, `NMEAEncoder`, `RXPacket`, `TXPacket`, channel tables) follows the upstream design.

---

## License

Firmware sources remain under **GNU GPL v3** (see file headers). This README describes layout and behaviour for maintainers; it does not replace the upstream license text.
