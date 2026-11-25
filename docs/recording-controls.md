# Recording controls and captured pixels

The shared `RecordingWorkspace` owns the toolbar, outline, start/stop/output journey and actual stop-entry availability. Platform workers only check native surfaces against real readback. The controls contract does not request pixel masking, change the selected rectangle, or manufacture an exclusion capability.

## Surface ownership

`Core::CaptureControls` publishes an immutable native-surface list. The GUI publishes the toolbar and each visible outline after applying the platform adapter. Workers retain that exact snapshot over native readback and reject the frame if the snapshot changed. Native-surface destruction or a changed `WinId` closes admission and requests normal finalization; a replacement window is not silently trusted as the old one.

The surface values distinguish actual Windows exclusion, an unmapped control, and an outside control. The distinction describes a condition for the worker to verify; it is not evidence that a setter succeeded. Visible tooltips owned by this application are unmapped before starting. Recording buttons retain accessible names but do not create unguarded tooltip windows during capture.

## Windows

The existing `excludeFromCapture` adapter remains responsible for checking the supported OS build and applying `WDA_EXCLUDEFROMCAPTURE`. GIF/GDI and MP4/DXGI or the existing GDI fallback verify the real HWND, owning process, visibility and current affinity before and after reading pixels. A visible window without the required exclusion is an explicit capture failure. No old-Windows black rectangle is accepted as modern exclusion.

The change does not replace DXGI, GDI, WASAPI, Media Foundation or gifski. Frame rates, audio mixing and encoder selection retain their existing contracts. Windows compilation, actual affinity behavior, mixed DPI and compositor timing still require the Windows native probes and physical-desktop acceptance; local Linux compilation does not prove them.

## Native X11

The toolbar first tries the four outside work-area candidates adapted from the same repository's fixed PR #7. It uses each verified screen binding independently; it does not apply one DPR to a virtual desktop. Subsequent drag, move, translation and layout changes must keep the complete native toolbar within an allowed area. If the new size cannot fit, the widget unmaps before reporting the loss of placement.

The outline has a real nonempty bounding mask containing only its outside ring. The selected interior is absent from the native window shape, including with no compositing window manager. An empty mask is never used to mean an empty window because Qt interprets that value as removing the mask.

`X11Capture::captureGuarded` reuses the sole native readback implementation. On its private worker connection it grabs the X server, checks actual map state, root-relative geometry, frame extents and bounded SHAPE rectangles, then reads pixels. The grab is released immediately after the native image reply, before CPU row copying, encoding, queue waits or any GUI callback. RAII releases it on failure; cancellation still shuts down only this worker's own connection. `xcb-shape` is a direct build dependency. A visible X11 window cannot claim Windows-style exclusion.

This closes the native geometry-to-readback race on that X connection. It does not by itself certify compositor animation or a compositing window manager's previous texture. The actual Xvfb probe checks exact selected pixels with a visible outside control, a hidden control and a hollow SHAPE; it also checks move/resize overlap, unexpected remapping, a removed mask and a destroyed native ID. Composited X11, multiple physical displays and mixed DPI remain separate desktop acceptance cases.

## Hidden controls and stopping

If neither actual exclusion nor verified outside placement is available, starting with hidden controls requires a currently reachable external stop entry. The application host reports actual tray/registered-shortcut availability through `setExternalStopAvailable`. `canStop` and `stopActive` route that entry into the one recording session. Losing the final entry while recording requests normal Save finalization. A tooltip or context menu must not be created by this stop route.

Restoring hidden controls is delayed until the backend reports finalization, after its actual frame source stopped. Shutdown never treats a write/finalization error as a clean cancellation: the workspace reports the concrete failure and any durable archive or recovery path before the application finishes draining.

## Wayland refresh and remaining presentation gate

A Screenshot rectangle is not reused as a ScreenCast rectangle. The same authorized source is selected through its actual first-frame preview. That preview and its queue can contain selection or recording controls. Unmapping the controls and receiving `wl_display.sync` alone does **not** establish that a later PipeWire buffer contains a newly composed scene. Creating another PipeWire remote alone also does not establish that the producer has discarded an older frame.

The [shared live-source refresh](wayland-capture-refresh.md) now drains the old producer/preview/leases, retains the same Portal authorization and node identity, and validates unchanged format/crop/transform before announcing the replacement first frame. Both recording and scrolling callers wait for actual Qt unexposure, request this refresh and claim only after its terminal. Recording preparation remains externally stoppable before a recording worker or staging output exists; cancellation and shutdown await refresh drain. It does not create a second authorization owner.

The independent `live_capture_refresh_probe` first captures a conspicuous real control, hides it, and checks every pixel of the exact replacement first lease against the displayed background. It also verifies old native socket identities are gone before claim and no producer socket survives shutdown. It never substitutes a later frame to hide a dirty first one. GNOME/Mutter and Plasma/KWin require their own actual desktop evidence; the new Sway test has not run in this local workspace. Neither this implementation nor a new FD is described as a universal presentation guarantee.

## Reuse and evidence

Exact fixed paths and blob identifiers are in `third_party/recording-controls/upstream.json`. The four-candidate placement and native-geometry review reuse PR #7 commit `251bc0071278c6b711c840bf5188d16870bc7349`; the new continuous guard retains this branch's shared session and native capture owner. Project MIT authorship and the root license apply. No third-party compositor implementation was copied into this component.

Local verification with the restored Qt 6.12.1 SDK and GCC 14 compiled the shared workspace, toolbar, X11 backend and actual guard probe with `-Wall -Wextra -Werror -Wpedantic`. The real QWidget test passed outside placement, move/reflow constraints, hidden visibility, finalization restoration and existing option/output journeys on the offscreen Qt platform. The X11 guard executable was compiled but was not run in the local workspace because no native X server was available. The corresponding CI/native and physical outcomes must be recorded separately, without converting compilation into a capture pass.

The actual isolated Xvfb run at commit `1f516bf5799551af1776839f56a21a16e652a4c1` failed a pixel assertion. Its original common error did not identify which of the four accepted readbacks failed; it is not evidence that the hollow outline was specifically at fault. The probe now saves every actual readback and reports the stage, first mismatching coordinate and exact ARGB values. It also calls the public `QGuiApplication::sync()` before dispatching its independent capture worker: fixed Qt's XCB implementation drains Qt events and roundtrips Qt's own connection. A paint callback or geometry query on another connection did not establish that order. All native acceptance/refusal and every-pixel assertions remain unchanged. This ordering is used only for the owned test windows; it is not a Wayland or composited-X11 presentation guarantee. The changed probe compiles locally; a successful real rerun is still required.
