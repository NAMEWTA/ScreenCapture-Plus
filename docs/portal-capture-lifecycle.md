# Portal screenshot ownership and shutdown

A screenshot requested from the tray or global shortcut may have no application
window. `PortalCapture::requestScreenshot(nullptr, ...)` passes an actual null
parent to libportal's existing screenshot API. It creates no hidden/fictional
parent window and performs no Wayland parent export. A supplied parent still
requires its real visible native surface and retains the existing asynchronous
foreign-handle and parent-destruction behavior. ScreenCast still requires its
real authorization parent.

`shutdown()` stops admission, cancels initialization and the pending request,
closes the owned active session/remote and unregisters the service/bus watches.
It does not equate cancellation with completion. Initial GTask completion,
interface probes, libportal request callbacks, parent preparation and terminal
notification delivery retain their actual ownership until they finish on Qt's
shared GLib dispatcher. Only then are the controller's portal/bus references
released and `shutdownFinished()` queued. Reentrant shutdown from a terminal
handler cannot report a drained controller while that handler is still active.
The application keeps its GUI dispatcher running through that signal.
Both request methods return zero as soon as shutdown closes admission; they
cannot append a rejected request behind an already queued shutdown completion.
The final dispatch also rechecks callback/delivery counts before reporting drain.

Immediate QObject destruction remains a cancellation-and-weak-owner fallback;
it does not prove that every asynchronous callback has already run. Production
application shutdown uses the explicit drain contract. The screenshot service
also waits for its independent native/PNG worker and destroys its capture DC or
XCB connection on that same worker before reporting its own shutdown.

When the real Portal name or session bus disappears, the controller publishes
updated capability values. Consumers no longer advertise screenshot admission
from stale initialized state. Original libportal failure details remain intact.

## Actual verification

The fixed local Qt 6.12.1/libportal 0.11.0 SDK compiled MOC, the actual parent
exporter/protocol, controller and updated fault tests with C++20 and
`-Wall -Wextra -Wpedantic -Werror`. Both `portal_fault_tests --no-bus` and
`portal_fault_tests --no-bus --shutdown-initializing` executed successfully: a
real GIO initialization error/cancellation completed before the shutdown signal.
`--shutdown-with-late-request` also ran successfully with actual Qt delivery:
requests both before and after the queued shutdown signal are refused with zero,
so they cannot append completion work behind that signal.
The attempted local independent session bus could not bind a socket in this
executor (`Operation not permitted`); its tests are not recorded as passed.

The existing CI `portal_missing_service` path now also waits for controller
shutdown before its independent libportal cancellation/reference-release cases.
The additional `--shutdown-initializing` mode can run under the same real
`dbus-run-session`. Native Screenshot acceptance is a separate desktop gate;
`portal_capture_probe --screenshot --unparented` exercises the real entry, and
`--noninteractive` is available only where the actual desktop permits it.
No successful authorization, returned image, compositor handle or bus response
is fabricated by these tests.
