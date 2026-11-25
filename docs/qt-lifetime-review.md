# Qt owner and shutdown review

This review covers the shared application's lifetime boundaries at development
base `adcf6972575c9c5d91380b13417a446d0761a7aa`. It records concrete findings and
the verification actually performed; it is not a complete desktop acceptance.

## Owner ordering and ordinary shutdown

`QtApplication::State` declares the loaded settings, language catalog and Qt
translations before its workspaces and top-level windows. C++ reverse member
destruction therefore keeps these dependencies alive while the views destroy
their callbacks. The catalog callback retained by editors and settings windows
does not reproduce the test-fixture ordering error found during migration.

Ordinary application shutdown stops new admission before asking recording and
scroll consumers to finish. Only after those consumers report completion does it
stop capture and media output. It waits for media jobs and file-revealer requests,
then the system clipboard's real shutdown receipt. Existing error messages must
be acknowledged before exit. A Linux selection owner is retained visibly until
the user explicitly releases it or another owner replaces it; successful
publication alone is not evidence of clipboard persistence after process exit.

The inspected CaptureWorkspace, ScrollWorkspace, MediaWorkspace and editor
destructors keep their respective state alive while deleting owned top-level
views. Capture and media workers own the data they use; deleting a view cancels
its continuation rather than invalidating a worker's image or archive owner.
This static review did not establish new defects in those ownership paths.

## Recording controls during abnormal owner loss

RecordingWorkspace's control snapshot was declared after its outline windows,
so implicit State destruction would release that snapshot before destroying the
outlines. The windows' native-surface events could still invoke the workspace's
event filter: QObject removes filters in its base destructor, after the derived
class's State has been destroyed. An active recording's exceptional owner loss
could therefore access the already-destroyed control state.

The workspace now disables the control guard, removes its filters from the
owned widgets and native windows, and destroys the outlines and recording window
explicitly while the whole State still exists. No backend, encoder, source
ownership, or normal asynchronous shutdown contract is replaced.

The fixed Qt 6.12.1/GCC 14 source passed a direct C++20 syntax build with
`-Wall -Wextra -Wpedantic -Werror`. Native Windows/X11 event delivery during an
active recording's abnormal owner destruction has not been executed locally.

## Completed media errors racing shutdown

A media worker can finish before its QFutureWatcher completion is dispatched on
the GUI thread. Previously, beginning shutdown during that interval discarded
the returned result, including a genuine archive or export failure. Stopping
further UI stages must not turn an already-completed failure into silent success.

The completion now removes the job from the bounded queue, retains a real
non-cancellation error (including an already-failed removal result), and reports
it to the existing host error channel. A published or retained file path remains
in the report where available. It does not restart an output stage, reopen an
editor, ask for permanent deletion, or call an abandoned output completion. The
final busy notification is guarded against a host which synchronously destroys
the workspace while receiving that error.

`tests/qt/media_workspace_tests.cpp` uses a real regular file in place of an
archive parent directory. It waits for the actual QtConcurrent filesystem worker
without dispatching GUI events, then invokes shutdown before delivering the
result. The new assertion failed before the production change and passed after
it. The second case deletes the workspace inside the real error callback. Both
preserve the independent file's original bytes and produce no editor or output
completion. The fixture's language catalog was also moved before its workspace
so failed assertions cannot destroy translated-window dependencies first.

The complete existing `qt_media_workspace` executable and CTest entry passed
locally against the fixed SDK with Qt's offscreen platform. This validates real
widgets, workers, file operations and callback lifetimes. It does not validate
native clipboard ownership, desktop presentation, Windows shell behavior or
physical display interaction.

## Separate remaining shortcut drain finding

The reviewed GlobalShortcut/SettingsController shutdown path did not yet wait
for PortalShortcuts' already-running `list_shortcuts` refresh callback. Its
shared owner and session references protect the callback's objects, but issuing
cancellation alone does not prove that GLib completion and resource cleanup
finished before the event dispatcher stops. This is a separate follow-up; the
review does not mark that asynchronous drain branch as verified.
