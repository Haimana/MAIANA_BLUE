# MAIANA Blue AIS firmware — VS Code / PlatformIO (board rev 12.1)

This directory is the **PlatformIO** build (VS Code + PlatformIO, `platformio.ini`) of the MAIANA-class AIS transponder firmware for the **MAIANA Blue board revision 12.1** (`BSP_HW_REV` `12.1.0`). It was ported from the original **STM32CubeIDE / Eclipse** project, which has been removed from this repository; this tree is now the only AIS firmware source.

Upstream concept and license remain those of the **MAIANA** project (GPL v3, Peter Antypas).

- **Firmware version:** `FW_REV` **4.6.2** (`Inc/config.h`), reported in `$PAISYS`.
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

- `maiana_l432kb_hw12_1_fw4_6_1_boot0x4800.bin` — application only (starts at `0x08004800`);
- `maiana_l432kb_hw12_1_fw4_6_1_boot0x4800_combined.bin` — **bootloader + metadata + application** in one image starting at `0x08000000`: `bootloader/maiana-bootloader-l432.bin`, `0xFF` padding, a 16-byte metadata record at `0x4000` (magic `0xABADBABE`, application size, CRC32, flags) and the application at `0x4800`.

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
- **`Python/fwupdate-py3.py <port> <imagefile>`** talks to the **MAIANA bootloader** (waits for the `MAIANA bootloader` banner, then `load <size> <crc32>` → `READY`) and sends the **application** image (`maiana_l432kb_hw12_1_fw4_6_1_boot0x4800.bin`).

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

Commands are case-sensitive, one per line (CR ignored, LF ends the line). `help` prints the list. Replies are NMEA-style `$PAI...*hh` sentences unless shown otherwise.

Line and matching rules (4.6.2):

- Commands that transmit, reset or write configuration must be the **whole line** (surrounding blanks are ignored): `dfu`, `reboot`, `cli`, `factory reset`, `erase station`, `hourlyreboot on|off`, `gps on|off`, `heading off`, `tx on|off`, `tx test`, `stoptx`, `msg24`. Anything else (e.g. `reboot now`, or an echoed `help` line) is ignored.
- Commands with arguments (`station`, `xotrim`, `pa_level`, `heading`, `otp`, `gnss`) must start the line and their arguments are validated; invalid input is rejected and nothing is written.
- Up to 4 junk bytes in front of a **read-only/idempotent** command (`help`, the `…?` queries, `heading <deg>`, `heading off`) are skipped (TX interference on the RX line). Other commands are never searched for inside a line.
- `$` or `!` anywhere in a line restarts it (NMEA resync), so a cut heading sentence followed by a full one is still accepted. Exceptions: inside a `station ` line, where name and callsign may contain `$` and `!` (AIS 6-bit charset), and inside a `gnss ` line, whose argument is a `$...` sentence.
- Lines starting with `!` (e.g. our own `!AIVDM` echoed back by a multiplexer) are ignored; lines starting with `$` are only parsed as heading input (`$--HDT`, `$--HDG`, `$PAIHDG`, checksum required).
- A line that was partly lost because USART1 RX is muted around our own transmission is dropped instead of being merged with the next line.

| Command | Action | Reply |
|---|---|---|
| `help` | List commands | text |
| `station <mmsi>,<name>,<callsign>,<type>,<len>,<beam>,<portoffset>,<bowoffset>` | Save station data. Exactly 8 fields; MMSI exactly 9 digits; type/len/beam/offsets digits only, 0–255 (types other than 30, 34, 36, 37 are stored as 0) | `$PAISTN,...` or `ERROR: invalid station data (...)` |
| `station?` | Report station data | `$PAISTN,<mmsi>,<name>,<callsign>,<type>,<len>,<beam>,<port>,<bow>` |
| `erase station` | Erase station data | `$PAISTN` (zeroed fields) |
| `factory reset` | Erase station data and config flags (XO trim and TX config are kept) | `$PAISTN`, `$PAIHRB` |
| `sys?` | System data | `$PAISYS,<hwrev>,<fwrev>,<serial>,<mcu>,<breakout>,<bootloader 0/1>` |
| `hourlyreboot on` / `off` / `?` | Hourly self-reboot setting | `$PAIHRB,<0/1>,<uptime s>` |
| `tx on` / `tx off` / `tx?` | Soft TX switch | `$PAITXCFG,<hardwired>,<hw switch on>,<soft on>,<station set>,<TX allowed>` |
| `tx test` | Send a 1-second test packet (9600 random bits) on a random channel — use a dummy load. Refused while a TX is pending/running; at most one per 10 s (`TX_TEST_MIN_INTERVAL_MS`) | `OK, test packet on channel A/B`, then `$PAITX,<A/B>,...` when done; or `ERROR: TX busy, test packet not sent` / `ERROR: tx test rate limited, retry in N s` |
| `stoptx` | Return the transceiver to receive | — |
| `msg24` | Queue message 24 | `$PAISCHTX,...` |
| `gps on` / `gps off` | Power-cycle / switch off the GNSS | — |
| `gnss?` | GNSS link | `$PAIGNSS,<baud>,<link state>,<hint baud>,<probe switches>,<rx errors>,<has TX 0/1>` |
| `gnss <$sentence>` | Forward one whitelisted LC76G configuration sentence (`$PAIR...` / `$PQTM...`, `*HH` optional; computed if missing, verified if given) to the GNSS module over USART2. Only with a locked GNSS link. See *GNSS passthrough* | `OK, sent to GNSS: $...*HH` then the module's own reply (`$PAIR001,<cmd>,<result>`, `$PQTM...`) in the NMEA stream; or `ERROR: invalid GNSS sentence` / `bad checksum` / `GNSS sentence too long` / `GNSS command not allowed` / `invalid or unsafe GNSS parameters` / `GNSS off, not locked or busy, retry` |
| `heading <0-360>` / `heading off` / `heading?` | True heading for message 18 | `$PAIHDG,<deg>,...` or `$PAIHDG,,511` |
| `xotrim <0-255>` | Apply and save XO trim (digits only) | `$PAIXOTR,<n>` (preceded by `ERROR: EEPROM write failed` on failure) or `ERROR: invalid xotrim (0-255)` |
| `xotrim?` | Report XO trim | `$PAIXOTR,<n>` |
| `pa_level <0-127>` | Save and apply Si4463 `PA_PWR_LVL` | `OK, saved and applied.` / `OK, saved, applying after current TX.` / `ERROR: EEPROM write failed` / `Invalid level (0-127)` |
| `pa_level?` | Report PA level | `$PAITXC,<level>,<stored 0/1>` |
| `otp?` / `otp <hwrev> [serial]` | One-time-programmable data | `$PAIOTP,<serial>,<hwrev>,<BSP_HW_REV>` or `ERROR: invalid otp (...)` |
| `reboot` / `cli` / `dfu` | Reboot / reboot into CLI mode / bootloader | — |
| `nmea?` | NMEA path diagnostics (only with `NMEA_DIAG=1`) | `$PAINMEA,...` |

Unsolicited sentences: `$PAIEEP` (boot), `$PAITXCFG` (boot), `$PAITX,<A/B>,<msg>` after each transmission, `$PAISCHTX,<msg>,<code>` (scheduling: 0 = OK, 1 = alloc error, 2 = queue full, 3 = packet too old), `$PAINF,<A/B>,0x..` (noise floor), `$PAIERR,RFIC,<id>` (radio init error), `$PAIERR,TXWDT,<count>,<0|1>` (TX watchdog fired: 0 = transmitter recovered to RX, 1 = transmitter shut down, board reboots ~0.5 s later).

### PA level details

- `pa_level` is the Si4463 `PA_PWR_LVL` (7-bit, **0–127**). Input must be a plain decimal integer; anything else is rejected and **not** saved.
- After a **verified** EEPROM write the level is applied live (`Transceiver::setTXPower`); if a transmission is active or imminent it is applied right after it.
- `$PAITXC`: first field = stored level, or the default if nothing is stored; second field = `1` if a stored TX config exists, else `0`.
- Defaults (`Transceiver::paDefaults()`): Si4463 `pa_mode` 0x48 / `pa_level` 0x28 (40). `pa_mode` and `pa_bias_clkduty` always follow the chip; only `pa_level` is stored. The output stage is an external LDMOS (Q1) driven by the Si4463, so the level is not linear in dBm — measure on your hardware.

### GNSS passthrough (`gnss <$sentence>`, 4.6.2)

Validation lives in `Inc/GnssCmd.hpp` (host-testable). Max 72 characters after `gnss `, printable ASCII, no blanks, no `$`/`!`/`*` inside. Bare `$PAIR...` lines from the console are **not** forwarded (they are heading input), so a multiplexer echoing the module's replies back cannot loop.

Allowed (anything else is refused):

- Restarts / NVM / info: `PAIR002`, `PAIR004` hot, `PAIR005` warm, `PAIR006` cold, `PAIR511` save nav data, `PAIR513` save settings, `PAIR514` defaults, `PQTMSAVEPAR`, `PQTMRESTOREPAR`, `PQTMVERNO`.
- Queries: `PAIR051`, `PAIR059`, `PAIR063,<-1..9>`, `PAIR067`, `PAIR071`, `PAIR073`, `PAIR075`, `PAIR081`, `PAIR163`, `PAIR401`, `PAIR411`, `PAIR470,<0..3>`, `PAIR491`, `PAIR865,0,0`, `PQTMCFG{PPS,ANTENNA,NMEADP,NMEATID,ODO,GEOFENCE,SLAS},R[,n[,n]]`.
- Settings with field checks: `PAIR050,1000` only; `PAIR058,<9..30>`; `PAIR062,<type -1..9>,<rate 0..20>` with GGA (0) and RMC (4) only at 1; `PAIR066,1,x,x,x,x,0` / `PQTMCFGCNST,W,1,x,x,x,x,0` (GPS stays on) and `PQTMCFGCNST,R`; `PAIR070,<0..20>`; `PAIR072,<0..45>`; `PAIR074/162/391/410/490/496,<0|1>`; `PAIR080,<0|1|3|4|5|7>`; `PAIR400,<0|2|3>`; `PAIR498,<1|3|5>`; `PAIR590,YYYY,M,D,h,m,s`; `PAIR600,lat,lon,h,accMaj,accMin,bear,accV`; `PAIR864,0,0,115200` only; `PQTMCFGMSGRATE,R,<name>[,ver]`; `PQTMCFGMSGRATE,W,` GLL/GSA/GSV/VTG/ZDA/GRS/GST/GNS `<0|1>`, RMC/GGA `1` only, `PQTMEPE,<0|1>,2`, `PQTMDOP,<0|1>,1`, `PQTMJAMMINGSTATUS,<0|1>,1`.
- Refused on purpose: `PAIR003` (power off), `PAIR650`/`382`/`680`/`690`/`730`/`732` (sleep / low power), `PAIR086` (debug log), `PAIR75x`/`76x` and `PQTMCFGPPS,W` (1PPS), other `PAIR864` rates, `PAIR43x` (RTCM), `PAIR9xx` (LOCUS), `PAIR471`/`472` (EPO data; lines too long for the CLI), `PQTMPVT`, PQTM config writes other than above, `PAIR080` modes 2/6.

Settings not saved with `PAIR513` / `PQTMSAVEPAR` are lost on `gps off` / `gps on` (power cycle). `gnss $PAIR514` restores the module defaults.

---

## Changes in 4.6.2

- **New CLI command `gnss <$sentence>`** (hardware 12.1, `BSP_GNSS_HAS_TX`): forwards one `$PAIR...` / `$PQTM...` configuration sentence to the Quectel LC76G over USART2, using the existing TX DMA path (`bsp_gnss_send()`). Listed in `help`. Replies: `OK, sent to GNSS: $...*HH`, then the module's own answer (`$PAIR001,<cmd>,<result>`, `$PQTM...`) arrives through the normal GNSS echo; errors are `ERROR: invalid GNSS sentence` / `bad checksum` / `GNSS sentence too long` / `GNSS command not allowed` / `invalid or unsafe GNSS parameters` / `GNSS off, not locked or busy, retry` / `no GNSS TX line on this board`.
- **Checksum optional**: without `*HH` it is computed; with `*HH` it is verified and a wrong one is refused.
- **Whitelist with field checks** in the new header **`Inc/GnssCmd.hpp`** (`gnss_cmd::build()`, host-testable): max 72 characters, printable ASCII, no blanks or embedded `$`/`!`/`*`, canonical integers (no leading zeros / `-0`). Only the commands listed in *GNSS passthrough* are accepted, and their parameters are range-checked: RMC/GGA output (rate 1, `PAIR050,1000`, `PQTMCFGMSGRATE` RMC/GGA 1), GPS enabled in `PAIR066`/`PQTMCFGCNST`, UART rate (`PAIR864,0,0,115200` only) and 1PPS cannot be changed, so the AIS position input keeps working.
- **Refused on purpose**: power off (`PAIR003`), sleep / low-power modes (`PAIR650/382/680/690/730/732`), debug log (`PAIR086`), 1PPS settings (`PAIR75x/76x`, `PQTMCFGPPS,W`), other baud rates, RTCM (`PAIR43x`), LOCUS (`PAIR9xx`), EPO data (`PAIR471/472`, too long for the CLI), `PQTMPVT`, other PQTM writes, `PAIR080` modes 2/6.
- **`GPS::sendCommand()`** (`Inc/GPS.hpp`, `Src/GPS.cpp`): sends only while the GNSS link is locked (auto-baud done), otherwise the command is refused.
- **CLI line assembler** (`Inc/CliParse.hpp`): no `$`/`!` resync inside a `gnss ` line (like `station `), since its argument is a `$` sentence; `gnss ` is in the verb list but is **not** a junk-skip target, so it must start the line. Bare `$PAIR...` console lines are still heading input only and are never forwarded (no loop with an echoing multiplexer).
- **Persistence**: settings are volatile in the LC76G until saved with `gnss $PAIR513` or `gnss $PQTMSAVEPAR`; unsaved settings are lost on `gps off` / `gps on`. `gnss $PAIR514` restores module defaults. `PAIR864` needs a module restart and is not saved.
- `FW_REV` 4.6.2. Binaries: `maiana_l432kb_hw12_1_fw4_6_2_boot0x4800.bin` (141472 B, CRC32 00A5BE32) and `..._combined.bin` (159904 B, CRC32 6E6A1422).
- No other behaviour change (AIS RX/TX, pa_level, xotrim, station data unchanged).

## Changes in 4.6.1

- **TX watchdog** (TIM7 one-shot, same IRQ priority as the radio bit clocks): armed at START_TX for the packet's air time + 10 ms (normal packets capped at 5 slots; `tx test` uses its own 1 s length). On expiry PA bias (PA8) goes low, the transmitter is returned to RX if it still answers CTS, otherwise shut down (SDN1 high) and the board reboots; `$PAIERR,TXWDT,<count>,<0|1>` is reported.
- **RF safe shutdown** before `reboot` and `dfu`: radio/SOTDMA interrupts off, PA8 low, SDN1 high, so the PA is never left biased across a reset.
- **Constant PA ramp-up (~250 us)**: `PA_DIG_PWR_SEQ_CONFIG` (0x2206) is computed per PA level (single steps, step delay ≈ 250 us / level, limited to 0.3–12.7 us) instead of a fixed 0xFF (which gave ~813 us at level 127). Below level ~20 the ramp is shorter (minimum step delay).
- **`tx test`**: refused while a TX is pending/running, rate-limited to one per 10 s, now replies `OK, test packet on channel A/B`.
- **Exact matching for TX commands**: `tx test`, `tx on`, `tx off`, `stoptx` and `msg24` must be the whole line; `tx test <anything else>` replies `ERROR: unknown tx test option`, `tx on…`/`tx off…` with extra text reply `ERROR: unknown tx option` (previously any line starting with `tx test` fired a test packet).
- **Robust CLI input** (see *Line and matching rules*): `dfu`, `reboot`, `cli`, `factory reset`, `erase station`, `hourlyreboot on|off`, `gps on|off`, `heading off` are now exact-match too; the junk-skip (added in 4.5.6 against TX interference) only skips ≤4 bytes and only toward read-only commands and `heading`; `$`/`!` resync the line (except inside `station `); `!` lines (echoed `!AIVDM`) are ignored; lines cut by the TX RX-mute window are dropped instead of merged; strict `xotrim` (0–255) and `station` (8 fields, 9-digit MMSI, numeric fields) validation; `otp` checks its arguments. Before this, an echoed `!AIVDM` payload or `help` text could trigger `reboot`/`dfu`/`cli`. The Python tools send exactly `dfu` and are unaffected.
- **Safer `stopTX()`**: runs with interrupts masked and properly aborts an in-progress transmission.
- **Si446x errata 2.12 ramp-down workaround** behind `SI446X_ERRATA_2_12_RAMPDOWN_FIX` in `Inc/config.h` (default **0 / off**).
- GPIO3 comments corrected (0x20 = TX_STATE, 0x21 = RX_STATE); no behaviour change.
- Bench-only commands `tx test wdt` / `tx test wdt hard` (force the watchdog after 100 ms, optionally simulating an unresponsive transmitter) exist only when built with `TX_WDT_TEST_CMD=1` (`Inc/config.h`, default **0**; not in release bins).
- Bounded CTS polling (`RFIC::pollCTS`, `TX_ABORT_CTS_POLLS`) on the TX abort / recovery paths, so an unresponsive Si4463 cannot hang the firmware.
- Binaries: `maiana_l432kb_hw12_1_fw4_6_1_boot0x4800.bin` (137104 B, CRC32 2F35E9C5) and `..._combined.bin` (CRC32 76007D6B).
- Not changed: no MCU-temperature TX inhibit (no ADC driver in tree), no PWM bias ramp.

## Notable changes in 4.6.0

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
