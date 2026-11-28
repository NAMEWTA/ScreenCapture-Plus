#!/usr/bin/env python3
"""Create/restore an isolated Linux development SDK; never a product package."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shlex
import shutil
import stat
import subprocess
import tarfile


HOST_LIBRARIES = re.compile(r"^(ld-linux.*|lib(c|m|pthread|dl|rt|util|resolv|anl|nss_.*)\.so(?:\..*)?|libstdc\+\+\.so(?:\..*)?|libgcc_s\.so(?:\..*)?)$")
HOST_PACKAGES = {"libc6", "libc6-dev", "libc-dev-bin", "linux-libc-dev", "libstdc++6", "libgcc-s1"}
MODULES = ("gio-2.0", "gio-unix-2.0", "gobject-2.0", "gthread-2.0", "glib-2.0",
           "wayland-client", "wayland-cursor", "wayland-egl", "xkbcommon", "gl", "egl",
           "xcb", "xcb-keysyms", "xcb-randr", "xcb-shm", "xcb-xfixes", "xcb-xtest", "xcb-shape", "openssl")


def output(*arguments, env=None):
    return subprocess.check_output(arguments, env=env, text=True, encoding="utf-8", errors="strict").strip()


def digest(path):
    checksum = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def soname_alias(path):
    """Find the installed loader name of an actual shared ELF object."""
    name = elf_soname(path)
    if name is None:
        return None
    alias = path.with_name(name)
    if not alias.exists() or alias.resolve() != path.resolve():
        raise RuntimeError(f"Installed ELF SONAME does not identify its actual bytes: {alias}")
    return alias


def elf_soname(path, env=None):
    with path.open("rb") as stream:
        if stream.read(4) != b"\x7fELF":
            return None
    dynamic = output("readelf", "--wide", "--dynamic", str(path), env=env)
    names = re.findall(r"\(SONAME\).*?\[([^\]]+)\]", dynamic)
    if len(names) > 1 or (names and (PurePosixPath(names[0]).name != names[0] or names[0] in (".", ".."))):
        raise RuntimeError(f"Invalid ELF SONAME in the development SDK: {path}")
    return names[0] if names else None


def verify_soname_aliases(bundle):
    """ldd checks a DSO's dependencies, not that DSO's own loader name."""
    found = []
    for binary in sorted(bundle.rglob("*")):
        if binary.is_symlink() or not binary.is_file():
            continue
        name = elf_soname(binary)
        if not name:
            continue
        alias = binary.parent / name
        if not alias.exists() or alias.resolve(strict=True) != binary.resolve(strict=True):
            raise RuntimeError(f"SDK is missing the actual ELF SONAME alias: {alias} -> {binary.name}")
        found.append({"path": str(binary.relative_to(bundle)), "soname": name})
    return found


def require_empty(path):
    path.mkdir(parents=True, exist_ok=True)
    if path.is_symlink() or any(path.iterdir()):
        raise RuntimeError(f"Use an empty real directory; existing data retained: {path}")


def owner(path):
    candidates = (str(path), str(path.resolve()))
    for candidate in dict.fromkeys(candidates):
        result = subprocess.run(("dpkg-query", "-S", candidate), text=True, capture_output=True)
        if result.returncode == 0:
            matches = [line.split(": ", 1)[0] for line in result.stdout.splitlines() if ": " in line]
            if len(matches) == 1:
                return matches[0]
    raise RuntimeError(f"Cannot attribute a copied system file to its actual dpkg package: {path}")


def system_modules():
    pending = list(MODULES)
    found = {}
    while pending:
        module = pending.pop()
        if module in found:
            continue
        directory = Path(output("pkg-config", "--variable=pcfiledir", module))
        pc = directory / (module + ".pc")
        if not pc.is_file() or not str(pc).startswith("/usr/"):
            raise RuntimeError(f"Host development module is missing or not from the runner: {module}")
        found[module] = {"pc": str(pc), "package": owner(pc), "version": output("pkg-config", "--modversion", module)}
        dependencies = output("pkg-config", "--print-requires", "--print-requires-private", module)
        for line in dependencies.splitlines():
            match = re.match(r"([A-Za-z0-9_.+\-]+)", line)
            if match:
                pending.append(match.group(1))
    return found


def development_packages(packages):
    """Include headers imported by the selected development packages themselves."""
    found = set(packages)
    pending = list(packages)
    while pending:
        package = pending.pop()
        dependencies = output("dpkg-query", "-W", "-f=${Depends}, ${Pre-Depends}", package)
        for group in dependencies.split(","):
            for alternative in group.split("|"):
                match = re.match(r"\s*([A-Za-z0-9+.-]+)(?::[A-Za-z0-9-]+)?", alternative)
                if not match:
                    continue
                name = match.group(1)
                if not name.endswith("-dev") or name in HOST_PACKAGES or name in found:
                    continue
                installed = subprocess.run(("dpkg-query", "-W", "-f=${db:Status-Status}", name),
                                           text=True, capture_output=True)
                if installed.returncode == 0 and installed.stdout == "installed":
                    canonical = output("dpkg-query", "-W", "-f=${binary:Package}", name)
                    if canonical not in found:
                        found.add(canonical)
                        pending.append(canonical)
                    break
    return found


def make_bundle(options):
    if platform.system() != "Linux" or platform.machine() not in ("x86_64", "amd64"):
        raise RuntimeError("The development bundle must come from the real Linux amd64 CI runner")
    root = Path(__file__).resolve().parents[1]
    if options.output.is_symlink():
        raise RuntimeError("The output must be an empty real directory")
    destination = options.output.resolve()
    require_empty(destination)
    bundle = destination / "sdk"
    bundle.mkdir()
    lock = json.loads((root / "build/dependencies.lock.json").read_text())
    qt = options.qt_prefix.resolve()
    qt_version = output(str(qt / "bin/qmake"), "-query", "QT_VERSION")
    if qt_version != lock["qtMigration"]["qt"]["version"]:
        raise RuntimeError("The installed Qt SDK differs from the fixed lock")
    if not (qt / "translations").is_dir() or not list((qt / "translations").glob("qtbase_*.qm")):
        raise RuntimeError("The fixed official Qt translations archive is required for deployment")
    originals = {"qt": qt,
        "portal": (root / ".deps/portal-validation/install").resolve(),
        "pipewire": (root / ".deps/pipewire-validation/install").resolve(),
        "media": (root / ".deps/media-validation/install").resolve()}
    if any(destination.is_relative_to(prefix) for prefix in originals.values()):
        raise RuntimeError("The output cannot be inside a dependency prefix being copied")
    metadata = {"schema": 1, "purpose": "development dependencies only; not an installer or release acceptance",
        "commit": output("git", "-C", str(root), "rev-parse", "HEAD"),
        "qtVersion": qt_version, "originalPrefixes": {name: str(path) for name, path in originals.items()},
        "dependencyLock": lock, "dependencyBuilds": {}, "systemModules": {}, "systemPackages": {},
        "host": {"system": platform.platform(), "libc": platform.libc_ver(),
                 "compiler": output("g++", "--version"), "pkgConfig": output("pkg-config", "--version")}}
    for name, prefix in originals.items():
        if not prefix.is_dir():
            raise RuntimeError(f"Actual installed dependency prefix is missing: {prefix}")
        shutil.copytree(prefix, bundle / name, symlinks=True)
        if name != "qt":
            information = prefix.parent / "build-info.json"
            metadata["dependencyBuilds"][name] = json.loads(information.read_text())
    # huhucapture does not use QtSql. The official qtbase archive also ships
    # optional Oracle/Mimer/etc. drivers whose proprietary client runtimes are
    # deliberately absent from our build and product. Omit that unused module,
    # its driver binaries and import metadata together instead of treating an
    # unresolved optional database driver as an application runtime dependency.
    # Every retained ELF still has to satisfy the strict closure check below.
    metadata["excludedQtModule"] = {"name": "QtSql", "reason": "Not linked by huhucapture; external database clients are outside this development SDK", "paths": []}
    for pattern in ("plugins/sqldrivers", "include/QtSql", "lib/cmake/Qt6Sql*",
                    "lib/libQt6Sql*", "lib/pkgconfig/Qt6Sql.pc", "modules/Sql.json",
                    "mkspecs/modules/qt_lib_sql*.pri", "metatypes/qt6sql_*.json"):
        for path in sorted((bundle / "qt").glob(pattern)):
            metadata["excludedQtModule"]["paths"].append(str(path.relative_to(bundle / "qt")))
            if path.is_dir() and not path.is_symlink():
                shutil.rmtree(path)
            else:
                path.unlink()
    # QtWayland's archive contains both the desktop-client implementation used
    # by Widgets and an unrelated compositor/QML product. The latter requires
    # Qt Quick, which is not an application dependency. Keep the client, its
    # plugins, private headers and protocol generator intact. Remove the unused
    # compositor modules and QML imports together with their import metadata.
    # Source: qt/qtwayland v6.12.1 src/compositor/CMakeLists.txt,
    # src/imports/texture-sharing/CMakeLists.txt, and the compositor hardware
    # integration CMakeLists.txt. Retained ELF closure still must be complete.
    excluded_wayland = {"name": "QtWaylandCompositor and QML imports",
        "reason": "huhucapture uses Qt Widgets as a Wayland client, not a compositor or QML application",
        "source": "https://github.com/qt/qtwayland/tree/v6.12.1/src", "paths": []}
    metadata["excludedQtWaylandComponents"] = excluded_wayland
    patterns = ["qml", "plugins/wayland-graphics-integration-server",
                "plugins/wayland-hardware-layer-integration"]
    for module, configuration in (("WaylandCompositor", "waylandcompositor"),
                                  ("WaylandEglCompositorHwIntegration", "wayland_egl_compositor_hw_integration"),
                                  ("WaylandTextureSharing", "waylandtexturesharing")):
        patterns.extend((f"include/Qt{module}*", f"lib/cmake/Qt6{module}*",
                         f"lib/libQt6{module}*", f"lib/pkgconfig/Qt6{module}*.pc",
                         f"modules/{module}*.json", f"mkspecs/modules/qt_lib_{configuration}*.pri",
                         f"metatypes/qt6{module.lower()}_*.json"))
    for pattern in patterns:
        for path in sorted((bundle / "qt").glob(pattern)):
            excluded_wayland["paths"].append(str(path.relative_to(bundle / "qt")))
            if path.is_dir() and not path.is_symlink():
                shutil.rmtree(path)
            else:
                path.unlink()
    if metadata["dependencyBuilds"]["portal"]["commit"] != lock["qtMigration"]["libportal"]["commit"] or \
       metadata["dependencyBuilds"]["pipewire"]["commit"] != lock["qtMigration"]["pipewire"]["commit"]:
        raise RuntimeError("Installed Portal/PipeWire metadata differ from the lock")
    for name, key in (("portal", "libportal"), ("pipewire", "pipewire")):
        if metadata["dependencyBuilds"][name]["patches"] != lock["qtMigration"][key]["patches"]:
            raise RuntimeError(f"Installed {name} controlled patches differ from the lock")
    if metadata["dependencyBuilds"]["portal"]["qtVersion"] != qt_version:
        raise RuntimeError("libportal was built against a different Qt SDK")
    for name in ("ffmpeg", "x264"):
        if metadata["dependencyBuilds"]["media"]["components"][name]["commit"] != lock["qtMigration"]["softwareMedia"][name]["commit"]:
            raise RuntimeError(f"Installed {name} metadata differ from the lock")
        if metadata["dependencyBuilds"]["media"]["components"][name].get("patches", []) != lock["qtMigration"]["softwareMedia"][name].get("patches", []):
            raise RuntimeError(f"Installed {name} controlled patches differ from the lock")
        if metadata["dependencyBuilds"]["media"]["components"][name]["configure"][2:] != lock["qtMigration"]["softwareMedia"][name]["configure"]:
            raise RuntimeError(f"Installed {name} configure options differ from the lock")

    # The runner installs CMake into /usr/local alongside unrelated software.
    # Reconstruct its complete binary/data prefix, not the whole /usr/local.
    information = output("cmake", "--system-information")
    match = re.search(r'^CMAKE_ROOT "([^"]+)"$', information, re.MULTILINE)
    if not match:
        raise RuntimeError("The real CMake did not report its complete module/data directory")
    cmake_data = Path(match.group(1)).resolve()
    cmake_prefix = cmake_data.parent.parent
    cmake = bundle / "cmake"
    (cmake / "bin").mkdir(parents=True)
    for name in ("cmake", "ctest", "cpack", "ccmake", "cmake-gui"):
        executable = cmake_prefix / "bin" / name
        if not executable.is_file():
            resolved = shutil.which(name)
            executable = Path(resolved).resolve() if resolved else executable
        if executable.is_file():
            shutil.copy2(executable, cmake / "bin" / name)
        elif name not in ("ccmake", "cmake-gui"):
            raise RuntimeError(f"The runner's CMake prefix is incomplete: {name}")
    shutil.copytree(cmake_data, cmake / "share" / cmake_data.name, symlinks=True)
    for parent in (cmake_prefix / "doc", cmake_prefix / "share/doc"):
        if parent.is_dir():
            for child in parent.glob("cmake*"):
                if child.is_dir():
                    shutil.copytree(child, cmake / "doc" / child.name, symlinks=True, dirs_exist_ok=True)
    for parent in (cmake_prefix / "man", cmake_prefix / "share/man"):
        if parent.is_dir():
            for child in parent.rglob("*"):
                if child.is_file() and child.name.startswith(("cmake", "ctest.", "cpack.", "ccmake.")):
                    target = cmake / "share/man" / child.relative_to(parent)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(child.resolve(), target)
    metadata["cmake"] = {"version": output("cmake", "--version"), "originalBinary": str(Path(shutil.which("cmake")).resolve()),
                         "originalData": str(cmake_data), "dataRelative": "share/" + cmake_data.name}
    metadata["originalPrefixes"]["cmake"] = str(cmake_prefix)
    (bundle / "tools/bin").mkdir(parents=True)
    tool_packages = set()
    for name in ("ninja", "pkg-config", "dbus-run-session", "dbus-daemon"):
        # GitHub's /usr/local/bin/ninja may precede the official Ubuntu package
        # without belonging to dpkg. Select the installed package explicitly so
        # its observed version, executable bytes and copyright share one source.
        executable = Path("/usr/bin") / name
        if not executable.is_file():
            raise RuntimeError(f"Required official Ubuntu runner tool is missing: {executable}")
        shutil.copy2(executable.resolve(), bundle / "tools/bin" / name)
        tool_packages.add(owner(executable.resolve()))
        metadata[name] = {"version": output(str(executable), "--version"), "originalBinary": str(executable.resolve())}

    modules = system_modules()
    metadata["systemModules"] = modules
    packages = development_packages({item["package"] for item in modules.values()})
    header_packages = tuple(packages)
    packages.update(tool_packages)
    copied_system = {}
    package_owners = {}
    package_files = {}
    for package in header_packages:
        files = output("dpkg-query", "-L", package).splitlines()
        package_files[package] = files
        for filename in files:
            path = Path(filename)
            if path.is_file() or path.is_symlink():
                package_owners[str(path.parent.resolve() / path.name)] = package

    def copy_system(path):
        path = Path(path)
        if not path.exists() or path.is_dir():
            return
        if HOST_LIBRARIES.match(path.name):
            return
        package = package_owners.get(str(path.parent.resolve() / path.name))
        if package is None:
            package = owner(path)
        if package.split(":", 1)[0] in HOST_PACKAGES:
            return
        packages.add(package)
        # Canonicalize usr-merge parents but retain the actual SONAME link name.
        normalized = path.parent.resolve() / path.name
        relative = PurePosixPath("system") / str(normalized).lstrip("/")
        target = bundle / relative
        if str(relative) in copied_system:
            return
        target.parent.mkdir(parents=True, exist_ok=True)
        copied_system[str(relative)] = {"source": str(normalized), "package": package}
        if normalized.is_symlink():
            resolved = normalized.resolve(strict=True)
            copy_system(resolved)
            relocated = bundle / "system" / str(resolved).lstrip("/")
            if not relocated.exists():
                raise RuntimeError(f"Selected development symlink points to an excluded host library: {normalized}")
            target.symlink_to(os.path.relpath(relocated, target.parent))
        else:
            shutil.copy2(normalized, target)
            # A development .so link can point directly to .so.0.0.0. Retain
            # the real loader's .so.0 alias even if no dependency in Qt itself
            # imports this optional native module (for example XCB XTEST).
            alias = soname_alias(normalized)
            if alias is not None and alias != normalized:
                copy_system(alias)

    for package in header_packages:
        files = package_files[package]
        for filename in files:
            path = Path(filename)
            selected = (filename.startswith("/usr/include/") or "/include/" in filename or
                        filename.endswith(".pc") or filename.endswith(".so"))
            if selected and (path.is_file() or path.is_symlink()):
                copy_system(path)
    scanner = shutil.which("wayland-scanner")
    if not scanner:
        raise RuntimeError("The real Wayland protocol scanner is missing")
    copy_system(Path(scanner))

    # Resolve the actual ELF closure of the copied SDK and selected native tools.
    # glibc, its loader and the compiler ABI remain explicit host requirements.
    environment = os.environ.copy()
    environment["LD_LIBRARY_PATH"] = ":".join(str(p / "lib") for p in originals.values())
    environment["LC_ALL"] = "C"
    examined = set()
    abi_requirements = set()
    while True:
        pending = []
        for path in bundle.rglob("*"):
            if path.is_file() and not path.is_symlink() and str(path) not in examined:
                examined.add(str(path))
                with path.open("rb") as stream:
                    if stream.read(4) == b"\x7fELF":
                        pending.append(path)
        if not pending:
            break
        for binary in pending:
            # A -dev .so link can point straight to a versioned DSO. That DSO
            # need not be a dependency of any other SDK file, so ldd alone
            # misses its intermediate SONAME link (for example XTEST). Copy
            # the real runner-owned alias and attribute its runtime package.
            relative_binary = str(binary.relative_to(bundle))
            source = copied_system.get(relative_binary)
            soname = elf_soname(binary, environment)
            if source and soname:
                original = Path(source["source"])
                alias = original.parent / soname
                if not alias.exists() or alias.resolve(strict=True) != original.resolve(strict=True):
                    raise RuntimeError(f"Runner library has no matching SONAME alias: {original} ({soname})")
                copy_system(alias)
            versions = output("readelf", "--wide", "--version-info", str(binary), env=environment)
            # Only the required-version section describes this binary's host
            # requirement; do not mistake its exported versions for a demand.
            if "Version needs section" in versions:
                needed = versions.split("Version needs section", 1)[1]
                abi_requirements.update(re.findall(r"Name: ((?:GLIBC_|GLIBCXX_|CXXABI_|GCC_)[A-Za-z0-9_.]+)", needed))
            linked = subprocess.run(("ldd", str(binary)), env=environment, text=True, capture_output=True)
            if "not found" in linked.stdout:
                raise RuntimeError(f"Runner dependency closure is incomplete for {binary}:\n{linked.stdout}")
            if linked.returncode != 0 and "not a dynamic executable" not in linked.stderr + linked.stdout:
                raise RuntimeError(f"Cannot inspect the real ELF dependencies of {binary}: {linked.stderr}")
            for line in linked.stdout.splitlines():
                match = re.search(r"(?:=>\s+)?(/\S+)\s+\(", line)
                if not match:
                    continue
                library = Path(match.group(1))
                if HOST_LIBRARIES.match(library.name) or library.is_relative_to(bundle) or \
                   any(library.is_relative_to(p) for p in originals.values()):
                    continue
                copy_system(library)

    licenses = bundle / "licenses"
    licenses.mkdir()
    for package in sorted(packages):
        version = output("dpkg-query", "-W", "-f=${Version}", package)
        metadata["systemPackages"][package] = version
        copyright_file = Path("/usr/share/doc") / package.split(":", 1)[0] / "copyright"
        if not copyright_file.is_file():
            raise RuntimeError(f"Actual dpkg copyright notice is missing: {package}")
        target = licenses / "system" / (package.replace(":", "_") + ".copyright")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(copyright_file.resolve(), target)
    # Debian copyright files refer to these exact installed license texts.
    # Preserve those references' contents as well as each package's notice.
    common_licenses = Path("/usr/share/common-licenses")
    if not common_licenses.is_dir() or not any(common_licenses.iterdir()):
        raise RuntimeError("The runner's common license texts are missing")
    shutil.copytree(common_licenses, licenses / "system/common-licenses", symlinks=False)
    for path in (root / "third_party").rglob("*"):
        if path.is_file() and (path.name.upper().startswith(("LICENSE", "COPYING", "NOTICE")) or path.name == "upstream.json"):
            target = licenses / "project" / path.relative_to(root / "third_party")
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
    for name, source in (("portal", root / ".deps/portal-validation/source"),
                         ("pipewire", root / ".deps/pipewire-validation/source"),
                         ("ffmpeg", root / ".deps/media-validation/ffmpeg-source"),
                         ("x264", root / ".deps/media-validation/x264-source")):
        for path in source.iterdir():
            if path.is_file() and path.name.upper().startswith(("LICENSE", "COPYING", "NOTICE")):
                target = licenses / name / path.name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)

    # Keep original relative links. Only absolute links into a copied prefix
    # are rewritten in this owned copy; no host prefix is modified.
    metadata["symlinkRewrites"] = []
    for path in bundle.rglob("*"):
        if not path.is_symlink():
            continue
        original = os.readlink(path)
        if os.path.isabs(original):
            resolved = Path(original).resolve()
            destination_target = None
            for name, prefix in originals.items():
                if resolved.is_relative_to(prefix):
                    destination_target = bundle / name / resolved.relative_to(prefix)
                    break
            if destination_target is None:
                raise RuntimeError(f"An absolute SDK symlink escapes all recorded prefixes: {path} -> {original}")
            relative = os.path.relpath(destination_target, path.parent)
            path.unlink()
            path.symlink_to(relative)
            metadata["symlinkRewrites"].append({"path": str(path.relative_to(bundle)), "from": original, "to": relative})
        if not path.resolve(strict=True).is_relative_to(bundle):
            raise RuntimeError(f"A copied SDK symlink escapes the bundle: {path}")
    metadata["systemFiles"] = copied_system
    metadata["elfSonames"] = verify_soname_aliases(bundle)
    metadata["hostAbiVersions"] = sorted(abi_requirements)
    metadata["hostRequirements"] = "Linux amd64 satisfying hostAbiVersions, a C++20 compiler and its matching standard-library/libc/kernel development headers, and binutils readelf for actual SONAME validation. Matching dbus-run-session/dbus-daemon binaries are supplied for isolated validation, without a running user bus. No glibc, loader, compiler, GPU driver, compositor or user session is supplied. GUI/Portal acceptance is separate."
    (bundle / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    shutil.copy2(Path(__file__), bundle / "bundle-development-dependencies.py")
    inventory = {}
    for path in sorted(bundle.rglob("*")):
        relative = str(path.relative_to(bundle))
        if path.is_symlink():
            inventory[relative] = {"type": "symlink", "target": os.readlink(path)}
        elif path.is_file():
            inventory[relative] = {"type": "file", "size": path.stat().st_size, "sha256": digest(path), "mode": stat.S_IMODE(path.stat().st_mode)}
    (bundle / "manifest.json").write_text(json.dumps({"schema": 1, "files": inventory}, indent=2) + "\n")
    archive = destination / "huhucapture-development-linux-amd64.tar.gz"
    with tarfile.open(archive, "w:gz", compresslevel=6, dereference=False) as stream:
        stream.add(bundle, arcname="sdk", recursive=True)
    checksum = digest(archive)
    archive.with_suffix(archive.suffix + ".sha256").write_text(checksum + "  " + archive.name + "\n")
    shutil.copy2(bundle / "manifest.json", destination / "manifest.json")
    shutil.copy2(bundle / "metadata.json", destination / "metadata.json")
    print(f"Development archive: {archive}\nSHA256: {checksum}")


def restore_bundle(options):
    archive = options.archive.resolve()
    if digest(archive) != options.sha256.lower():
        raise RuntimeError("The development archive does not match its expected SHA-256")
    if options.destination.is_symlink():
        raise RuntimeError("The restore destination must be an empty real directory")
    destination = options.destination.resolve()
    require_empty(destination)
    with tarfile.open(archive, "r:gz") as stream:
        for entry in stream.getmembers():
            path = PurePosixPath(entry.name)
            if path.is_absolute() or ".." in path.parts or not path.parts or path.parts[0] != "sdk":
                raise RuntimeError(f"Unsafe SDK archive path: {entry.name}")
            if not (entry.isfile() or entry.isdir() or entry.issym()):
                raise RuntimeError(f"Unexpected SDK archive entry type: {entry.name}")
            if entry.issym() and os.path.isabs(entry.linkname):
                raise RuntimeError(f"Absolute SDK archive symlink: {entry.name}")
        stream.extractall(destination, filter="data")
    bundle = destination / "sdk"
    manifest = json.loads((bundle / "manifest.json").read_text())
    if manifest.get("schema") != 1:
        raise RuntimeError("Unknown SDK inventory schema")
    for relative, entry in manifest["files"].items():
        relative_path = PurePosixPath(relative)
        if relative_path.is_absolute() or ".." in relative_path.parts or not relative_path.parts:
            raise RuntimeError(f"Unsafe SDK inventory path: {relative}")
        path = bundle / relative
        if entry["type"] == "symlink":
            if not path.is_symlink() or os.readlink(path) != entry["target"] or not path.resolve(strict=True).is_relative_to(bundle):
                raise RuntimeError(f"SDK symlink differs from its inventory: {relative}")
        elif entry["type"] != "file" or path.is_symlink() or not path.is_file() or path.stat().st_size != entry["size"] or digest(path) != entry["sha256"]:
            raise RuntimeError(f"SDK file differs from its inventory: {relative}")
    actual_files = {str(path.relative_to(bundle)) for path in bundle.rglob("*") if path.is_file() or path.is_symlink()}
    if actual_files != set(manifest["files"]) | {"manifest.json"}:
        raise RuntimeError("The SDK archive has files absent from its inventory")
    metadata = json.loads((bundle / "metadata.json").read_text())
    sonames = verify_soname_aliases(bundle)
    if "elfSonames" in metadata and sonames != metadata["elfSonames"]:
        raise RuntimeError("Restored ELF SONAMEs differ from the bundle's actual library metadata")
    rewrites = []
    mapping = [(old, str(bundle / name)) for name, old in metadata["originalPrefixes"].items() if name != "cmake"]
    for path in bundle.rglob("*"):
        if path.is_symlink() or not path.is_file() or path.suffix not in (".pc", ".cmake", ".prl", ".pri", ".la", ".json", ".conf"):
            continue
        if path.name in ("manifest.json", "metadata.json") or "licenses" in path.relative_to(bundle).parts:
            continue
        try:
            before = path.read_text(encoding="utf-8")
        except UnicodeError:
            continue
        after = before
        for old, new in sorted(mapping, key=lambda item: len(item[0]), reverse=True):
            after = after.replace(old, new)
        if path.suffix == ".pc" and path.is_relative_to(bundle / "system"):
            # Include direct -I/usr and -L/usr flags, while avoiding a partial
            # match in another absolute path. /lib is the runner's /usr merge.
            system = str(bundle / "system/usr")
            # A path after ${prefix} is already relative to the relocated
            # prefix. Match only standalone absolute paths, not the /lib in
            # ${prefix}/lib (which used to duplicate the complete SDK path).
            after = re.sub(r"(?:(?<=-I)|(?<=-L)|(?<==)|(?<=[\s\"'])|^)/(usr|lib)(?=/|\s|$)",
                           lambda match: system + ("/lib" if match.group(1) == "lib" else ""), after)
        if after != before:
            before_hash = digest(path)
            path.write_text(after, encoding="utf-8")
            rewrites.append({"file": str(path.relative_to(bundle)), "beforeSha256": before_hash, "afterSha256": digest(path)})
    (bundle / "relocation.json").write_text(json.dumps({"archiveSha256": options.sha256,
        "originalPrefixes": metadata["originalPrefixes"], "restoredRoot": str(bundle), "rewrites": rewrites}, indent=2) + "\n")
    libdirs = [bundle / name / "lib" for name in ("qt", "portal", "pipewire", "media")]
    libdirs += [bundle / "system/usr/lib/x86_64-linux-gnu", bundle / "system/usr/lib"]
    pcdirs = [path / "pkgconfig" for path in libdirs] + [bundle / "system/usr/share/pkgconfig"]
    variables = {
        "QT_ROOT_DIR": str(bundle / "qt"), "QTDIR": str(bundle / "qt"),
        "Qt6_DIR": str(bundle / "qt/lib/cmake/Qt6"),
        "QT_HOST_PATH": str(bundle / "qt"),
        "CMAKE_PREFIX_PATH": ":".join(str(bundle / name) for name in ("qt", "portal", "pipewire", "media", "system/usr")),
        "PKG_CONFIG_PATH": ":".join(str(path) for path in pcdirs),
        "PKG_CONFIG_LIBDIR": ":".join(str(path) for path in pcdirs),
        "LD_LIBRARY_PATH": ":".join(str(path) for path in libdirs),
        "PIPEWIRE_MODULE_DIR": str(bundle / "pipewire/lib/pipewire-0.3"),
        "SPA_PLUGIN_DIR": str(bundle / "pipewire/lib/spa-0.2"),
        "PIPEWIRE_CONFIG_DIR": str(bundle / "pipewire/share/pipewire"),
        "QT_PLUGIN_PATH": str(bundle / "qt/plugins"),
    }
    script = "# Source this file only for this isolated development session.\nunset PKG_CONFIG_SYSROOT_DIR\n"
    script += "".join("export " + key + "=" + shlex.quote(value) + "\n" for key, value in variables.items())
    paths = ":".join(str(bundle / name) for name in ("cmake/bin", "tools/bin", "qt/bin", "system/usr/bin"))
    script += "export PATH=" + shlex.quote(paths) + ':"$PATH"\n'
    (bundle / "environment.sh").write_text(script)
    print(f"Verified and restored development SDK: {bundle}\nSource {bundle / 'environment.sh'} for an isolated build.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("create")
    create.add_argument("--qt-prefix", type=Path, required=True)
    create.add_argument("--output", type=Path, required=True)
    restore = commands.add_parser("restore")
    restore.add_argument("--archive", type=Path, required=True)
    restore.add_argument("--sha256", required=True)
    restore.add_argument("--destination", type=Path, required=True)
    options = parser.parse_args()
    (make_bundle if options.command == "create" else restore_bundle)(options)


if __name__ == "__main__":
    main()
