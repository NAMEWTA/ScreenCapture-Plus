#!/usr/bin/env python3
"""Run capture tests on an owned Xvfb or dummy-Xorg display, never the user desktop.

The server uses host libraries, no devices or TCP, and a private Xauthority file.
Only the application client receives the caller's pinned SDK library environment.
This validates real XCB/SHM/XTEST and Qt coordination, not physical monitors.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
from pathlib import Path
import secrets
import signal
import subprocess
import tempfile
import time


def host_binary(name: str) -> Path:
    roots = (Path("/usr/lib/xorg"), Path("/usr/bin")) if name == "Xorg" else (Path("/usr/bin"),)
    for root in roots:
        candidate = root / name
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    raise RuntimeError(f"Required host executable is unavailable: {name}")


def stop_owned(process: subprocess.Popen) -> None:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("--artifacts", type=Path, default=Path("artifacts/tests/x11-capture"))
    parsed = parser.parse_args()
    owner_pid = os.getpid()

    def child_guard() -> None:
        libc = ctypes.CDLL(None, use_errno=True)
        if libc.prctl(1, int(signal.SIGKILL), 0, 0, 0) != 0 or os.getppid() != owner_pid:
            os._exit(127)

    def interrupted(signum: int, _frame) -> None:
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGTERM, interrupted)
    probe = parsed.probe.resolve(strict=True)
    if not os.access(probe, os.X_OK):
        raise RuntimeError("The capture probe is not executable")
    artifacts = parsed.artifacts.resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    (artifacts / "result.json").unlink(missing_ok=True)
    try:
        server = host_binary("Xvfb")
        kind = "Xvfb"
    except RuntimeError:
        server = host_binary("Xorg")
        if not Path("/usr/lib/xorg/modules/drivers/dummy_drv.so").is_file():
            raise RuntimeError("Neither Xvfb nor the Xorg dummy video driver is available") from None
        kind = "dummy-Xorg"
    display_number = next((number for number in range(150, 350)
        if not Path(f"/tmp/.X11-unix/X{number}").exists() and not Path(f"/tmp/.X{number}-lock").exists()), None)
    if display_number is None:
        raise RuntimeError("No unused isolated X11 display number is available")
    display = f":{display_number}"
    with tempfile.TemporaryDirectory(prefix="huhucapture-x11-") as directory:
        private = Path(directory)
        runtime = private / "runtime"
        runtime.mkdir(mode=0o700)
        authority = private / "authority"
        authority.touch(mode=0o600)
        host_env = {key: value for key, value in os.environ.items() if key not in {
            "LD_LIBRARY_PATH", "LD_PRELOAD", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
            "DISPLAY", "XAUTHORITY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS"}}
        host_env.update({"PATH": "/usr/bin:/bin", "DISPLAY": display, "XAUTHORITY": str(authority),
            "XDG_RUNTIME_DIR": str(runtime), "XDG_SESSION_TYPE": "x11"})
        subprocess.run([str(host_binary("xauth")), "-f", str(authority), "add", display, ".", secrets.token_hex(16)],
            env=host_env, check=True, timeout=5, capture_output=True)
        command = [str(server), display, "-noreset", "-nolisten", "tcp", "-auth", str(authority)]
        if kind == "Xvfb":
            command += ["-screen", "0", "1280x1024x24"]
        else:
            config = private / "xorg.conf"
            config.write_text('''Section "ServerFlags"
    Option "AutoAddDevices" "false"
    Option "AutoEnableDevices" "false"
    Option "DontVTSwitch" "true"
    Option "AllowMouseOpenFail" "true"
EndSection
Section "Device"
    Identifier "OwnedDummy"
    Driver "dummy"
    VideoRam 256000
EndSection
Section "Monitor"
    Identifier "OwnedMonitor"
    HorizSync 28.0-80.0
    VertRefresh 48.0-75.0
    Modeline "1280x1024" 108.00 1280 1328 1440 1688 1024 1025 1028 1066
EndSection
Section "Screen"
    Identifier "OwnedScreen"
    Device "OwnedDummy"
    Monitor "OwnedMonitor"
    DefaultDepth 24
    SubSection "Display"
        Depth 24
        Modes "1280x1024"
        Virtual 1280 1024
    EndSubSection
EndSection
Section "ServerLayout"
    Identifier "OwnedLayout"
    Screen "OwnedScreen"
EndSection
''', encoding="utf-8")
            command += ["-config", str(config), "-logfile", str(artifacts / "xorg.log"),
                "-novtswitch", "-sharevts"]
        with (artifacts / "server.log").open("wb") as log:
            process = subprocess.Popen(command, env=host_env, stdout=log, stderr=log, preexec_fn=child_guard)
            try:
                deadline = time.monotonic() + 15
                while True:
                    if process.poll() is not None:
                        raise RuntimeError("Owned X server exited before readiness; inspect server.log")
                    reply = subprocess.run([str(host_binary("xdpyinfo"))], env=host_env,
                        capture_output=True, timeout=3, text=True)
                    if reply.returncode == 0:
                        (artifacts / "display.txt").write_text(reply.stdout, encoding="utf-8")
                        break
                    if time.monotonic() > deadline:
                        raise TimeoutError("Owned X server did not become ready")
                    time.sleep(.05)
                client_env = os.environ.copy()
                for key in ("WAYLAND_DISPLAY", "QT_SCREEN_SCALE_FACTORS", "QT_SCALE_FACTOR_ROUNDING_POLICY"):
                    client_env.pop(key, None)
                client_env.update({"DISPLAY": display, "XAUTHORITY": str(authority),
                    "XDG_RUNTIME_DIR": str(runtime), "XDG_SESSION_TYPE": "x11", "QT_QPA_PLATFORM": "xcb",
                    "QT_SCALE_FACTOR": "1", "QT_ACCESSIBILITY": "0", "NO_AT_BRIDGE": "1",
                    "DBUS_SESSION_BUS_ADDRESS": f"unix:path={private / 'no-session-bus'}",
                    "HUHUCAPTURE_TEST_X11_ISOLATED": "1"})
                with (artifacts / "probe.stdout.log").open("wb") as output, (artifacts / "probe.stderr.log").open("wb") as errors:
                    result = subprocess.run([str(probe)], env=client_env, stdout=output, stderr=errors, timeout=120)
                if result.returncode:
                    raise RuntimeError(f"The isolated X11 capture probe failed ({result.returncode}); inspect probe logs")
                (artifacts / "result.json").write_text(json.dumps({"passed": True, "server": kind,
                    "serverExecutable": str(server), "probe": str(probe), "physicalDisplay": False,
                    "waylandAuthorization": False, "audioTested": False}, indent=2) + "\n", encoding="utf-8")
                print(f"Real isolated {kind} capture probe passed", flush=True)
            finally:
                stop_owned(process)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
