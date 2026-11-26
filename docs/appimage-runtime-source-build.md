# AppImage runtime built from complete original sources

This producer addresses the outer AppImage runtime's static input closure.
It does not establish the full Qt application's P12 validation or authorize
release publication. Issue #5 remains the migration and release authority.

## Scope and differences from the previous official asset

The type-2 runtime remains fixed to
[`AppImage/type2-runtime@dd6cebedcbddde9c82f89b011e8e1d40b6e43868`](https://github.com/AppImage/type2-runtime/tree/dd6cebedcbddde9c82f89b011e8e1d40b6e43868).
Its original dependency script pins fuse 3.15.0 and squashfuse 0.5.2, but its
floating Alpine container does not establish the exact old musl, allocator,
compression-library or compiler-support bytes. A container tag, a package name
or a retained source tree alone cannot fill this gap.

The new producer builds an independent runtime from an explicit source set.
It uses canonical musl-cross-make's documented `litecross/Makefile` entry with
separately restored, patched sources. GCC 9.4.0 replaces the original build's
Clang invocation. Target shared toolchain libraries are disabled. This is a new
runtime with recorded differences, not a claim that its bytes match the older
official runtime asset. The original binary's SHA256 remains unchanged in the
lock and the original development packaging route remains available.

| Input | Exact version or Git commit |
| --- | --- |
| Canonical musl-cross-make | `5e8042e63de29f8473fc3b0b831a587ba284e1a8` |
| musl | 1.2.6, exact archive and 3 canonical patches |
| GCC, including actual compiler support and CRT objects | 9.4.0, exact archive and 19 canonical patches |
| GNU binutils | 2.44, exact archive and 4 canonical patches |
| GMP / MPFR / MPC | 6.3.0 / 4.2.2 / 1.3.1, exact archives |
| Linux headers used by the target sysroot | 4.19.88-2, exact canonical cross-make archive |
| libfuse | 3.15.0, exact original archive and type-2 runtime mount patch |
| squashfuse | 0.5.2, exact original archive |
| zstd | `794ea1b0afca0f020f4e57b6732332231fb23c70` (v1.5.6) |
| zlib | `51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf` (v1.3.1) |
| mimalloc | `8c532c32c3c96e5ba1f2283e032f69ead8add00f` (v2.1.7) |

Full URLs, archive SHA256/SHA1 values, release tag objects and patch SHA256
values are in `build/package/dependencies.lock.json`.
`third_party/appimage/source-reviews.json` and
`third_party/appimage/toolchain-patches/upstream.json` retain the review origins.
The actual build compares the patch bytes and canonical SHA1 records with the
restored original musl-cross-make source. It preserves the complete original
archives and Git checkouts for later source collection.

Archive extraction preserves each original regular file's modification time,
including the ordering between distributed `configure`, `Makefile.in`, and
their maintainer inputs. It restores directory times after creating their
children and never applies a symlink's timestamp to its target. Original PAX
decimal timestamps must be exactly representable by the filesystem; invalid
or rounded values fail. The retained, hash-checked original archive supplies
these metadata, and the actual retained collector code implements their
restoration before any original patches run. The source inventory still checks
the complete original bytes, links and executable modes.

The timestamp helper checks an ordinary file/directory's identity before and
after writing. It uses `follow_symlinks=False` only where the interpreter
advertises support; the
[Python 3.13 implementation](https://github.com/python/cpython/blob/v3.13.17/Modules/posixmodule.c)
on Windows does not support that keyword for `utime`. Windows reparse entries
are rejected. The shared timestamp fixture uses an exact 100 ns multiple,
matching [Windows FILETIME precision](https://learn.microsoft.com/en-us/windows/win32/api/minwinbase/ns-minwinbase-filetime); finer
archive values that a filesystem rounds are rejected rather than normalized.
The GNU Make ordering fixture runs on Unix. Local Linux fixture results do not
claim that the Windows collector or a complete cross toolchain ran.

This matters for MPFR 4.2.2, whose original configure enables maintainer rules:
replacing archive times with extraction times can make `aclocal.m4` newer than
the distributed `configure` and `Makefile.in`, triggering source regeneration.
The producer keeps that upstream configure behavior. It does not install a
different generator to rewrite the source tree or disable maintainer rules to
hide this extraction error. An actual unchanged source tree and successful
native producer receipts remain required.

All Git inputs use the shared `original-git-blob-bytes-v1` checkout profile.
Before the first fetch/checkout, each new owned repository gets the exact
`info/attributes` policy from `build/package/source_checkout.py`. This preserves
original LF and CRLF blob bytes and disables working-tree text/filter/encoding
conversions without modifying upstream attributes or the index. In particular,
zstd's original `*.vcproj*`, `*.rc`, `*.bat` and `*.cmd` attributes request CRLF
checkout even when `core.autocrlf=false`; ordinary checkout therefore need not
equal the original Git objects required by source collection.

The real runtime producer retains both the installed policy and the helper
bytes before fetching. Each Git input manifest binds those materials. Final
runtime validation checks the policy and retained copies again, then compares
the original checkout to its exact source manifest. Existing populated
repositories or changed policies fail. The collector's original byte comparison
is unchanged; this is the explicit original-blob source-build profile, not a
claim that normal Git checkout conversions were reproduced.

## Exact original toolchain patch application

Only the three locked musl/GCC/binutils archives use the named
`musl-cross-make-cowpatch-v1` profile. The canonical
[`Makefile`](https://github.com/richfelker/musl-cross-make/blob/5e8042e63de29f8473fc3b0b831a587ba284e1a8/Makefile)
concatenates each directory's original patches in filename order and sends them
through its original
[`cowpatch.sh`](https://github.com/richfelker/musl-cross-make/blob/5e8042e63de29f8473fc3b0b831a587ba284e1a8/cowpatch.sh)
to GNU `patch -p1`. Some canonical binutils hunks require GNU patch's default
fuzz limit of two; strict `git apply` does not implement that matching rule.
The producer executes the retained, byte-identical script with `-p1 -N -t -F2`:
the fuzz limit is explicit, reversed/already-applied patches are rejected, and
failure is noninteractive. It never enables whitespace-insensitive matching or
changes an original archive or patch to make the operation succeed.

The lock fixes both the complete pristine inventory digest and the complete
patched inventory digest for each archive. These inventories include every
regular file, executable mode and symbolic link, including GNU patch's `.orig`
backups and any copy-on-write output. Source preparation uses independent fully
materialized copies, so it does not recreate the original recipe's optional
lazy symlink scaffold. Unexpected copied, backup, reject or temporary files
change the inventory and fail verification; backups are not discarded.

The helper retains the actual original combined patch and script used by each
command, the helper itself, GNU patch/shell identities, actual arguments,
return codes and full stdout/stderr. Each source manifest references its
`source-inputs/<component>-patch-replay.json`. Runtime validation binds that
record to the original executed command and retained bytes. The source collector
independently applies the same pinned recipe to the retained original archive,
checks both complete inventories again and includes its own actual replay
record in source materials. A collection plan cannot choose another profile.
All other source patches keep the existing strict `git apply` behavior.

## Original Linux header paths

The exact `linux-headers-4.19.88-2.tar.xz` contains eight pairs of distinct Linux
UAPI headers whose names differ by case, such as `ip6t_HL.h` and `ip6t_hl.h`.
Both files are original inputs and must survive extraction and installation.
`build/package/linux_header_paths.py` fixes all sixteen complete relative paths,
their original sizes, SHA256 values and modes. The lock also fixes the complete
1,443-entry source inventory, with its original archive SHA256 unchanged.

Only this fixed source component, its actual `x86_64-linux-musl/include` output,
and its own component directory in the Linux source bundle use the reviewed
path profile. The producer checks the complete source tree; its actual copy
installation records both header identities independently. The collector checks
the original archive and complete tree again, retains the policy, validates the
receipt's separate output entries and reopens the written source archive to
compare every entry. Source copying and archive roundtrips preserve both names.
A collection plan cannot enable a path exception. Unknown names, duplicate
entries, links in place of these headers or changed original bytes still fail.
Ordinary source archives, Windows inputs and final package paths retain their
existing case-collision checks. The Linux source archive requires a filesystem
that can hold the case-distinct names.

This archive has no standalone license/copyright filename recognized by the
collector. Its separate locked license supplement retains original upstream
texts without adding anything to the source directory. All 1,443 archive file
and link blobs and modes match Sabotage tag `v4.19.88-2`, commit
`22bba0124dfe2728ef5b4555baa1ae0960492665`, except that the archive omits the
three top-level shell scripts. The retained original `create-dist.sh` explicitly
removes those scripts. Restoring their original blob identities reconstructs
the exact tag tree; no other archive addition, omission or change is accepted.

`build/package/linux_header_licenses.py` checks the fixed original Git objects
from Linux tag `v4.19.88`, commit `fb683b5e3f53a73e761952735736180939a313df`,
through its tree to `COPYING` and the ten license/exception texts referenced by
the actual header declarations. The original texts and provenance live under
`third_party/appimage/linux-headers-licenses` and are retained as **external**
materials. The component's original `licenses` list remains empty; its separate
`externalLicenses` record binds those materials and all per-file observations.
Every original regular file, its SPDX expression, original notice block byte
ranges, and the list of files without SPDX markers remain independently
inspectable. The 18 different expressions are not collapsed to a single GPL
label, and the 311 files without SPDX markers remain unclassified.

Only the missing retained-text gap for this exact component can be resolved by
these verified materials. The original archive and source-tree hashes remain
unchanged. An arbitrary materials entry, another source or platform, an unknown
profile, a plan-supplied replacement, a changed Git object, or a changed text is
rejected. Missing source/build receipts and other completion gaps are still
computed normally; retaining these texts is not a conclusion that every
file-specific licensing question or distribution review is resolved.

After the real archive is available, run the source-material roundtrip and
tamper checks explicitly, so this integration test cannot skip for a missing
input:

```sh
HUHUCAPTURE_TEST_LINUX_HEADER_ARCHIVE=.deps/appimage-runtime-source/downloads/appimage-linux-headers-linux-headers-4.19.88-2.tar.xz \
  python tests/appimage_runtime_tests.py -v
```

## Run on a real Linux amd64 host

The workspace must be new, separate from the packaging tools, and use a simple
path without whitespace or shell/Make metacharacters. This prevents accidental
reuse of a previous SDK or configuration. On Ubuntu 24.04 the build programs are:

```sh
sudo apt-get update
sudo apt-get install --no-install-recommends -y \
  build-essential cmake ninja-build meson autoconf automake libtool \
  pkg-config curl file texinfo bison flex gawk patch xz-utils zstd

python scripts/restore-packaging-tools.py --platform linux \
  --output .deps/runtime-packaging-tools
python scripts/build-appimage-runtime.py \
  --tools .deps/runtime-packaging-tools \
  --workspace .deps/appimage-runtime-source --jobs 2
```

Both target C and C++ compilers are built: the original zstd and mimalloc CMake
projects probe both languages even when building only their C static libraries.
The target compiler, linker, sysroot, include/library paths and pkg-config search
directories are explicit. Old Qt, compiler, loader, Make and pkg-config overrides
are cleared. Configure probes use `-static-pie`, so they do not depend on a
preinstalled musl dynamic loader. The canonical toolchain's `PWD` is explicitly
set to its actual build directory because its Makefile uses it for target tools.

The builder uses original sources in separate directories. Squashfuse's autogen
step runs in a verified owned copy because it generates files in the source
directory. All original input trees are compared again after building. A failed
command preserves its actual return code and logs, and no successful receipt is
created for that failed producer. A final report claims build completion only
after independent evidence validation succeeds.

## Actual compiler support and exception handling

GCC's target C++ libraries use the explicit configure assignment
`CXXFLAGS_FOR_TARGET=-g -O2 -fPIC`. This preserves GCC 9.4.0's default target
optimization/debug flags and adds position-independent code for the static
`libstdc++` objects used by the C++ static-PIE probe. The original top-level
`configure.ac` supports this assignment. In this version, libstdc++'s configure
clears its automatic PIC flags when shared libraries are disabled and later
resets libtool's `pic_mode`; merely adding `--with-pic` does not establish the
required static object flags. The producer quotes the complete assignment as
one shell argument in the actual retained `config.mak`.

The fixed original GCC/libstdc++ configure files and the generated target
library configuration records are retained. Neither the original source bytes
nor libgcc/musl's existing configuration is changed. This setting addresses the
target C++ probe's static library link requirement; it is not evidence that the
final C runtime has linked or run. The real target probe and final runtime
still have to pass their complete link, ELF, input and execution checks.

The locked GCC 9.4.0 archive's `libgcc/Makefile.in` places `LIB2ADDEH` objects
inside `libgcc.a` when shared target libraries are disabled. A separately named
`libgcc_eh.a` is therefore not a universal installation requirement. The
producer preserves the actual `-print-file-name` command and output for every
queried support file. Only the exact unchanged `libgcc_eh.a` not-found response
can mean that this separate archive is absent. Required CRT/libc/libgcc queries
and any resolved EH archive still have to match the newly installed producer
output, including its exact bytes and recorded installation owner.

The installed target C++ compiler then compiles the original
`build/package/toolchain-eh-probe.cpp`, links it as static PIE with a full GNU
link map and trace, and executes it. The probe throws across a separate stack
frame, checks the caught value, and requires that frame's destructor to have
run. Its actual `LOAD` inputs must all be the probe object or unchanged outputs
of this same new toolchain installation. Every archive is retained by identity
and complete member inventory, with member mentions from the actual link map.
Those observations preserve the original `bin/../lib/...` LOAD spellings while
binding them to canonical installed identities. They do not infer unwinder
placement from a library name or assert that every mentioned section survives.

`toolchain-source-receipt/probe/toolchain-probe.json` binds the original source,
object, executable, queries, map, input inventories and successful commands.
Independent runtime verification checks it again against the real toolchain
receipt. The original GCC Makefile and generated libgcc configuration are also
retained. Failure to compile, link, execute, or account for any input still
prevents a successful producer receipt. This compiler probe supplements the
separate complete final runtime link closure; it is not part of the product.

## What the evidence establishes

The successful workspace contains:

| Material | Meaning |
| --- | --- |
| `sources/`, `checkouts/`, `downloads/`, `source-inputs/` | Complete original inputs, source pins, per-file bytes and source-tree hashes |
| `toolchain-source-receipt/` | Actual canonical toolchain commands, configuration records, logs and installation phase inventories |
| `toolchain-source-receipt/probe/` | Actual target static-PIE exception execution, complete probe link inputs and compiler support queries |
| `appimage-*-source-receipt/` | Actual separate static library producer records and outputs |
| `runtime-source-receipt/` | Actual compile, link, strip, marker, native probe and output records |
| `runtime-link.map` | The actual GNU link map, also accompanied by recorded linker trace output |
| `runtime-link-closure.json` | Every `LOAD` object/archive, exact producer output, original source identity and every archive member's bytes |
| `install/runtime-x86_64` | Actual new runtime that passed ELF and native extraction/execution checks |
| `install/runtime-x86_64.debug` | Actual separate debug output |
| `install/LICENSE.runtime-sources` | Original notices for the runtime and all final static components |
| `probe/` | Actual compressed AppImage, original AppDir, extraction, command output and probe report |
| `build-info.json` | Computed build and static closure results; it always leaves `correspondingSourceComplete` false |
| `source-plan-fragment.json` | Fourteen original components and seven actual producer receipts for the normal source collector |

All linker `LOAD` files must be regular static archives or objects inside an
actual producer's recorded output. No host library is accepted by name. The
toolchain receipt assigns installed outputs to the real musl/GCC/binutils/header
installation commands and their before/after inventories. Thus every actually
selected CRT file, `libgcc.a`, separate `libgcc_eh.a`, or `libatomic.a` has source
ownership and hashes. Every actual archive member is hashed, including members
not retained by section garbage collection. Link-map member mentions are
recorded separately, without claiming every mentioned section survives.

The final ELF must be amd64 static PIE, have no interpreter or `DT_NEEDED`
dependency, and contain the type-2 marker. The runtime must report the fixed
original commit and the exact appended payload offset. The fixed original
appimagetool builds a real zstd-compressed test AppImage with that runtime.
Its complete extracted inventory must equal the actual AppDir, and running it
with `APPIMAGE_EXTRACT_AND_RUN=1` must execute AppRun and produce the recorded
sentinel. The verifier checks those exact successful commands and original stdout.

The pinned appimagetool always writes the first 16 bytes of `.digest_md5`.
The verifier finds this field through the actual ELF section table, permits
only that exact byte range to change, and checks every other runtime byte.
It records original and embedded SHA256 identities plus before/after field
bytes, bound to the actual pinned appimagetool command. MD5 is not used as a
security boundary. See the original
[`appimagetool.c`](https://github.com/AppImage/appimagetool/blob/8c8c91f762b412a19f4e8d2c4b35afb98f2d7c81/src/appimagetool.c)
for the field write.

The probe covers compressed extraction and AppRun execution. It does not prove
a FUSE desktop mount, screen capture, or the complete product's native behavior.

### Separate bounded diagnostics

The complete original source/build archive remains required. The standalone
workflow additionally uploads `appimage-runtime-diagnostics-<sha>` from:

```sh
python scripts/retain-appimage-runtime-diagnostics.py \
  --workspace .deps/appimage-runtime-source \
  --output artifacts/appimage-runtime-diagnostics
```

This independent snapshot keeps original control/producer records, configuration,
logs, the actual probe files, and selected complete compiler/libc/C++ archives
and CRT objects. It does not copy the entire source tree or large compiler
executables. `diagnostics.json` maps every original absolute path to its retained
relative path, size, SHA256 and mode. Original receipts and their executed paths
remain byte-for-byte unchanged. Missing workspaces are explicitly reported;
the index has no build-success field.

Files are limited to 64 MiB each, with at most 20,000 files and 256 MiB including
the index. Exceeding a bound, selected links, changed input bytes or a changed
copy fails retention. Unselected library aliases are not traversed. The index
is written only after the complete selected set and both sides of every copy
have been rechecked. These diagnostic materials help inspect a failed or
successful run when the complete archive exceeds a retrieval interface's size
limit; they never replace its full original source and build evidence.

## Select the actual result in packaging

For Linux preparation, add the completed runtime workspace to the existing
native packaging command:

```sh
python scripts/package-qt.py prepare \
  --installed artifacts/install/linux \
  --stage artifacts/packages/linux-prepared \
  --minimum-system 'Ubuntu 24.04 amd64' \
  --tools .deps/runtime-packaging-tools \
  --qmake /absolute/path/to/the/actual/qt/install/bin/qmake \
  --appimage-runtime-build .deps/appimage-runtime-source
```

The ordinary installed-build and clean-source checks still apply. The packager
first verifies the unchanged original tool pins, then independently validates
the actual new runtime's complete source/build evidence. It copies only the
verified output and combined notice, records the selection and actual embedding,
and repeats the checks before source assembly/finalization. A path to a bare
binary, an intended-command JSON document or a failed build report is insufficient.

Include `.deps/appimage-runtime-source/source-plan-fragment.json` among the
actual producer fragments passed to the existing source-plan assembly. Select
the producer's `appimage-runtime` source entry instead of a duplicate development
retention entry for the same ID. Do not rewrite receipts, construct an alternate
`complete` flag or remove missing-source gaps. The collector still independently
checks all full application sources, actual receipt inputs and final deployed
bytes before formal packaging can finish.

## Independent native CI and current evidence boundary

`.github/workflows/appimage-runtime-source.yml` runs only on a push to
`validation/appimage-runtime-source`. It has `contents: read`, a 360-minute
budget and no release, publishing or tag operation. It runs alongside ordinary
Qt validation without changing those workflows or their concurrency groups.

The complete application's Linux job also builds this runtime from the same
checkout after it restores the reviewed packaging tools. Package preparation
selects that actual completed workspace, and source-plan assembly includes its
original components and producer receipts. This job has a 360-minute budget
for the combined Qt, media, Rust and runtime toolchain builds. Its always-run
evidence step preserves successful or failed runtime inputs and records beside
the other native application artifacts. A failed runtime build prevents package
preparation; it does not fall back to the old binary or make a source-completeness
claim.

The workflow retains complete original inputs, Git source objects, produced
toolchain/libraries, receipts, original configuration/log files, link map and
native probe in a compressed evidence archive. Failure artifacts retain the
actual partial work and configure logs. The evidence paths are absolute where
they record original executions; preserve the same checkout/workspace location
when restoring them for final source assembly. Do not rewrite historic command
arguments merely to relocate a report.

Offline tests use real local compiler/archiver output to validate parsers,
static ELF checks, exact MD5-field boundaries, original patch identities,
source recognition, environment isolation and failure handling. Real GNU patch
fixtures also prove fuzz-two behavior, original command retention, backup-tree
checking and nonzero failure handling. The three complete original toolchain
archives have been replayed locally with GNU patch 2.8 and on the native Ubuntu
host with GNU patch 2.7.6; their complete inventories match. An independent
collector replay of binutils also matches the producer's entire inventory.
The original Linux header archive has been extracted, copied through a real
recorded installation command, collected with that receipt and archived through
an actual roundtrip with all case-distinct paths intact. These source-preparation
checks do not establish compilation of the toolchain and runtime. The offline tests do not
execute the full musl toolchain/runtime build. Native success requires the
dedicated CI's actual completed report and retained evidence. At implementation
time, local source download returned HTTP 403; the original toolchain archives
used for local replay came from the retained failed native build artifact. No
native runtime build success was inferred from source preparation or fixtures.
