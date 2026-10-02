#!/usr/bin/env python3
"""Back up the full 16 MB flash once, then flash the slime firmware -- in ONE
esptool session, so the ROM download mode is entered only once.

Run inside the ESP-IDF environment (. ~/esp/esp-idf/export.sh):
    python tools/backup_and_flash.py              # backup if missing, then flash
    python tools/backup_and_flash.py --no-flash   # backup only
"""
import fcntl
import glob
import json
import hashlib
import os
import sys
import time

from esptool.cmds import attach_flash, detect_chip, read_flash, reset_chip, run_stub, write_flash

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(os.path.dirname(ROOT))
BUILD = os.path.join(ROOT, "build")
# the very first run saves the board's whole flash (the factory firmware) before anything is
# overwritten; any earlier full backup in this folder counts (git ignores the folder)
BACKUP_DIR = os.path.join(REPO, "backup")
_existing = sorted(glob.glob(os.path.join(BACKUP_DIR, "*_full16MB.bin")))
BACKUP = _existing[0] if _existing else os.path.join(BACKUP_DIR, "factory_full16MB.bin")
FLASH_SIZE = 16 * 1024 * 1024
NVS_OFFSET, NVS_SIZE = 0x9000, 0x6000  # settings, Lv/EXP, Wi-Fi: saved before every flash


def load_images():
    """Everything idf.py would flash (bootloader, partition table, otadata, app), from the build
    itself, so a new image such as ota_data_initial.bin cannot be forgotten."""
    with open(os.path.join(BUILD, "flasher_args.json")) as f:
        files = json.load(f)["flash_files"]
    return sorted((int(addr, 16), rel) for addr, rel in files.items())


IMAGES = load_images() if os.path.isfile(os.path.join(BUILD, "flasher_args.json")) else []


DL_MAGIC = b"SLIME-ENTER-DOWNLOAD"  # must match USB_LINK_DL_MAGIC in main/usb_link.h


def kick_into_download(port):
    """If the slime firmware owns the port, ask it to reboot into ROM download mode.

    Harmless when the ROM is already there: the ROM ignores bytes outside SLIP frames,
    and if the port does not drop we simply carry on with it."""
    import serial

    try:
        with serial.Serial(port, 115200, timeout=0.2) as s:
            s.write(DL_MAGIC + b"\n")
            s.flush()
    except Exception as e:  # noqa: BLE001
        print(f"could not write to {port}: {e}")
        return port
    end = time.time() + 12  # the app acts on the magic only once its main loop runs (~seconds after boot)
    while time.time() < end:
        if not glob.glob("/dev/cu.usbmodem*"):
            print("app rebooted into download mode, waiting for ROM port", flush=True)
            return wait_port(timeout=15)
        time.sleep(0.1)
    return port


def wait_port(timeout=600):
    print("waiting for /dev/cu.usbmodem* (download mode: hold BOOT + power on)", flush=True)
    end = time.time() + timeout
    while time.time() < end:
        ports = sorted(glob.glob("/dev/cu.usbmodem*"))
        if ports:
            return ports[0]
        time.sleep(0.5)
    sys.exit("RESULT: TIMEOUT no port")


# Shared with bridge/slime_hook.py: while we hold it, Claude Code hooks stay off the port
# (a stray "cc ..." line in the ROM loader would corrupt the transfer).
PORT_LOCK = os.path.join(REPO, "bridge", ".port.lock")


def main():
    no_flash = "--no-flash" in sys.argv
    os.makedirs(os.path.dirname(PORT_LOCK), exist_ok=True)
    lock = open(PORT_LOCK, "w")
    fcntl.flock(lock, fcntl.LOCK_EX)
    if not IMAGES:
        sys.exit("RESULT: no build/flasher_args.json; run idf.py build first")
    print("images: " + ", ".join(f"0x{a:x} {r}" for a, r in IMAGES), flush=True)
    for _, rel in IMAGES:
        if not no_flash and not os.path.isfile(os.path.join(BUILD, rel)):
            sys.exit(f"RESULT: missing build output {rel}; run idf.py build first")
    port = wait_port()
    print(f"port: {port}", flush=True)
    port = kick_into_download(port)
    time.sleep(1)

    esp = detect_chip(port, connect_mode="no-reset")
    print(f"chip: {esp.get_chip_description()}  mac: {':'.join(f'{b:02x}' for b in esp.read_mac())}", flush=True)
    esp = run_stub(esp)
    attach_flash(esp)

    if not os.path.isfile(BACKUP):
        os.makedirs(BACKUP_DIR, exist_ok=True)
        t0 = time.time()
        data = read_flash(esp, 0, FLASH_SIZE, no_progress=True)
        if len(data) != FLASH_SIZE:
            sys.exit(f"RESULT: BACKUP FAILED, got {len(data)} bytes")
        if data[:65536].count(0xFF) > 65536 - 1024:
            sys.exit("RESULT: BACKUP SUSPICIOUS, first 64 KB is blank")
        with open(BACKUP, "wb") as f:
            f.write(data)
        digest = hashlib.sha256(data).hexdigest()
        with open(BACKUP + ".sha256", "w") as f:
            f.write(f"{digest}  {os.path.basename(BACKUP)}\n")
        print(f"backup ok: {BACKUP} ({time.time() - t0:.0f}s) sha256 {digest}", flush=True)
    else:
        print(f"backup already present: {BACKUP}", flush=True)

    if no_flash:
        print("RESULT: BACKUP DONE (chip left in download mode)")
        return

    nvs = read_flash(esp, NVS_OFFSET, NVS_SIZE, no_progress=True)
    nvs_path = os.path.join(BACKUP_DIR, time.strftime("nvs-%Y%m%d-%H%M%S.bin"))
    os.makedirs(BACKUP_DIR, exist_ok=True)
    with open(nvs_path, "wb") as f:
        f.write(nvs)
    print(f"settings backup: {nvs_path}", flush=True)

    t0 = time.time()
    write_flash(
        esp,
        [(addr, os.path.join(BUILD, rel)) for addr, rel in IMAGES],
        flash_mode="dio",
        flash_freq="80m",
        flash_size="16MB",
    )
    print(f"flash written and verified ({time.time() - t0:.0f}s)", flush=True)
    reset_chip(esp, "hard-reset")
    print("RESULT: FLASH OK, rebooting into slime firmware")


if __name__ == "__main__":
    main()
