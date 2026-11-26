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


def wait_for_owned_focus(swaymsg: str, env: dict[str, str], sender: subprocess.Popen,
                         compositor: subprocess.Popen, terminate, child_guard) -> dict:
    """Wait on real IPC events, then verify this sender's visible focused view.

    Sway IPC's initial tick proves that the window subscription is installed
    before get_tree. This avoids losing a map/focus event between the snapshot
    and subscription. No delay or repeated key press substitutes for readiness.
    See sway/sway-ipc.7.scd and swaymsg/main.c at upstream sway 1.9.
    """
    watcher = subprocess.Popen([swaymsg, "--socket", env["SWAYSOCK"], "--monitor", "--raw",
        "--type", "subscribe", '["window", "tick"]'], env=env, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, start_new_session=True, preexec_fn=child_guard, bufsize=0)
    assert watcher.stdout is not None
    pending = bytearray()
    deadline = time.monotonic() + 6
    subscribed = False

    def focused(node: dict) -> dict | None:
        if node.get("pid") == sender.pid and node.get("visible") is True and node.get("focused") is True:
            return node
        for child in node.get("nodes", []) + node.get("floating_nodes", []):
            found = focused(child)
            if found is not None:
                return found
        return None

    try:
        with selectors.DefaultSelector() as selector:
            selector.register(watcher.stdout, selectors.EVENT_READ)
            while time.monotonic() < deadline:
                if sender.poll() is not None or compositor.poll() is not None:
                    raise RuntimeError("The owned clipboard sender/compositor exited before receiving focus")
                ready = selector.select(timeout=max(0, deadline - time.monotonic()))
                if not ready:
                    break
                chunk = os.read(watcher.stdout.fileno(), 65536)
                if not chunk:
                    raise RuntimeError("The owned Sway IPC subscription ended before the sender received focus")
                pending.extend(chunk)
                while b"\n" in pending:
                    line, _, remaining = pending.partition(b"\n")
                    pending[:] = remaining
                    event = json.loads(line)
                    if event.get("first") is True:
                        subscribed = True
                    if not subscribed:
                        continue
                    tree = subprocess.run([swaymsg, "--socket", env["SWAYSOCK"], "--raw", "--type", "get_tree"],
                        env=env, check=True, capture_output=True, text=True,
                        timeout=max(0.001, deadline - time.monotonic()))
                    node = focused(json.loads(tree.stdout))
                    if node is not None:
                        return node
        raise TimeoutError("The real compositor never made the exposed clipboard sender visible and focused")
    finally:
        terminate(watcher)
        watcher.stdout.close()
        if watcher.stderr is not None:
            watcher.stderr.close()


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
    wait_bus_name = helper["wait_bus_name"]
    require_alive = helper["require_alive"]
    unmount_owned_documents = helper["unmount_owned_documents"]
    sway, wtype, swaymsg = host_binary("sway"), host_binary("wtype"), host_binary("swaymsg")
    host_base = os.environ.copy()
    for key in list(host_base):
        if key.startswith(("PIPEWIRE_", "SPA_", "WIREPLUMBER_", "WP_", "WLR_")) or key in (
            "LD_LIBRARY_PATH", "LD_PRELOAD", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DISPLAY",
            "SWAYSOCK", "I3SOCK", "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS", "DBUS_STARTER_ADDRESS",
            "DBUS_SESSION_BUS_PID", "DBUS_SESSION_BUS_WINDOWID", "DBUS_STARTER_BUS_TYPE",
            "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH"):
            host_base.pop(key, None)
    version = subprocess.run([sway, "--version"], env=host_base, check=True, capture_output=True, text=True).stdout.strip()
    packages = subprocess.run([host_binary("dpkg-query"), "-W", "sway", "wtype", "libwayland-client0", "dbus", "xdg-desktop-portal"],
                              env=host_base, capture_output=True, text=True, check=True).stdout
    (artifacts / "environment.json").write_text(json.dumps({"compositor": version,
        "packages": packages, "input": "wtype virtual keyboard with an application-lifetime idle seat device",
        "physicalDesktopAcceptance": False}, indent=2) + "\n")
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
            "QT_QPA_PLATFORM": "wayland", "QT_WAYLAND_DISABLE_WINDOWDECORATION": "1"})
        # Only the Qt sender and its independent Qt receiver inherit the fixed
        # application loader/plugin paths. Sway, wtype and package tools use
        # host libraries, never the application's Qt/libportal/PipeWire prefix.
        client_env = host_env.copy()
        for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH"):
            if key in os.environ:
                client_env[key] = os.environ[key]
        # Isolated test protocol evidence only: diagnose capabilities, keyboard
        # binding, enter/key and selection events on the actual Qt connection.
        client_env["WAYLAND_DEBUG"] = "client"
        config = Path(__file__).resolve().parents[1] / "tests/platform/sway-parent.conf"
        with (artifacts / "sway.log").open("wb") as log, (artifacts / "keyboard.stderr.log").open("wb") as keyboard_errors:
            compositor = None
            process = None
            keyboard = None
            services: list[tuple[str, subprocess.Popen]] = []
            service_logs = []

            def start_service(name: str, command: list[str], pipe_output=False) -> subprocess.Popen:
                service_log = (artifacts / f"{name}.log").open("wb")
                service_logs.append(service_log)
                child = subprocess.Popen(command, env=host_env, stdout=subprocess.PIPE if pipe_output else service_log,
                    stderr=service_log, start_new_session=True, preexec_fn=child_guard, bufsize=0)
                services.append((name, child))
                return child

            try:
                # Qt 6.12 offers application/vnd.portal.filetransfer for local
                # URLs, and another Qt process preferentially retrieves that
                # format through the actual Documents portal. The complete
                # desktop contract therefore needs a real session bus and
                # document service; a nonexistent bus cannot validate URIs.
                bus = start_service("dbus", [host_binary("dbus-daemon"), "--session", "--nofork", "--print-address=1",
                    f"--address=unix:path={runtime / 'bus'}"], True)
                assert bus.stdout is not None
                with selectors.DefaultSelector() as selector:
                    selector.register(bus.stdout, selectors.EVENT_READ)
                    if not selector.select(timeout=5):
                        raise TimeoutError("The owned clipboard session bus did not provide its address")
                    address = os.read(bus.stdout.fileno(), 4096).decode().strip()
                if not address.startswith("unix:") or "\n" in address:
                    raise RuntimeError("The owned clipboard session bus returned an invalid address")
                host_env["DBUS_SESSION_BUS_ADDRESS"] = address
                client_env["DBUS_SESSION_BUS_ADDRESS"] = address
                start_service("permission-store", [host_binary("xdg-permission-store")])
                wait_bus_name("org.freedesktop.impl.portal.PermissionStore", host_env, services)
                start_service("document-portal", [host_binary("xdg-document-portal")])
                wait_bus_name("org.freedesktop.portal.Documents", host_env, services)
                compositor = subprocess.Popen([sway, "--config", str(config), "--debug"], env=host_env,
                    stdout=log, stderr=subprocess.STDOUT, start_new_session=True, preexec_fn=child_guard)
                deadline = time.monotonic() + 10
                socket = None
                ipc_socket = None
                while time.monotonic() < deadline:
                    if compositor.poll() is not None:
                        raise RuntimeError("The owned Sway failed before creating a Wayland socket")
                    sockets = [path for path in runtime.glob("wayland-*") if stat.S_ISSOCK(path.stat().st_mode)]
                    ipc_sockets = [path for path in runtime.glob("sway-ipc.*.sock") if stat.S_ISSOCK(path.stat().st_mode)]
                    if sockets:
                        socket = sockets[0]
                    if ipc_sockets:
                        ipc_socket = ipc_sockets[0]
                    if socket is not None and ipc_socket is not None:
                        break
                    time.sleep(0.02)
                if socket is None or ipc_socket is None:
                    raise TimeoutError("The owned compositor never produced its real Wayland and IPC sockets")
                host_env["WAYLAND_DISPLAY"] = socket.name
                host_env["SWAYSOCK"] = str(ipc_socket)
                client_env["WAYLAND_DISPLAY"] = socket.name
                # wtype 0.4 main.c creates the real virtual keyboard and uploads
                # its keymap before run_text_stdin() blocks in fgetc(). Keeping
                # stdin open therefore keeps a genuine keyboard capability on
                # this otherwise device-less headless seat. Without it, a short
                # one-shot wtype can send its key before Qt has reacted to the
                # newly advertised capability and bound wl_keyboard. It also
                # lets independently started receivers obtain keyboard focus.
                keyboard = subprocess.Popen([wtype, "-"], env=host_env, stdin=subprocess.PIPE,
                    stdout=subprocess.DEVNULL, stderr=keyboard_errors, start_new_session=True,
                    preexec_fn=child_guard)
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
                            require_alive(services)
                            if time.monotonic() > deadline:
                                raise TimeoutError("The real clipboard owner/receiver did not terminate")
                            if compositor.poll() is not None:
                                raise RuntimeError("The owned compositor exited during clipboard validation")
                            if keyboard.poll() is not None:
                                raise RuntimeError("The owned idle keyboard exited before clipboard validation completed")
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
                                        focused = wait_for_owned_focus(swaymsg, host_env, process, compositor, terminate, child_guard)
                                        (artifacts / f"input-focus-{inputs + 1}.json").write_text(json.dumps(focused, indent=2) + "\n")
                                        # The real compositor creates the serial
                                        # from input received by Qt's window.
                                        subprocess.run([wtype, "-k", "F8"], env=host_env, check=True, timeout=5)
                                        inputs += 1
                    status = process.wait(timeout=2)
                    process.stdout.close()
                    # Initial image, archived PNG, GIF file, changed-file
                    # rejection, Unicode text, and the final image each demand
                    # a fresh actual input event. A partial sequence is failure.
                    if status != 0 or inputs != 6:
                        raise RuntimeError(f"Real Wayland clipboard validation failed: exit={status}, inputs={inputs}")
            finally:
                if process is not None:
                    terminate(process)
                if keyboard is not None:
                    terminate(keyboard)
                    if keyboard.stdin is not None:
                        keyboard.stdin.close()
                if compositor is not None:
                    terminate(compositor)
                cleanup_errors = []
                for name, service in reversed(services):
                    try:
                        terminate(service)
                        if name == "document-portal":
                            unmount_owned_documents(runtime, host_env)
                    except (OSError, subprocess.SubprocessError) as error:
                        cleanup_errors.append(f"{name}: {error}")
                    finally:
                        if service.stdout is not None:
                            service.stdout.close()
                for service_log in service_logs:
                    service_log.close()
                (artifacts / "cleanup.json").write_text(json.dumps({"errors": cleanup_errors}, indent=2) + "\n")
                if cleanup_errors:
                    raise RuntimeError("Owned clipboard service cleanup failed: " + "; ".join(cleanup_errors))
    print("Real Wayland publication receipts and separate-process PNG/URI/Unicode reception passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
