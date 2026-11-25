#!/usr/bin/env python3
"""Capture actual Portal-wlr/PipeWire frames in an owned headless Sway session.

The system daemon, pinned desktop and application client use separate library
environments. A painted Wayland surface, native SHM transport, output resize and
loss of this script's own daemon supply the evidence. No frames or Portal replies
are replaced. This does not prove GNOME/Plasma authorization or physical devices.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import selectors
import signal
import stat
import subprocess
import tempfile
import threading
import time


MODES = ("capture", "resize", "daemon-loss")


def host_binary(name: str) -> str:
    # The validation prefix can also contain a daemon built as an upstream
    # build side effect. Never select it through a caller's modified PATH.
    for directory in ("/usr/bin", "/usr/libexec", "/usr/lib"):
        path = Path(directory) / name
        if path.is_file() and os.access(path, os.X_OK):
            return str(path)
    raise RuntimeError(f"Required Ubuntu host binary is missing: {name}")


def terminate(child: subprocess.Popen) -> None:
    if child.poll() is None:
        child.send_signal(signal.SIGCONT)
        child.terminate()
        try:
            child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait(timeout=3)


def unmount_owned_documents(runtime: Path, env: dict[str, str]) -> None:
    # xdg-document-portal normally unmounts on SIGTERM. After an exceptional
    # forced exit, detach only this run's known FUSE mount before removing its
    # private directory; never target the user's session or another mount.
    mount = str(runtime / "doc")
    for line in Path("/proc/self/mountinfo").read_text().splitlines():
        fields = line.split()
        if len(fields) > 5 and fields[4] == mount:
            subprocess.run([host_binary("fusermount3"), "-u", mount], env=env,
                check=True, capture_output=True, text=True, timeout=5)
            return


def require_alive(children: list[tuple[str, subprocess.Popen]], ignored: set[str] | None = None) -> None:
    for name, child in children:
        if name not in (ignored or set()) and child.poll() is not None:
            raise RuntimeError(f"Owned {name} service exited ({child.returncode}); see its log")


def wait_socket(runtime: Path, pattern: str, children: list[tuple[str, subprocess.Popen]]) -> Path:
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        require_alive(children)
        sockets = [path for path in runtime.glob(pattern) if stat.S_ISSOCK(path.stat().st_mode)]
        if len(sockets) == 1:
            return sockets[0]
        if len(sockets) > 1:
            raise RuntimeError(f"Private runtime has ambiguous sockets for {pattern}")
        time.sleep(0.03)
    raise TimeoutError(f"Private runtime did not create {pattern}")


def wait_bus_name(name: str, env: dict[str, str], children: list[tuple[str, subprocess.Popen]]) -> None:
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        require_alive(children)
        reply = subprocess.run([host_binary("gdbus"), "call", "--session", "--dest", "org.freedesktop.DBus",
            "--object-path", "/org/freedesktop/DBus", "--method", "org.freedesktop.DBus.NameHasOwner", name],
            env=env, check=True, text=True, capture_output=True, timeout=3)
        if reply.stdout.strip() == "(true,)":
            return
        time.sleep(0.05)
    raise TimeoutError(f"Real session bus did not acquire {name}")


def run_probe(probe_path: Path, mode: str, env: dict[str, str], service_env: dict[str, str],
              children: list[tuple[str, subprocess.Popen]], daemon: subprocess.Popen,
              artifacts: Path, swaymsg: str, child_guard) -> None:
    records: list[dict] = []
    result = None
    ready = False
    action_done = mode == "capture"
    expired = threading.Event()
    evidence = artifacts / "captured-frame.png"
    with (artifacts / "probe.stderr.log").open("wb") as errors:
        probe = subprocess.Popen([str(probe_path), "--mode", mode, "--evidence", str(evidence)],
            env=env, stdout=subprocess.PIPE, stderr=errors, start_new_session=True,
            preexec_fn=child_guard, bufsize=0)
        assert probe.stdout is not None

        def watchdog() -> None:
            expired.set()
            if probe.poll() is None:
                probe.kill()

        timer = threading.Timer(75, watchdog)
        timer.daemon = True
        timer.start()
        pending = bytearray()
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(probe.stdout, selectors.EVENT_READ)
                while probe.poll() is None or selector.get_map():
                    for key, _ in selector.select(timeout=0.1):
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
                            if event.get("event") == "streamReady":
                                if ready:
                                    raise RuntimeError("Probe emitted streamReady twice")
                                ready = True
                                if mode == "resize":
                                    response = subprocess.run([swaymsg, "--socket", service_env["SWAYSOCK"],
                                        "output", "HEADLESS-1", "mode", "1024x768"], env=service_env,
                                        check=True, text=True, capture_output=True, timeout=5)
                                    outcomes = json.loads(response.stdout)
                                    if not outcomes or not all(item.get("success") is True for item in outcomes):
                                        raise RuntimeError("Owned Sway rejected the real output resize")
                                    action_done = True
                                elif mode == "daemon-loss":
                                    # Only a Popen object created by this script is terminated.
                                    if daemon.poll() is not None:
                                        raise RuntimeError("The owned daemon disappeared before fault injection")
                                    terminate(daemon)
                                    action_done = True
                                records.append({"event": "coordinatorAction", "mode": mode,
                                                "completed": action_done})
                            elif event.get("event") == "result":
                                if result is not None:
                                    raise RuntimeError("Probe emitted a second terminal result")
                                result = event
                    if expired.is_set():
                        raise TimeoutError(f"{mode}: external watchdog stopped the stuck capture probe")
                    # Backend/policy may exit when their owned daemon is deliberately killed.
                    ignored = {"pipewire", "wireplumber", "portal-wlr"} if mode == "daemon-loss" and action_done else set()
                    require_alive(children, ignored)
                status = probe.wait(timeout=2)
            if pending:
                raise RuntimeError("Probe ended with incomplete evidence output")
            if status != 0 or not ready or not action_done or not result or result.get("passed") is not True:
                raise RuntimeError(f"{mode}: actual Portal/PipeWire frame validation failed (exit {status})")
            if not evidence.is_file() or evidence.stat().st_size == 0:
                raise RuntimeError("No PNG was saved from the actual received frame")
        finally:
            timer.cancel()
            timer.join(timeout=3)
            terminate(probe)
            probe.stdout.close()
            (artifacts / "result.json").write_text(json.dumps(records, ensure_ascii=False, indent=2) + "\n")


def run_case(probe: Path, prefix: Path, desktop: Path, mode: str, artifacts: Path,
             programs: dict[str, str], child_guard) -> None:
    artifacts.mkdir(parents=True, exist_ok=True)
    children: list[tuple[str, subprocess.Popen]] = []
    logs = []
    with tempfile.TemporaryDirectory(prefix="huhucapture-pipewire-") as directory:
        private = Path(directory)
        runtime = private / "runtime"
        runtime.mkdir(mode=0o700)
        env = os.environ.copy()
        for key in list(env):
            if key.startswith(("PIPEWIRE_", "SPA_", "WIREPLUMBER_", "WP_", "WLR_")) or key in (
                "LD_LIBRARY_PATH", "LD_PRELOAD", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DISPLAY",
                "SWAYSOCK", "I3SOCK", "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS", "DBUS_STARTER_ADDRESS",
                "DBUS_SESSION_BUS_PID", "DBUS_SESSION_BUS_WINDOWID", "DBUS_STARTER_BUS_TYPE",
                "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH"):
                env.pop(key, None)
        for kind in ("config", "data", "state", "cache"):
            path = private / kind
            path.mkdir(mode=0o700)
            env[f"XDG_{kind.upper()}_HOME"] = str(path)
        env.update({"XDG_RUNTIME_DIR": str(runtime), "XDG_SESSION_TYPE": "wayland", "XDG_CURRENT_DESKTOP": "sway",
            "XDG_SESSION_DESKTOP": "sway", "DESKTOP_SESSION": "sway",
            "XDG_CONFIG_DIRS": "/etc/xdg", "XDG_DATA_DIRS": "/usr/local/share:/usr/share",
            "WLR_BACKENDS": "headless", "WLR_RENDERER": "pixman", "WLR_HEADLESS_OUTPUTS": "1",
            "WLR_LIBINPUT_NO_DEVICES": "1", "QT_QPA_PLATFORM": "wayland", "QT_ACCESSIBILITY": "0",
            "QT_WAYLAND_DISABLE_WINDOWDECORATION": "1", "NO_AT_BRIDGE": "1", "GSETTINGS_BACKEND": "memory"})
        # Keep the host services on host libraries; only the application probe
        # receives the built Qt/libportal/PipeWire loader paths.
        client_loader_path = os.environ.get("LD_LIBRARY_PATH", "")
        portal_config = private / "config/xdg-desktop-portal"
        portal_config.mkdir()
        (portal_config / "portals.conf").write_text("[preferred]\ndefault=wlr\norg.freedesktop.impl.portal.ScreenCast=wlr\n")
        wlr_config = private / "config/xdg-desktop-portal-wlr"
        wlr_config.mkdir()
        (wlr_config / "config").write_text("[screencast]\noutput_name=HEADLESS-1\nchooser_type=none\nmax_fps=30\n")

        def start(name: str, command: list[str], pipe_output=False) -> subprocess.Popen:
            log = (artifacts / f"{name}.log").open("wb")
            logs.append(log)
            # Only the explicitly verified compositor/backend loads its private
            # libraries. PipeWire, policy, portals and bus keep host libraries.
            child_env = env.copy()
            if name in ("sway", "portal-wlr"):
                child_env["LD_LIBRARY_PATH"] = str(desktop / "lib")
            child = subprocess.Popen(command, env=child_env, stdout=subprocess.PIPE if pipe_output else log,
                stderr=log, start_new_session=True, preexec_fn=child_guard, bufsize=0)
            children.append((name, child))
            return child

        try:
            bus = start("dbus", [programs["dbus-daemon"], "--session", "--nofork", "--print-address=1",
                f"--address=unix:path={runtime / 'bus'}"], True)
            assert bus.stdout is not None
            with selectors.DefaultSelector() as selector:
                selector.register(bus.stdout, selectors.EVENT_READ)
                if not selector.select(timeout=5):
                    raise TimeoutError("The owned session bus did not provide its address")
                address = os.read(bus.stdout.fileno(), 4096).decode().strip()
            if not address.startswith("unix:") or "\n" in address:
                raise RuntimeError("The owned session bus returned an invalid address")
            env["DBUS_SESSION_BUS_ADDRESS"] = address
            config = Path(__file__).resolve().parents[1] / "tests/platform/sway-parent.conf"
            start("sway", [programs["sway"], "--config", str(config), "--debug"])
            env["WAYLAND_DISPLAY"] = wait_socket(runtime, "wayland-*", children).name
            env["SWAYSOCK"] = str(wait_socket(runtime, "sway-ipc.*.sock", children))
            # Start the permission store explicitly so cleanup owns its process,
            # before WirePlumber can automatically activate it.
            start("permission-store", [programs["xdg-permission-store"]])
            wait_bus_name("org.freedesktop.impl.portal.PermissionStore", env, children)
            # Portal activates this real daemon even for screen capture. Own it
            # so its FUSE mount is stopped before the bus/private runtime ends.
            start("document-portal", [programs["xdg-document-portal"]])
            wait_bus_name("org.freedesktop.portal.Documents", env, children)
            daemon = start("pipewire", [programs["pipewire"]])
            wait_socket(runtime, "pipewire-0", children)
            start("wireplumber", [programs["wireplumber"]])
            start("portal-wlr", [programs["xdg-desktop-portal-wlr"], "--loglevel=DEBUG"])
            wait_bus_name("org.freedesktop.impl.portal.desktop.wlr", env, children)
            start("portal", [programs["xdg-desktop-portal"], "--verbose"])
            wait_bus_name("org.freedesktop.portal.Desktop", env, children)
            client_env = env.copy()
            client_env["LD_LIBRARY_PATH"] = str(prefix / "lib") + (":" + client_loader_path if client_loader_path else "")
            client_env["PIPEWIRE_MODULE_DIR"] = str(prefix / "lib/pipewire-0.3")
            client_env["SPA_PLUGIN_DIR"] = str(prefix / "lib/spa-0.2")
            client_env["PIPEWIRE_CONFIG_DIR"] = str(prefix / "share/pipewire")
            for key in ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH"):
                if key in os.environ:
                    client_env[key] = os.environ[key]
            desktop_env = env.copy()
            desktop_env["LD_LIBRARY_PATH"] = str(desktop / "lib")
            run_probe(probe, mode, client_env, desktop_env, children, daemon, artifacts, programs["swaymsg"], child_guard)
        finally:
            cleanup_errors = []
            for name, child in reversed(children):
                try:
                    terminate(child)
                    if name == "document-portal":
                        unmount_owned_documents(runtime, env)
                except (OSError, subprocess.SubprocessError) as error:
                    cleanup_errors.append(f"{name}: {error}")
                finally:
                    if child.stdout is not None:
                        child.stdout.close()
            for log in logs:
                log.close()
            (artifacts / "cleanup.json").write_text(json.dumps({"errors": cleanup_errors}, indent=2) + "\n")
            if cleanup_errors:
                raise RuntimeError("Owned service cleanup failed: " + "; ".join(cleanup_errors))


def main() -> int:
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("probe", type=Path)
    args.add_argument("--pipewire-prefix", required=True, type=Path)
    args.add_argument("--portal-backend", required=True, type=Path)
    args.add_argument("--backend-build-info", required=True, type=Path)
    args.add_argument("--artifacts", type=Path, default=Path("artifacts/tests/pipewire"))
    args.add_argument("--modes", nargs="+", choices=MODES, default=list(MODES),
        help="Actual native scenarios to run; default preserves all original capture, resize and daemon-loss checks")
    parsed = args.parse_args()
    probe = parsed.probe.resolve(strict=True)
    prefix = parsed.pipewire_prefix.resolve(strict=True)
    if not (prefix / "lib/libpipewire-0.3.so").is_file():
        raise RuntimeError("The pinned PipeWire client prefix is missing")
    backend = parsed.portal_backend.resolve(strict=True)
    build_info = json.loads(parsed.backend_build_info.read_text(encoding="utf-8"))
    if not os.access(backend, os.X_OK) or hashlib.sha256(backend.read_bytes()).hexdigest() != build_info["binarySha256"]:
        raise RuntimeError("The real Portal backend differs from its actual build evidence")
    root = Path(__file__).resolve().parents[1]
    locked = json.loads((root / "build/dependencies.lock.json").read_text(encoding="utf-8"))["qtMigration"]["headlessPortalValidation"]
    if build_info.get("schema") != 2 or build_info.get("sources") != locked:
        raise RuntimeError("Rebuild the owned validation desktop from the current exact source lock")
    desktop = (parsed.backend_build_info.resolve(strict=True).parent / "install").resolve(strict=True)
    if desktop != Path(build_info["prefix"]).resolve(strict=True):
        raise RuntimeError("Validation desktop prefix does not match its build receipt")
    desktop_binaries = {}
    for name in ("sway", "swaymsg", "xdg-desktop-portal-wlr"):
        record = build_info["binaries"][name]
        path = (desktop / record["relativePath"]).resolve(strict=True)
        if not path.is_relative_to(desktop) or not os.access(path, os.X_OK) or \
                hashlib.sha256(path.read_bytes()).hexdigest() != record["sha256"]:
            raise RuntimeError(f"Validation desktop binary differs from its build receipt: {name}")
        desktop_binaries[name] = str(path)
    if backend != Path(desktop_binaries["xdg-desktop-portal-wlr"]):
        raise RuntimeError("The backend must belong to this verified desktop prefix")
    recorded = set()
    for record in build_info["runtimeFiles"]:
        path = desktop / record["relativePath"]
        if not path.resolve(strict=True).is_relative_to(desktop) or \
                (os.readlink(path) if path.is_symlink() else None) != record["symlink"] or \
                hashlib.sha256(path.read_bytes()).hexdigest() != record["sha256"]:
            raise RuntimeError(f"Validation desktop library differs from its build receipt: {path.name}")
        recorded.add(path.relative_to(desktop).as_posix())
    actual = {path.relative_to(desktop).as_posix() for path in (desktop / "lib").glob("*.so*")}
    if not recorded or actual != recorded:
        raise RuntimeError("The private validation loader directory has changed")
    artifacts = parsed.artifacts.resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    programs = {name: host_binary(name) for name in ("dbus-daemon", "pipewire", "wireplumber", "gdbus")}
    for name in ("xdg-desktop-portal", "xdg-document-portal", "xdg-permission-store"):
        programs[name] = host_binary(name)
    programs.update(desktop_binaries)
    versions = subprocess.run([host_binary("dpkg-query"), "--show", "--showformat=${Package} ${Version}\n",
        "sway", "pipewire", "wireplumber", "xdg-desktop-portal", "xdg-desktop-portal-wlr"],
        check=True, text=True, capture_output=True).stdout.splitlines()
    (artifacts / "environment.json").write_text(json.dumps({"systemPackages": versions,
        "clientPrefix": str(prefix), "desktopPrefix": str(desktop), "modes": parsed.modes, "backend": "headless", "renderer": "pixman",
        "portalBackendBuild": build_info,
        "portalSelection": "owned output_name=HEADLESS-1, chooser_type=none",
        "physicalDisplay": False, "gnomeAuthorization": False, "plasmaAuthorization": False,
        "audioAcceptance": False, "dmaBufAcceptance": False, "hdrAcceptance": False}, indent=2) + "\n")
    owner_pid = os.getpid()

    def child_guard() -> None:
        libc = ctypes.CDLL(None, use_errno=True)
        if libc.prctl(1, int(signal.SIGKILL), 0, 0, 0) != 0 or os.getppid() != owner_pid:
            os._exit(127)

    def interrupted(signum: int, _frame) -> None:
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGTERM, interrupted)
    for mode in parsed.modes:
        run_case(probe, prefix, desktop, mode, artifacts / mode, programs, child_guard)
    print("Real Portal-wlr/PipeWire probe passed: " + ", ".join(parsed.modes), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
