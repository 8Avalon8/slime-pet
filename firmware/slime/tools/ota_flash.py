#!/usr/bin/env python3
"""Wireless firmware update for the slime pet.

Sends build/slime_pet.bin to http://<device>/api/ota, then waits for the device to reboot into
the other app slot and confirm itself (until then the bootloader would roll back on a reset).

The update token is fetched once over USB ("otatoken") and cached in bridge/.ota_token; after
that no cable is needed. USB flashing (backup_and_flash.py) stays the recovery path.

Usage: python tools/ota_flash.py [--host 192.168.x.y] [--log /tmp/slime_build.log]
"""
import argparse
import fcntl
import glob
import http.client
import json
import os
import re
import sys
import termios
import time
import tty
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(os.path.dirname(ROOT))
IMAGE = os.path.join(ROOT, "build", "slime_pet.bin")
BRIDGE = os.path.join(REPO, "bridge")
TOKEN_FILE = os.path.join(BRIDGE, ".ota_token")
ADDR_FILE = os.path.join(BRIDGE, ".device_addr")
PORT_LOCK = os.path.join(BRIDGE, ".port.lock")


def device_host(arg):
    if arg:
        return arg
    try:
        first = open(ADDR_FILE).read().split()[0]
        if re.fullmatch(r"\d+\.\d+\.\d+\.\d+", first):
            return first
    except OSError:
        pass
    return "slime.local"


def token_over_usb():
    ports = sorted(glob.glob("/dev/cu.usbmodem1234561")) or sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("RESULT: no update token yet and no USB connection; plug the device in once")
    lock = open(PORT_LOCK, "w")
    fcntl.flock(lock, fcntl.LOCK_EX)
    fd = os.open(ports[0], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        tty.setraw(fd)  # no echo: the device must not get its own log back
        os.write(fd, b"otatoken\n")
        termios.tcdrain(fd)
        buf, end = b"", time.time() + 5
        while time.time() < end:
            try:
                buf += os.read(fd, 4096)
            except BlockingIOError:
                time.sleep(0.05)
            m = re.search(rb"OTATOKEN ([0-9a-f]{16})", buf)
            if m:
                return m.group(1).decode()
    finally:
        os.close(fd)
    sys.exit("RESULT: the device did not answer the token request over USB")


def get_token():
    try:
        tok = open(TOKEN_FILE).read().strip()
        if re.fullmatch(r"[0-9a-f]{16}", tok):
            return tok
    except OSError:
        pass
    tok = token_over_usb()
    with open(TOKEN_FILE, "w") as f:
        f.write(tok + "\n")
    os.chmod(TOKEN_FILE, 0o600)
    print("update token fetched over USB and saved", flush=True)
    return tok


def status(host, timeout=4):
    with urllib.request.urlopen(f"http://{host}/api/status", timeout=timeout) as r:
        return json.load(r)


class Progress:
    """File wrapper that prints how much http.client has read."""

    def __init__(self, path):
        self.f = open(path, "rb")
        self.total = os.path.getsize(path)
        self.done = 0
        self.shown = -1

    def read(self, n=-1):
        b = self.f.read(65536 if n is None or n < 0 else min(n, 65536))
        self.done += len(b)
        pct = self.done * 100 // self.total
        if pct // 10 != self.shown // 10 or pct == 100:
            self.shown = pct
            print(f"  sent {pct}%", flush=True)
        return b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host")
    ap.add_argument("--log", default="/tmp/slime_build.log")
    args = ap.parse_args()

    try:
        if "Project build complete" not in open(args.log, errors="replace").read():
            sys.exit(f"RESULT: {args.log} has no successful build; not sending a stale image")
    except OSError:
        sys.exit(f"RESULT: no build log at {args.log}; run idf.py build > {args.log} first")
    if not os.path.isfile(IMAGE):
        sys.exit("RESULT: build/slime_pet.bin missing")

    host = device_host(args.host)
    tok = get_token()
    before = status(host)
    fw0 = before.get("fw", {})
    print(f"device {host}: running {fw0.get('part')} ({fw0.get('state')}), built {fw0.get('built')}, up {before.get('up')} s",
          flush=True)

    body = Progress(IMAGE)
    print(f"sending {body.total / 1e6:.2f} MB", flush=True)
    conn = http.client.HTTPConnection(host, 80, timeout=300)
    t0 = time.time()
    conn.request("POST", "/api/ota", body=body,
                 headers={"X-OTA-Token": tok, "Content-Length": str(body.total),
                          "Content-Type": "application/octet-stream"})
    resp = conn.getresponse()
    text = resp.read().decode(errors="replace")
    if resp.status == 401:
        os.remove(TOKEN_FILE)  # stale (device NVS erased?): fetch again over USB next time
        sys.exit("RESULT: token rejected; deleted the cached one, run again with USB plugged in")
    if resp.status != 200:
        sys.exit(f"RESULT: UPDATE FAILED ({resp.status}): {text}")
    target = json.loads(text).get("part")
    print(f"written to {target} in {time.time() - t0:.0f} s; waiting for the reboot", flush=True)

    # back up and running in the new slot?
    end = time.time() + 90
    st = None
    while time.time() < end:
        time.sleep(2)
        try:
            st = status(host, timeout=3)
        except Exception:  # noqa: BLE001 - rebooting
            continue
        if st.get("fw", {}).get("part") == target and st.get("up", 999) < 60:
            break
        st = None
    if not st:
        sys.exit("RESULT: device did not come back in the new slot within 90 s (it may have rolled back)")
    print(f"booted {target}, built {st['fw'].get('built')}; waiting for it to confirm itself", flush=True)

    end = time.time() + 60
    while time.time() < end:
        try:
            st = status(host, timeout=3)
            if st.get("fw", {}).get("state") == "valid":
                print(f"RESULT: OTA OK, running {target}, confirmed")
                return
        except Exception:  # noqa: BLE001
            pass
        time.sleep(3)
    sys.exit("RESULT: new firmware runs but did not confirm within 60 s (a reset now would roll back)")


if __name__ == "__main__":
    main()
