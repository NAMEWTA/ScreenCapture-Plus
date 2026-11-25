/*
 * XCB readback, MIT-SHM and premultiplied cursor blending adapted from
 * FFmpeg 9.0.2 libavdevice/xcbgrab.c, Copyright (C) 2014 Luca Barbato.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * See third_party/ffmpeg-xcbgrab/upstream.md for exact sources and changes.
 */
#include "platform/linux/X11Capture.h"
#include "platform/linux/X11SignalMask.h"
#include "core/PixelBuffer.h"
#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <xcb/shm.h>
#include <xcb/shape.h>
#include <xcb/xfixes.h>
#include <xcb/xtest.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>

namespace HuhuCapture::Platform::Linux {
namespace {
struct Free { void operator()(void* value) const noexcept { std::free(value); } };
template<class T> using Reply = std::unique_ptr<T, Free>;
template<class T> Reply<T> reply(T* value) { return Reply<T>(value); }
constexpr std::size_t maximumBytes = 512ull * 1024 * 1024;
bool validPosition(int x, int y)
{ return x >= std::numeric_limits<std::int16_t>::min() && x <= std::numeric_limits<std::int16_t>::max() &&
    y >= std::numeric_limits<std::int16_t>::min() && y <= std::numeric_limits<std::int16_t>::max(); }
}
struct X11Capture::State {
    // PR7's cancellation only shuts down this worker-owned socket. It never
    // closes/reuses the descriptor or touches the Qt/X11 connection.
    struct InterruptConnection {
        int descriptor;
        void operator()() const noexcept { static_cast<void>(::shutdown(descriptor, SHUT_RDWR)); }
    };
    std::stop_token stop;
    std::optional<std::stop_callback<InterruptConnection>> interrupt;
    xcb_connection_t* connection{};
    xcb_screen_t* screen{}; // owned by connection setup
    int screenNumber{};
    unsigned randrMajor{}, randrMinor{};
    bool wayland{}, shmAvailable{}, xfixesAvailable{}, xtestAvailable{}, serverGrabbed{};
    std::uint8_t bitsPerPixel{}, scanlinePad{}, byteOrder{};
    std::uint32_t red{}, green{}, blue{};
    xcb_shm_seg_t segment{};
    std::uint8_t* shared{};
    std::size_t sharedBytes{};
    std::vector<std::uint8_t> image;
    int width{}, height{};
    std::size_t stride{};
    std::string failure;
    xcb_window_t scrollTarget{};
    bool scrollPaused{};
    explicit State(std::stop_token token) : stop(token) {}
    bool fail(std::string text) { failure = stop.stop_requested() ? "X11 capture was cancelled" : std::move(text); return false; }
    bool connected()
    { return !stop.stop_requested() && connection && !xcb_connection_has_error(connection) ? true : fail("The X11 connection is unavailable"); }
    bool checked(xcb_void_cookie_t cookie, const char* detail)
    {
        const auto error = reply(xcb_request_check(connection, cookie));
        return error ? fail(std::string(detail) + " (X11 error " + std::to_string(error->error_code) + ")") : true;
    }
    void releaseGrab() noexcept
    {
        if (std::exchange(serverGrabbed, false) && connection) {
            xcb_ungrab_server(connection); xcb_flush(connection);
        }
    }
    void releaseShared(bool notifyServer = true) noexcept
    {
        if (segment && connection && notifyServer) xcb_shm_detach(connection, segment);
        segment = 0;
        if (shared) shmdt(std::exchange(shared, nullptr));
        sharedBytes = 0;
    }
    bool ensureShared(std::size_t bytes)
    {
        if (shared && sharedBytes == bytes) return true;
        releaseShared();
        const int id = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
        if (id < 0) return false;
        void* address = shmat(id, nullptr, 0);
        if (address == reinterpret_cast<void*>(-1)) { shmctl(id, IPC_RMID, nullptr); return false; }
        const auto native = xcb_generate_id(connection);
        const auto error = reply(xcb_request_check(connection, xcb_shm_attach_checked(connection, native, static_cast<std::uint32_t>(id), 0)));
        // The server has either attached or rejected before marking deletion.
        // Both this mapping and the server reference are released on reset.
        const int marked = shmctl(id, IPC_RMID, nullptr);
        if (error || marked < 0) {
            if (!error) xcb_shm_detach(connection, native);
            shmdt(address);
            if (marked < 0) shmctl(id, IPC_RMID, nullptr);
            return false;
        }
        segment = native; shared = static_cast<std::uint8_t*>(address); sharedBytes = bytes; return true;
    }
    xcb_atom_t atom(const char* name)
    {
        const auto value = reply(xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 1,
            static_cast<std::uint16_t>(std::strlen(name)), name), nullptr));
        return value ? value->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    }
    std::string atomName(xcb_atom_t atom)
    {
        const auto value = reply(xcb_get_atom_name_reply(connection, xcb_get_atom_name(connection, atom), nullptr));
        if (!value) return {};
        return {xcb_get_atom_name_name(value.get()), static_cast<std::size_t>(xcb_get_atom_name_name_length(value.get()))};
    }
    std::string defaultName() const
    {
        const char* display = std::getenv("DISPLAY");
        std::string name = display ? display : "";
        // Match Qt's actual default X-screen naming, not its logical geometry.
        const auto dot = name.find_last_of('.');
        if (dot != std::string::npos) name.resize(dot);
        return name + "." + std::to_string(screenNumber);
    }
    bool monitors(std::vector<X11Monitor>& result)
    {
        if (randrMajor > 1 || (randrMajor == 1 && randrMinor >= 5)) {
            const auto values = reply(xcb_randr_get_monitors_reply(connection, xcb_randr_get_monitors(connection, screen->root, 1), nullptr));
            if (!values) return fail("RandR did not return the active monitor configuration");
            for (auto it = xcb_randr_get_monitors_monitors_iterator(values.get()); it.rem; xcb_randr_monitor_info_next(&it)) {
                const auto& value = *it.data;
                if (!value.width || !value.height) continue;
                X11Monitor monitor;
                monitor.name = atomName(value.name); monitor.identity = value.name; monitor.latin1Name = true;
                if (monitor.name.empty()) monitor.name = defaultName();
                monitor.pixels = {value.x, value.y, value.width, value.height};
                monitor.rotation = XCB_RANDR_ROTATION_ROTATE_0;
                // As in Qt, rotation describes a single CRTC only when the
                // monitor is exactly that output; grouped monitors are already
                // composed in root coordinates and have no additional transform.
                if (value.nOutput == 1) {
                    const auto outputId = *xcb_randr_monitor_info_outputs(it.data);
                    const auto output = reply(xcb_randr_get_output_info_reply(connection,
                        xcb_randr_get_output_info(connection, outputId, values->timestamp), nullptr));
                    if (output && output->crtc) {
                        const auto crtc = reply(xcb_randr_get_crtc_info_reply(connection,
                            xcb_randr_get_crtc_info(connection, output->crtc, values->timestamp), nullptr));
                        if (crtc && monitor.pixels == X11Rectangle{crtc->x, crtc->y, crtc->width, crtc->height}) monitor.rotation = crtc->rotation;
                    }
                }
                result.push_back(std::move(monitor));
            }
        } else if (randrMajor > 1 || (randrMajor == 1 && randrMinor >= 2)) {
            const auto resources = reply(xcb_randr_get_screen_resources_reply(connection,
                xcb_randr_get_screen_resources(connection, screen->root), nullptr));
            if (!resources) return fail("RandR did not return active outputs");
            const auto* outputs = xcb_randr_get_screen_resources_outputs(resources.get());
            for (int index = 0; index < xcb_randr_get_screen_resources_outputs_length(resources.get()); ++index) {
                const auto output = reply(xcb_randr_get_output_info_reply(connection,
                    xcb_randr_get_output_info(connection, outputs[index], resources->config_timestamp), nullptr));
                if (!output || output->connection != XCB_RANDR_CONNECTION_CONNECTED || !output->crtc) continue;
                const auto crtc = reply(xcb_randr_get_crtc_info_reply(connection,
                    xcb_randr_get_crtc_info(connection, output->crtc, resources->config_timestamp), nullptr));
                if (!crtc || !crtc->width || !crtc->height || !crtc->mode) continue;
                X11Monitor monitor;
                monitor.name.assign(reinterpret_cast<const char*>(xcb_randr_get_output_info_name(output.get())),
                    static_cast<std::size_t>(xcb_randr_get_output_info_name_length(output.get())));
                monitor.identity = outputs[index]; monitor.pixels = {crtc->x, crtc->y, crtc->width, crtc->height}; monitor.rotation = crtc->rotation;
                result.push_back(std::move(monitor));
            }
        }
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
        return true;
    }
    bool windows(std::vector<X11Rectangle>& result)
    {
        std::vector<xcb_window_t> stack;
        const auto stacking = atom("_NET_CLIENT_LIST_STACKING");
        if (stacking) {
            const auto values = reply(xcb_get_property_reply(connection, xcb_get_property(connection, 0,
                screen->root, stacking, XCB_ATOM_WINDOW, 0, 65536), nullptr));
            if (values && values->type == XCB_ATOM_WINDOW && values->format == 32 && !values->bytes_after) {
                const auto* windows = static_cast<const xcb_window_t*>(xcb_get_property_value(values.get()));
                stack.assign(windows, windows + values->value_len);
            }
        }
        if (stack.empty()) {
            const auto tree = reply(xcb_query_tree_reply(connection, xcb_query_tree(connection, screen->root), nullptr));
            if (!tree) return fail("X11 did not return the visible window tree");
            const auto* windows = xcb_query_tree_children(tree.get());
            stack.assign(windows, windows + xcb_query_tree_children_length(tree.get()));
        }
        const auto frameExtents = atom("_NET_FRAME_EXTENTS");
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            const auto attributes = reply(xcb_get_window_attributes_reply(connection, xcb_get_window_attributes(connection, *it), nullptr));
            if (!attributes || attributes->map_state != XCB_MAP_STATE_VIEWABLE || attributes->_class == XCB_WINDOW_CLASS_INPUT_ONLY) continue;
            const auto geometry = reply(xcb_get_geometry_reply(connection, xcb_get_geometry(connection, *it), nullptr));
            const auto translated = reply(xcb_translate_coordinates_reply(connection,
                xcb_translate_coordinates(connection, *it, screen->root, 0, 0), nullptr));
            if (!geometry || !translated || !translated->same_screen || geometry->width <= 6 || geometry->height <= 6) continue;
            int left = translated->dst_x, top = translated->dst_y;
            int right = left + geometry->width, bottom = top + geometry->height;
            if (frameExtents) {
                const auto values = reply(xcb_get_property_reply(connection, xcb_get_property(connection, 0,
                    *it, frameExtents, XCB_ATOM_CARDINAL, 0, 4), nullptr));
                if (values && values->type == XCB_ATOM_CARDINAL && values->format == 32 && values->value_len == 4) {
                    const auto* extents = static_cast<const std::uint32_t*>(xcb_get_property_value(values.get()));
                    // A decoration cannot validly extend further than the X11
                    // root coordinate domain. Ignore unrepresentable metadata.
                    if (std::all_of(extents, extents + 4, [](auto value) { return value <= 65535; })) {
                        left -= static_cast<int>(extents[0]); right += static_cast<int>(extents[1]);
                        top -= static_cast<int>(extents[2]); bottom += static_cast<int>(extents[3]);
                    }
                }
            }
            result.push_back({left, top, right - left, bottom - top});
        }
        return true;
    }
    bool controlOutside(const Core::CaptureControlSurface& control, const X11Rectangle& selected)
    {
        // Native geometry/frame-extents checking reuses this repository's
        // PR #7 X11Capture::windowRectangle (251bc0071278c6b711c840bf5188d16870bc7349).
        // Shape validation additionally checks the real hollow outline, rather
        // than assuming a transparent ARGB window is invisible without a WM.
        if (!control.nativeId || control.nativeId > (std::numeric_limits<xcb_window_t>::max)() ||
            control.placement == Core::CaptureControlPlacement::Excluded)
            return fail("The X11 capture-control surface has no verifiable native placement");
        const auto window = static_cast<xcb_window_t>(control.nativeId);
        const auto attributes = reply(xcb_get_window_attributes_reply(connection,
            xcb_get_window_attributes(connection, window), nullptr));
        if (!attributes) return fail("The X11 capture-control surface was destroyed or replaced");
        if (attributes->map_state != XCB_MAP_STATE_VIEWABLE || attributes->_class == XCB_WINDOW_CLASS_INPUT_ONLY) return true;
        if (control.placement == Core::CaptureControlPlacement::Hidden)
            return fail("A hidden recording control became visible during capture");
        const auto geometryCookie = xcb_get_geometry(connection, window);
        const auto originCookie = xcb_translate_coordinates(connection, window, screen->root, 0, 0);
        const auto geometry = reply(xcb_get_geometry_reply(connection, geometryCookie, nullptr));
        const auto origin = reply(xcb_translate_coordinates_reply(connection, originCookie, nullptr));
        if (!geometry || !origin || !origin->same_screen || !geometry->width || !geometry->height)
            return fail("The actual X11 recording control bounds are unavailable");
        const auto intersects = [&](std::int64_t x, std::int64_t y, std::int64_t width, std::int64_t height) {
            return width > 0 && height > 0 && x < std::int64_t(selected.x) + selected.width &&
                y < std::int64_t(selected.y) + selected.height && x + width > selected.x && y + height > selected.y;
        };
        const auto frameExtents = atom("_NET_FRAME_EXTENTS");
        if (frameExtents) {
            const auto extents = reply(xcb_get_property_reply(connection,
                xcb_get_property(connection, 0, window, frameExtents, XCB_ATOM_CARDINAL, 0, 4), nullptr));
            if (!extents) return fail("The actual X11 control decorations are unavailable");
            if (extents->type != XCB_ATOM_NONE) {
                if (extents->type != XCB_ATOM_CARDINAL || extents->format != 32 || extents->value_len != 4 || extents->bytes_after)
                    return fail("The X11 control has invalid frame-extents metadata");
                const auto* values = static_cast<const std::uint32_t*>(xcb_get_property_value(extents.get()));
                if (!std::all_of(values, values + 4, [](auto value) { return value <= 65535; }))
                    return fail("The X11 control decorations exceed the native coordinate budget");
                if ((values[0] || values[1] || values[2] || values[3]) &&
                    intersects(std::int64_t(origin->dst_x) - values[0], std::int64_t(origin->dst_y) - values[2],
                        std::int64_t(geometry->width) + values[0] + values[1], std::int64_t(geometry->height) + values[2] + values[3]))
                    return fail("The actual recording-control decorations overlap the captured pixels");
            }
        }
        const auto* shape = xcb_get_extension_data(connection, &xcb_shape_id);
        if (!shape || !shape->present)
            return !intersects(origin->dst_x - geometry->border_width, origin->dst_y - geometry->border_width,
                       geometry->width + 2 * geometry->border_width, geometry->height + 2 * geometry->border_width) ||
                fail("The X11 server cannot verify the recording control's hollow shape");
        const auto bounds = reply(xcb_shape_get_rectangles_reply(connection,
            xcb_shape_get_rectangles(connection, window, XCB_SHAPE_SK_BOUNDING), nullptr));
        if (!bounds || bounds->rectangles_len > 256)
            return fail("The actual X11 recording-control shape is unavailable or exceeds its rectangle budget");
        for (auto it = xcb_shape_get_rectangles_rectangles_iterator(bounds.get()); it.rem; xcb_rectangle_next(&it)) {
            const auto& rectangle = *it.data;
            if (intersects(std::int64_t(origin->dst_x) + rectangle.x, std::int64_t(origin->dst_y) + rectangle.y,
                    rectangle.width, rectangle.height))
                return fail("The actual recording control moved or resized into the captured pixels");
        }
        return true;
    }
    void convert(std::span<const std::uint8_t> source, std::size_t sourceStride, int width, int height)
    {
        const auto layout = *Core::pixelLayout(width, height, 4);
        image.resize(layout.bytes);
        if (bitsPerPixel == 32 && byteOrder == XCB_IMAGE_ORDER_LSB_FIRST && red == 0xff0000 && green == 0xff00 && blue == 0xff) {
            for (int y = 0; y < height; ++y) std::memcpy(image.data() + static_cast<std::size_t>(y) * layout.stride,
                source.data() + static_cast<std::size_t>(y) * sourceStride, layout.stride);
            Core::makeOpaque(image);
        } else {
            const auto channel = [](std::uint32_t value, std::uint32_t mask) {
                const auto shift = std::countr_zero(mask);
                const auto maximum = mask >> shift;
                return static_cast<std::uint8_t>((static_cast<std::uint64_t>((value & mask) >> shift) * 255 + maximum / 2) / maximum);
            };
            const auto pixelBytes = static_cast<unsigned>(bitsPerPixel / 8);
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                const auto* input = source.data() + static_cast<std::size_t>(y) * sourceStride + static_cast<std::size_t>(x) * pixelBytes;
                std::uint32_t value{};
                for (unsigned byte = 0; byte < pixelBytes; ++byte) {
                    const auto shift = byteOrder == XCB_IMAGE_ORDER_LSB_FIRST ? byte * 8 : (pixelBytes - 1 - byte) * 8;
                    value |= static_cast<std::uint32_t>(input[byte]) << shift;
                }
                auto* output = image.data() + static_cast<std::size_t>(y) * layout.stride + static_cast<std::size_t>(x) * 4;
                output[0] = channel(value, blue); output[1] = channel(value, green); output[2] = channel(value, red); output[3] = 255;
            }
        }
        this->width = width; this->height = height; stride = layout.stride;
    }
    bool drawCursor(int x, int y)
    {
        if (!xfixesAvailable) return fail("XFixes does not provide a real cursor image");
        const auto cursor = reply(xcb_xfixes_get_cursor_image_reply(connection, xcb_xfixes_get_cursor_image(connection), nullptr));
        if (!cursor) return fail("Read the actual X11 cursor image");
        const auto* pixels = xcb_xfixes_get_cursor_image_cursor_image(cursor.get());
        const auto count = static_cast<std::size_t>(cursor->width) * cursor->height;
        const auto length = xcb_xfixes_get_cursor_image_cursor_image_length(cursor.get());
        if (count > maximumBytes / 4 || length < 0 || static_cast<std::size_t>(length) < count)
            return fail("The X11 cursor image was incomplete or exceeded the image budget");
        const int left = cursor->x - cursor->xhot, top = cursor->y - cursor->yhot;
        const int beginX = (std::max)(left, x), beginY = (std::max)(top, y);
        const int endX = (std::min)(left + cursor->width, x + width), endY = (std::min)(top + cursor->height, y + height);
        for (int row = beginY; row < endY; ++row) for (int column = beginX; column < endX; ++column) {
            const auto color = pixels[static_cast<std::size_t>(row - top) * cursor->width + static_cast<std::size_t>(column - left)];
            const auto alpha = color >> 24;
            if (!alpha) continue;
            auto* target = image.data() + static_cast<std::size_t>(row - y) * stride + static_cast<std::size_t>(column - x) * 4;
            for (unsigned channel = 0; channel < 3; ++channel) {
                const auto value = (color >> (8 * channel)) & 255;
                target[channel] = static_cast<std::uint8_t>((std::min)(255u, value + (target[channel] * (255 - alpha) + 127) / 255));
            }
        }
        return true;
    }
    bool wheel(int steps)
    {
        if (!xtestAvailable) return fail("The X11 server does not support real XTest scrolling");
        if (steps < -120 || steps > 120) return fail("The requested scroll exceeds one bounded input operation");
        const auto button = static_cast<std::uint8_t>(steps >= 0 ? 5 : 4);
        for (int index = 0; index < std::abs(steps); ++index) {
            if (stop.stop_requested()) return fail("X11 capture was cancelled");
            // Reuse PR7's paired submission: a stopped press acknowledgement
            // cannot prevent the release from reaching the owned connection.
            const auto press = xcb_test_fake_input_checked(connection, XCB_BUTTON_PRESS, button, XCB_CURRENT_TIME, screen->root, 0, 0, 0);
            const auto release = xcb_test_fake_input_checked(connection, XCB_BUTTON_RELEASE, button, XCB_CURRENT_TIME, screen->root, 0, 0, 0);
            const bool pressed = checked(press, "Send an actual X11 scroll press");
            const bool released = checked(release, "Send an actual X11 scroll release");
            if (!pressed || !released || !connected()) return false;
        }
        failure.clear(); return true;
    }
};
X11Capture::X11Capture(std::stop_token stop) : state_(std::make_unique<State>(stop)) {}
X11Capture::~X11Capture() { reset(); }
void X11Capture::reset() noexcept
{
    const X11SignalMask signals;
    state_->releaseGrab();
    state_->releaseShared(signals.valid());
    state_->interrupt.reset(); // waits for any shutdown callback before close
    if (state_->connection) xcb_disconnect(std::exchange(state_->connection, nullptr));
    state_->screen = nullptr; state_->image.clear(); state_->width = state_->height = 0; state_->stride = 0;
    state_->wayland = state_->shmAvailable = state_->xfixesAvailable = state_->xtestAvailable = false;
    state_->randrMajor = state_->randrMinor = 0;
    state_->scrollTarget = 0; state_->scrollPaused = false;
}
bool X11Capture::initialize()
{
    const X11SignalMask signals;
    if (!signals.valid()) return state_->fail("Cannot prepare the capture worker's native signal mask");
    reset(); state_->failure.clear();
    if (state_->stop.stop_requested()) return state_->fail("X11 capture was cancelled");
    // xcb_connect's authentication/setup precedes exposure of the socket and
    // cannot be interrupted through libxcb. Established I/O is cancellable.
    state_->connection = xcb_connect(nullptr, &state_->screenNumber);
    if (!state_->connected()) return false;
    const auto descriptor = xcb_get_file_descriptor(state_->connection);
    if (descriptor < 0) return state_->fail("The owned X11 connection has no socket");
    state_->interrupt.emplace(state_->stop, State::InterruptConnection{descriptor});
    if (!state_->connected()) return false;
    const auto* setup = xcb_get_setup(state_->connection);
    auto screen = xcb_setup_roots_iterator(setup);
    for (int index = 0; index < state_->screenNumber && screen.rem; ++index) xcb_screen_next(&screen);
    if (!screen.rem) return state_->fail("The selected X11 screen is unavailable");
    state_->screen = screen.data;
    const auto xwayland = reply(xcb_query_extension_reply(state_->connection,
        xcb_query_extension(state_->connection, 8, "XWAYLAND"), nullptr));
    state_->wayland = xwayland && xwayland->present;
    if (state_->wayland) return state_->fail("XWayland cannot provide the compositor's complete desktop; use Portal capture");
    const auto version = reply(xcb_randr_query_version_reply(state_->connection,
        xcb_randr_query_version(state_->connection, 1, 5), nullptr));
    if (version) { state_->randrMajor = version->major_version; state_->randrMinor = version->minor_version; }
    const auto shm = reply(xcb_shm_query_version_reply(state_->connection, xcb_shm_query_version(state_->connection), nullptr));
    state_->shmAvailable = !!shm;
    const auto fixes = reply(xcb_xfixes_query_version_reply(state_->connection,
        xcb_xfixes_query_version(state_->connection, XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION), nullptr));
    state_->xfixesAvailable = !!fixes;
    const auto test = reply(xcb_test_get_version_reply(state_->connection, xcb_test_get_version(state_->connection, 2, 2), nullptr));
    state_->xtestAvailable = !!test;
    const xcb_visualtype_t* visual{};
    for (auto depth = xcb_screen_allowed_depths_iterator(state_->screen); depth.rem; xcb_depth_next(&depth))
        for (auto candidate = xcb_depth_visuals_iterator(depth.data); candidate.rem; xcb_visualtype_next(&candidate))
            if (candidate.data->visual_id == state_->screen->root_visual) visual = candidate.data;
    if (!visual || visual->_class != XCB_VISUAL_CLASS_TRUE_COLOR || !visual->red_mask || !visual->green_mask || !visual->blue_mask)
        return state_->fail("The X11 root is not a supported TrueColor image");
    state_->red = visual->red_mask; state_->green = visual->green_mask; state_->blue = visual->blue_mask;
    state_->bitsPerPixel = state_->scanlinePad = 0; state_->byteOrder = setup->image_byte_order;
    for (auto format = xcb_setup_pixmap_formats_iterator(setup); format.rem; xcb_format_next(&format)) {
        if (format.data->depth != state_->screen->root_depth) continue;
        state_->bitsPerPixel = format.data->bits_per_pixel; state_->scanlinePad = format.data->scanline_pad; break;
    }
    if ((state_->bitsPerPixel != 16 && state_->bitsPerPixel != 24 && state_->bitsPerPixel != 32) ||
        (state_->scanlinePad != 8 && state_->scanlinePad != 16 && state_->scanlinePad != 32))
        return state_->fail("The X11 root pixel format is unsupported");
    return true;
}
bool X11Capture::isXWayland() const noexcept { return state_->wayland; }
bool X11Capture::supportsAutomaticScroll() const noexcept { return state_->xtestAvailable && !state_->wayland; }
bool X11Capture::desktop(X11Desktop& output, bool includeWindows)
{
    const X11SignalMask signals;
    if (!signals.valid()) return state_->fail("Cannot prepare the capture worker's native signal mask");
    if (!state_->connected() || !state_->screen) return false;
    X11Desktop result;
    const auto geometry = reply(xcb_get_geometry_reply(state_->connection, xcb_get_geometry(state_->connection, state_->screen->root), nullptr));
    if (!geometry || !geometry->width || !geometry->height) return state_->fail("Read the actual X11 root geometry");
    result.bounds = {0, 0, geometry->width, geometry->height};
    if (!state_->monitors(result.monitors)) return false;
    if (result.monitors.empty()) result.monitors.push_back({state_->defaultName(), state_->screen->root, true, result.bounds, XCB_RANDR_ROTATION_ROTATE_0});
    if (includeWindows && !state_->windows(result.windows)) return false;
    const auto pointer = reply(xcb_query_pointer_reply(state_->connection, xcb_query_pointer(state_->connection, state_->screen->root), nullptr));
    if (!pointer || !pointer->same_screen) return state_->fail("Read the cursor on the selected X11 root");
    result.cursorX = pointer->root_x; result.cursorY = pointer->root_y;
    output = std::move(result); state_->failure.clear(); return true;
}
bool X11Capture::capture(int x, int y, int width, int height, bool includeCursor)
{
    const X11SignalMask signals;
    if (!signals.valid()) return state_->fail("Cannot prepare the capture worker's native signal mask");
    if (!state_->connected() || !state_->screen) return false;
    const auto layout = Core::pixelLayout(width, height, 4);
    if (!layout || layout->bytes > maximumBytes || width > 65535 || height > 65535 || !validPosition(x, y))
        return state_->fail("The requested X11 image exceeds the pixel or native coordinate budget");
    const auto geometry = reply(xcb_get_geometry_reply(state_->connection, xcb_get_geometry(state_->connection, state_->screen->root), nullptr));
    if (!geometry || x < 0 || y < 0 || x + width > geometry->width || y + height > geometry->height)
        return state_->fail("The requested X11 image is outside the current root window");
    if (geometry->depth != state_->screen->root_depth) return state_->fail("The X11 root pixel depth changed");
    const auto sourceStride = ((static_cast<std::size_t>(width) * state_->bitsPerPixel + state_->scanlinePad - 1) / state_->scanlinePad) * (state_->scanlinePad / 8);
    const auto bytes = sourceStride * static_cast<std::size_t>(height);
    bool copied = false;
    if (state_->shmAvailable && state_->ensureShared(bytes)) {
        xcb_generic_error_t* nativeError{};
        const auto value = reply(xcb_shm_get_image_reply(state_->connection, xcb_shm_get_image(state_->connection,
            state_->screen->root, static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
            static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height), ~0u, XCB_IMAGE_FORMAT_Z_PIXMAP, state_->segment, 0), &nativeError));
        const auto error = reply(nativeError);
        if (value && !error && value->size == bytes && value->depth == state_->screen->root_depth) {
            state_->releaseGrab(); // the real server readback is complete
            state_->convert({state_->shared, bytes}, sourceStride, width, height); copied = true;
        } else { state_->releaseShared(); state_->shmAvailable = false; }
    }
    if (!copied) {
        xcb_generic_error_t* nativeError{};
        const auto value = reply(xcb_get_image_reply(state_->connection, xcb_get_image(state_->connection,
            XCB_IMAGE_FORMAT_Z_PIXMAP, state_->screen->root, static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
            static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height), ~0u), &nativeError));
        const auto error = reply(nativeError);
        if (!value || error) return state_->fail("Read the complete X11 root image");
        const auto length = xcb_get_image_data_length(value.get());
        if (length < 0 || static_cast<std::size_t>(length) < bytes || value->depth != state_->screen->root_depth)
            return state_->fail("The X11 image reply has an incomplete pixel layout");
        state_->releaseGrab(); // never keep a server grab across CPU conversion
        state_->convert({xcb_get_image_data(value.get()), bytes}, sourceStride, width, height);
    }
    if (includeCursor && !state_->drawCursor(x, y)) return false;
    state_->failure.clear(); return true;
}
bool X11Capture::captureGuarded(int x, int y, int width, int height, bool includeCursor,
    std::span<const Core::CaptureControlSurface> controls)
{
    const X11SignalMask signals;
    if (!signals.valid()) return state_->fail("Cannot prepare the capture worker's native signal mask");
    if (!state_->connected() || !state_->screen || controls.empty() || controls.size() > 64)
        return state_->fail("The recording has no bounded native control-surface guard");
    // Geometry validation and root readback must not race another client's
    // MoveWindow, resize, map or shape requests. Release on every path, before
    // copying into a frame lease or calling an encoder. Cancellation shuts down
    // only this worker-owned connection, which also releases any server grab.
    if (!state_->checked(xcb_grab_server_checked(state_->connection), "Protect native control geometry during readback")) return false;
    state_->serverGrabbed = true;
    struct Ungrab {
        State* state;
        ~Ungrab() { state->releaseGrab(); }
    } ungrab{state_.get()};
    const X11Rectangle selected{x, y, width, height};
    for (const auto& control : controls) if (!state_->controlOutside(control, selected)) return false;
    return capture(x, y, width, height, includeCursor);
}
bool X11Capture::moveCursor(int x, int y, int& actualX, int& actualY)
{
    const X11SignalMask signals;
    if (!signals.valid()) return state_->fail("Cannot prepare the capture worker's native signal mask");
    if (!state_->connected() || !state_->screen) return false;
    if (!validPosition(x, y)) return state_->fail("The cursor target exceeds X11 coordinates");
    if (!state_->checked(xcb_warp_pointer_checked(state_->connection, XCB_NONE, state_->screen->root,
            0, 0, 0, 0, static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)), "Move the actual X11 cursor")) return false;
    const auto value = reply(xcb_query_pointer_reply(state_->connection, xcb_query_pointer(state_->connection, state_->screen->root), nullptr));
    if (!value || !value->same_screen) return state_->fail("Read the moved X11 cursor");
    actualX = value->root_x; actualY = value->root_y; state_->failure.clear(); return true;
}
bool X11Capture::scroll(int x, int y, int steps)
{
    const X11SignalMask signals;
    if (!signals.valid()) return state_->fail("Cannot prepare the capture worker's native signal mask");
    if (!state_->connected() || !state_->screen) return false;
    int actualX{}, actualY{};
    if (!moveCursor(x, y, actualX, actualY)) return false;
    if (actualX != x || actualY != y) return state_->fail("The X11 cursor did not reach the scroll target");
    return state_->wheel(steps);
}
bool X11Capture::scrollAtPointer(const X11Rectangle& selection, int steps)
{
    const X11SignalMask signals;
    if (!signals.valid()) return state_->fail("Cannot prepare the capture worker's native signal mask");
    state_->scrollPaused = false;
    if (!state_->connected() || !state_->screen) return false;
    const auto pointer = reply(xcb_query_pointer_reply(state_->connection, xcb_query_pointer(state_->connection, state_->screen->root), nullptr));
    if (!pointer || !pointer->same_screen) return state_->fail("The current X11 scrolling target is unavailable");
    const auto target = pointer->child ? pointer->child : state_->screen->root;
    if (!state_->scrollTarget) {
        if (pointer->root_x < selection.x || pointer->root_y < selection.y ||
            std::int64_t(pointer->root_x) >= std::int64_t(selection.x) + selection.width ||
            std::int64_t(pointer->root_y) >= std::int64_t(selection.y) + selection.height) {
            state_->scrollPaused = true; return state_->fail("Move the pointer into the selected scrolling region to start");
        }
        state_->scrollTarget = target;
    }
    if (target != state_->scrollTarget) {
        state_->scrollPaused = true; return state_->fail("Move the pointer back over the scrolling target to continue");
    }
    return state_->wheel(steps);
}
bool X11Capture::scrollPaused() const noexcept { return state_->scrollPaused; }
std::span<const std::uint8_t> X11Capture::pixels() const noexcept { return state_->image; }
int X11Capture::width() const noexcept { return state_->width; }
int X11Capture::height() const noexcept { return state_->height; }
std::size_t X11Capture::stride() const noexcept { return state_->stride; }
const std::string& X11Capture::error() const noexcept { return state_->failure; }
} // namespace HuhuCapture::Platform::Linux
