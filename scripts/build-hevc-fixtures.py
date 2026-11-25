#!/usr/bin/env python3
"""Build isolated, pinned HEVC fixture tools and encode real Main/Main10 files."""

import argparse
from array import array
import hashlib
import json
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request


def run(*arguments, cwd=None):
    subprocess.run(arguments, cwd=cwd, check=True)


def output(*arguments, cwd=None):
    return subprocess.check_output(arguments, cwd=cwd, text=True, encoding="utf-8",
                                   errors="replace", stderr=subprocess.STDOUT).strip()


def digest(path, algorithm="sha256"):
    result = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def source_blob(path):
    data = path.read_bytes()
    return hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\0" + data).hexdigest()


def restore_x265(specification, workspace):
    archive = workspace / "x265.tar.gz"
    errors = []
    for url in specification["urls"]:
        try:
            with urllib.request.urlopen(url, timeout=60) as response, archive.open("xb") as target:
                shutil.copyfileobj(response, target)
            if digest(archive, "sha512") != specification["sha512"]:
                raise RuntimeError("The official x265 archive differs from the recorded VLC checksum")
            break
        except Exception as error:
            if archive.exists():
                archive.unlink()  # Only this private, exclusively created download.
            errors.append(str(error))
    else:
        raise RuntimeError("Cannot restore the fixed x265 source: " + "; ".join(errors))
    extracted = workspace / "upstream"
    extracted.mkdir()
    with tarfile.open(archive, "r:gz") as stream:
        for member in stream.getmembers():
            path = PurePosixPath(member.name)
            if path.is_absolute() or ".." in path.parts or not (member.isfile() or member.isdir() or member.issym()):
                raise RuntimeError("Unsafe x265 source archive entry: " + member.name)
        stream.extractall(extracted, filter="data")
    roots = [path.parent.parent for path in extracted.glob("*/source/CMakeLists.txt")]
    if len(roots) != 1:
        raise RuntimeError("The official x265 source archive has an unexpected root")
    source = roots[0]
    actual = {entry["path"]: source_blob(source / entry["path"]) for entry in specification["reviewedFiles"]}
    (workspace / "x265-source-files.json").write_text(json.dumps(actual, indent=2) + "\n")
    for entry in specification["reviewedFiles"]:
        if actual[entry["path"]] != entry["blob"]:
            raise RuntimeError("The archive source differs from the reviewed 4.1 source: " + entry["path"])
    return source, url, actual


def write_pattern(path, depth):
    # The Y/Cb/Cr pattern is fill_yuv_image from the fixed FFmpeg mux.c,
    # Copyright (c) 2003 Fabrice Bellard (MIT). Its complete notice remains in
    # third_party/ffmpeg-examples/mux.c. No codec or color conversion is copied.
    with path.open("xb") as stream:
        for frame in range(251):
            planes = (bytes((x + y + frame * 3) & 255 for y in range(288) for x in range(352)),
                      bytes((128 + y + frame * 2) & 255 for y in range(144) for x in range(176)),
                      bytes((64 + x + frame * 5) & 255 for y in range(144) for x in range(176)))
            for plane in planes:
                if depth == 8:
                    stream.write(plane)
                else:
                    samples = array("H", (value << 2 for value in plane))
                    if sys.byteorder != "little":
                        samples.byteswap()
                    stream.write(samples.tobytes())
    expected = 251 * 352 * 288 * 3 // 2 * (1 if depth == 8 else 2)
    if path.stat().st_size != expected:
        raise RuntimeError("The raw test pattern is physically incomplete")


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixtures", type=Path, required=True)
    parser.add_argument("--generator", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, default=root / ".deps/hevc-fixtures")
    parser.add_argument("--cmake", type=Path, default=Path("/usr/bin/cmake"))
    parser.add_argument("--jobs", type=int, default=2)
    options = parser.parse_args()
    if platform.system() != "Linux" or platform.machine() != "x86_64" or not 1 <= options.jobs <= 64:
        raise SystemExit("Use the actual Linux amd64 CI runner and 1..64 build jobs")
    lock = json.loads((root / "build/dependencies.lock.json").read_text())["qtMigration"]
    specification = lock["hevcFixtureTools"]
    fixed_ffmpeg = lock["softwareMedia"]["ffmpeg"]
    fixtures = options.fixtures.resolve()
    manifest = json.loads((fixtures / "manifest.json").read_text())
    original = fixtures / "valid.mp4"
    listed = [entry for entry in manifest["cases"] if entry["file"] == "valid.mp4"]
    if len(listed) != 1 or digest(original) != listed[0]["sha256"] or len(manifest["cases"]) != 15:
        raise SystemExit("Generate and preserve the original 15 real fixtures before the HEVC extension")
    workspace = options.workspace.resolve()
    workspace.mkdir(parents=True, exist_ok=True)
    if any(workspace.iterdir()):
        raise SystemExit("Use an empty private HEVC tool workspace; existing files retained: " + str(workspace))
    source, source_url, source_files = restore_x265(specification["x265"], workspace)
    cmake = options.cmake.resolve()
    cmake_version = output(str(cmake), "--version")
    if not cmake_version.startswith("cmake version 3."):
        raise RuntimeError("x265 4.1 requires reviewed CMake 3.x policies; use Ubuntu's /usr/bin/cmake")
    metadata = {"schema": 1, "purpose": specification["purpose"],
        "x265": {"version": specification["x265"]["version"], "sourceUrl": source_url,
                 "archiveSha512": specification["x265"]["sha512"], "reviewedSourceFiles": source_files,
                 "cmakeVersion": cmake_version, "cmakeSha256": digest(cmake),
                 "cmakeDebianPackage": output("dpkg-query", "-W", "-f=${binary:Package} ${Version}", "cmake"),
                 "encoders": {}}, "ffmpeg": {}, "fixtures": {}}
    encoders = {}
    for depth in (8, 10):
        build = workspace / f"x265-{depth}-build"
        arguments = [str(cmake), "-S", str(source / "source"), "-B", str(build), "-G", "Unix Makefiles",
                     *specification["x265"]["cmake"], "-DHIGH_BIT_DEPTH=" + ("ON" if depth == 10 else "OFF")]
        run(*arguments)
        run(str(cmake), "--build", str(build), "--target", "cli", "--parallel", str(options.jobs))
        executable = build / "x265"
        version = output(str(executable), "--version")
        if not re.search(r"encoder version 4\.1(?:\b|\+)", version):
            raise RuntimeError("The built x265 CLI did not report the pinned 4.1 version")
        encoders[depth] = executable
        metadata["x265"]["encoders"][str(depth)] = {"versionOutput": version, "sha256": digest(executable), "cmake": arguments}

    # Reuse the already restored/patched FFmpeg source. This separate static CLI
    # only remuxes the elementary HEVC and existing AAC; application libraries,
    # configure options and dynamic-loader search paths remain unchanged.
    ffmpeg_source = root / ".deps/media-validation/ffmpeg-source"
    actual_commit = output("git", "-C", str(ffmpeg_source), "rev-parse", "HEAD")
    if actual_commit != fixed_ffmpeg["commit"] or actual_commit != "946fcce07b6dcd0331c8cc609192aeff5e1924f8":
        raise RuntimeError("The fixture remuxer source is not the fixed FFmpeg 9.0.2 tree")
    for entry in specification["ffmpeg"]["reviewedSourceFiles"]:
        if source_blob(ffmpeg_source / entry["path"]) != entry["blob"]:
            raise RuntimeError("A reviewed remuxer source file changed: " + entry["path"])
    for patch in fixed_ffmpeg["patches"]:
        path = root / patch["path"]
        if digest(path) != patch["sha256"]:
            raise RuntimeError("The controlled FFmpeg patch differs from the lock")
        run("git", "-C", str(ffmpeg_source), "apply", "--reverse", "--check", str(path))
    build = workspace / "ffmpeg-build"
    build.mkdir()
    arguments = [str(ffmpeg_source / "configure"), *specification["ffmpeg"]["configure"]]
    run(*arguments, cwd=build)
    run("make", "-j" + str(options.jobs), "ffmpeg", cwd=build)
    ffmpeg = build / "ffmpeg"
    components = (build / "config_components.h").read_text()
    if re.search(r"^#define CONFIG_\w+_(ENCODER|HWACCEL) 1$", components, re.MULTILINE):
        raise RuntimeError("The HEVC remux CLI unexpectedly enables a recording encoder or hardware backend")
    metadata["ffmpeg"] = {"commit": actual_commit, "patches": fixed_ffmpeg["patches"],
        "reviewedSourceFiles": specification["ffmpeg"]["reviewedSourceFiles"],
        "configure": arguments, "versionOutput": output(str(ffmpeg), "-version"), "sha256": digest(ffmpeg)}
    encoded = workspace / "encoded"
    encoded.mkdir()
    for depth in (8, 10):
        raw = workspace / f"pattern-{depth}.yuv"
        bitstream = workspace / f"encoded-{depth}.hevc"
        media = encoded / f"valid-hevc-{depth}bit.mp4"
        write_pattern(raw, depth)
        encode = [str(encoders[depth]), "--input", str(raw), "--input-res", "352x288", "--input-csp", "i420",
                  "--input-depth", str(depth), "--output-depth", str(depth), "--fps", "25", "--frames", "251",
                  "--profile", "main10" if depth == 10 else "main", "--preset", "ultrafast", "--pools", "none",
                  "--frame-threads", "1", "--bframes", "0", "--keyint", "64", "--min-keyint", "64",
                  "--no-open-gop", "--no-wpp", "--range", "full", "--colorprim", "bt709",
                  "--transfer", "bt709", "--colormatrix", "bt709", "--no-progress", "--output", str(bitstream)]
        encode_log = output(*encode)
        (encoded / f"x265-{depth}.log").write_text(encode_log + "\n")
        if not re.search(r"encoded 251 frames", encode_log) or not bitstream.is_file() or bitstream.stat().st_size == 0:
            raise RuntimeError("x265 did not finish every requested actual frame")
        remux = [str(ffmpeg), "-hide_banner", "-nostdin", "-n", "-fflags", "+genpts", "-f", "hevc",
                 "-framerate", "25", "-i", str(bitstream), "-i", str(original), "-map", "0:v:0", "-map", "1:a:0",
                 "-c", "copy", "-bsf:v", "setts=pts=N/(25*TB):dts=N/(25*TB):duration=1/(25*TB)",
                 "-tag:v", "hvc1", "-f", "mp4", str(media)]
        remux_log = output(*remux)
        (encoded / f"remux-{depth}.log").write_text(remux_log + "\n")
        metadata["fixtures"][media.name] = {"sha256": digest(media), "rawSha256": digest(raw),
            "elementarySha256": digest(bitstream), "encode": encode, "remux": remux, "bitDepth": depth}
    licenses = encoded / "licenses"
    licenses.mkdir()
    shutil.copy2(source / "COPYING", licenses / "COPYING.x265")
    shutil.copy2(ffmpeg_source / "COPYING.LGPLv2.1", licenses / "COPYING.ffmpeg")
    shutil.copy2(root / "third_party/ffmpeg-examples/mux.c", licenses / "mux.c")
    (encoded / "hevc-source.json").write_text(json.dumps(metadata, indent=2) + "\n")
    run(str(options.generator.resolve()), "--hevc", str(fixtures), str(encoded))
    shutil.copytree(encoded, fixtures / "hevc-evidence", ignore=shutil.ignore_patterns("*.mp4"))
    print("Generated real 8-bit and 10-bit HEVC fixture inputs; archive acceptance is decided by the platform tests.")


if __name__ == "__main__":
    main()
