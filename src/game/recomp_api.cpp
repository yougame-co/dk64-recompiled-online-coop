#ifdef _WIN32
// Winsock must precede headers that may include windows.h.
#include <winsock2.h>
#endif
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "recomp.h"
#include "librecomp/overlays.hpp"
#include "librecomp/addresses.hpp"
#include "donk_config.h"
#include "recompinput/recompinput.h"
#include "recompui/recompui.h"
#include "recompui/renderer.h"
#include "donk_sound.h"
#include "donk_draw.h"
#include "librecomp/helpers.hpp"
#include "../patches/input.h"
#include "../patches/graphics.h"
#include "../patches/sound.h"
#include "../patches/options.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"
#include "../lib/N64ModernRuntime/thirdparty/xxHash/xxh3.h"
#include "yg_coop_capture.h"
#include "yg_coop_net.hpp"
#include "yg_coop_smooth.hpp"
#define YGNET_IMPLEMENTATION
#include "yougame_net.h"
#include "yg_coop_relay.hpp"
#include <memory>

namespace {
// Isolate all ordinary stdout writers before main starts. This includes the
// character-at-a-time recomp_puts implementation below and third-party prints.
const int yg_relay_protocol_fd = yg_coop::isolate_relay_stdout();
}

namespace {
struct YGFrameRequest {
    uint32_t task_dl = 0;
    uint32_t framebuffer = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t seconds = 0;
};
std::mutex yg_frame_mutex;
YGFrameRequest yg_frame_request;
}

namespace {
class YGNativeTransport {
public:
    YGNativeTransport() : udp_(yg_coop::config(
        std::getenv("YG_COOP_NET"), std::getenv("YG_COOP_ROLE"),
        std::getenv("YG_COOP_PORT"), std::getenv("YG_COOP_PEER_PORT"))),
        smoother_(1) {
        if (yg_coop::relay_enabled()) relay_ = std::make_unique<yg_coop::RelayTransport>(yg_relay_protocol_fd);
        // The low-level test transport keeps its historical default echo mode.
        // A normal game without an explicit co-op setting stays single-player.
        standalone_off_ = !relay_ && !std::getenv("YG_COOP_NET");
    }
    int mode() const { return relay_ ? relay_->mode() : standalone_off_ ? YG_NET_ERROR : udp_.mode(); }
    char role() const { return mode() == YG_NET_ROLE_A ? 'a' : 'b'; }
    uint32_t step(const yg_coop::Sample &sample, uint32_t frame) {
        if (standalone_off_) return 0;
        auto accept = [&](const yg_coop::Sample &remote, uint32_t sender_frame) {
            smoother_.accept(remote, sender_frame, frame);
        };
        const uint32_t status = relay_ ? relay_->step(sample, frame, accept) : udp_.step(sample, frame, accept);
        if (mode() == YG_NET_ROLE_A || mode() == YG_NET_ROLE_B) {
            if (status & YG_NET_HAS_SAMPLE) {
                if (yg_coop::get32(sample.data() + YG_S_MAP) != 0xFFFFFFFFu &&
                    (!have_local_ || yg_coop::get32(sample.data()) != yg_coop::get32(last_local_.data()) ||
                     sample[YG_S_SPAWN] != last_local_[YG_S_SPAWN] || sample[YG_S_KONG] != last_local_[YG_S_KONG]))
                    smoother_.snap_to_newest(yg_coop::SmoothSnap::local_spawn, frame);
                if ((status & YG_NET_LIVE) && !was_live_)
                    smoother_.snap_to_newest(yg_coop::SmoothSnap::peer_resume, frame);
            }
            const bool visible = (status & YG_NET_LIVE) &&
                yg_coop::get32(sample.data()) != 0xFFFFFFFFu &&
                yg_coop::get32(sample.data()) == yg_coop::get32(smoother_.newest().data());
            smoother_.render(frame, visible);
            last_local_ = sample; have_local_ = true; was_live_ = (status & YG_NET_LIVE) != 0;
        }
        return status;
    }
    const yg_coop::Sample &remote() const { return smoother_.sample(); }
private:
    yg_coop::Transport udp_;
    std::unique_ptr<yg_coop::RelayTransport> relay_;
    yg_coop::Smoother smoother_;
    yg_coop::Sample last_local_{};
    bool have_local_ = false, was_live_ = false, standalone_off_ = false;
};
YGNativeTransport &yg_transport() {
    // Function-local initialization preserves the milestone-2 echo/UDP config.
    // Relay mode creates the stdio worker; UDP keeps literal loopback endpoints.
    static YGNativeTransport transport;
    return transport;
}
}

// 4A test-kong import
extern "C" void yg_coop_test_kong(uint8_t *rdram, recomp_context *ctx) {
    // 0=off; low byte=Kong+1; bit 8 marks the file source. Valid env wins.
    const auto parse = [](const char *value) -> uint32_t {
        if (!value) return 0;
        while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') ++value;
        if (*value < '0' || *value > '4') return 0;
        const uint32_t kong = static_cast<uint32_t>(*value++ - '0') + 1;
        while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') ++value;
        return *value == '\0' ? kong : 0;
    };
    uint32_t selection = parse(std::getenv("YG_COOP_TEST_KONG"));
    if (!selection) {
        const char *folder = std::getenv("APP_FOLDER_PATH");
        if (folder && *folder) {
            try {
                std::ifstream input(std::filesystem::path(folder) / "yg-coop-test-kong.txt");
                char line[32]{};
                if (input.getline(line, sizeof(line))) {
                    selection = parse(line);
                    if (selection) selection |= 0x100;
                }
            } catch (const std::exception &) {
                std::fprintf(stderr, "YG-COOP error test-kong file-read\n");
            }
        }
    }
    _return(ctx, selection);
}

extern "C" void yg_coop_net_mode(uint8_t *rdram, recomp_context *ctx) {
    _return(ctx, static_cast<uint32_t>(yg_transport().mode()));
}

extern "C" void yg_coop_net_step(uint8_t *rdram, recomp_context *ctx) {
    const uint32_t local = _arg<0, uint32_t>(rdram, ctx);
    const uint32_t remote = _arg<1, uint32_t>(rdram, ctx);
    const uint32_t frame = _arg<2, uint32_t>(rdram, ctx);
    const uint32_t size = _arg<3, uint32_t>(rdram, ctx);
    if (size != YG_SAMPLE_SIZE ||
        !yg_coop::valid_ram_address(local, recomp::mem_size) ||
        !yg_coop::valid_ram_address(remote, recomp::mem_size)) {
        static bool logged = false;
        if (!logged) std::fprintf(stderr, "YG-COOP error net-ram sample-size/address\n");
        logged = true;
        _return(ctx, uint32_t(0));
        return;
    }
    // Raw host pointers would reverse bytes within each N64 word. These
    // accessors produce canonical N64 big-endian bytes, including float bits.
    const gpr local_address = static_cast<gpr>(static_cast<int32_t>(local));
    const gpr remote_address = static_cast<gpr>(static_cast<int32_t>(remote));
    const auto sample = yg_coop::read_sample([&](size_t i) { return MEM_BU(i, local_address); });
    auto &transport = yg_transport();
    const uint32_t status = transport.step(sample, frame);
    if (status & YG_NET_HAS_SAMPLE) {
        yg_coop::write_sample(transport.remote(), [&](size_t i, uint8_t value) {
            MEM_B(i, remote_address) = static_cast<int8_t>(value);
        });
    }
    _return(ctx, status);
}

extern "C" void yg_coop_camera_input(uint8_t *rdram, recomp_context *ctx) {
    // These getters inspect the app's current game input snapshot without
    // consuming it. This covers camera input that is absent from OSContPad.
    float x = 0.0f, y = 0.0f;
    recompinput::get_right_analog(0, &x, &y);
    // Ignore residual drift even when the configured joystick deadzone is 0.
    bool active = std::fabs(x) > 0.10f || std::fabs(y) > 0.10f;
    recompinput::get_mouse_deltas(&x, &y);
    // Mouse motion is intentional input; it has no stationary sensor noise.
    active |= x != 0.0f || y != 0.0f;
    recompinput::get_gyro_deltas(0, &x, &y);
    // Frontend returns accumulated player-space angular rate (degrees/sec),
    // scaled by gyro sensitivity; a resting DualSense is rarely exactly zero.
    active |= std::fabs(x) > 2.0f || std::fabs(y) > 2.0f;
    _return(ctx, static_cast<uint32_t>(active));
}

extern "C" void yg_coop_request_frame(uint8_t *rdram, recomp_context *ctx) {
    YGFrameRequest request;
    request.task_dl = _arg<0, uint32_t>(rdram, ctx) & 0x1FFFFFFF;
    request.framebuffer = _arg<1, uint32_t>(rdram, ctx) & 0x1FFFFFFF;
    const uint32_t dimensions = _arg<2, uint32_t>(rdram, ctx);
    request.width = dimensions >> 16;
    request.height = dimensions & 0xFFFF;
    request.seconds = _arg<3, uint32_t>(rdram, ctx);
    if (request.width == 0 || request.width > 640 || request.height == 0 || request.height > 480 ||
        request.framebuffer >= 0x800000 ||
        request.width * request.height * 2 > 0x800000 - request.framebuffer ||
        (request.framebuffer & 1) ||
        (request.seconds != 5 && request.seconds != 15 && request.seconds != 25 && request.seconds != 33)) {
        fprintf(stderr, "YG-COOP error frame-dump-invalid-request\n");
        return;
    }
    std::lock_guard lock(yg_frame_mutex);
    if (yg_frame_request.seconds != 0) {
        fprintf(stderr, "YG-COOP error frame-dump-request-pending\n");
        return;
    }
    yg_frame_request = request;
}

void yg_coop_capture_after_dl(uint8_t *rdram, uint32_t task_dl) {
    YGFrameRequest request;
    {
        std::lock_guard lock(yg_frame_mutex);
        if (yg_frame_request.seconds == 0 ||
            yg_frame_request.task_dl != (task_dl & 0x1FFFFFFF)) return;
        request = yg_frame_request;
        yg_frame_request = {};
    }
    // This reads only this game's rendered RGB5551 framebuffer. No window,
    // desktop, personal app, OS capture API, permission, or new library.
#ifdef _WIN32
    const wchar_t *app_folder = _wgetenv(L"APP_FOLDER_PATH");
#else
    const char *app_folder = std::getenv("APP_FOLDER_PATH");
#endif
    if (app_folder == nullptr || app_folder[0] == '\0') {
        fprintf(stderr, "YG-COOP error frame-dump APP_FOLDER_PATH is unset\n");
        return;
    }
    try {
        const auto &transport = yg_transport();
        const std::string prefix = transport.mode() == YG_NET_ECHO ? "yg-coop-walk" :
            std::string("yg-coop-") + transport.role() + "-walk";
        const auto path = std::filesystem::path(app_folder) /
            (prefix + std::to_string(request.seconds) + "s.ppm");
        std::vector<uint8_t> pixels(request.width * request.height * 3);
        for (uint32_t i = 0; i < request.width * request.height; ++i) {
            // RT64 stores N64 halfwords in the runtime's word-swapped RAM.
            const gpr framebuffer = static_cast<gpr>(static_cast<int32_t>(request.framebuffer | 0x80000000u));
            const uint16_t pixel = MEM_HU(i * 2, framebuffer);
            const uint8_t red = (pixel >> 11) & 31;
            const uint8_t green = (pixel >> 6) & 31;
            const uint8_t blue = (pixel >> 1) & 31;
            pixels[i * 3] = (red << 3) | (red >> 2);
            pixels[i * 3 + 1] = (green << 3) | (green >> 2);
            pixels[i * 3 + 2] = (blue << 3) | (blue >> 2);
        }
#ifdef _WIN32
        // Only diagnostics need UTF-8; filesystem I/O keeps the wide native path.
        const auto utf8_path = path.u8string();
        const std::string log_path(reinterpret_cast<const char *>(utf8_path.data()), utf8_path.size());
#endif
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "P6\n" << request.width << ' ' << request.height << "\n255\n";
        file.write(reinterpret_cast<const char *>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        file.close();
        if (!file) {
#ifdef _WIN32
            fprintf(stderr, "YG-COOP error frame-dump-write path=%s\n", log_path.c_str());
#else
            fprintf(stderr, "YG-COOP error frame-dump-write path=%s\n", path.string().c_str());
#endif
            return;
        }
#ifdef _WIN32
        printf("YG-COOP frame-dump seconds=%u path=%s\n", request.seconds, log_path.c_str());
#else
        printf("YG-COOP frame-dump seconds=%u path=%s\n", request.seconds, path.string().c_str());
#endif
        fflush(stdout);
    } catch (const std::exception &error) {
        fprintf(stderr, "YG-COOP error frame-dump %s\n", error.what());
    }
}

#ifndef _WIN32
// Preserve Mac quick_exit source-line diagnostics after the guarded additions.
#line 258
#endif
extern "C" void recomp_update_inputs(uint8_t* rdram, recomp_context* ctx) {
    recompinput::poll_inputs();
}

extern "C" void recomp_puts(uint8_t* rdram, recomp_context* ctx) {
    PTR(char) cur_str = _arg<0, PTR(char)>(rdram, ctx);
    u32 length = _arg<1, u32>(rdram, ctx);

    for (u32 i = 0; i < length; i++) {
        fputc(MEM_B(i, (gpr)cur_str), stdout);
    }
}

extern "C" void recomp_exit(uint8_t* rdram, recomp_context* ctx) {
    ultramodern::quit();
}

extern "C" void recomp_error(uint8_t* rdram, recomp_context* ctx) {
    std::string str{};
    PTR(u8) str_ptr = _arg<0, PTR(u8)>(rdram, ctx);

    for (size_t i = 0; MEM_B(str_ptr, i) != '\x00'; i++) {
        str += (char)MEM_B(str_ptr, i);
    }

    recompui::message_box(str.c_str());
    assert(false);
    ultramodern::error_handling::quick_exit(__FILE__, __LINE__, __FUNCTION__);
}

extern "C" void recomp_get_gyro_deltas(uint8_t* rdram, recomp_context* ctx) {
    float* x_out = _arg<0, float*>(rdram, ctx);
    float* y_out = _arg<1, float*>(rdram, ctx);

    // TODO: use controller number
    recompinput::get_gyro_deltas(0, x_out, y_out);
}

extern "C" void recomp_get_mouse_deltas(uint8_t* rdram, recomp_context* ctx) {
    float* x_out = _arg<0, float*>(rdram, ctx);
    float* y_out = _arg<1, float*>(rdram, ctx);

    recompinput::get_mouse_deltas(x_out, y_out);
}

extern "C" void recomp_powf(uint8_t* rdram, recomp_context* ctx) {
    float a = _arg<0, float>(rdram, ctx);
    float b = ctx->f14.fl; //_arg<1, float>(rdram, ctx);

    _return(ctx, std::pow(a, b));
}

extern "C" void recomp_get_target_framerate(uint8_t* rdram, recomp_context* ctx) {
    int frame_divisor = _arg<0, u32>(rdram, ctx);

    _return(ctx, ultramodern::get_target_framerate(60 / frame_divisor));
}

extern "C" void recomp_get_window_resolution(uint8_t* rdram, recomp_context* ctx) {
    int width, height;
    recompui::get_window_size(width, height);

    gpr width_out = _arg<0, PTR(u32)>(rdram, ctx);
    gpr height_out = _arg<1, PTR(u32)>(rdram, ctx);

    MEM_W(0, width_out) = (u32)width;
    MEM_W(0, height_out) = (u32)height;
}

extern "C" void recomp_get_target_aspect_ratio(uint8_t* rdram, recomp_context* ctx) {
    ultramodern::renderer::GraphicsConfig graphics_config = ultramodern::renderer::get_graphics_config();
    float original = _arg<0, float>(rdram, ctx);
    int width, height;
    recompui::get_window_size(width, height);

    switch (graphics_config.ar_option) {
        case ultramodern::renderer::AspectRatio::Original:
        default:
            _return(ctx, original);
            return;
        case ultramodern::renderer::AspectRatio::Expand:
            _return(ctx, std::max(static_cast<float>(width) / height, original));
            return;
    }
}

extern "C" void recomp_get_bgm_volume(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, dk64::get_bgm_volume() / 2.5f);
}

extern "C" void recomp_get_sfx_volume(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, dk64::get_sfx_volume() / 2.5f);
}

extern "C" void recomp_get_draw_distance(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, dk64::get_draw_distance() * 50.0f);
}

extern "C" void recomp_get_story_skip(uint8_t* rdram, recomp_context* ctx) {
    switch (dk64::get_story_skip()) {
        case dk64::StorySkipMode::Off:
            _return(ctx, 0);
            return;
        case dk64::StorySkipMode::IntroStory:
            _return(ctx, 1);
            return;
        case dk64::StorySkipMode::VanillaOn:
            _return(ctx, 2);
            return;
    }
}

extern "C" void recomp_get_camera_type(uint8_t* rdram, recomp_context* ctx) {
    switch (dk64::get_camera_type()) {
        case dk64::CameraTypeMode::Free:
            _return(ctx, 0);
            return;
        case dk64::CameraTypeMode::Follow:
            _return(ctx, 1);
            return;
        case dk64::CameraTypeMode::BetterFree:
            _return(ctx, 2);
            return;
        case dk64::CameraTypeMode::Analog:
            _return(ctx, 3);
            return;
    }
}

extern "C" void recomp_get_lightning_frequency(uint8_t* rdram, recomp_context* ctx) {
    switch (dk64::get_lightning_flash()) {
        case dk64::LightningFlashMode::Off:
            _return(ctx, 0);
            return;
        case dk64::LightningFlashMode::Reduced:
            _return(ctx, 10);
            return;
        case dk64::LightningFlashMode::Vanilla:
            _return(ctx, 1);
            return;
    }
}

extern "C" void recomp_get_cutscene_bordering(uint8_t* rdram, recomp_context* ctx) {
    switch (dk64::get_cutscene_borders()) {
        case dk64::CutsceneBordersMode::Off:
            _return(ctx, 0);
            return;
        case dk64::CutsceneBordersMode::On:
            _return(ctx, 40);
            return;
    }
}

extern "C" void recomp_get_mp_enabled(uint8_t* rdram, recomp_context* ctx) {
    switch (dk64::get_multiplayer_enabled()) {
        case dk64::MultiplayerEnabled::Off:
            _return(ctx, 0);
            return;
        case dk64::MultiplayerEnabled::On:
            _return(ctx, 1);
            return;
    }
}

extern "C" void recomp_get_ui_bounds(uint8_t* rdram, recomp_context* ctx) {
    ultramodern::renderer::GraphicsConfig graphics_config = ultramodern::renderer::get_graphics_config();
    int width, height;
    recompui::get_window_size(width, height);
    switch (graphics_config.ar_option) {
        case ultramodern::renderer::AspectRatio::Original:
        default:
            width = 320;
            break;
        case ultramodern::renderer::AspectRatio::Expand:
            width = (width * 240) / height;
            if (width < 320) {
                width = 320;
            }
            break;
    }
    s32* x_out = _arg<0, s32*>(rdram, ctx);
    s32* y_out = _arg<1, s32*>(rdram, ctx);
    *x_out = width;
    *y_out = 240;
    return;
}

extern "C" void recomp_get_ui_pillar(uint8_t* rdram, recomp_context* ctx) {
    ultramodern::renderer::GraphicsConfig graphics_config = ultramodern::renderer::get_graphics_config();
    int width, height, delta;
    recompui::get_window_size(width, height);
    switch (graphics_config.ar_option) {
        case ultramodern::renderer::AspectRatio::Original:
        default:
            width = 320;
            break;
        case ultramodern::renderer::AspectRatio::Expand:
            width = (width * 240) / height;
            if (width < 320) {
                width = 320;
            }
            break;
    }
    delta = 0;
    switch (graphics_config.hr_option) {
        default:
        case ultramodern::renderer::HUDRatioMode::Original:
            delta = (width - 320) / 2;
            break;
        case ultramodern::renderer::HUDRatioMode::Clamp16x9:
            delta = (width - 426) / 2;
            break;
        case ultramodern::renderer::HUDRatioMode::Full:
            delta = 0;
            break;
    }
    if (delta < 0) {
        delta = 0;
    }
    _return(ctx, delta);
    return;
}

extern "C" void recomp_get_analog_cam_sensitivity(uint8_t* rdram, recomp_context* ctx) {
    _return<uint32_t>(ctx, dk64::get_analog_cam_sensitivity());
}


extern "C" void recomp_time_us(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, static_cast<u32>(std::chrono::duration_cast<std::chrono::microseconds>(ultramodern::time_since_start()).count()));
}

extern "C" void recomp_load_overlays(uint8_t * rdram, recomp_context * ctx) {
    u32 rom = _arg<0, u32>(rdram, ctx);
    PTR(void) ram = _arg<1, PTR(void)>(rdram, ctx);
    u32 size = _arg<2, u32>(rdram, ctx);

    load_overlays(rom, ram, size);
}

extern "C" void recomp_high_precision_fb_enabled(uint8_t * rdram, recomp_context * ctx) {
    _return(ctx, static_cast<s32>(recompui::renderer::RT64HighPrecisionFBEnabled()));
}

extern "C" void recomp_get_resolution_scale(uint8_t* rdram, recomp_context* ctx) {
    _return(ctx, ultramodern::get_resolution_scale());
}

extern "C" void recomp_get_inverted_axes(uint8_t* rdram, recomp_context* ctx) {
    s32* x_out = _arg<0, s32*>(rdram, ctx);
    s32* y_out = _arg<1, s32*>(rdram, ctx);

    dk64::CameraInvertMode mode = dk64::get_camera_invert_mode();

    *x_out = (mode == dk64::CameraInvertMode::InvertX || mode == dk64::CameraInvertMode::InvertBoth);
    *y_out = (mode == dk64::CameraInvertMode::InvertY || mode == dk64::CameraInvertMode::InvertBoth);
}

extern "C" void recomp_get_analog_inverted_axes(uint8_t* rdram, recomp_context* ctx) {
    s32* x_out = _arg<0, s32*>(rdram, ctx);
    s32* y_out = _arg<1, s32*>(rdram, ctx);

    dk64::CameraInvertMode mode = dk64::get_third_person_camera_mode();

    *x_out = (mode == dk64::CameraInvertMode::InvertX || mode == dk64::CameraInvertMode::InvertBoth);
    *y_out = (mode == dk64::CameraInvertMode::InvertY || mode == dk64::CameraInvertMode::InvertBoth);
}

extern "C" void recomp_get_swimming_inverted_axes(uint8_t* rdram, recomp_context* ctx) {
    s32* x_out = _arg<0, s32*>(rdram, ctx);
    s32* y_out = _arg<1, s32*>(rdram, ctx);

    dk64::CameraInvertMode mode = dk64::get_swimming_invert_mode();

    *x_out = (mode == dk64::CameraInvertMode::InvertX || mode == dk64::CameraInvertMode::InvertBoth);
    *y_out = (mode == dk64::CameraInvertMode::InvertY || mode == dk64::CameraInvertMode::InvertBoth);
}

extern "C" void recomp_get_first_person_inverted_axes(uint8_t* rdram, recomp_context* ctx) {
    s32* x_out = _arg<0, s32*>(rdram, ctx);
    s32* y_out = _arg<1, s32*>(rdram, ctx);

    dk64::CameraInvertMode mode = dk64::get_first_person_invert_mode();

    *x_out = (mode == dk64::CameraInvertMode::InvertX || mode == dk64::CameraInvertMode::InvertBoth);
    *y_out = (mode == dk64::CameraInvertMode::InvertY || mode == dk64::CameraInvertMode::InvertBoth);
}

extern "C" void recomp_get_gyro_inverted_axes(uint8_t* rdram, recomp_context* ctx) {
    s32* x_out = _arg<0, s32*>(rdram, ctx);
    s32* y_out = _arg<1, s32*>(rdram, ctx);

    dk64::CameraInvertMode mode = dk64::get_gyro_invert_mode();

    *x_out = (mode == dk64::CameraInvertMode::InvertX || mode == dk64::CameraInvertMode::InvertBoth);
    *y_out = (mode == dk64::CameraInvertMode::InvertY || mode == dk64::CameraInvertMode::InvertBoth);
}

extern "C" void recomp_get_mouse_inverted_axes(uint8_t* rdram, recomp_context* ctx) {
    s32* x_out = _arg<0, s32*>(rdram, ctx);
    s32* y_out = _arg<1, s32*>(rdram, ctx);

    dk64::CameraInvertMode mode = dk64::get_mouse_invert_mode();

    *x_out = (mode == dk64::CameraInvertMode::InvertX || mode == dk64::CameraInvertMode::InvertBoth);
    *y_out = (mode == dk64::CameraInvertMode::InvertY || mode == dk64::CameraInvertMode::InvertBoth);
}

extern "C" void recomp_get_right_analog_inputs(uint8_t* rdram, recomp_context* ctx) {
    float* x_out = _arg<0, float*>(rdram, ctx);
    float* y_out = _arg<1, float*>(rdram, ctx);

    // Don't return right analog inputs while game input is disabled.
    if (recompinput::game_input_disabled()) {
        *x_out = 0.0f;
        *y_out = 0.0f;
        return;
    }

    // TODO: Use controller number.
    recompinput::get_right_analog(0, x_out, y_out);
}

extern "C" void recomp_set_right_analog_suppressed(uint8_t* rdram, recomp_context* ctx) {
    s32 suppressed = _arg<0, s32>(rdram, ctx);

    recompinput::set_right_analog_suppressed(suppressed);
}

constexpr uint32_t k1_to_phys(uint32_t addr) {
    return addr & 0x1FFFFFFF;
}

extern "C" void osPiReadIo_recomp(RDRAM_ARG recomp_context * ctx) {
    uint32_t devAddr = recomp::rom_base | ctx->r4;
    gpr dramAddr = ctx->r5;
    uint32_t physical_addr = k1_to_phys(devAddr);

    if (physical_addr > recomp::rom_base) {
        // cart rom
        recomp::do_rom_pio(PASS_RDRAM dramAddr, physical_addr);
    } else {
        // sram
        assert(false && "SRAM ReadIo unimplemented");
    }

    ctx->r2 = 0;
}

extern "C" void osPfsInit_recomp(uint8_t * rdram, recomp_context* ctx) {
    ctx->r2 = 11; // PFS_ERR_DEVICE
}

// u32 rom_addr, void *ram_addr, u32 size
extern "C" void recomp_load_overlays_by_rom(uint8_t* rdram, recomp_context* ctx) {
    u32 rom_addr = _arg<0, u32>(rdram, ctx);
    PTR(void) ram_addr = _arg<1, PTR(void)>(rdram, ctx);
    u32 size = _arg<2, u32>(rdram, ctx);

    load_overlays(rom_addr, ram_addr, size);
}

extern "C" void recomp_abort(uint8_t* rdram, recomp_context* ctx) {
    std::string msg = _arg_string<0>(rdram, ctx);
    recompui::message_box(msg.c_str());
    assert(false);
    ultramodern::error_handling::quick_exit(__FILE__, __LINE__, __FUNCTION__);
}

extern "C" void recomp_xxh3(uint8_t* rdram, recomp_context* ctx) {
    PTR(void) data = _arg<0, PTR(void)>(rdram, ctx);
    u32 size = _arg<1, u32>(rdram, ctx);
    XXH3_state_t xxh3;
    XXH3_64bits_reset(&xxh3);

    // Hash 1 byte at a time to account for byteswapping.
    for (size_t i = 0; i < size; i++) {
        XXH3_64bits_update(&xxh3, TO_PTR(u8, data + i), 1);
    }

    uint64_t ret = XXH3_64bits_digest(&xxh3);
    
    ctx->r2 = (int32_t)(ret >> 32);
    ctx->r3 = (int32_t)(ret >> 0);
}

//map compressed rom address -> decompressed rom address, then load the overlay
extern "C" void load_dk64_overlay(uint32_t compressed_rom, int32_t ram_addr, uint32_t size) {
    uint32_t decompressed_rom = 0;
    switch (compressed_rom) {
        case 0x113F0: //global_asm
            decompressed_rom = 0x2000000;
            ram_addr = 0x805FB300;
            size = 0x165D50;
            break;
        case 0xCBE70: //menu
            decompressed_rom = 0x2165D50;
            ram_addr = 0x80024000;
            size = 0xFF10;
            break;
        case 0xD4B00: //multiplayer
            decompressed_rom = 0x2175C60;
            ram_addr = 0x80024000;
            size = 0x3100;
            break;
        case 0xD6B00: //minecart
            decompressed_rom = 0x2178D60;
            ram_addr = 0x80024000;
            size = 0x4E10;
            break;
        case 0xD9A40: //bonus
            decompressed_rom = 0x217DB70;
            ram_addr = 0x80024000;
            size = 0x9EF0;
            break;
        case 0xDF600: //race
            decompressed_rom = 0x2187A60;
            ram_addr = 0x80024000;
            size = 0xC160;
            break;
        case 0xE6780: //critter
            decompressed_rom = 0x2193BC0;
            ram_addr = 0x80024000;
            size = 0x61B0;
            break;
        case 0xEA0B0: //boss
            decompressed_rom = 0x2199D70;
            ram_addr = 0x80024000;
            size = 0x12DC0;
            break;
        case 0xF41A0: //arcade
            decompressed_rom = 0x21ACB30;
            ram_addr = 0x80024000;
            size = 0x26C00;
            break;
        case 0xFD2F0: //jetpac
            decompressed_rom = 0x21D3730;
            ram_addr = 0x80024000;
            size = 0xAC30;
            break;
    }
    if (decompressed_rom != 0) {
        load_overlays(decompressed_rom, ram_addr, size);
    }
}

extern "C" void boot_osPiRawStartDma(uint8_t* rdram, recomp_context* ctx) {
    uint32_t direction = ctx->r4;
    uint32_t device_address = ctx->r5;
    gpr rdram_address = ctx->r6;
    uint32_t size = ctx->r7;

    assert(direction == 0); // Only reads

    printf("boot_osPiRawStartDma: Rom %08X, Ram %08X, Size: %08X\n", device_address, rdram_address, size);

    // Complete the DMA synchronously (the game immediately waits until it's done anyways)
    recomp::do_rom_read(rdram, rdram_address, device_address + recomp::rom_base, size);
}

extern "C" void __f_to_ull_recomp(uint8_t * rdram, recomp_context * ctx) {
    uint64_t ret = (uint64_t)ctx->f12.fl;
    ctx->r2 = (int32_t)(ret >> 32);
    ctx->r3 = (int32_t)(ret >> 0);
}

extern "C" void __osSpSetStatus_recomp(RDRAM_ARG recomp_context * ctx) {
    //assert(false && "SRAM ReadIo unimplemented");
}

extern "C" void osViGetCurrentMode_recomp(uint8_t* rdram, recomp_context* ctx) {
    constexpr gpr os_vi_curr_ptr_addr = 0xFFFFFFFF80010190ull;
    const gpr vi_curr_ctx = (gpr)(int32_t)MEM_W(0, os_vi_curr_ptr_addr);

    if (vi_curr_ctx == 0) {
        ctx->r2 = 0;
        return;
    }

    const gpr modep = (gpr)(int32_t)MEM_W(0x8, vi_curr_ctx);
    if (modep == 0) {
        ctx->r2 = 0;
        return;
    }

    ctx->r2 = MEM_BU(0x3, modep);
}
