/**
 * @file src/platform/windows/winuhid.cpp
 * @brief Load WinUHidDevs at runtime: relative mouse and DualSense.
 */
// standard includes
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <ranges>
#include <string_view>

// platform includes
#include <Windows.h>

// local includes
#include "src/globals.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "winuhid.h"

using namespace std::literals;

namespace platf::winuhid {
  namespace {

    using mouse_create_fn = void *(__cdecl *)(const void *);
    using mouse_destroy_fn = void(__cdecl *)(void *);
    using mouse_motion_fn = BOOL(__cdecl *)(void *, SHORT, SHORT);
    using mouse_button_fn = BOOL(__cdecl *)(void *, UCHAR, BOOL);
    using mouse_scroll_fn = BOOL(__cdecl *)(void *, SHORT, BOOL);
    using interface_version_fn = DWORD(__cdecl *)();

    using rumble_cb = void(__cdecl *)(void *, UCHAR, UCHAR);
    using lightbar_cb = void(__cdecl *)(void *, UCHAR, UCHAR, UCHAR);
    using player_led_cb = void(__cdecl *)(void *, UCHAR);

#pragma pack(push, 1)
    struct trigger_effect_t {
      UCHAR Type;
      UCHAR Data[10];
    };

    struct ps5_report_t {
      UCHAR ReportId;
      UCHAR LeftStickX;
      UCHAR LeftStickY;
      UCHAR RightStickX;
      UCHAR RightStickY;
      UCHAR LeftTrigger;
      UCHAR RightTrigger;
      UCHAR SequenceNumber;
      UCHAR Hat : 4;
      UCHAR ButtonSquare : 1;
      UCHAR ButtonCross : 1;
      UCHAR ButtonCircle : 1;
      UCHAR ButtonTriangle : 1;
      UCHAR ButtonL1 : 1;
      UCHAR ButtonR1 : 1;
      UCHAR ButtonL2 : 1;
      UCHAR ButtonR2 : 1;
      UCHAR ButtonShare : 1;
      UCHAR ButtonOptions : 1;
      UCHAR ButtonL3 : 1;
      UCHAR ButtonR3 : 1;
      UCHAR ButtonHome : 1;
      UCHAR ButtonTouchpad : 1;
      UCHAR ButtonMute : 1;
      UCHAR Reserved : 1;
      UCHAR ButtonLeftFunction : 1;
      UCHAR ButtonRightFunction : 1;
      UCHAR ButtonLeftPaddle : 1;
      UCHAR ButtonRightPaddle : 1;
      UCHAR Reserved2[5];
      USHORT GyroX;
      USHORT GyroY;
      USHORT GyroZ;
      USHORT AccelX;
      USHORT AccelY;
      USHORT AccelZ;
      UINT SensorTimestamp;
      UCHAR Temperature;
      struct {
        struct {
          UCHAR ContactSeq;
          UCHAR XLowPart;
          UCHAR XHighPart : 4;
          UCHAR YLowPart : 4;
          UCHAR YHighPart;
        } TouchPoints[2];
        UCHAR Timestamp;
      } TouchReport;
      UCHAR TriggerRightStopLocation : 4;
      UCHAR TriggerRightStatus : 4;
      UCHAR TriggerLeftStopLocation : 4;
      UCHAR TriggerLeftStatus : 4;
      UINT HostTimestamp;
      UCHAR TriggerRightEffect : 4;
      UCHAR TriggerLeftEffect : 4;
      UINT DeviceTimestamp;
      UCHAR BatteryPercent : 4;
      UCHAR BatteryState : 4;
      UCHAR Reserved3[10];
    };
#pragma pack(pop)

    static_assert(sizeof(ps5_report_t) == 64, "DualSense USB input report must be 64 bytes");

    using trigger_cb = void(__cdecl *)(void *, const trigger_effect_t *, const trigger_effect_t *);
    using ps5_create_fn = void *(__cdecl *)(const void *, rumble_cb, lightbar_cb, player_led_cb, trigger_cb, void *);
    using ps5_destroy_fn = void(__cdecl *)(void *);
    using ps5_init_fn = void(__cdecl *)(ps5_report_t *);
    using ps5_hat_fn = void(__cdecl *)(ps5_report_t *, INT, INT);
    using ps5_battery_fn = void(__cdecl *)(ps5_report_t *, BOOL, UCHAR);
    using ps5_touch_fn = void(__cdecl *)(ps5_report_t *, UCHAR, BOOL, USHORT, USHORT);
    using ps5_accel_fn = void(__cdecl *)(ps5_report_t *, float, float, float);
    using ps5_gyro_fn = void(__cdecl *)(ps5_report_t *, float, float, float);
    using ps5_report_fn = BOOL(__cdecl *)(void *, const ps5_report_t *);

    constexpr SHORT k_short_max = 32767;
    constexpr SHORT k_short_min = -32767;

    void send_mouse_input(DWORD flags, LONG dx, LONG dy, DWORD data) {
      INPUT event {};
      event.type = INPUT_MOUSE;
      event.mi.dx = dx;
      event.mi.dy = dy;
      event.mi.mouseData = data;
      event.mi.dwFlags = flags;
      if (!SendInput(1, &event, sizeof(event))) {
        BOOST_LOG(warning) << "SendInput mouse failed: "sv << GetLastError();
      }
    }

    std::optional<UCHAR> hid_button_index(int button) {
      // WinUHidMouseReportButton uses `1 << ButtonIndex` (HID bit0=left).
      // WUHM_BUTTON_* in the header are 1-based and do not match that.
      switch (button) {
        case BUTTON_LEFT:
          return 0;
        case BUTTON_RIGHT:
          return 1;
        case BUTTON_MIDDLE:
          return 2;
        case BUTTON_X1:
          return 3;
        case BUTTON_X2:
          return 4;
        default:
          BOOST_LOG(warning) << "Unknown mouse button: "sv << button;
          return std::nullopt;
      }
    }

    std::optional<DWORD> sendinput_button_flags(int button, bool release) {
      switch (button) {
        case BUTTON_LEFT:
          return release ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN;
        case BUTTON_RIGHT:
          return release ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN;
        case BUTTON_MIDDLE:
          return release ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_MIDDLEDOWN;
        case BUTTON_X1:
        case BUTTON_X2:
          return release ? MOUSEEVENTF_XUP : MOUSEEVENTF_XDOWN;
        default:
          BOOST_LOG(warning) << "Unknown mouse button: "sv << button;
          return std::nullopt;
      }
    }

    DWORD sendinput_xbutton_data(int button) {
      if (button == BUTTON_X1) {
        return XBUTTON1;
      }
      if (button == BUTTON_X2) {
        return XBUTTON2;
      }
      return 0;
    }

    std::filesystem::path executable_dir() {
      wchar_t path[MAX_PATH] {};
      const auto length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
      if (length == 0 || length >= ARRAYSIZE(path)) {
        return {};
      }
      return std::filesystem::path {path}.parent_path();
    }

    HMODULE load_dll(const wchar_t *name) {
      const auto dir = executable_dir();
      if (!dir.empty()) {
        if (const auto module = LoadLibraryW((dir / name).c_str())) {
          return module;
        }
      }
      return LoadLibraryW(name);
    }

    SHORT clamp_short(int value) {
      return static_cast<SHORT>(std::clamp(value, static_cast<int>(k_short_min), static_cast<int>(k_short_max)));
    }

    UCHAR stick_x(std::int16_t value) {
      return static_cast<UCHAR>((value + std::numeric_limits<std::uint16_t>::max() / 2 + 1) / 257);
    }

    UCHAR stick_y(std::int16_t value) {
      auto inverted = -((std::numeric_limits<std::uint16_t>::max() / 2 + value - 1)) / 257;
      return inverted == 0 ? 0xFF : static_cast<UCHAR>(inverted);
    }

    struct api_t {
      HMODULE core = nullptr;
      HMODULE devs = nullptr;
      interface_version_fn version = nullptr;
      mouse_create_fn mouse_create = nullptr;
      mouse_destroy_fn mouse_destroy = nullptr;
      mouse_motion_fn mouse_motion = nullptr;
      mouse_button_fn mouse_button = nullptr;
      mouse_scroll_fn mouse_scroll = nullptr;
      ps5_create_fn ps5_create = nullptr;
      ps5_destroy_fn ps5_destroy = nullptr;
      ps5_init_fn ps5_init = nullptr;
      ps5_hat_fn ps5_hat = nullptr;
      ps5_battery_fn ps5_battery = nullptr;
      ps5_touch_fn ps5_touch = nullptr;
      ps5_accel_fn ps5_accel = nullptr;
      ps5_gyro_fn ps5_gyro = nullptr;
      ps5_report_fn ps5_report = nullptr;
      bool mouse = false;
      bool ps5 = false;
    };

    api_t &api() {
      static api_t loaded = [] {
        api_t result;
        result.core = load_dll(L"WinUHid.dll");
        result.devs = load_dll(L"WinUHidDevs.dll");
        if (!result.core || !result.devs) {
          BOOST_LOG(warning) << "WinUHid DLLs not found next to sunshine.exe"sv;
          return result;
        }

        result.version = reinterpret_cast<interface_version_fn>(GetProcAddress(result.core, "WinUHidGetDriverInterfaceVersion"));
        result.mouse_create = reinterpret_cast<mouse_create_fn>(GetProcAddress(result.devs, "WinUHidMouseCreate"));
        result.mouse_destroy = reinterpret_cast<mouse_destroy_fn>(GetProcAddress(result.devs, "WinUHidMouseDestroy"));
        result.mouse_motion = reinterpret_cast<mouse_motion_fn>(GetProcAddress(result.devs, "WinUHidMouseReportMotion"));
        result.mouse_button = reinterpret_cast<mouse_button_fn>(GetProcAddress(result.devs, "WinUHidMouseReportButton"));
        result.mouse_scroll = reinterpret_cast<mouse_scroll_fn>(GetProcAddress(result.devs, "WinUHidMouseReportScroll"));
        result.mouse = result.mouse_create && result.mouse_destroy && result.mouse_motion && result.mouse_button && result.mouse_scroll;

        result.ps5_create = reinterpret_cast<ps5_create_fn>(GetProcAddress(result.devs, "WinUHidPS5Create"));
        result.ps5_destroy = reinterpret_cast<ps5_destroy_fn>(GetProcAddress(result.devs, "WinUHidPS5Destroy"));
        result.ps5_init = reinterpret_cast<ps5_init_fn>(GetProcAddress(result.devs, "WinUHidPS5InitializeInputReport"));
        result.ps5_hat = reinterpret_cast<ps5_hat_fn>(GetProcAddress(result.devs, "WinUHidPS5SetHatState"));
        result.ps5_battery = reinterpret_cast<ps5_battery_fn>(GetProcAddress(result.devs, "WinUHidPS5SetBatteryState"));
        result.ps5_touch = reinterpret_cast<ps5_touch_fn>(GetProcAddress(result.devs, "WinUHidPS5SetTouchState"));
        result.ps5_accel = reinterpret_cast<ps5_accel_fn>(GetProcAddress(result.devs, "WinUHidPS5SetAccelState"));
        result.ps5_gyro = reinterpret_cast<ps5_gyro_fn>(GetProcAddress(result.devs, "WinUHidPS5SetGyroState"));
        result.ps5_report = reinterpret_cast<ps5_report_fn>(GetProcAddress(result.devs, "WinUHidPS5ReportInput"));
        result.ps5 = result.ps5_create && result.ps5_destroy && result.ps5_init && result.ps5_hat && result.ps5_battery &&
                     result.ps5_touch && result.ps5_accel && result.ps5_gyro && result.ps5_report;
        return result;
      }();
      return loaded;
    }

    void copy_trigger_effect(std::array<std::uint8_t, 10> &dst, const trigger_effect_t *effect) {
      if (!effect) {
        dst = {};
        return;
      }
      std::memcpy(dst.data(), effect->Data, dst.size());
    }

  }  // namespace

  struct mouse_t::impl {
    void *device = nullptr;
    bool hid = false;

    void warn_once(std::string_view operation) {
      static std::once_flag flag;
      std::call_once(flag, [operation]() {
        BOOST_LOG(warning) << "WinUHid "sv << operation << " failed: "sv << GetLastError();
      });
    }

    ~impl() {
      if (device && api().mouse_destroy) {
        api().mouse_destroy(device);
        device = nullptr;
      }
    }
  };

  mouse_t::mouse_t():
      impl_ {std::make_unique<impl>()} {
    auto &fns = api();
    if (!fns.mouse) {
      BOOST_LOG(warning) << "WinUHidDevs is missing mouse exports; relative mouse uses SendInput"sv;
      return;
    }

    const auto interface_version = fns.version ? fns.version() : 0;
    SetLastError(0);
    impl_->device = fns.mouse_create(nullptr);
    if (!impl_->device) {
      const auto create_error = GetLastError();
      BOOST_LOG(warning) << "WinUHidMouseCreate failed: "sv << create_error << "; relative mouse uses SendInput"sv;
      return;
    }

    impl_->hid = true;
    BOOST_LOG(info) << "WinUHid relative mouse: HID (interface "sv << interface_version << ")"sv;
  }

  mouse_t::~mouse_t() = default;

  mouse_t::mouse_t(mouse_t &&other) noexcept = default;
  mouse_t &mouse_t::operator=(mouse_t &&other) noexcept = default;

  void mouse_t::move(int delta_x, int delta_y) {
    if (!impl_) {
      return;
    }

    auto remaining_x = delta_x;
    auto remaining_y = delta_y;
    while (remaining_x != 0 || remaining_y != 0) {
      const auto step_x = clamp_short(remaining_x);
      const auto step_y = clamp_short(remaining_y);
      remaining_x -= step_x;
      remaining_y -= step_y;

      if (impl_->hid) {
        if (!api().mouse_motion(impl_->device, step_x, step_y)) {
          impl_->warn_once("ReportMotion"sv);
        }
        continue;
      }
      send_mouse_input(MOUSEEVENTF_MOVE, step_x, step_y, 0);
    }
  }

  void mouse_t::button(int button, bool release) {
    if (!impl_) {
      return;
    }

    const auto hid_index = hid_button_index(button);
    if (impl_->hid) {
      if (!hid_index || !api().mouse_button(impl_->device, *hid_index, release ? FALSE : TRUE)) {
        impl_->warn_once("ReportButton"sv);
      }
      return;
    }

    const auto flags = sendinput_button_flags(button, release);
    if (!flags) {
      return;
    }
    send_mouse_input(*flags, 0, 0, sendinput_xbutton_data(button));
  }

  void mouse_t::scroll(int high_res_distance, bool horizontal) {
    if (!impl_ || high_res_distance == 0) {
      return;
    }

    auto remaining = high_res_distance;
    while (remaining != 0) {
      const auto step = clamp_short(remaining);
      remaining -= step;

      if (impl_->hid) {
        if (!api().mouse_scroll(impl_->device, step, horizontal ? TRUE : FALSE)) {
          impl_->warn_once("ReportScroll"sv);
        }
        continue;
      }
      send_mouse_input(horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(static_cast<SHORT>(step)));
    }
  }

  status_t status() {
    auto &fns = api();
    status_t result;
    result.dlls = fns.core && fns.devs;
    result.mouse = fns.mouse;
    result.ps5 = fns.ps5;
    result.interface_version = fns.version ? fns.version() : 0;
    return result;
  }

  bool ps5_available() {
    return api().ps5;
  }

  struct ps5_t::impl: std::enable_shared_from_this<impl> {
    void *device = nullptr;
    ps5_report_t report {};
    std::uint8_t client_index = 0;
    feedback_queue_t feedback_queue;
    std::mutex feedback_mutex;
    gamepad_feedback_msg_t last_rumble {};
    gamepad_feedback_msg_t last_rgb {};
    bool has_rumble = false;
    bool has_rgb = false;
    thread_pool_util::ThreadPool::task_id_t rumble_stop_task {};
    std::array<std::optional<std::uint32_t>, 2> touch_ids;

    // DualSense rumble is a held level. WinUHid now heartbeats on every
    // CompatibleVibration(2) report; Sunshine forwards level *changes* to
    // Moonlight and sends 0 when flagged HID rumble stops arriving.
    static constexpr auto rumble_hold = std::chrono::milliseconds {300};

    void raise(gamepad_feedback_msg_t msg) {
      auto queue = feedback_queue;
      if (!queue) {
        return;
      }
      task_pool.push([queue, msg]() {
        queue->raise(msg);
      });
    }

    void cancel_rumble_stop() {
      if (rumble_stop_task) {
        task_pool.cancel(rumble_stop_task);
        rumble_stop_task = nullptr;
      }
    }

    void stop_rumble_locked(std::string_view reason) {
      cancel_rumble_stop();
      if (!has_rumble || (last_rumble.data.rumble.lowfreq == 0 && last_rumble.data.rumble.highfreq == 0)) {
        return;
      }
      BOOST_LOG(info) << "WinUHid DualSense rumble stop ("sv << reason << ")"sv;
      auto msg = gamepad_feedback_msg_t::make_rumble(client_index, 0, 0);
      last_rumble = msg;
      raise(msg);
    }

    void schedule_rumble_stop() {
      cancel_rumble_stop();
      auto weak = weak_from_this();
      rumble_stop_task = task_pool.pushDelayed([weak]() {
        auto self = weak.lock();
        if (!self) {
          return;
        }
        std::lock_guard lock {self->feedback_mutex};
        self->rumble_stop_task = nullptr;
        self->stop_rumble_locked("no HID refresh"sv);
      }, rumble_hold).task_id;
    }

    static void __cdecl on_rumble(void *context, UCHAR left_motor, UCHAR right_motor) {
      auto *self = static_cast<impl *>(context);
      const auto low = static_cast<std::uint16_t>(left_motor) << 8;
      const auto high = static_cast<std::uint16_t>(right_motor) << 8;
      std::lock_guard lock {self->feedback_mutex};
      if (self->has_rumble && self->last_rumble.data.rumble.lowfreq == low && self->last_rumble.data.rumble.highfreq == high) {
        if (low != 0 || high != 0) {
          self->schedule_rumble_stop();
        }
        return;
      }
      auto msg = gamepad_feedback_msg_t::make_rumble(self->client_index, low, high);
      self->last_rumble = msg;
      self->has_rumble = true;
      self->raise(msg);
      if (low == 0 && high == 0) {
        self->cancel_rumble_stop();
      } else {
        self->schedule_rumble_stop();
      }
    }

    static void __cdecl on_lightbar(void *context, UCHAR red, UCHAR green, UCHAR blue) {
      auto *self = static_cast<impl *>(context);
      std::lock_guard lock {self->feedback_mutex};
      if (self->has_rgb && self->last_rgb.data.rgb_led.r == red && self->last_rgb.data.rgb_led.g == green && self->last_rgb.data.rgb_led.b == blue) {
        return;
      }
      auto msg = gamepad_feedback_msg_t::make_rgb_led(self->client_index, red, green, blue);
      self->last_rgb = msg;
      self->has_rgb = true;
      self->raise(msg);
    }

    static void __cdecl on_player_led(void *context, UCHAR led_value) {
      auto *self = static_cast<impl *>(context);
      self->raise(gamepad_feedback_msg_t::make_player_leds(self->client_index, led_value & 0x1F, 0));
    }

    static void __cdecl on_triggers(void *context, const trigger_effect_t *left, const trigger_effect_t *right) {
      auto *self = static_cast<impl *>(context);
      std::uint8_t flags = 0;
      std::uint8_t type_left = 0;
      std::uint8_t type_right = 0;
      std::array<std::uint8_t, 10> left_data {};
      std::array<std::uint8_t, 10> right_data {};
      if (left) {
        flags |= 0x01;
        type_left = left->Type;
        copy_trigger_effect(left_data, left);
      }
      if (right) {
        flags |= 0x02;
        type_right = right->Type;
        copy_trigger_effect(right_data, right);
      }
      self->raise(gamepad_feedback_msg_t::make_adaptive_triggers(self->client_index, flags, type_left, type_right, left_data, right_data));
    }

    ~impl() {
      {
        std::lock_guard lock {feedback_mutex};
        stop_rumble_locked("pad destroyed"sv);
      }
      if (device && api().ps5_destroy) {
        api().ps5_destroy(device);
        device = nullptr;
      }
    }
  };

  ps5_t::ps5_t():
      impl_ {std::make_shared<impl>()} {}

  ps5_t::~ps5_t() = default;

  std::shared_ptr<ps5_t> ps5_t::create(const gamepad_id_t &id, feedback_queue_t feedback_queue) {
    auto &fns = api();
    if (!fns.ps5) {
      return nullptr;
    }

    auto pad = std::shared_ptr<ps5_t>(new ps5_t());
    pad->impl_->client_index = id.clientRelativeIndex;
    pad->impl_->feedback_queue = std::move(feedback_queue);
    fns.ps5_init(&pad->impl_->report);

    SetLastError(0);
    pad->impl_->device = fns.ps5_create(nullptr, &impl::on_rumble, &impl::on_lightbar, &impl::on_player_led, &impl::on_triggers, pad->impl_.get());
    if (!pad->impl_->device) {
      BOOST_LOG(warning) << "WinUHidPS5Create failed: "sv << GetLastError();
      return nullptr;
    }

    if (!fns.ps5_report(pad->impl_->device, &pad->impl_->report)) {
      BOOST_LOG(warning) << "WinUHidPS5ReportInput failed on create: "sv << GetLastError();
    }

    if (pad->impl_->feedback_queue) {
      pad->impl_->raise(gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_ACCEL, 100));
      pad->impl_->raise(gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_GYRO, 100));
    }

    BOOST_LOG(info) << "Gamepad "sv << id.globalIndex << " will be DualSense (WinUHid)"sv;
    return pad;
  }

  void ps5_t::rebind(feedback_queue_t feedback_queue) {
    std::lock_guard lock {impl_->feedback_mutex};
    impl_->feedback_queue = std::move(feedback_queue);
  }

  void ps5_t::update(const gamepad_state_t &state) {
    if (!impl_ || !impl_->device) {
      return;
    }

    auto &report = impl_->report;
    const auto flags = state.buttonFlags;
    report.ButtonCross = (flags & A) != 0;
    report.ButtonCircle = (flags & B) != 0;
    report.ButtonSquare = (flags & X) != 0;
    report.ButtonTriangle = (flags & Y) != 0;
    report.ButtonL1 = (flags & LEFT_BUTTON) != 0;
    report.ButtonR1 = (flags & RIGHT_BUTTON) != 0;
    report.ButtonL2 = state.lt > 0;
    report.ButtonR2 = state.rt > 0;
    report.ButtonShare = (flags & BACK) != 0;
    report.ButtonOptions = (flags & START) != 0;
    report.ButtonL3 = (flags & LEFT_STICK) != 0;
    report.ButtonR3 = (flags & RIGHT_STICK) != 0;
    report.ButtonHome = (flags & HOME) != 0;
    report.ButtonTouchpad = (flags & TOUCHPAD_BUTTON) != 0;
    report.ButtonMute = (flags & MISC_BUTTON) != 0;
    report.ButtonLeftPaddle = (flags & PADDLE1) != 0;
    report.ButtonRightPaddle = (flags & PADDLE2) != 0;
    report.ButtonLeftFunction = (flags & PADDLE3) != 0;
    report.ButtonRightFunction = (flags & PADDLE4) != 0;
    report.LeftTrigger = state.lt;
    report.RightTrigger = state.rt;
    report.LeftStickX = stick_x(state.lsX);
    report.LeftStickY = stick_y(state.lsY);
    report.RightStickX = stick_x(state.rsX);
    report.RightStickY = stick_y(state.rsY);

    int hat_x = 0;
    int hat_y = 0;
    if (flags & DPAD_LEFT) {
      hat_x = -1;
    } else if (flags & DPAD_RIGHT) {
      hat_x = 1;
    }
    if (flags & DPAD_UP) {
      hat_y = -1;
    } else if (flags & DPAD_DOWN) {
      hat_y = 1;
    }
    api().ps5_hat(&report, hat_x, hat_y);

    if (!api().ps5_report(impl_->device, &report)) {
      BOOST_LOG(debug) << "WinUHidPS5ReportInput failed: "sv << GetLastError();
    }
  }

  void ps5_t::touch(const gamepad_touch_t &touch) {
    if (!impl_ || !impl_->device) {
      return;
    }

    auto &ids = impl_->touch_ids;
    if (touch.eventType == LI_TOUCH_EVENT_CANCEL_ALL) {
      for (std::size_t index = 0; index < ids.size(); ++index) {
        if (ids[index]) {
          api().ps5_touch(&impl_->report, static_cast<UCHAR>(index), FALSE, 0, 0);
          ids[index].reset();
        }
      }
      api().ps5_report(impl_->device, &impl_->report);
      return;
    }

    auto slot = std::ranges::find(ids, touch.pointerId);
    if (touch.eventType == LI_TOUCH_EVENT_DOWN && slot == ids.end()) {
      slot = std::ranges::find_if(ids, [](const auto &id) {
        return !id.has_value();
      });
      if (slot == ids.end()) {
        BOOST_LOG(warning) << "No free WinUHid DualSense touch slots"sv;
        return;
      }
      *slot = touch.pointerId;
    }
    if (slot == ids.end()) {
      return;
    }

    const auto index = static_cast<UCHAR>(std::distance(ids.begin(), slot));
    const auto down = touch.eventType == LI_TOUCH_EVENT_DOWN || touch.eventType == LI_TOUCH_EVENT_MOVE;
    const auto x = static_cast<USHORT>(std::clamp(touch.x, 0.0F, 1.0F) * 1920.0F);
    const auto y = static_cast<USHORT>(std::clamp(touch.y, 0.0F, 1.0F) * 1080.0F);
    api().ps5_touch(&impl_->report, index, down ? TRUE : FALSE, x, y);
    if (touch.eventType == LI_TOUCH_EVENT_UP || touch.eventType == LI_TOUCH_EVENT_CANCEL) {
      slot->reset();
    }
    api().ps5_report(impl_->device, &impl_->report);
  }

  void ps5_t::motion(const gamepad_motion_t &motion) {
    if (!impl_ || !impl_->device) {
      return;
    }

    switch (motion.motionType) {
      case LI_MOTION_TYPE_ACCEL:
        api().ps5_accel(&impl_->report, motion.x, motion.y, motion.z);
        break;
      case LI_MOTION_TYPE_GYRO:
        {
          constexpr auto deg_to_rad = std::numbers::pi_v<float> / 180.0F;
          api().ps5_gyro(&impl_->report, motion.x * deg_to_rad, motion.y * deg_to_rad, motion.z * deg_to_rad);
          break;
        }
      default:
        return;
    }
    api().ps5_report(impl_->device, &impl_->report);
  }

  void ps5_t::battery(const gamepad_battery_t &battery) {
    if (!impl_ || !impl_->device) {
      return;
    }

    if (battery.state == LI_BATTERY_STATE_UNKNOWN || battery.state == LI_BATTERY_STATE_NOT_PRESENT) {
      return;
    }

    const auto wired = battery.state == LI_BATTERY_STATE_CHARGING || battery.state == LI_BATTERY_STATE_FULL;
    const auto percent = battery.percentage == LI_BATTERY_PERCENTAGE_UNKNOWN ? static_cast<UCHAR>(100) : std::min<UCHAR>(battery.percentage, 100);
    api().ps5_battery(&impl_->report, wired ? TRUE : FALSE, percent);
    api().ps5_report(impl_->device, &impl_->report);
  }

  pads_t::pads_t():
      slots(static_cast<std::size_t>(MAX_GAMEPADS)) {}

  int pads_t::alloc(const gamepad_id_t &id, feedback_queue_t feedback_queue) {
    if (id.globalIndex < 0 || id.globalIndex >= static_cast<int>(slots.size())) {
      return -1;
    }
    auto pad = ps5_t::create(id, std::move(feedback_queue));
    if (!pad) {
      return -1;
    }
    slots[id.globalIndex] = std::move(pad);
    return 0;
  }

  int pads_t::rebind(const gamepad_id_t &id, feedback_queue_t feedback_queue) {
    if (!has(id.globalIndex)) {
      return -1;
    }
    slots[id.globalIndex]->rebind(std::move(feedback_queue));
    return 0;
  }

  bool pads_t::has(int nr) const {
    return nr >= 0 && nr < static_cast<int>(slots.size()) && slots[nr] != nullptr;
  }

  void pads_t::free(int nr) {
    if (has(nr)) {
      slots[nr].reset();
    }
  }

  ps5_t *pads_t::get(int nr) {
    return has(nr) ? slots[nr].get() : nullptr;
  }

}  // namespace platf::winuhid
