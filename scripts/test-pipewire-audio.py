#!/usr/bin/env python3
"""Verify production PCM against an owned real pinned PipeWire graph.

Two explicit upstream-derived test signals use real native streams, SPA rate
conversion, sink monitor ports and transport. A small explicit link policy acts
only inside this private graph. No physical device, user's daemon or production
audio callback is replaced, and physical device acceptance is never claimed.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
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


MODES = ("system", "microphone", "mixed", "clipped-mix", "daemon-loss", "source-loss")


def terminate(child: subprocess.Popen) -> None:
    # Same owned-Popen cleanup convention as test-pipewire-capture.py. Never
    # signal a process discovered by name, a server supplied by the user or a
    # session-wide PID. SIGCONT also makes interrupted CI cleanup reliable.
    if child.poll() is None:
        child.send_signal(signal.SIGCONT)
        child.terminate()
        try:
            child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait(timeout=3)


def graph(prefix: Path, env: dict[str, str], evidence: Path) -> list[dict]:
    reply = subprocess.run([str(prefix / "bin/pw-dump"), "--no-colors"], env=env,
        text=True, capture_output=True, timeout=3)
    evidence.with_suffix(".stderr.log").write_text(reply.stderr, encoding="utf-8")
    reply.check_returncode()
    values = json.loads(reply.stdout)
    if not isinstance(values, list):
        raise RuntimeError("Actual PipeWire registry is not an object list")
    evidence.write_text(reply.stdout, encoding="utf-8")
    return values


def node(values: list[dict], key: str, value: str, pid: int | None = None) -> int | None:
    found = []
    for item in values:
        if item.get("type") != "PipeWire:Interface:Node":
            continue
        props = item.get("info", {}).get("props", {})
        if props.get(key) != value:
            continue
        if pid is not None:
            # A stream Node identifies its owning Client through client.id.
            # The actual process credentials belong to that Client, not to
            # the Node's optional application properties. Require the native
            # protocol's server-observed peer PID; a matching node name or a
            # self-advertised application.process.id is not ownership proof.
            client_id = props.get("client.id")
            clients = [entry for entry in values
                if entry.get("type") == "PipeWire:Interface:Client" and
                client_id is not None and str(entry.get("id")) == str(client_id)]
            if len(clients) != 1:
                raise RuntimeError("A named test node has no unique owning native Client")
            client = clients[0].get("info", {}).get("props", {})
            if str(client.get("pipewire.sec.pid")) != str(pid) or (
                    "application.process.id" in client and str(client["application.process.id"]) != str(pid)):
                raise RuntimeError("A named test node is not owned by the process this wrapper launched")
        found.append(item["id"])
    if len(found) > 1:
        raise RuntimeError(f"The isolated native graph has ambiguous nodes for {value}")
    return found[0] if found else None


def stereo_ports(values: list[dict], identity: int, direction: str) -> dict[str, int]:
    result = {}
    for item in values:
        if item.get("type") != "PipeWire:Interface:Port":
            continue
        info = item.get("info", {})
        props = info.get("props", {})
        if str(props.get("node.id")) != str(identity) or str(info.get("direction", "")).lower() != direction:
            continue
        channel = props.get("audio.channel")
        if channel in ("FL", "FR"):
            if channel in result:
                raise RuntimeError("The native graph has ambiguous stereo channel ports")
            result[channel] = item["id"]
    return result


def run_case(probe: Path, signal_program: Path, prefix: Path, mode: str,
             artifacts: Path, child_guard, root: Path) -> None:
    artifacts.mkdir(parents=True, exist_ok=False)
    children: dict[str, subprocess.Popen] = {}
    logs = []
    expired = threading.Event()
    records = []
    completed = None
    failure = None
    with tempfile.TemporaryDirectory(prefix="huhucapture-native-audio-") as temporary:
        private = Path(temporary)
        runtime = private / "runtime"; runtime.mkdir(mode=0o700)
        config = private / "config"; config.mkdir(mode=0o700)
        shutil.copyfile(root / "tests/platform/pipewire-audio.conf", config / "audio-daemon.conf")
        shutil.copyfile(root / "tests/platform/pipewire-audio-client.conf", config / "client.conf")
        env = {key: value for key, value in os.environ.items() if not key.startswith("PIPEWIRE_")}
        env.update({"XDG_RUNTIME_DIR": str(runtime), "XDG_CONFIG_HOME": str(config),
            "XDG_CONFIG_DIRS": str(config), "XDG_DATA_HOME": str(private / "data"),
            "DBUS_SESSION_BUS_ADDRESS": f"unix:path={runtime / 'absent-bus'}",
            "PIPEWIRE_RUNTIME_DIR": str(runtime), "PIPEWIRE_REMOTE": "huhu-audio-probe",
            "PIPEWIRE_CONFIG_DIR": str(config), "PIPEWIRE_MODULE_DIR": str(prefix / "lib/pipewire-0.3"),
            "SPA_PLUGIN_DIR": str(prefix / "lib/spa-0.2"),
            "LD_LIBRARY_PATH": str(prefix / "lib") + (":" + os.environ["LD_LIBRARY_PATH"] if os.environ.get("LD_LIBRARY_PATH") else ""),
            "NO_COLOR": "1"})

        def start(name: str, command: list[str], stdout=False) -> subprocess.Popen:
            log = (artifacts / f"{name}.log").open("wb"); logs.append(log)
            child = subprocess.Popen(command, env=env, stdout=subprocess.PIPE if stdout else log,
                stderr=log, start_new_session=True, preexec_fn=child_guard, bufsize=0)
            children[name] = child
            return child

        def alive(ignored=()) -> None:
            for name, child in children.items():
                if name not in ignored and child.poll() is not None:
                    raise RuntimeError(f"Owned {name} exited ({child.returncode}); see {name}.log")

        def watchdog() -> None:
            expired.set()
            for child in tuple(children.values()):
                if child.poll() is None:
                    child.kill()

        timer = threading.Timer(45, watchdog); timer.daemon = True; timer.start()
        try:
            daemon = start("daemon", [str(prefix / "bin/pipewire"), "--config", "audio-daemon.conf"])
            deadline = time.monotonic() + 10
            socket = runtime / "huhu-audio-probe"
            while not socket.exists():
                alive()
                if time.monotonic() > deadline:
                    raise TimeoutError("The owned real PipeWire daemon did not create its private socket")
                time.sleep(0.02)
            if not stat.S_ISSOCK(socket.stat().st_mode):
                raise RuntimeError("The private native endpoint is not an actual socket")
            extra = ["--clip"] if mode == "clipped-mix" else []
            system_signal = start("system-signal", [str(signal_program), "system", *extra])
            microphone_signal = start("microphone-signal", [str(signal_program), "microphone", *extra])
            capture = start("capture", [str(probe), mode], True)
            assert capture.stdout is not None
            connections: set[tuple[int, int]] = set()
            wanted = {"microphone"} if mode == "microphone" else {"system"} if mode == "system" else {"system", "microphone"}
            linked_sources: set[str] = set()
            pending = bytearray()
            injected = False
            next_graph = 0.0
            with selectors.DefaultSelector() as selector:
                selector.register(capture.stdout, selectors.EVENT_READ)
                while capture.poll() is None or selector.get_map():
                    if expired.is_set():
                        raise TimeoutError("The owned real audio graph exceeded its independent watchdog")
                    ignored = {"capture"}
                    if injected and mode == "daemon-loss":
                        ignored.update(("daemon", "system-signal", "microphone-signal"))
                    elif injected and mode == "source-loss":
                        ignored.add("microphone-signal")
                    alive(ignored)
                    if linked_sources != wanted and time.monotonic() >= next_graph:
                        values = graph(prefix, env, artifacts / "registry-latest.json")
                        next_graph = time.monotonic() + 0.05
                        sink = node(values, "node.name", "huhu-audio-probe-sink")
                        sources = {"system": node(values, "node.name", "huhu-audio-tone-system", system_signal.pid),
                            "microphone": node(values, "node.name", "huhu-audio-tone-microphone", microphone_signal.pid)}

                        def link(output_node: int | None, input_node: int | None) -> bool:
                            if output_node is None or input_node is None:
                                return False
                            outputs = stereo_ports(values, output_node, "output")
                            inputs = stereo_ports(values, input_node, "input")
                            if set(outputs) != {"FL", "FR"} or set(inputs) != {"FL", "FR"}:
                                return False
                            for channel in ("FL", "FR"):
                                pair = (outputs[channel], inputs[channel])
                                if pair in connections:
                                    continue
                                response = subprocess.run([str(prefix / "bin/pw-link"), "--linger", str(pair[0]), str(pair[1])],
                                    env=env, capture_output=True, text=True, timeout=3)
                                with (artifacts / "links.log").open("a") as log:
                                    log.write(json.dumps({"outputPort": pair[0], "inputPort": pair[1],
                                        "status": response.returncode, "stdout": response.stdout, "stderr": response.stderr}) + "\n")
                                response.check_returncode()
                                connections.add(pair)
                            return True

                        system_connected = link(sources["system"], sink)
                        for name in wanted:
                            capture_node = node(values, "media.name", f"huhucapture {name if name == 'microphone' else 'system audio'}", capture.pid)
                            if capture_node is None:
                                continue
                            props = next(value for value in values if value["id"] == capture_node)["info"]["props"]
                            selected_sink = str(props.get("stream.capture.sink", "false")).lower() == "true"
                            if selected_sink != (name == "system"):
                                raise RuntimeError("The production adapter did not request the correct real monitor/input source kind")
                            if (system_connected if name == "system" else True) and link(sink if name == "system" else sources[name], capture_node):
                                linked_sources.add(name)
                        if linked_sources == wanted:
                            graph(prefix, env, artifacts / "registry-linked.json")
                    for key, _ in selector.select(timeout=0.03):
                        chunk = os.read(key.fd, 65536)
                        if not chunk:
                            selector.unregister(key.fileobj); continue
                        pending.extend(chunk)
                        while b"\n" in pending:
                            line, _, rest = pending.partition(b"\n"); pending[:] = rest
                            event = json.loads(line); records.append(event)
                            print(json.dumps({"mode": mode, **event}, ensure_ascii=False), flush=True)
                            if event.get("event") == "fault-ready":
                                if injected or mode not in ("daemon-loss", "source-loss"):
                                    raise RuntimeError("Unexpected repeated native fault request")
                                # Only these exact Popen handles are stopped.
                                terminate(daemon if mode == "daemon-loss" else microphone_signal)
                                injected = True
                            elif event.get("event") == "completed":
                                if completed is not None:
                                    raise RuntimeError("The native audio probe completed twice")
                                completed = event
            if capture.wait(timeout=3) != 0 or completed is None or linked_sources != wanted or pending:
                raise RuntimeError("The real PCM probe did not complete every required native assertion; see capture.log")
            if mode in ("daemon-loss", "source-loss") and not injected:
                raise RuntimeError("The expected owned native service/source loss was never injected")
        except BaseException as error:
            failure = str(error)
            raise
        finally:
            timer.cancel()
            cleanup_errors = []
            for name, child in reversed(tuple(children.items())):
                try:
                    terminate(child)
                    if child.stdout is not None:
                        child.stdout.close()
                except (OSError, subprocess.SubprocessError) as error:
                    cleanup_errors.append(f"{name}: {error}")
            for log in logs:
                log.close()
            (artifacts / "result.json").write_text(json.dumps({"mode": mode, "events": records,
                "completed": completed, "failure": failure, "cleanupErrors": cleanup_errors,
                "physicalDevices": False, "longRunningAvDrift": False}, ensure_ascii=False, indent=2) + "\n")
            if cleanup_errors:
                raise RuntimeError("Owned native audio cleanup failed: " + "; ".join(cleanup_errors))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("signal_program", type=Path)
    parser.add_argument("--pipewire-prefix", type=Path, required=True)
    parser.add_argument("--pipewire-build-info", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--mode", choices=MODES, action="append")
    options = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    prefix = options.pipewire_prefix.resolve(strict=True)
    probe = options.probe.resolve(strict=True); signal_program = options.signal_program.resolve(strict=True)
    locked = json.loads((root / "build/dependencies.lock.json").read_text())["qtMigration"]["pipewire"]
    build = json.loads(options.pipewire_build_info.read_text())
    if any(build.get(key) != locked[key] for key in ("version", "commit", "patches", "mesonOptions")):
        raise RuntimeError("The actual native prefix metadata differs from the fixed reviewed PipeWire build")
    required = [prefix / "bin" / name for name in ("pipewire", "pw-dump", "pw-link")]
    required.extend((probe, signal_program, prefix / "lib/libpipewire-0.3.so"))
    if any(not value.is_file() for value in required):
        raise RuntimeError("The actual native test binary/runtime is missing")
    artifacts = options.artifacts.resolve(); artifacts.mkdir(parents=True, exist_ok=True)
    metadata = {"build": build, "modes": options.mode or MODES,
        "files": {str(value): hashlib.sha256(value.read_bytes()).hexdigest() for value in required},
        "graphPolicy": "Explicit links only inside this owned graph; no physical device policy/session manager",
        "signalProvenance": "PipeWire 1.6.11 audio-src.c; explicit 440/880 Hz test inputs, not device audio"}
    with (artifacts / "environment.json").open("x") as output:
        json.dump(metadata, output, indent=2); output.write("\n")
    owner = os.getpid()

    def child_guard() -> None:
        libc = ctypes.CDLL(None, use_errno=True)
        if libc.prctl(1, int(signal.SIGKILL), 0, 0, 0) != 0 or os.getppid() != owner:
            os._exit(127)

    def interrupted(signum, _frame) -> None:
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGTERM, interrupted)
    failures = []
    for mode in options.mode or MODES:
        try:
            run_case(probe, signal_program, prefix, mode, artifacts / mode, child_guard, root)
        except (OSError, subprocess.SubprocessError, RuntimeError, ValueError) as error:
            failures.append({"mode": mode, "error": str(error)})
            print(f"FAIL {mode}: {error}", flush=True)
    (artifacts / "summary.json").write_text(json.dumps({"failures": failures,
        "passed": not failures, "physicalDevices": False, "longRunningAvDrift": False}, indent=2) + "\n")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
