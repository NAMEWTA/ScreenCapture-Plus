#!/usr/bin/env python3
"""Run real Wayland parent lifecycle tests in an owned headless Sway process.

The probe has no PID/signal API. Only this coordinator can stop its own direct
child, and an independent watchdog resumes it even if the probe GUI blocks.
This is not a Portal, PipeWire, GNOME, Plasma or physical-display acceptance test.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
from pathlib import Path
import selectors
import shutil
import signal
import stat
import subprocess
import tempfile
import threading
import time


MODES = ("export", "cancel", "destroy", "timeout", "destroy-owner", "invalidate")
PAUSED = frozenset(("cancel", "destroy", "timeout", "destroy-owner"))


def resume(child: subprocess.Popen) -> None:
    if child.poll() is None:
        child.send_signal(signal.SIGCONT)


def stop_owned(child: subprocess.Popen) -> None:
    if child.poll() is not None:
        raise RuntimeError("The owned compositor exited before the pause test")
    child.send_signal(signal.SIGSTOP)
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        pid, status = os.waitpid(child.pid, os.WNOHANG | os.WUNTRACED)
        if pid:
            if os.WIFSTOPPED(status):
                return
            if os.WIFEXITED(status) or os.WIFSIGNALED(status):
                child.returncode = os.waitstatus_to_exitcode(status)
                raise RuntimeError("The owned compositor exited instead of stopping")
        time.sleep(0.01)
    raise TimeoutError("The owned compositor did not acknowledge SIGSTOP")


def terminate(child: subprocess.Popen) -> None:
    resume(child)
    if child.poll() is None:
        child.terminate()
        try:
            child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait(timeout=3)


def run_case(probe_path: Path, mode: str, env: dict[str, str], compositor: subprocess.Popen, artifacts: Path) -> None:
    records: list[dict] = []
    expired = threading.Event()
    with (artifacts / f"{mode}.stderr.log").open("wb") as errors:
        probe = subprocess.Popen([str(probe_path), "--mode", mode], env=env,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors,
                                 start_new_session=True, bufsize=0)
        assert probe.stdin is not None and probe.stdout is not None

        def watchdog() -> None:
            expired.set()
            resume(compositor)
            if probe.poll() is None:
                probe.kill()

        timer = threading.Timer(15, watchdog)
        timer.daemon = True
        timer.start()
        ready = False
        result = None
        drained = False
        pending = bytearray()
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(probe.stdout, selectors.EVENT_READ)
                while probe.poll() is None or selector.get_map():
                    for key, _ in selector.select(timeout=0.2):
                        chunk = os.read(key.fd, 65536)
                        if not chunk:
                            selector.unregister(key.fileobj)
                            continue
                        pending.extend(chunk)
                        while b"\n" in pending:
                            line, _, remaining = pending.partition(b"\n")
                            pending[:] = remaining
                            event = json.loads(line)
                            records.append(event)
                            print(json.dumps(event, ensure_ascii=False), flush=True)
                            if event.get("event") == "ready":
                                if ready:
                                    raise RuntimeError("Probe emitted ready twice")
                                ready = True
                                if mode in PAUSED:
                                    stop_owned(compositor)
                                probe.stdin.write(b"start\n")
                                probe.stdin.flush()
                            elif event.get("event") == "result":
                                if result is not None:
                                    raise RuntimeError("Probe emitted a second terminal result")
                                result = event
                                resume(compositor)
                                probe.stdin.write(b"resume\n")
                                probe.stdin.flush()
                            elif event.get("event") == "drained":
                                drained = event.get("passed") is True and event.get("ownerReleased") is True
                    if expired.is_set():
                        raise TimeoutError(f"{mode}: external watchdog rescued the compositor after a stuck probe")
                    if compositor.poll() is not None:
                        raise RuntimeError("The isolated compositor exited during the probe")
                status = probe.wait(timeout=2)
            if status != 0 or not ready or not result or not result.get("passed") or not drained:
                raise RuntimeError(f"{mode}: real Wayland validation failed (exit {status}); see recorded logs")
        finally:
            timer.cancel()
            timer.join(timeout=3)
            resume(compositor)
            terminate(probe)
            probe.stdin.close()
            probe.stdout.close()
            (artifacts / f"{mode}.json").write_text(json.dumps(records, ensure_ascii=False, indent=2) + "\n")


def main() -> int:
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("probe", type=Path)
    args.add_argument("--artifacts", type=Path, default=Path("artifacts/tests/wayland-parent"))
    parsed = args.parse_args()
    probe = parsed.probe.resolve(strict=True)
    sway = shutil.which("sway")
    if not sway:
        raise RuntimeError("Sway is required; no fake compositor or skipped pass is substituted")
    artifacts = parsed.artifacts.resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    config = Path(__file__).resolve().parents[1] / "tests/platform/sway-parent.conf"
    version = subprocess.run([sway, "--version"], check=True, capture_output=True, text=True).stdout.strip()
    (artifacts / "environment.json").write_text(json.dumps({"compositor": version, "renderer": "pixman",
        "backend": "headless", "modes": MODES, "portalAcceptance": False, "physicalDisplay": False}, indent=2) + "\n")

    # SIGTERM/interrupt goes through finally. PR_SET_PDEATHSIG additionally
    # kills this owned compositor if the coordinator itself is forcibly killed.
    def interrupted(signum: int, _frame) -> None:
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGTERM, interrupted)
    owner_pid = os.getpid()

    def child_guard() -> None:
        libc = ctypes.CDLL(None, use_errno=True)
        if libc.prctl(1, int(signal.SIGKILL), 0, 0, 0) != 0 or os.getppid() != owner_pid:
            os._exit(127)

    with tempfile.TemporaryDirectory(prefix="huhucapture-wayland-") as directory:
        runtime = Path(directory)
        runtime.chmod(0o700)
        env = os.environ.copy()
        for name in ("WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DISPLAY", "SWAYSOCK", "I3SOCK", "XAUTHORITY"):
            env.pop(name, None)
        env.update({"XDG_RUNTIME_DIR": str(runtime), "XDG_SESSION_TYPE": "wayland", "XDG_CURRENT_DESKTOP": "sway",
                    "WLR_BACKENDS": "headless", "WLR_RENDERER": "pixman", "WLR_HEADLESS_OUTPUTS": "1",
                    "WLR_LIBINPUT_NO_DEVICES": "1", "QT_QPA_PLATFORM": "wayland",
                    "QT_WAYLAND_DISABLE_WINDOWDECORATION": "1"})
        # Do not register a test compositor in an existing user's session bus.
        env["DBUS_SESSION_BUS_ADDRESS"] = "unix:path=/nonexistent/huhucapture-parent-probe"
        with (artifacts / "sway.log").open("wb") as log:
            compositor = subprocess.Popen([sway, "--config", str(config), "--debug"], env=env,
                stdout=log, stderr=subprocess.STDOUT, start_new_session=True, preexec_fn=child_guard)
            try:
                deadline = time.monotonic() + 10
                socket = None
                while time.monotonic() < deadline:
                    if compositor.poll() is not None:
                        raise RuntimeError("Headless Sway failed to start; see sway.log")
                    sockets = [path for path in runtime.glob("wayland-*") if stat.S_ISSOCK(path.stat().st_mode)]
                    if sockets:
                        socket = sockets[0]
                        break
                    time.sleep(0.02)
                if socket is None:
                    raise TimeoutError("Headless Sway did not create a real Wayland socket")
                env["WAYLAND_DISPLAY"] = socket.name
                for mode in MODES:
                    run_case(probe, mode, env, compositor, artifacts)
            finally:
                terminate(compositor)
    print("Real headless Sway parent export, cancellation, destruction, timeout and event-drain checks passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
