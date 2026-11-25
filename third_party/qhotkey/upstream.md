# QHotkey X11 reference and adaptation

- Upstream: [Skycoder42/QHotkey](https://github.com/Skycoder42/QHotkey).
- Reviewed commit: `4e3a244d87f1f7e741e1395f2ffe825f3a8ada45`.
- Project version at that commit: 1.5.0. This records the source version, not
  an additional linked QHotkey runtime.
- Original author and license: Copyright (c) 2016, Felix Barz,
  BSD-3-Clause. The unmodified [LICENSE](LICENSE) is retained.

The original [QHotkey/qhotkey_x11.cpp](https://github.com/Skycoder42/QHotkey/blob/4e3a244d87f1f7e741e1395f2ffe825f3a8ada45/QHotkey/qhotkey_x11.cpp)
was read in full, including native key mapping, lock modifier variants,
registration, unregistration and X11 error handling. This is source-level
adaptation of the passive-grab/lock-variant/rollback approach, not a claim
that a README establishes correct native behavior.

| Original input | Git blob | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| `QHotkey/qhotkey_x11.cpp` | `d9d73f7556516725c5b4a734b5adb0d1e741ffb9` | 7497 | `79c05764f6657188a70d0bbee1b3572bca4e14d33d1646c529b5ca5dac0bfcab` |
| `LICENSE` | `55e0b00a3b75082f5c8ec241ca98de716a79dba8` | 1475 | `05e1dae17942f85c6b92dfb10f4fbded17ae38ede72f370fdb60e9f3ce666523` |

The adaptation lives in
[`src/platform/linux/GlobalShortcut.cpp`](../../src/platform/linux/GlobalShortcut.cpp).
It uses checked XCB requests and an owned XCB connection instead of Xlib's
process-global error handler. It reads actual Alt/Super and lock modifier maps,
supports each returned root within explicit bounds, retains the old grab set
until shared settings persistence succeeds, and checks cancellation and
registration cleanup. It does not import QHotkey's QObject API, thread
dispatcher, key capture widget or Windows backend. Those responsibilities
reuse this application's shared services and existing Windows behavior.

For Wayland, the existing libportal service remains authoritative; no X11
grab is advertised as a desktop-global Wayland shortcut. The shared behavior,
intentional no-repeat policy, transactional limits and actual validation
scope are documented in [settings-controller.md](../../docs/settings-controller.md).

Windows key names and registration derive from the already used MIT-licensed
Ling commit `2ae45005b2863b004b4432bfe85d6e63d7ee44ca`, particularly
[`src/Util.cpp`](https://github.com/xland/Ling/blob/2ae45005b2863b004b4432bfe85d6e63d7ee44ca/src/Util.cpp)
and [`src/App.cpp`](https://github.com/xland/Ling/blob/2ae45005b2863b004b4432bfe85d6e63d7ee44ca/src/App.cpp),
plus the inherited application's `Setting` and shortcut recorder. Existing
Ling notices remain in the repository's third-party distribution set.

GLib GKeyFile/GDesktopAppInfo and xcb-keysyms/xkbcommon are linked public system
APIs, not source copied into the repository. The Exec and launch checks were
verified against [GLib 2.80.0 gio/gdesktopappinfo.c](https://github.com/GNOME/glib/blob/2.80.0/gio/gdesktopappinfo.c).
These dependencies retain their own licenses when packaged; this BSD notice
does not replace GLib or Portal LGPL obligations.
