#!/usr/bin/env python3
"""Build only the fixed FFmpeg inspection DLLs with matching MSVC CRTs."""

import argparse
import hashlib
import json
import os
import re
from pathlib import Path, PurePosixPath
import shlex
import shutil
import subprocess
import urllib.request
import zipfile


def run(*arguments, cwd=None, env=None):
    subprocess.run(arguments, cwd=cwd, env=env, check=True)


def output(*arguments, cwd=None, env=None):
    return subprocess.check_output(arguments, cwd=cwd, env=env, text=True, encoding="utf-8", errors="replace").strip()


def download(specification, destination):
    failures = []
    for url in specification["urls"]:
        try:
            with urllib.request.urlopen(url, timeout=60) as response, destination.open("xb") as target:
                shutil.copyfileobj(response, target)
            with destination.open("rb") as downloaded:
                actual = hashlib.file_digest(downloaded, "sha512").hexdigest()
            if actual != specification["sha512"]:
                raise RuntimeError(f"Pinned tool checksum mismatch: {destination.name}")
            return url
        except Exception as error:
            if destination.exists():
                destination.unlink()  # This function exclusively created this private download.
            failures.append(f"{url}: {error}")
    raise RuntimeError("Cannot restore fixed build tool: " + "; ".join(failures))


def safe_members(names):
    for name in names:
        item = PurePosixPath(name.replace("\\", "/"))
        if item.is_absolute() or ".." in item.parts or ":" in name:
            raise RuntimeError(f"Unsafe archive member: {name}")


def msvc_environment(workspace):
    base = Path(os.environ["ProgramFiles(x86)"])
    vswhere = base / "Microsoft Visual Studio/Installer/vswhere.exe"
    installation = output(str(vswhere), "-latest", "-products", "*", "-requires",
                          "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath")
    developer = Path(installation) / "Common7/Tools/VsDevCmd.bat"
    if not developer.is_file() or any(character in str(developer) for character in '"%\r\n'):
        raise RuntimeError("Cannot locate the real installed Visual Studio developer command")
    # Python's Windows list2cmdline uses C-runtime quote escaping, whereas cmd
    # parses /c with different rules. Pass only an unquoted fixed filename to
    # cmd and let an actual batch file perform the quoted VsDevCmd invocation.
    # Keep the discovered Unicode path in the child environment, not batch-file
    # text interpreted through the runner's current console code page.
    bootstrap = workspace / "load-msvc-environment.cmd"
    with bootstrap.open("x", encoding="ascii", newline="\r\n") as stream:
        stream.write('@echo off\nsetlocal DisableDelayedExpansion\n'
                     'call "%HUHUCAPTURE_VSDEVCMD%" -no_logo -arch=x64 -host_arch=x64 >nul\n'
                     'if errorlevel 1 exit /b %errorlevel%\nset\n')
    child = os.environ.copy()
    child["HUHUCAPTURE_VSDEVCMD"] = str(developer)
    captured = subprocess.check_output(("cmd.exe", "/d", "/u", "/c", bootstrap.name),
        cwd=workspace, env=child, text=True, encoding="utf-16-le", errors="strict")
    environment = {key.upper(): value for key, value in os.environ.items()}
    for line in captured.splitlines():
        if "=" in line and not line.startswith("="):
            key, value = line.split("=", 1)
            environment[key.upper()] = value
    environment["VSLANG"] = "1033"
    environment.pop("CL", None)
    environment.pop("_CL_", None)
    environment.pop("HUHUCAPTURE_VSDEVCMD", None)
    return environment


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, default=root / ".deps/video-inspection-windows")
    parser.add_argument("--msys", type=Path, default=Path("C:/msys64"))
    parser.add_argument("--configuration", action="append", choices=("Debug", "Release"))
    parser.add_argument("--jobs", type=int, default=2)
    options = parser.parse_args()
    if os.name != "nt":
        raise SystemExit("This script requires the real Windows x64 MSVC runner")
    if not 1 <= options.jobs <= 64:
        raise SystemExit("--jobs must be between 1 and 64")
    configurations = options.configuration or ["Debug", "Release"]
    if len(set(configurations)) != len(configurations):
        raise SystemExit("Do not repeat a build configuration")
    lock = json.loads((root / "build/dependencies.lock.json").read_text(encoding="utf-8"))["qtMigration"]
    package = lock["softwareMedia"]["ffmpeg"]
    profile = lock["videoInspectionWindows"]
    if package["version"] != "9.0.2" or package["commit"] != "946fcce07b6dcd0331c8cc609192aeff5e1924f8":
        raise SystemExit("Review the inspection API and bad-file suite before changing the FFmpeg source")
    if not package.get("patches"):
        raise SystemExit("Strict MOV validation must have a recorded controlled patch")
    # FFmpeg's fixed msvc_common_flags translates only the exact -g spelling to
    # -Z7. Numeric --enable-debug levels produce GCC-only -gN unchanged, which
    # MSVC rejects. Both configurations retain native symbols; the dynamic CRT
    # remains independently selected below.
    if "--enable-debug" not in profile["configure"] or any(
            flag.startswith("--enable-debug=") for flag in profile["configure"]):
        raise SystemExit("The fixed MSVC profile requires --enable-debug without a numeric level")
    workspace = options.workspace.resolve()
    workspace.mkdir(parents=True, exist_ok=True)
    if any(workspace.iterdir()):
        raise SystemExit(f"Use an empty private workspace; all existing files retained: {workspace}")
    msys = options.msys.resolve()
    bash = msys / "usr/bin/bash.exe"
    if not bash.is_file():
        raise SystemExit("The runner's official MSYS2 Bash/core utilities are required; no automatic system installation is performed")
    environment = msvc_environment(workspace)
    tools = workspace / "tools"
    tools.mkdir()
    nasm_archive = tools / "nasm.zip"
    nasm_url = download(profile["nasm"], nasm_archive)
    nasm_root = tools / "nasm"
    with zipfile.ZipFile(nasm_archive) as archive:
        safe_members(archive.namelist())
        archive.extractall(nasm_root)
    nasms = list(nasm_root.rglob("nasm.exe"))
    if len(nasms) != 1:
        raise SystemExit("Pinned NASM archive must contain exactly one assembler")

    # Reuse the runner's MSYS2 runtime/coreutils, but restore GNU Make from an
    # exact official package with vcpkg's recorded digest into our own directory.
    make_archive = tools / "make.pkg.tar.zst"
    make_url = download(profile["make"], make_archive)
    safe_members(output("cmake", "-E", "tar", "tf", str(make_archive)).splitlines())
    make_root = tools / "make"
    make_root.mkdir()
    run("cmake", "-E", "tar", "xf", str(make_archive), cwd=make_root)
    make = make_root / "usr/bin/make.exe"
    if not make.is_file():
        raise SystemExit("Pinned GNU Make package is incomplete")
    # Keep native MSVC link.exe before MSYS's unrelated link command.
    compiler_bin = Path(environment["VCTOOLSINSTALLDIR"]) / "bin/Hostx64/x64"
    if not (compiler_bin / "cl.exe").is_file():
        raise SystemExit("The native x64 MSVC compiler is unavailable")
    environment["PATH"] = os.pathsep.join((str(nasms[0].parent), str(make.parent),
        str(compiler_bin), str(msys / "usr/bin"), environment["PATH"]))
    source = workspace / "source"
    run("git", "init", str(source), env=environment)
    run("git", "remote", "add", "origin", package["repository"], cwd=source, env=environment)
    run("git", "fetch", "--depth=1", "--no-tags", "origin", package["commit"], cwd=source, env=environment)
    run("git", "-c", "advice.detachedHead=false", "checkout", "--detach", "FETCH_HEAD", cwd=source, env=environment)
    if output("git", "rev-parse", "HEAD", cwd=source, env=environment) != package["commit"]:
        raise SystemExit("Restored FFmpeg revision differs from the lock")
    for patch in package["patches"]:
        path = root / patch["path"]
        if hashlib.sha256(path.read_bytes()).hexdigest() != patch["sha256"]:
            raise SystemExit(f"Controlled patch checksum mismatch: {path}")
        run("git", "apply", "--check", str(path), cwd=source, env=environment)
        run("git", "apply", str(path), cwd=source, env=environment)

    def posix(path):
        return output(str(bash), "--noprofile", "--norc", "-c", 'cygpath -u "$1"',
                      "huhucapture", str(path), env=environment)

    metadata = {"source": package["commit"], "patches": package["patches"], "configurations": {},
                "nasm": {"url": nasm_url, "sha512": profile["nasm"]["sha512"]},
                "make": {"url": make_url, "sha512": profile["make"]["sha512"]},
                "host": {"VCToolsVersion": environment.get("VCTOOLSVERSION"),
                         "WindowsSDKVersion": environment.get("WINDOWSSDKVERSION"),
                         "msysRuntime": output(str(bash), "--noprofile", "--norc", "-c", "uname -r", env=environment)}}
    metadata["nasm"]["versionOutput"] = output(str(nasms[0]), "-v", env=environment)
    metadata["make"]["versionOutput"] = output(str(make), "--version", env=environment)
    for configuration in configurations:
        build = workspace / ("build-" + configuration)
        prefix = workspace / "install" / configuration
        build.mkdir()
        crt = "MDd" if configuration == "Debug" else "MD"
        arguments = [posix(source / "configure"), "--prefix=" + posix(prefix), *profile["configure"],
                     "--extra-cflags=-" + crt, "--extra-cxxflags=-" + crt,
                     "--extra-ldflags=-debug -opt:ref -opt:icf", "--x86asmexe=" + posix(nasms[0])]
        script = build / "configure-fixed.sh"
        script.write_text("set -eu\n" + "cd " + shlex.quote(posix(build)) + "\n" +
                          shlex.join(arguments) + "\n", encoding="utf-8", newline="\n")
        run(str(bash), "--noprofile", "--norc", posix(script), env=environment)
        configuration_text = (build / "ffbuild/config.mak").read_text(encoding="utf-8", errors="replace")
        cflags = next((line for line in configuration_text.splitlines() if line.startswith("CFLAGS=")), "")
        if "-" + crt not in cflags or "-MT" in cflags or (crt == "MD" and "-MDd" in cflags):
            raise SystemExit(f"Actual {configuration} compiler flags do not use the requested dynamic CRT")
        if not re.search(r"(?:^|\s)-Z7(?:\s|$)", cflags.removeprefix("CFLAGS=")) or re.search(
                r"(?:^|\s)-g\d*(?:\s|$)", cflags.removeprefix("CFLAGS=")):
            raise SystemExit(f"Actual {configuration} compiler flags must contain native -Z7, not a GCC debug level")
        components = (build / "config_components.h").read_text(encoding="utf-8")
        if re.search(r"^#define CONFIG_\w+_(ENCODER|HWACCEL) 1$", components, re.MULTILINE):
            raise SystemExit("The inspection-only profile unexpectedly enabled an encoder or hardware backend")
        for required in ("MP3FLOAT_DECODER", "MPEGAUDIO_PARSER"):
            if f"#define CONFIG_{required} 1" not in components.splitlines():
                raise SystemExit(f"The locked inspection profile must retain Windows MP3 recordings: {required}")
        if "CONFIG_SWSCALE=yes" not in configuration_text.splitlines():
            raise SystemExit("Software HEVC covers require the locked swscale library")
        script = build / "build-fixed.sh"
        script.write_text("set -eu\n" + "cd " + shlex.quote(posix(build)) + "\n" +
                          shlex.join((posix(make), "-j" + str(options.jobs))) + "\n" +
                          shlex.join((posix(make), "install")) + "\n", encoding="utf-8", newline="\n")
        run(str(bash), "--noprofile", "--norc", posix(script), env=environment)
        files = []
        for name, major in (("avcodec", 63), ("avformat", 63), ("avutil", 61), ("swscale", 10)):
            for filename in (f"{name}-{major}.dll", f"{name}.lib"):
                binary = prefix / "bin" / filename
                if not binary.is_file():
                    raise SystemExit(f"Actual MSVC DLL/import library missing: {binary}")
                files.append({"path": str(binary.relative_to(prefix)), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest()})
            symbols = list(build.rglob(f"{name}-{major}.pdb"))
            if len(symbols) != 1:
                raise SystemExit(f"Expected one actual {name} linker PDB, got {symbols}")
            symbols_directory = prefix / "symbols"
            symbols_directory.mkdir(exist_ok=True)
            shutil.copy2(symbols[0], symbols_directory / symbols[0].name)
        licenses = prefix / "licenses/ffmpeg"
        licenses.mkdir(parents=True)
        for name in ("COPYING.LGPLv2.1", "LICENSE.md"):
            shutil.copy2(source / name, licenses / name)
        metadata["configurations"][configuration] = {"prefix": str(prefix), "crt": crt,
            "configure": arguments, "actualCflags": cflags, "files": files}
    (workspace / "build-info.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print("Built pinned MSVC AVCodec/AVFormat/AVUtil/swscale inspection and cover DLLs with per-configuration dynamic CRTs.")
    print("These dependencies contain no FFmpeg recording encoder, x264, network protocol or GUI runtime.")


if __name__ == "__main__":
    main()
