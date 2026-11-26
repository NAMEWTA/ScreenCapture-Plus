# Original external Linux header license materials

These are separate original upstream materials for the fixed
`linux-headers-4.19.88-2.tar.xz` source archive. They were not present as
standalone license files in that archive and are never inserted into its
extracted source directory.

The source archive has SHA256
`dc7abf734487553644258a3822cfd429d74656749e309f2b25f09f4282e05588`.
Its 1,443 file/link Git blobs and modes match
[Sabotage tag v4.19.88-2](https://github.com/sabotage-linux/kernel-headers/tree/22bba0124dfe2728ef5b4555baa1ae0960492665).
The only omissions are `UPDATE.sh`, `create-dist.sh`, and `test.sh`; the
unchanged original distribution script removes the top-level `*.sh` files.
The three scripts here preserve those original bytes. They are evidence and
are not executed by the collector.

The `linux/` directory contains original `COPYING` and the license/exception
files needed by the archive's actual SPDX expressions, from
[Linux v4.19.88](https://github.com/gregkh/linux/tree/fb683b5e3f53a73e761952735736180939a313df).
`provenance.json` records the exact Git tag and commit bytes, the directory
tree objects connecting that commit to each text, and each original blob,
size and SHA256. Git object hashes are recomputed during collection. The
stored annotated tag includes the original signature; this offline checker
checks its exact object identity and does not claim to perform a new trust
or signing-key verification.

All original header contents and notices remain in the source archive. The
collector separately records each regular file's actual SPDX expressions and
notice comment byte ranges, with hashes and a complete list of files without
SPDX identifiers. Those files remain unclassified. The supplemental texts
must not be described as one blanket license for the component or as resolving
all file-specific licensing and distribution questions.

The fixed supplement can satisfy only the missing retained license-text
material check for this exact source. It supplies no native build receipt,
static input evidence, package source mapping, or release approval.
