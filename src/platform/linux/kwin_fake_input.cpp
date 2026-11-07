/**
 * @file src/platform/linux/kwin_fake_input.cpp
 * @brief Per-session input injection into a KWin compositor via org_kde_kwin_fake_input.
 *
 * A headless KWin (`kwin_wayland --virtual`) has no libinput seat, so it never sees
 * the shared virtual HID devices (those are consumed by whatever owns the real seat,
 * e.g. gamescope). Sessions streaming such a compositor inject their mouse and
 * keyboard input over its Wayland socket instead. The compositor must allow the
 * interface (KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 or an X-KDE-Wayland-Interfaces grant).
 */

// standard includes
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

// lib includes
#include <linux/input-event-codes.h>
#include <wayland-client.h>

// generated protocol
#include <fake-input.h>

// local includes
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/linux/session_input.h"

using namespace std::literals;

namespace platf {
  namespace {
    using session_input::moonlight_button_to_evdev;
    using session_input::vk_to_evdev;

    constexpr std::uint32_t AXIS_VERTICAL = 0;
    constexpr std::uint32_t AXIS_HORIZONTAL = 1;
    // Moonlight sends 120 units per wheel notch; a Wayland axis notch is 15 units.
    constexpr double WHEEL_UNITS_PER_AXIS_UNIT = 120.0 / 15.0;

    class kwin_fake_input_t: public session_input_t {
    public:
      explicit kwin_fake_input_t(std::string socket):
          _socket {std::move(socket)} {
      }

      ~kwin_fake_input_t() override {
        std::lock_guard lg {_mutex};
        disconnect();
      }

      bool connect_now() {
        std::lock_guard lg {_mutex};
        return ensure_connected();
      }

      void move_mouse(int delta_x, int delta_y) override {
        with_input([&](org_kde_kwin_fake_input *fi) {
          org_kde_kwin_fake_input_pointer_motion(fi, wl_fixed_from_int(delta_x), wl_fixed_from_int(delta_y));
        });
      }

      void abs_mouse(const touch_port_t &, float x, float y) override {
        with_input([&](org_kde_kwin_fake_input *fi) {
          org_kde_kwin_fake_input_pointer_motion_absolute(fi, wl_fixed_from_double(x), wl_fixed_from_double(y));
        });
      }

      void button_mouse(int button, bool release) override {
        const int code = moonlight_button_to_evdev(button);
        if (code < 0) {
          return;
        }
        with_input([&](org_kde_kwin_fake_input *fi) {
          org_kde_kwin_fake_input_button(fi, static_cast<std::uint32_t>(code), release ? WL_POINTER_BUTTON_STATE_RELEASED : WL_POINTER_BUTTON_STATE_PRESSED);
        });
      }

      void scroll(int high_res_distance) override {
        // Moonlight: positive = wheel up. Wayland: positive = scroll down.
        axis(AXIS_VERTICAL, -high_res_distance / WHEEL_UNITS_PER_AXIS_UNIT);
      }

      void hscroll(int high_res_distance) override {
        axis(AXIS_HORIZONTAL, high_res_distance / WHEEL_UNITS_PER_AXIS_UNIT);
      }

      void keyboard_update(std::uint16_t modcode, bool release, std::uint8_t) override {
        const int code = vk_to_evdev(modcode);
        if (code < 0) {
          BOOST_LOG(debug) << "[kwin fake-input] unmapped virtual key 0x"sv << std::hex << modcode << std::dec;
          return;
        }
        with_input([&](org_kde_kwin_fake_input *fi) {
          org_kde_kwin_fake_input_keyboard_key(fi, static_cast<std::uint32_t>(code), release ? WL_KEYBOARD_KEY_STATE_RELEASED : WL_KEYBOARD_KEY_STATE_PRESSED);
        });
      }

    private:
      void axis(std::uint32_t which, double value) {
        with_input([&](org_kde_kwin_fake_input *fi) {
          org_kde_kwin_fake_input_axis(fi, which, wl_fixed_from_double(value));
        });
      }

      template<typename F>
      void with_input(F &&f) {
        std::lock_guard lg {_mutex};
        if (!ensure_connected()) {
          return;
        }
        f(_fake_input);
        if (wl_display_flush(_display) < 0 && errno != EAGAIN) {
          BOOST_LOG(warning) << "[kwin fake-input] lost connection to "sv << _socket << ": "sv << std::strerror(errno);
          disconnect();
          return;
        }
        // Drain any events (errors, delete_id) so the socket never backs up.
        wl_display_dispatch_pending(_display);
        if (wl_display_get_error(_display) != 0) {
          BOOST_LOG(warning) << "[kwin fake-input] protocol error on "sv << _socket;
          disconnect();
        }
      }

      static void registry_global(void *data, wl_registry *registry, std::uint32_t name, const char *interface, std::uint32_t version) {
        auto *self = static_cast<kwin_fake_input_t *>(data);
        if (std::strcmp(interface, org_kde_kwin_fake_input_interface.name) == 0) {
          const auto bind_version = std::min<std::uint32_t>(version, 4);
          self->_fake_input = static_cast<org_kde_kwin_fake_input *>(
            wl_registry_bind(registry, name, &org_kde_kwin_fake_input_interface, bind_version)
          );
          self->_version = bind_version;
        }
      }

      static void registry_global_remove(void *, wl_registry *, std::uint32_t) {}

      bool ensure_connected() {
        if (_fake_input) {
          return true;
        }

        const auto now = std::chrono::steady_clock::now();
        if (_last_attempt && now - *_last_attempt < 1s) {
          return false;
        }
        _last_attempt = now;

        _display = wl_display_connect(_socket.c_str());
        if (!_display) {
          BOOST_LOG(warning) << "[kwin fake-input] cannot connect to Wayland socket "sv << _socket;
          return false;
        }

        static const wl_registry_listener listener {
          .global = registry_global,
          .global_remove = registry_global_remove,
        };
        _registry = wl_display_get_registry(_display);
        wl_registry_add_listener(_registry, &listener, this);
        wl_display_roundtrip(_display);

        if (!_fake_input) {
          BOOST_LOG(warning) << "[kwin fake-input] "sv << _socket << " does not offer org_kde_kwin_fake_input"sv;
          disconnect();
          return false;
        }

        org_kde_kwin_fake_input_authenticate(_fake_input, "Sunshine", "Remote input from a Moonlight streaming session");
        wl_display_roundtrip(_display);
        if (wl_display_get_error(_display) != 0) {
          BOOST_LOG(warning) << "[kwin fake-input] "sv << _socket << " rejected fake input"sv;
          disconnect();
          return false;
        }

        if (_version < 4) {
          BOOST_LOG(warning) << "[kwin fake-input] "sv << _socket << " only offers v"sv << _version << "; keyboard input unavailable"sv;
        }
        BOOST_LOG(info) << "[kwin fake-input] injecting session input into "sv << _socket;
        return true;
      }

      void disconnect() {
        if (_fake_input) {
          org_kde_kwin_fake_input_destroy(_fake_input);
          _fake_input = nullptr;
        }
        if (_registry) {
          wl_registry_destroy(_registry);
          _registry = nullptr;
        }
        if (_display) {
          wl_display_disconnect(_display);
          _display = nullptr;
        }
      }

      std::string _socket;
      std::mutex _mutex;
      wl_display *_display {};
      wl_registry *_registry {};
      org_kde_kwin_fake_input *_fake_input {};
      std::uint32_t _version {};
      std::optional<std::chrono::steady_clock::time_point> _last_attempt;
    };
  }  // namespace

  std::unique_ptr<session_input_t> session_input::alloc_kwin_fake_input(const std::string &wayland_display) {
    if (wayland_display.empty()) {
      return nullptr;
    }
    auto sink = std::make_unique<kwin_fake_input_t>(wayland_display);
    // Connect eagerly so problems show up in the log at session start; the sink
    // still retries lazily if the compositor is not up yet.
    sink->connect_now();
    return sink;
  }
}  // namespace platf
