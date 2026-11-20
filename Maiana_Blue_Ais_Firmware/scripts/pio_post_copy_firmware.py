# PlatformIO post-build: copy firmware.bin into Binaries/ with a descriptive name.
# For bootloader-layout builds (VECT_TAB_OFFSET defined), also produces a combined
# binary (bootloader + padding + app) if the bootloader .bin is present in bootloader/.
#
# Disable all:          PIO_COPY_FIRMWARE_BIN=0 pio run
# Skip combined only:   PIO_SKIP_COMBINED=1 pio run

Import("env")

import binascii
import os
import re
import shutil
import struct

_MCU_BOOTLOADER_MAP = {
    "l432": "maiana-bootloader-l432.bin",
    "l431": "maiana-bootloader-l431.bin",
    "l422": "maiana-bootloader-l422.bin",
    "l412": "maiana-bootloader-l412.bin",
}


def _read_fw_rev(project_dir):
    path = os.path.join(project_dir, "Inc", "config.h")
    if not os.path.isfile(path):
        return "unknown"
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        m = re.search(r"#\s*define\s+FW_REV\s+\"([^\"]+)\"", f.read())
    return m.group(1) if m else "unknown"


def _get_cpp_define(env, key):
    for d in env.get("CPPDEFINES", []):
        if isinstance(d, (list, tuple)) and len(d) >= 1 and d[0] == key:
            return str(d[1]) if len(d) > 1 else "1"
        if isinstance(d, str):
            if d == key:
                return "1"
            if d.startswith(key + "="):
                return d.split("=", 1)[1]
    return None


def _sanitize_token(s):
    return "".join(c if (c.isalnum() or c in "-_.") else "_" for c in str(s))


def _find_bootloader_bin(project_dir, mcu):
    mcu_lower = mcu.lower().replace("stm32", "")
    for prefix, filename in _MCU_BOOTLOADER_MAP.items():
        if mcu_lower.startswith(prefix):
            path = os.path.join(project_dir, "bootloader", filename)
            if os.path.isfile(path):
                return path
    return None


_METADATA_OFFSETS = {
    "l432": 0x4000,
    "l431": 0x4000,
    "l422": 0x4000,
    "l412": 0x1800,
}

METADATA_MAGIC = 0xABADBABE


def _metadata_offset_for_mcu(mcu):
    mcu_lower = mcu.lower().replace("stm32", "")
    for prefix, offset in _METADATA_OFFSETS.items():
        if mcu_lower.startswith(prefix):
            return offset
    return None


def _build_combined(app_bin_path, bl_bin_path, vect_offset_str, dest_path, mcu):
    """Merge bootloader + metadata page + padding + application into a single
    flashable binary.  The metadata page (magic + size + crc32 + flags) is
    required by the MAIANA bootloader to validate and chain-load the app."""
    offset = int(str(vect_offset_str), 0)

    with open(bl_bin_path, "rb") as f:
        bl_data = f.read()

    if len(bl_data) > offset:
        print("pio_post_copy_firmware: ERROR bootloader (%d bytes) exceeds "
              "VECT_TAB_OFFSET (0x%X = %d bytes), combined binary NOT created"
              % (len(bl_data), offset, offset))
        return False

    with open(app_bin_path, "rb") as f:
        app_data = f.read()

    image = bytearray(offset + len(app_data))
    for i in range(len(image)):
        image[i] = 0xFF

    image[0:len(bl_data)] = bl_data

    meta_off = _metadata_offset_for_mcu(mcu)
    if meta_off is not None and meta_off + 16 <= offset:
        app_crc = binascii.crc32(app_data) & 0xFFFFFFFF
        metadata = struct.pack("<IIII", METADATA_MAGIC, len(app_data), app_crc, 0)
        image[meta_off:meta_off + 16] = metadata
        print("pio_post_copy_firmware: metadata @ 0x%X: size=%d, crc32=0x%08X"
              % (meta_off, len(app_data), app_crc))
    else:
        print("pio_post_copy_firmware: WARNING no metadata offset for %s, "
              "bootloader may not chain-load the application" % mcu)

    image[offset:offset + len(app_data)] = app_data

    with open(dest_path, "wb") as f:
        f.write(image)

    return True


def _post_copy(source, target, env):
    if os.environ.get("PIO_COPY_FIRMWARE_BIN", "1").strip() == "0":
        return

    project_dir = env["PROJECT_DIR"]
    build_dir = env.subst("$BUILD_DIR")
    bin_src = os.path.join(build_dir, "firmware.bin")
    if not os.path.isfile(bin_src):
        print("pio_post_copy_firmware: firmware.bin not found, skip")
        return

    board = env.BoardConfig()
    mcu = board.get("build.mcu", "mcu")
    mcu_short = _sanitize_token(mcu.replace("stm32", "").replace("STM32", ""))

    pioenv = _sanitize_token(env["PIOENV"])
    fw = _read_fw_rev(project_dir)
    fw_token = _sanitize_token(fw.replace(".", "_"))

    br = _get_cpp_define(env, "BOARD_REV")
    br_token = f"br{_sanitize_token(br)}" if br is not None else None

    vect = _get_cpp_define(env, "VECT_TAB_OFFSET")
    boot_token = "boot0x4800" if vect else "fullflash"

    leg = _get_cpp_define(env, "LEGACY_BREAKOUTS")
    if leg is not None:
        breakout = "legacyBo" if str(leg) in ("1", "true", "True") else "newBo"
    else:
        breakout = "boUnknown"

    parts = [
        "maiana",
        mcu_short,
        pioenv,
        f"fw{fw_token}",
        breakout,
        boot_token,
    ]
    if br_token:
        parts.append(br_token)

    name = "_".join(parts) + ".bin"
    out_dir = os.path.join(project_dir, "Binaries")
    os.makedirs(out_dir, exist_ok=True)
    dest = os.path.join(out_dir, name)
    shutil.copy2(bin_src, dest)
    print("pio_post_copy_firmware: %s (%d bytes)" % (dest, os.path.getsize(dest)))

    # --- Combined binary (bootloader + app) for bootloader-layout builds ---
    if vect and os.environ.get("PIO_SKIP_COMBINED", "0").strip() != "1":
        bl_bin = _find_bootloader_bin(project_dir, mcu)
        if bl_bin:
            combined_name = "_".join(parts + ["combined"]) + ".bin"
            combined_dest = os.path.join(out_dir, combined_name)
            if _build_combined(bin_src, bl_bin, vect, combined_dest, mcu):
                print("pio_post_copy_firmware: %s (%d bytes) [bootloader + app]"
                      % (combined_dest, os.path.getsize(combined_dest)))
        else:
            print("pio_post_copy_firmware: no bootloader binary found for %s "
                  "in bootloader/ — combined binary skipped" % mcu)


env.AddPostAction("buildprog", _post_copy)
