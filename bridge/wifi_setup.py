#!/usr/bin/env python3
"""Give the slime your Wi-Fi credentials over USB (run it yourself, in your own terminal).

    python3 bridge/wifi_setup.py

The passphrase is read with getpass (not echoed), sent once over the USB cable and stored
by the device's Wi-Fi driver; it is never printed or written to disk here. The ESP32-S31
only does 2.4 GHz. Afterwards the panel is at http://slime.local/.
"""
import getpass
import json
import os
import socket
import subprocess
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slime_hook  # noqa: E402  (same port lock and raw-tty handling as the hook)


def current_ssid():
    try:
        out = subprocess.run(["ipconfig", "getsummary", "en0"], capture_output=True, text=True, timeout=3).stdout
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("SSID :"):
                ssid = line.split(":", 1)[1].strip()
                return "" if ssid == "<redacted>" else ssid
    except Exception:
        pass
    return ""


def main():
    import glob

    if not glob.glob(slime_hook.PORT_GLOB):
        sys.exit("没找到史莱姆的 USB 串口（%s）。先用数据线连上设备。" % slime_hook.PORT_GLOB)
    guess = current_ssid()
    ssid = input("Wi-Fi 名称（只支持 2.4 GHz）%s: " % (f"[回车用 {guess}]" if guess else "")).strip() or guess
    if not ssid or len(ssid.encode()) > 32:
        sys.exit("名称为空或超过 32 字节。")
    psk = getpass.getpass("Wi-Fi 密码（输入时不显示，开放网络直接回车）: ")
    if len(psk.encode()) > 63 or any(c in psk + ssid for c in "\t\r\n"):
        sys.exit("密码超过 63 字节或含有制表符/换行，无法发送。")
    line = "wifi\t%s\t%s\n" % (ssid, psk)
    psk = None
    try:
        slime_hook.send_usb(line.encode("utf-8"))  # USB only; UTF-8 SSIDs (e.g. Chinese) pass through
    finally:
        line = None
    print("已发给史莱姆，正在等它连上 %s ……" % ssid)

    for i in range(30):
        time.sleep(1)
        try:
            ip = socket.gethostbyname("slime.local")
            with urllib.request.urlopen("http://%s/api/status" % ip, timeout=2) as r:
                st = json.load(r)
            net = st.get("net", {})
            if net.get("state") == "connected":
                with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".device_addr"), "w") as f:
                    f.write(ip + "\n")
                print("连上了：http://slime.local/  （IP %s，信号 %s dBm）" % (ip, net.get("rssi")))
                return
        except Exception:
            pass
    print("30 秒内没连上。看看设备屏幕上的提示（密码不对、或者是 5 GHz 网络），也可以在 USB 日志里找 net: 开头的行。")


if __name__ == "__main__":
    main()
