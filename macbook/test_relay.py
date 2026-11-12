#!/usr/bin/env python3
"""Manual test tool for running the KITB-Relay client from a MacBook.

Reuses ../pi/relay_client.py, so it exercises the exact code the Raspberry Pi runs.

  python macbook/test_relay.py ping
  python macbook/test_relay.py pulse 1 1000
  python macbook/test_relay.py health          # ping every 2 s, Ctrl+C to stop
  python macbook/test_relay.py demo            # ping, pulse relay 1 then 2
Optional: --port /dev/cu.usbmodemXXXX (auto-detected by default)
"""
import argparse
import glob
import sys
import time
from pathlib import Path

import importlib.util

import serial

# The client lives in pi/test_relay.py (renamed from relay_client.py); load it by path.
_spec = importlib.util.spec_from_file_location(
    "pi_client", Path(__file__).resolve().parents[1] / "pi" / "test_relay.py")
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)
RelayClient, RelayError, RelayTimeout = _mod.RelayClient, _mod.RelayError, _mod.RelayTimeout

_T0 = time.monotonic()


class TracedSerial:
    """Wraps serial.Serial and prints every line sent (TX) and received (RX)."""

    def __init__(self, port):
        self._s = serial.Serial(port, 115200, timeout=0.1)

    def write(self, data):
        print(f"[{time.monotonic() - _T0:7.3f}s] TX -> {data.decode().rstrip()}", flush=True)
        return self._s.write(data)

    def readline(self):
        raw = self._s.readline()
        if raw:
            print(f"[{time.monotonic() - _T0:7.3f}s] RX <- {raw.decode(errors='replace').rstrip()}", flush=True)
        return raw

    def close(self):
        self._s.close()


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("No /dev/cu.usbmodem* device found. Is the ESP32 plugged in?")
    return ports[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--port")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ping")
    p = sub.add_parser("pulse")
    p.add_argument("relay", type=int, choices=[1, 2])
    p.add_argument("ms", type=int, nargs="?", default=1000)
    sub.add_parser("health")
    sub.add_parser("demo")
    a = ap.parse_args()

    port = a.port or find_port()
    print(f"port: {port}  (first /dev/cu.usbmodem* found; use --port to choose)")
    with RelayClient(port, opener=lambda: TracedSerial(port), ping_interval=0) as c:
        time.sleep(1.5)  # USB CDC settle after open
        try:
            if a.cmd == "ping":
                print(c.ping())
            elif a.cmd == "pulse":
                t = time.perf_counter()
                print(c.pulse(a.relay, a.ms), f"({time.perf_counter() - t:.2f}s)")
            elif a.cmd == "health":
                while True:
                    t = time.perf_counter()
                    try:
                        r = c.ping()
                        print(f"OK {(time.perf_counter() - t) * 1000:.1f}ms uptime={r['uptime_ms']} "
                              f"busy={r['busy']} errs={r['errs']} drops={r['drops']} reset={r['reset']}")
                    except RelayTimeout:
                        print("MISS (no pong)")
                    time.sleep(2)
            elif a.cmd == "demo":
                print(c.ping())
                for r in (1, 2):
                    print(f"pulse relay {r} ...")
                    print(c.pulse(r, 1000))
        except RelayError as e:
            sys.exit(f"device error: {e.code}")
        except RelayTimeout as e:
            sys.exit(f"timeout: {e}")
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
