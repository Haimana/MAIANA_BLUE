# PlatformIO post-build: copy firmware.bin into Binaries/ with a descriptive name.
# Disable with:  PIO_COPY_FIRMWARE_BIN=0 pio run

Import("env")

import os
import re
import shutil


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
    print("pio_post_copy_firmware:", dest)


env.AddPostAction("buildprog", _post_copy)
