#include "platform/linux/X11Capture.h"

#include "core/PixelBuffer.h"
#include <xcb/randr.h>
#include <xcb/shm.h>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>
#include <xcb/xtest.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>
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
constexpr std::size_t maximumBytes = 512u * 1024u * 1024u;
class NativeSignalMask {
public:
    NativeSignalMask() noexcept
    {
        sigemptyset(&pipe_); sigaddset(&pipe_, SIGPIPE);
        if (pthread_sigmask(SIG_BLOCK, &pipe_, &previous_) != 0) return;
        valid_ = true;
        // A caller that already blocks SIGPIPE owns its pending-signal policy.
        // Nested capture operations likewise leave the outer guard in charge.
        changed_ = sigismember(&previous_, SIGPIPE) == 0;
        if (!changed_) return;
        sigset_t pending;
        if (sigpending(&pending) != 0) {
            pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
            changed_ = valid_ = false; return;
        }
        pendingBefore_ = sigismember(&pending, SIGPIPE) != 0;
    }
    ~NativeSignalMask()
    {
        if (!changed_) return;
        const int previousError = errno;
        if (!pendingBefore_) {
            // libxcb may use writev without MSG_NOSIGNAL. Consume a SIGPIPE
            // generated during this operation before restoring this worker's
            // mask; never replace the process-wide signal disposition.
            const timespec immediate{};
            while (sigtimedwait(&pipe_, nullptr, &immediate) < 0 && errno == EINTR) {}
        }
        pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
        errno = previousError;
    }
    bool valid() const noexcept { return valid_; }
    NativeSignalMask(const NativeSignalMask&) = delete;
    NativeSignalMask& operator=(const NativeSignalMask&) = delete;
private:
    sigset_t pipe_{}, previous_{};
    bool valid_{}, changed_{}, pendingBefore_{};
};
template<class T> using Reply = std::unique_ptr<T, decltype(&std::free)>;
template<class T> Reply<T> own(T* pointer) { return {pointer, &std::free}; }
bool contains(const X11Rectangle& outer, int x, int y, int width, int height)
{
    return width > 0 && height > 0 && x >= outer.x && y >= outer.y &&
        static_cast<std::int64_t>(x) + width <= static_cast<std::int64_t>(outer.x) + outer.width &&
        static_cast<std::int64_t>(y) + height <= static_cast<std::int64_t>(outer.y) + outer.height;
}
std::uint8_t channel(std::uint32_t pixel, std::uint32_t mask)
{
    if (!mask) return 0;
    const auto shift = std::countr_zero(mask);
    const auto maximum = mask >> shift;
    return static_cast<std::uint8_t>(((static_cast<std::uint64_t>((pixel & mask) >> shift) * 255u) + maximum / 2u) / maximum);
}
}

struct X11Capture::State {
    struct InterruptConnection {
        int descriptor;
        void operator()() const noexcept
        {
            // The worker keeps the XCB-owned descriptor open until this
            // callback is deregistered. Shutdown wakes its blocked I/O without
            // an XCB lock, closing/reusing an FD, or touching Qt's connection.
            static_cast<void>(::shutdown(descriptor, SHUT_RDWR));
        }
    };
    std::stop_token stop;
    std::optional<std::stop_callback<InterruptConnection>> interrupt;
    xcb_connection_t* connection{};
    xcb_screen_t screen{};
    X11Rectangle bounds;
    int bytesPerPixel{}, scanlinePad{};
    std::uint32_t red{}, green{}, blue{};
    bool initialized{}, littleEndian{}, directBgra{}, shmAvailable{}, xfixesAvailable{};
    xcb_shm_seg_t segment{};
    int segmentId{-1};
    std::uint8_t* shared{};
    std::size_t sharedBytes{};
    int width{}, height{};
    std::size_t stride{};
    std::vector<std::uint8_t> pixels;
    std::string error;
    xcb_window_t scrollTarget{};
    bool scrollPaused{};

    explicit State(std::stop_token token) : stop(token) {}
    bool stopped()
    {
        if (!stop.stop_requested()) return false;
        error = "X11 capture was cancelled"; return true;
    }
    void releaseShared(bool notifyServer = true) noexcept
    {
        if (notifyServer && segment && connection) xcb_shm_detach(connection, segment);
        if (shared) shmdt(shared);
        if (segmentId != -1) shmctl(segmentId, IPC_RMID, nullptr);
        segment = 0; segmentId = -1; shared = nullptr; sharedBytes = 0;
    }
    bool fail(std::string message)
    { if (!stopped()) error = std::move(message); return false; }
    xcb_atom_t atom(const char* name)
    {
        const auto reply = own(xcb_intern_atom_reply(connection,
            xcb_intern_atom(connection, true, static_cast<std::uint16_t>(std::strlen(name)), name), nullptr));
        return reply ? reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    }
    std::string atomName(xcb_atom_t atom)
    {
        const auto reply = own(xcb_get_atom_name_reply(connection, xcb_get_atom_name(connection, atom), nullptr));
        if (!reply) return {};
        return {xcb_get_atom_name_name(reply.get()), static_cast<std::size_t>(xcb_get_atom_name_name_length(reply.get()))};
    }
    std::vector<std::uint32_t> property(xcb_window_t window, xcb_atom_t name, xcb_atom_t type, std::uint32_t maximum = 65536)
    {
        if (!name) return {};
        const auto reply = own(xcb_get_property_reply(connection,
            xcb_get_property(connection, false, window, name, type, 0, maximum), nullptr));
        if (!reply || reply->format != 32 || reply->type != type || reply->bytes_after != 0) return {};
        const auto length = xcb_get_property_value_length(reply.get());
        if (length <= 0 || length % 4 != 0) return {};
        const auto* begin = static_cast<const std::uint32_t*>(xcb_get_property_value(reply.get()));
        return {begin, begin + length / 4};
    }
    bool refreshBounds()
    {
        const auto reply = own(xcb_get_geometry_reply(connection, xcb_get_geometry(connection, screen.root), nullptr));
        if (!reply) return fail("X11 root geometry is unavailable");
        bounds = {0, 0, reply->width, reply->height};
        return true;
    }
    bool ensureShared(std::size_t bytes)
    {
        if (!shmAvailable) return false;
        if (shared && sharedBytes >= bytes) return true;
        releaseShared();
        segmentId = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
        if (segmentId == -1) { shmAvailable = false; return false; }
        auto* memory = shmat(segmentId, nullptr, 0);
        if (memory == reinterpret_cast<void*>(-1)) {
            shmctl(segmentId, IPC_RMID, nullptr); segmentId = -1; shmAvailable = false; return false;
        }
        shared = static_cast<std::uint8_t*>(memory); sharedBytes = bytes;
        segment = xcb_generate_id(connection);
        const auto failure = own(xcb_request_check(connection, xcb_shm_attach_checked(connection, segment, segmentId, false)));
        if (failure || xcb_connection_has_error(connection) || stopped()) {
            segment = 0; releaseShared(); shmAvailable = false; return false;
        }
        // Linux keeps the segment alive while this process and X server have it
        // attached, while IPC_RMID prevents an abandoned segment after a crash.
        shmctl(segmentId, IPC_RMID, nullptr); segmentId = -1;
        return true;
    }
};

X11Capture::X11Capture(std::stop_token stop) : state_(std::make_unique<State>(stop)) {}
X11Capture::~X11Capture() { reset(); }
void X11Capture::reset() noexcept
{
    const NativeSignalMask signals;
    state_->releaseShared(signals.valid());
    // Keep cancellation active through any queued SHM detach. Destruction
    // waits for an in-flight callback before xcb_disconnect can close the FD.
    state_->interrupt.reset();
    if (state_->connection) xcb_disconnect(state_->connection);
    state_->connection = nullptr; state_->initialized = false; state_->pixels.clear();
    state_->width = state_->height = 0; state_->stride = 0;
    state_->scrollTarget = 0; state_->scrollPaused = false;
}
bool X11Capture::initialize()
{
    const NativeSignalMask signals;
    if (!signals.valid()) return state_->fail("Cannot prepare the capture worker's native signal mask");
    if (state_->stopped()) return false;
    if (state_->initialized && state_->connection && !xcb_connection_has_error(state_->connection)) return true;
    reset();
    if (state_->stopped()) return false;
    int number{};
    // libxcb performs its initial authentication/setup handshake here, before
    // exposing a socket. This narrow adapter does not reimplement X11 auth.
    state_->connection = xcb_connect(nullptr, &number);
    if (!state_->connection || xcb_connection_has_error(state_->connection))
        return state_->fail("Cannot connect to the current X11 display");
    const int descriptor = xcb_get_file_descriptor(state_->connection);
    if (descriptor < 0) return state_->fail("The owned X11 connection has no socket");
    state_->interrupt.emplace(state_->stop, State::InterruptConnection{descriptor});
    if (state_->stopped()) return false;
    const auto* setup = xcb_get_setup(state_->connection);
    auto screens = xcb_setup_roots_iterator(setup);
    for (int index = 0; screens.rem && index < number; ++index) xcb_screen_next(&screens);
    if (!screens.rem) return state_->fail("The current X11 screen is unavailable");
    state_->screen = *screens.data;
    state_->littleEndian = setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST;
    bool formatFound{};
    for (auto formats = xcb_setup_pixmap_formats_iterator(setup); formats.rem; xcb_format_next(&formats)) {
        if (formats.data->depth != state_->screen.root_depth) continue;
        if (formats.data->bits_per_pixel != 16 && formats.data->bits_per_pixel != 24 && formats.data->bits_per_pixel != 32) continue;
        state_->bytesPerPixel = formats.data->bits_per_pixel / 8;
        state_->scanlinePad = formats.data->scanline_pad;
        formatFound = state_->scanlinePad == 8 || state_->scanlinePad == 16 || state_->scanlinePad == 32;
        break;
    }
    bool visualFound{};
    for (auto depths = xcb_screen_allowed_depths_iterator(screens.data); depths.rem; xcb_depth_next(&depths)) {
        for (auto visuals = xcb_depth_visuals_iterator(depths.data); visuals.rem; xcb_visualtype_next(&visuals)) {
            const auto& visual = *visuals.data;
            if (visual.visual_id != state_->screen.root_visual || visual._class != XCB_VISUAL_CLASS_TRUE_COLOR) continue;
            state_->red = visual.red_mask; state_->green = visual.green_mask; state_->blue = visual.blue_mask;
            visualFound = true;
        }
    }
    // A 30-bit HDR visual cannot silently become an unlabeled SDR screenshot.
    if (!formatFound || !visualFound || state_->screen.root_depth > 24 ||
        std::popcount(state_->red) > 8 || std::popcount(state_->green) > 8 || std::popcount(state_->blue) > 8)
        return state_->fail("This X11 visual is not a supported 8-bit SDR TrueColor source");
    state_->directBgra = state_->bytesPerPixel == 4 && state_->littleEndian &&
        state_->red == 0xff0000 && state_->green == 0xff00 && state_->blue == 0xff;
    const auto* shm = xcb_get_extension_data(state_->connection, &xcb_shm_id);
    state_->shmAvailable = shm && shm->present;
    const auto* xfixes = xcb_get_extension_data(state_->connection, &xcb_xfixes_id);
    state_->xfixesAvailable = xfixes && xfixes->present;
    if (state_->xfixesAvailable) {
        const auto version = own(xcb_xfixes_query_version_reply(state_->connection,
            xcb_xfixes_query_version(state_->connection, 4, 0), nullptr));
        state_->xfixesAvailable = static_cast<bool>(version);
    }
    if (state_->stopped()) return false;
    state_->error.clear();
    state_->initialized = state_->refreshBounds();
    return state_->initialized;
}

bool X11Capture::desktop(X11Desktop& result)
{
    const NativeSignalMask signals;
    if (!signals.valid()) return state_->fail("Cannot prepare the capture worker's native signal mask");
    if (!initialize() || !state_->refreshBounds()) return false;
    X11Desktop desktop; desktop.bounds = state_->bounds;
    const auto* extension = xcb_get_extension_data(state_->connection, &xcb_randr_id);
    if (!extension || !extension->present) return state_->fail("RandR monitor coordinates are unavailable");
    const auto version = own(xcb_randr_query_version_reply(state_->connection,
        xcb_randr_query_version(state_->connection, 1, 5), nullptr));
    if (!version || version->major_version < 1 || (version->major_version == 1 && version->minor_version < 3))
        return state_->fail("RandR 1.3 or newer is required for verified monitor coordinates");
    const auto resources = own(xcb_randr_get_screen_resources_current_reply(state_->connection,
        xcb_randr_get_screen_resources_current(state_->connection, state_->screen.root), nullptr));
    if (!resources) return state_->fail("X11 monitor resources could not be read");
    const auto* outputs = xcb_randr_get_screen_resources_current_outputs(resources.get());
    const auto count = xcb_randr_get_screen_resources_current_outputs_length(resources.get());
    for (int i = 0; i < count; ++i) {
        const auto output = own(xcb_randr_get_output_info_reply(state_->connection,
            xcb_randr_get_output_info(state_->connection, outputs[i], resources->config_timestamp), nullptr));
        if (!output || !output->crtc || output->connection != XCB_RANDR_CONNECTION_CONNECTED) continue;
        const auto crtc = own(xcb_randr_get_crtc_info_reply(state_->connection,
            xcb_randr_get_crtc_info(state_->connection, output->crtc, resources->config_timestamp), nullptr));
        if (!crtc || !crtc->width || !crtc->height) continue;
        X11Monitor monitor;
        monitor.name.assign(reinterpret_cast<const char*>(xcb_randr_get_output_info_name(output.get())),
            static_cast<std::size_t>(xcb_randr_get_output_info_name_length(output.get())));
        monitor.pixels = {crtc->x, crtc->y, crtc->width, crtc->height}; monitor.rotation = crtc->rotation;
        if (!contains(desktop.bounds, monitor.pixels.x, monitor.pixels.y, monitor.pixels.width, monitor.pixels.height))
            return state_->fail("X11 monitor layout changed while being read");
        desktop.monitors.push_back(std::move(monitor));
    }
    if (desktop.monitors.empty()) return state_->fail("No active RandR monitor can be matched to the Qt display");
    const auto pointer = own(xcb_query_pointer_reply(state_->connection,
        xcb_query_pointer(state_->connection, state_->screen.root), nullptr));
    if (pointer) { desktop.cursorX = pointer->root_x; desktop.cursorY = pointer->root_y; }

    auto windows = state_->property(state_->screen.root, state_->atom("_NET_CLIENT_LIST_STACKING"), XCB_ATOM_WINDOW);
    if (windows.empty()) {
        const auto tree = own(xcb_query_tree_reply(state_->connection,
            xcb_query_tree(state_->connection, state_->screen.root), nullptr));
        if (tree) {
            const auto* begin = xcb_query_tree_children(tree.get());
            windows.assign(begin, begin + xcb_query_tree_children_length(tree.get()));
        }
    }
    const auto frameAtom = state_->atom("_NET_FRAME_EXTENTS");
    const auto stateAtom = state_->atom("_NET_WM_STATE");
    const auto hiddenAtom = state_->atom("_NET_WM_STATE_HIDDEN");
    for (auto i = windows.rbegin(); i != windows.rend(); ++i) {
        if (state_->stopped()) return false;
        const auto attributes = own(xcb_get_window_attributes_reply(state_->connection,
            xcb_get_window_attributes(state_->connection, *i), nullptr));
        if (!attributes || attributes->map_state != XCB_MAP_STATE_VIEWABLE) continue;
        const auto states = state_->property(*i, stateAtom, XCB_ATOM_ATOM, 64);
        if (hiddenAtom && std::find(states.begin(), states.end(), hiddenAtom) != states.end()) continue;
        const auto geometry = own(xcb_get_geometry_reply(state_->connection, xcb_get_geometry(state_->connection, *i), nullptr));
        const auto origin = own(xcb_translate_coordinates_reply(state_->connection,
            xcb_translate_coordinates(state_->connection, *i, state_->screen.root, 0, 0), nullptr));
        if (!geometry || !origin || geometry->width <= 6 || geometry->height <= 6) continue;
        auto frame = state_->property(*i, frameAtom, XCB_ATOM_CARDINAL, 4);
        X11Rectangle rect{origin->dst_x, origin->dst_y, geometry->width, geometry->height};
        if (frame.size() == 4 && std::all_of(frame.begin(), frame.end(), [](auto value) { return value <= 32767; })) {
            rect.x -= static_cast<int>(frame[0]); rect.y -= static_cast<int>(frame[2]);
            rect.width += static_cast<int>(frame[0] + frame[1]); rect.height += static_cast<int>(frame[2] + frame[3]);
        }
        desktop.windows.push_back(rect);
    }
    if (state_->stopped()) return false;
    result = std::move(desktop); state_->error.clear(); return true;
}

bool X11Capture::windowRectangle(std::uintptr_t windowId, X11Rectangle& rectangle)
{
    const NativeSignalMask signals;
    if (!signals.valid()) return state_->fail("Cannot prepare the capture worker's native signal mask");
    if (!initialize()) return false;
    if (!windowId || windowId > (std::numeric_limits<xcb_window_t>::max)())
        return state_->fail("The X11 control window identifier is invalid");
    const auto window = static_cast<xcb_window_t>(windowId);
    const auto attributes = own(xcb_get_window_attributes_reply(state_->connection,
        xcb_get_window_attributes(state_->connection, window), nullptr));
    if (!attributes || attributes->map_state != XCB_MAP_STATE_VIEWABLE)
        return state_->fail("The X11 control window is no longer visible");
    const auto geometry = own(xcb_get_geometry_reply(state_->connection,
        xcb_get_geometry(state_->connection, window), nullptr));
    const auto origin = own(xcb_translate_coordinates_reply(state_->connection,
        xcb_translate_coordinates(state_->connection, window, state_->screen.root, 0, 0), nullptr));
    if (!geometry || !origin || !origin->same_screen || !geometry->width || !geometry->height)
        return state_->fail("The X11 control window's physical bounds are unavailable");
    X11Rectangle result{origin->dst_x, origin->dst_y, geometry->width, geometry->height};
    const auto frame = state_->property(window, state_->atom("_NET_FRAME_EXTENTS"), XCB_ATOM_CARDINAL, 4);
    if (frame.size() == 4 && std::all_of(frame.begin(), frame.end(), [](auto value) { return value <= 32767; })) {
        result.x -= static_cast<int>(frame[0]); result.y -= static_cast<int>(frame[2]);
        result.width += static_cast<int>(frame[0] + frame[1]); result.height += static_cast<int>(frame[2] + frame[3]);
    }
    if (state_->stopped()) return false;
    rectangle = result; state_->error.clear(); return true;
}

bool X11Capture::capture(int x, int y, int width, int height, bool includeCursor)
{
    const NativeSignalMask signals;
    if (!signals.valid()) return state_->fail("Cannot prepare the capture worker's native signal mask");
    if (!initialize()) return false;
    // Geometry can change between recordings. The geometry query also reports a
    // disconnected display before handing old pixels to an encoder.
    if (!state_->refreshBounds()) return false;
    if (!contains(state_->bounds, x, y, width, height) || x > 32767 || y > 32767 || width > 65535 || height > 65535)
        return state_->fail("The capture rectangle is outside the current X11 source");
    const auto layout = Core::pixelLayout(width, height, 4);
    if (!layout || layout->bytes > maximumBytes) return state_->fail("The capture exceeds the bounded pixel budget");
    const auto sourceBits = static_cast<std::size_t>(width) * state_->bytesPerPixel * 8;
    const auto sourceStride = ((sourceBits + state_->scanlinePad - 1) / state_->scanlinePad) * state_->scanlinePad / 8;
    const auto sourceBytes = sourceStride * static_cast<std::size_t>(height);
    const std::uint8_t* source{};
    Reply<xcb_get_image_reply_t> ordinary(nullptr, &std::free);
    if (state_->ensureShared(sourceBytes)) {
        const auto reply = own(xcb_shm_get_image_reply(state_->connection,
            xcb_shm_get_image(state_->connection, state_->screen.root, static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                static_cast<std::uint16_t>(width), static_cast<std::uint16_t>(height), ~0u, XCB_IMAGE_FORMAT_Z_PIXMAP,
                state_->segment, 0), nullptr));
        if (reply && reply->depth == state_->screen.root_depth && reply->size >= sourceBytes && reply->size <= state_->sharedBytes)
            source = state_->shared;
        else { state_->releaseShared(); state_->shmAvailable = false; }
    }
    // Remote servers and restricted SHM environments retain a real GetImage
    // path. A capture error is never replaced by a stale successful frame.
    if (state_->stopped()) return false;
    if (!source) {
        ordinary = own(xcb_get_image_reply(state_->connection,
            xcb_get_image(state_->connection, XCB_IMAGE_FORMAT_Z_PIXMAP, state_->screen.root,
                static_cast<std::int16_t>(x), static_cast<std::int16_t>(y), static_cast<std::uint16_t>(width),
                static_cast<std::uint16_t>(height), ~0u), nullptr));
        if (!ordinary || ordinary->depth != state_->screen.root_depth ||
            xcb_get_image_data_length(ordinary.get()) < 0 ||
            static_cast<std::size_t>(xcb_get_image_data_length(ordinary.get())) < sourceBytes)
            return state_->fail("The X11 server did not return a complete capture frame");
        source = xcb_get_image_data(ordinary.get());
    }
    try { state_->pixels.resize(layout->bytes); }
    catch (...) { return state_->fail("The X11 frame buffer could not be allocated"); }
    for (int row = 0; row < height; ++row) {
        if (state_->stopped()) return false;
        const auto* input = source + static_cast<std::size_t>(row) * sourceStride;
        auto* output = state_->pixels.data() + static_cast<std::size_t>(row) * layout->stride;
        if (state_->directBgra) {
            std::memcpy(output, input, layout->stride);
            Core::makeOpaque({output, layout->stride});
            continue;
        }
        for (int column = 0; column < width; ++column) {
            std::uint32_t pixel{};
            for (int byte = 0; byte < state_->bytesPerPixel; ++byte) {
                const auto shift = (state_->littleEndian ? byte : state_->bytesPerPixel - byte - 1) * 8;
                pixel |= static_cast<std::uint32_t>(input[column * state_->bytesPerPixel + byte]) << shift;
            }
            output[column * 4] = channel(pixel, state_->blue);
            output[column * 4 + 1] = channel(pixel, state_->green);
            output[column * 4 + 2] = channel(pixel, state_->red);
            output[column * 4 + 3] = 255;
        }
    }
    if (includeCursor) {
        if (!state_->xfixesAvailable) return state_->fail("XFixes cursor capture is unavailable");
        const auto cursor = own(xcb_xfixes_get_cursor_image_reply(state_->connection,
            xcb_xfixes_get_cursor_image(state_->connection), nullptr));
        if (!cursor || static_cast<std::size_t>(xcb_xfixes_get_cursor_image_cursor_image_length(cursor.get())) <
            static_cast<std::size_t>(cursor->width) * cursor->height)
            return state_->fail("The cursor image could not be captured");
        const auto* argb = xcb_xfixes_get_cursor_image_cursor_image(cursor.get());
        const int left = cursor->x - cursor->xhot - x, top = cursor->y - cursor->yhot - y;
        for (int row = std::max(0, -top); row < cursor->height && row + top < height; ++row) {
            for (int column = std::max(0, -left); column < cursor->width && column + left < width; ++column) {
                const auto value = argb[static_cast<std::size_t>(row) * cursor->width + column];
                const auto alpha = value >> 24;
                auto* destination = state_->pixels.data() + static_cast<std::size_t>(row + top) * layout->stride + (column + left) * 4;
                for (int component = 0; component < 3; ++component) {
                    // XFixes ARGB is premultiplied; adding source*alpha again
                    // would turn antialiased cursor edges dark.
                    const auto sourceValue = (value >> (component * 8)) & 255u;
                    destination[component] = static_cast<std::uint8_t>(std::min(255u,
                        sourceValue + (destination[component] * (255u - alpha) + 127u) / 255u));
                }
            }
        }
    }
    if (state_->stopped()) return false;
    state_->width = width; state_->height = height; state_->stride = layout->stride;
    state_->error.clear(); return true;
}

bool X11Capture::scroll(int x, int y, int steps)
{
    const NativeSignalMask signals;
    if (!signals.valid()) return state_->fail("Cannot prepare the capture worker's native signal mask");
    state_->scrollPaused = false;
    if (!initialize() || !state_->refreshBounds()) return false;
    if (!contains(state_->bounds, x, y, 1, 1) || x > 32767 || y > 32767 || steps == 0 || steps < -20 || steps > 20)
        return state_->fail("The scroll request is outside the X11 source");
    const auto* extension = xcb_get_extension_data(state_->connection, &xcb_test_id);
    if (!extension || !extension->present) return state_->fail("XTEST automatic scrolling is unavailable");
    if (!state_->scrollTarget) {
        const auto movement = own(xcb_request_check(state_->connection, xcb_warp_pointer_checked(state_->connection,
            XCB_NONE, state_->screen.root, 0, 0, 0, 0, static_cast<std::int16_t>(x), static_cast<std::int16_t>(y))));
        if (movement) return state_->fail("The X11 pointer could not move to the scroll region");
    }
    const auto pointer = own(xcb_query_pointer_reply(state_->connection,
        xcb_query_pointer(state_->connection, state_->screen.root), nullptr));
    if (!pointer) return state_->fail("The current X11 pointer target is unavailable");
    if (!state_->scrollTarget) state_->scrollTarget = pointer->child ? pointer->child : state_->screen.root;
    if ((pointer->child ? pointer->child : state_->screen.root) != state_->scrollTarget) {
        state_->scrollPaused = true; return state_->fail("Move the pointer back over the scrolling target to continue");
    }
    const auto button = static_cast<std::uint8_t>(steps > 0 ? 5 : 4);
    for (int i = 0; i < std::abs(steps); ++i) {
        if (state_->stopped()) return false;
        // Queue the release before waiting for the press acknowledgement; an
        // interrupted reply must not prevent us from submitting that release.
        const auto pressed = xcb_test_fake_input_checked(state_->connection, XCB_BUTTON_PRESS, button, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
        const auto released = xcb_test_fake_input_checked(state_->connection, XCB_BUTTON_RELEASE, button, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
        const auto press = own(xcb_request_check(state_->connection, pressed));
        const auto release = own(xcb_request_check(state_->connection, released));
        if (press || release || xcb_connection_has_error(state_->connection))
            return state_->fail("The X11 server rejected automatic scrolling");
    }
    if (xcb_flush(state_->connection) <= 0) return state_->fail("The X11 scroll connection closed");
    if (state_->stopped()) return false;
    state_->error.clear(); return true;
}
std::span<const std::uint8_t> X11Capture::pixels() const noexcept { return state_->pixels; }
int X11Capture::width() const noexcept { return state_->width; }
int X11Capture::height() const noexcept { return state_->height; }
std::size_t X11Capture::stride() const noexcept { return state_->stride; }
const std::string& X11Capture::error() const noexcept { return state_->error; }
bool X11Capture::scrollPaused() const noexcept { return state_->scrollPaused; }

} // namespace HuhuCapture::Platform::Linux
