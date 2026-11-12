#!/usr/bin/env python3
"""Thread-safe client for the KITB-Relay ESP32-C3 (JSON lines over USB CDC).

* One reader thread dispatches replies to waiting requests by `id`.
* Pulses are retried with the SAME id; the firmware de-duplicates, so a retry
  can never fire the relay twice.
* A background health monitor pings the device and exposes `.online`;
  the port is reopened automatically after failures.
"""
import itertools
import json
import logging
import queue
import random
import threading
import time

import serial  # pip install pyserial

log = logging.getLogger("relay")


class RelayError(Exception):
    def __init__(self, code):
        super().__init__(code)
        self.code = code


class RelayTimeout(TimeoutError):
    pass


class RelayClient:
    PING_TIMEOUT = 0.5
    MISSES_BEFORE_OFFLINE = 3

    def __init__(self, port="/dev/ttyACM0", opener=None, ping_interval=3.0):
        self.port = port
        self._open = opener or (lambda: serial.Serial(port, 115200, timeout=0.1))
        self._ser = None
        self._lock = threading.Lock()          # guards writes and _waiters
        self._waiters = {}                     # id -> queue.Queue
        # Random start: avoids colliding with ids the firmware still remembers.
        self._ids = itertools.count(random.randint(1, 2**31))
        self._stop = threading.Event()
        self.online = False
        self.last_pong = None
        self._misses = 0
        self._ping_interval = ping_interval
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._monitor = threading.Thread(target=self._monitor_loop, daemon=True)

    # lifecycle ---------------------------------------------------------
    def start(self):
        self._connect()
        self._reader.start()
        if self._ping_interval:
            self._monitor.start()
        return self

    def close(self):
        self._stop.set()
        self._close_port()

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.close()

    def _connect(self):
        try:
            self._ser = self._open()
        except (serial.SerialException, OSError) as e:
            log.warning("open failed: %s", e)
            self._ser = None

    def _close_port(self):
        ser, self._ser = self._ser, None
        if ser:
            try:
                ser.close()
            except Exception:
                pass

    # io ------------------------------------------------------------------
    def _read_loop(self):
        while not self._stop.is_set():
            ser = self._ser
            if ser is None:
                time.sleep(0.2)
                continue
            try:
                raw = ser.readline()
            except (serial.SerialException, OSError):
                self._close_port()
                continue
            if not raw:
                continue
            try:
                msg = json.loads(raw)
            except ValueError:
                continue
            with self._lock:
                q = self._waiters.get(msg.get("id"))
            if q is not None:
                q.put(msg)

    def _send(self, payload):
        data = (json.dumps(payload, separators=(",", ":")) + "\n").encode()
        with self._lock:
            ser = self._ser
            if ser is None:
                raise RelayTimeout("port closed")
            try:
                ser.write(data)
            except (serial.SerialException, OSError) as e:
                self._close_port()
                raise RelayTimeout(f"write failed: {e}")

    def _transact(self, payload, final, timeout, attempts=1):
        rid = next(self._ids)
        payload = dict(payload, id=rid)
        q = queue.Queue()
        with self._lock:
            self._waiters[rid] = q
        try:
            last = None
            for _ in range(attempts):
                try:
                    self._send(payload)
                except RelayTimeout as e:
                    last = e
                    time.sleep(0.2)
                    if self._ser is None:
                        self._connect()
                    continue
                deadline = time.monotonic() + timeout
                while True:
                    left = deadline - time.monotonic()
                    if left <= 0:
                        last = RelayTimeout(f"no {final} for {payload['cmd']} id={rid}")
                        break
                    try:
                        msg = q.get(timeout=left)
                    except queue.Empty:
                        continue
                    if not msg.get("ok"):
                        raise RelayError(msg.get("err", "UNKNOWN"))
                    if msg.get("evt") == final:
                        return msg
            raise last
        finally:
            with self._lock:
                self._waiters.pop(rid, None)

    # api -----------------------------------------------------------------
    def ping(self):
        msg = self._transact({"cmd": "ping"}, "pong", self.PING_TIMEOUT)
        self.last_pong = msg
        return msg

    def pulse(self, relay, ms=1000, attempts=2):
        """Fire a relay for `ms`; returns the 'done' message. Safe to retry."""
        return self._transact({"cmd": "pulse", "relay": relay, "ms": ms},
                              "done", ms / 1000 + 1.0, attempts)

    # health ----------------------------------------------------------------
    def _monitor_loop(self):
        while not self._stop.wait(self._ping_interval):
            try:
                pong = self.ping()
                self._misses = 0
                if not self.online:
                    log.info("device online (reset=%s)", pong.get("reset"))
                self.online = True
            except (RelayTimeout, RelayError):
                self._misses += 1
                if self._misses >= self.MISSES_BEFORE_OFFLINE:
                    if self.online:
                        log.error("device offline")
                    self.online = False
                    self._close_port()
                    self._connect()


if __name__ == "__main__":
    import sys
    logging.basicConfig(level=logging.INFO)
    with RelayClient(sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0", ping_interval=0) as c:
        print(c.ping())
        print(c.pulse(1, 1000))
