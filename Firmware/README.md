# Firmware layout

- **`Maiana_Blue_AIS_Firmware`** — Original AIS transponder tree (STM32CubeIDE / Eclipse-style project: `.ioc`, OpenOCD scripts). Kept for reference and existing workflows.

- **`Maiana_Blue_AIS_Firmware_VSCode_PlatformIO`** — Same application line built with **VS Code + PlatformIO** (`platformio.ini`). Use this tree for `pio run` / multi-environment builds. Build artifacts (`.pio/`) are ignored by git.

- **`Maiana_Blue_N2K_Firmware`** — N2K variant (unchanged).
