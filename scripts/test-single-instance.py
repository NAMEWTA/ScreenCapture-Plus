#!/usr/bin/env python3
"""Exercise real Qt local IPC in separate processes; no capture success is implied."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import time


PRIMARY, INDEPENDENT, FORWARDED, FAILED = 1, 2, 3, 4
LISTEN, UNAVAILABLE, REJECTED, UNKNOWN = 4, 5, 6, 7


class Probe:
    def __init__(self, binary: Path, root: Path, scope: str, *arguments: str):
        self.process = subprocess.Popen([str(binary), f"--runtime={root}", f"--scope={scope}", *arguments],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
        self.events: list[dict] = []
        self.errors: list[str] = []
        self.pending: queue.Queue[dict | None] = queue.Queue()

        def output() -> None:
            assert self.process.stdout is not None
            for line in self.process.stdout:
                try:
                    self.pending.put(json.loads(line))
                except json.JSONDecodeError:
                    self.pending.put({"event": "invalid-json", "line": line})
            self.pending.put(None)

        def errors() -> None:
            assert self.process.stderr is not None
            for line in self.process.stderr:
                if len(self.errors) < 100:
                    self.errors.append(line.rstrip())

        self.readers = [threading.Thread(target=output, daemon=True), threading.Thread(target=errors, daemon=True)]
        for reader in self.readers:
            reader.start()

    def event(self, name: str, timeout: float = 12) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                item = self.pending.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty as error:
                raise AssertionError(f"Timed out waiting for {name}: {self.events}; stderr={self.errors}") from error
            if item is None:
                raise AssertionError(f"Probe exited before {name}: {self.events}; stderr={self.errors}")
            self.events.append(item)
            if item.get("event") == name:
                return item
            if item.get("event") == "invalid-json":
                raise AssertionError(item)
        raise AssertionError(f"No {name} event")

    def wait(self, expected: int = 0) -> None:
        status = self.process.wait(timeout=12)
        for reader in self.readers:
            reader.join(timeout=2)
        if status != expected:
            raise AssertionError(f"Probe exit {status}, expected {expected}: {self.events}; stderr={self.errors}")

    def close(self) -> None:
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait(timeout=5)
        for reader in self.readers:
            reader.join(timeout=2)
        if self.process.stdout:
            self.process.stdout.close()
        if self.process.stderr:
            self.process.stderr.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    args = parser.parse_args()
    binary = args.probe.resolve(strict=True)
    probes: list[Probe] = []
    with tempfile.TemporaryDirectory(prefix="huhu-ipc-") as temporary:
        root = Path(temporary)

        def spawn(scope: str, *arguments: str, runtime: Path | None = None) -> Probe:
            probe = Probe(binary, runtime or root, scope, *arguments)
            probes.append(probe)
            return probe

        def expect(probe: Probe, role: int, failure: int = 0) -> dict:
            event = probe.event("startup")
            if event["role"] != role or event["failure"] != failure:
                raise AssertionError(f"Unexpected startup: {event}")
            return event

        try:
            primary = spawn("forward", "--hold=12000", "--exit-after=2")
            expect(primary, PRIMARY)
            independent = spawn("forward", "--auto-quit", "--auto-start", "--enter=long")
            once = expect(independent, INDEPENDENT)
            assert once["oneShot"] and not once["background"] and once["entry"] == 2
            independent.wait()
            qr = spawn("forward", "--enter=qrcode")
            expect(qr, FORWARDED); qr.wait()
            action = primary.event("activation")
            assert action["count"] == 1 and action["entry"] == 4 and not action["oneShot"]
            tray = spawn("forward", "--enter=tray")
            expect(tray, FORWARDED); tray.wait()
            action = primary.event("activation")
            assert action["count"] == 2 and action["background"]
            primary.wait()
            restarted = spawn("forward")
            expect(restarted, PRIMARY); restarted.wait()

            absent = root / "must-not-be-created"
            independent = spawn("", "--auto-quit", runtime=absent)
            expect(independent, INDEPENDENT); independent.wait()
            assert not absent.exists(), "one-shot startup must not create IPC files"

            if os.name != "nt":
                private = root / "huhucapture"
                private.mkdir(mode=0o700, exist_ok=True)
                def occupied_endpoint(scope: str) -> Path:
                    digest = hashlib.sha256(str(root.resolve()).encode() + b"\0" + scope.encode()).hexdigest()[:32]
                    return private / ("huhucapture-" + digest)
                ordinary = occupied_endpoint("preserve-file")
                ordinary.write_text("Unrelated existing file must survive startup")
                guarded = spawn("preserve-file")
                expect(guarded, FAILED, LISTEN); guarded.wait(2)
                assert ordinary.read_text() == "Unrelated existing file must survive startup"
                linked = occupied_endpoint("preserve-link")
                linked.symlink_to(ordinary)
                guarded = spawn("preserve-link")
                expect(guarded, FAILED, LISTEN); guarded.wait(2)
                assert linked.is_symlink() and ordinary.read_text() == "Unrelated existing file must survive startup"

            refusing = spawn("reject", "--hold=12000", "--exit-after=1", "--reject")
            expect(refusing, PRIMARY)
            rejected = spawn("reject", "--enter=video")
            expect(rejected, FAILED, REJECTED); rejected.wait(2); refusing.wait()

            disappearing = spawn("lost-ack", "--hold=12000", "--destroy-on-activation")
            expect(disappearing, PRIMARY)
            uncertain = spawn("lost-ack", "--enter=pin")
            expect(uncertain, FAILED, UNKNOWN); uncertain.wait(2)
            action = disappearing.event("activation")
            assert action["count"] == 1
            disappearing.wait()

            throwing = spawn("throw-after-admission", "--hold=12000", "--exit-after=1", "--throw-after-activation")
            expect(throwing, PRIMARY)
            uncertain = spawn("throw-after-admission", "--enter=video")
            expect(uncertain, FAILED, UNKNOWN); uncertain.wait(2)
            assert throwing.event("activation")["count"] == 1
            throwing.wait()

            crashed = spawn("crash", "--hold=12000")
            expect(crashed, PRIMARY)
            crashed.close()  # A real process crash leaves the actual Qt lock/socket.
            recovered = spawn("crash")
            expect(recovered, PRIMARY); recovered.wait()

            stopping = spawn("stopping", "--hold=12000", "--stop-accepting")
            expect(stopping, PRIMARY)
            blocked = spawn("stopping")
            waiting = expect(blocked, FAILED, UNAVAILABLE)
            assert waiting["heartbeats"] >= 10, "waiting for the primary must not block the Qt event loop"
            blocked.wait(2); stopping.close()

            bounded = spawn("bounded", "--hold=15000", "--exit-after=1")
            endpoint = expect(bounded, PRIMARY)["endpoint"]
            for mode in ("oversized", "partial"):
                raw = spawn("ignored", f"--raw={mode}", f"--endpoint={endpoint}")
                assert raw.event("raw")["passed"] is True
                raw.wait()
            valid = spawn("bounded", "--enter=pin")
            expect(valid, FORWARDED); valid.wait()
            assert bounded.event("activation")["count"] == 1
            bounded.wait()

            first = spawn("race", "--hold=12000")
            second = spawn("race", "--hold=12000")
            outcomes = [first.event("startup"), second.event("startup")]
            assert sorted(event["role"] for event in outcomes) == [PRIMARY, FORWARDED]
            assert all(event["failure"] == 0 for event in outcomes)
            print("PASS real cross-process startup forwarding, one-shot isolation, rejection, lost acknowledgment, "
                  "crash recovery, shutdown exclusion, malformed peers and GUI heartbeat", flush=True)
        finally:
            for probe in reversed(probes):
                probe.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
