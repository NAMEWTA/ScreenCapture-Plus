#!/usr/bin/env python3
"""Use an owned headless Sway, real virtual-keyboard input, and two Qt processes.

Reuses the existing capture-probe host/cleanup helpers. No fake clipboard, mocked
selection, physical-desktop acceptance, or generated input serial is involved.
"""
from __future__ import annotations

import argparse
import ctypes
import json
import os
from pathlib import Path
import runpy
import selectors
import signal
import stat
import subprocess
import tempfile
import time


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("--artifacts", type=Path, default=Path("artifacts/tests/wayland-clipboard"))
    args = parser.parse_args()
    probe = args.probe.resolve(strict=True)
    artifacts = args.artifacts.resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    helper = runpy.run_path(str(Path(__file__).with_name("test-pipewire-capture.py")))
    terminate = helper["terminate"]
    host_binary = helper["host_binary"]
    sway, wtype = host_binary("sway"), host_binary("wtype")
    host_base = os.environ.copy()
    for key in list(host_base):
        if key.startswith(("PIPEWIRE_", "SPA_", "WIREPLUMBER_", "WP_", "WLR_")) or key in (
            "LD_LIBRARY_PATH", "LD_PRELOAD", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DISPLAY",
            "SWAYSOCK", "I3SOCK", "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS", "DBUS_STARTER_ADDRESS",
            "DBUS_SESSION_BUS_PID", "DBUS_SESSION_BUS_WINDOWID", "DBUS_STARTER_BUS_TYPE",
            "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH"):
            host_base.pop(key, None)
    version = subprocess.run([sway, "--version"], env=host_base, check=True, capture_output=True, text=True).stdout.strip()
    packages = subprocess.run([host_binary("dpkg-query"), "-W", "sway", "wtype", "libwayland-client0"],
                              env=host_base, capture_output=True, text=True, check=True).stdout
    (artifacts / "environment.json").write_text(json.dumps({"compositor": version,
        "packages": packages, "input": "wtype virtual keyboard", "physicalDesktopAcceptance": False}, indent=2) + "\n")
    def interrupted(signum: int, _frame) -> None:
        raise SystemExit(128 + signum)
    signal.signal(signal.SIGTERM, interrupted)
    owner_pid = os.getpid()
    def child_guard() -> None:
        libc = ctypes.CDLL(None, use_errno=True)
        if libc.prctl(1, int(signal.SIGKILL), 0, 0, 0) != 0 or os.getppid() != owner_pid:
            os._exit(127)
    with tempfile.TemporaryDirectory(prefix="huhucapture-clipboard-") as name:
        runtime = Path(name)
        runtime.chmod(0o700)
        host_env = host_base.copy()
        for kind in ("config", "data", "state", "cache"):
            path = runtime / kind
            path.mkdir(mode=0o700)
            host_env[f"XDG_{kind.upper()}_HOME"] = str(path)
        host_env.update({"XDG_RUNTIME_DIR": str(runtime), "XDG_SESSION_TYPE": "wayland", "XDG_CURRENT_DESKTOP": "sway",
            "XDG_CONFIG_DIRS": "/etc/xdg", "XDG_DATA_DIRS": "/usr/local/share:/usr/share",
            "WLR_BACKENDS": "headless", "WLR_RENDERER": "pixman", "WLR_HEADLESS_OUTPUTS": "1", "WLR_LIBINPUT_NO_DEVICES": "1",
            "QT_QPA_PLATFORM": "wayland", "QT_WAYLAND_DISABLE_WINDOWDECORATION": "1",
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/nonexistent/huhucapture-clipboard-probe"})
        # Only the Qt sender and its independent Qt receiver inherit the fixed
        # application loader/plugin paths. Sway, wtype and package tools use
        # host libraries, never the application's Qt/libportal/PipeWire prefix.
        client_env = host_env.copy()
        for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH"):
            if key in os.environ:
                client_env[key] = os.environ[key]
        config = Path(__file__).resolve().parents[1] / "tests/platform/sway-parent.conf"
        with (artifacts / "sway.log").open("wb") as log:
            compositor = subprocess.Popen([sway, "--config", str(config), "--debug"], env=host_env,
                stdout=log, stderr=subprocess.STDOUT, start_new_session=True, preexec_fn=child_guard)
            process = None
            try:
                deadline = time.monotonic() + 10
                socket = None
                while time.monotonic() < deadline:
                    if compositor.poll() is not None:
                        raise RuntimeError("The owned Sway failed before creating a Wayland socket")
                    sockets = [path for path in runtime.glob("wayland-*") if stat.S_ISSOCK(path.stat().st_mode)]
                    if sockets:
                        socket = sockets[0]
                        break
                    time.sleep(0.02)
                if socket is None:
                    raise TimeoutError("The owned compositor never produced a real socket")
                host_env["WAYLAND_DISPLAY"] = socket.name
                client_env["WAYLAND_DISPLAY"] = socket.name
                with (artifacts / "probe.stderr.log").open("wb") as errors, (artifacts / "probe.stdout.log").open("wb") as output:
                    process = subprocess.Popen([str(probe)], env=client_env, stdout=subprocess.PIPE,
                                               stderr=errors, start_new_session=True, preexec_fn=child_guard, bufsize=0)
                    assert process.stdout is not None
                    pending = bytearray()
                    inputs = 0
                    deadline = time.monotonic() + 90
                    with selectors.DefaultSelector() as selector:
                        selector.register(process.stdout, selectors.EVENT_READ)
                        while process.poll() is None or selector.get_map():
                            if time.monotonic() > deadline:
                                raise TimeoutError("The real clipboard owner/receiver did not terminate")
                            if compositor.poll() is not None:
                                raise RuntimeError("The owned compositor exited during clipboard validation")
                            for key, _ in selector.select(timeout=0.1):
                                chunk = os.read(key.fd, 65536)
                                if not chunk:
                                    selector.unregister(key.fileobj)
                                    continue
                                output.write(chunk)
                                output.flush()
                                pending.extend(chunk)
                                while b"\n" in pending:
                                    line, _, rest = pending.partition(b"\n")
                                    pending[:] = rest
                                    print(line.decode("utf-8", errors="replace"), flush=True)
                                    if line == b"NEED_INPUT":
                                        # The real compositor creates the serial
                                        # from input received by Qt's window.
                                        subprocess.run([wtype, "-k", "F8"], env=host_env, check=True, timeout=5)
                                        inputs += 1
                    status = process.wait(timeout=2)
                    process.stdout.close()
                    if status != 0 or inputs < 4:
                        raise RuntimeError(f"Real Wayland clipboard validation failed: exit={status}, inputs={inputs}")
            finally:
                if process is not None:
                    terminate(process)
                terminate(compositor)
    print("Real Wayland publication receipts and separate-process PNG/URI reception passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
