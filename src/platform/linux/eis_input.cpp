/**
 * @file src/platform/linux/eis_input.cpp
 * @brief Per-session input injection into an EIS server (e.g. gamescope) through libei.
 *
 * gamescope exposes an EIS socket (gamescope-0-ei) that feeds input straight into its
 * Wayland seat. A gamescope instance on a seat without input devices (multiseat) only
 * receives input this way. libei is loaded at runtime so the build has no hard
 * dependency on it.
 */

// standard includes
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>

// platform includes
#include <dlfcn.h>
#include <poll.h>

// local includes
#include "src/logging.h"
#include "src/platform/linux/session_input.h"

using namespace std::literals;

// Opaque libei types; only pointers are used.
struct ei;
struct ei_event;
struct ei_seat;
struct ei_device;

namespace platf {
  namespace {
    // Values from libei.h (stable ABI).
    constexpr int EI_EVENT_CONNECT = 1;
    constexpr int EI_EVENT_DISCONNECT = 2;
    constexpr int EI_EVENT_SEAT_ADDED = 3;
    constexpr int EI_EVENT_DEVICE_ADDED = 5;
    constexpr int EI_EVENT_DEVICE_REMOVED = 6;
    constexpr int EI_EVENT_DEVICE_PAUSED = 7;
    constexpr int EI_EVENT_DEVICE_RESUMED = 8;

    constexpr int EI_DEVICE_CAP_POINTER = 1 << 0;
    constexpr int EI_DEVICE_CAP_POINTER_ABSOLUTE = 1 << 1;
    constexpr int EI_DEVICE_CAP_KEYBOARD = 1 << 2;
    constexpr int EI_DEVICE_CAP_SCROLL = 1 << 4;
    constexpr int EI_DEVICE_CAP_BUTTON = 1 << 5;

    /**
     * @brief libei entry points, resolved once from libei.so.1.
     */
    struct libei_t {
      ei *(*new_sender)(void *user_data);
      void (*configure_name)(ei *, const char *);
      int (*setup_backend_socket)(ei *, const char *);
      int (*get_fd)(ei *);
      void (*dispatch)(ei *);
      ei_event *(*get_event)(ei *);
      int (*event_get_type)(ei_event *);
      ei_seat *(*event_get_seat)(ei_event *);
      ei_device *(*event_get_device)(ei_event *);
      ei_event *(*event_unref)(ei_event *);
      void (*seat_bind_capabilities)(ei_seat *, ...);
      bool (*device_has_capability)(ei_device *, int);
      ei_device *(*device_ref)(ei_device *);
      ei_device *(*device_unref)(ei_device *);
      void (*device_start_emulating)(ei_device *, std::uint32_t);
      void (*device_pointer_motion)(ei_device *, double, double);
      void (*device_pointer_motion_absolute)(ei_device *, double, double);
      void (*device_button_button)(ei_device *, std::uint32_t, bool);
      void (*device_scroll_discrete)(ei_device *, std::int32_t, std::int32_t);
      void (*device_keyboard_key)(ei_device *, std::uint32_t, bool);
      void (*device_frame)(ei_device *, std::uint64_t);
      std::uint64_t (*now)(ei *);
      ei *(*unref)(ei *);
    };

    template<typename T>
    bool resolve(void *handle, T &fn, const char *name) {
      fn = reinterpret_cast<T>(dlsym(handle, name));
      if (!fn) {
        BOOST_LOG(warning) << "[eis input] libei lacks "sv << name;
        return false;
      }
      return true;
    }

    const libei_t *load_libei() {
      static std::once_flag once;
      static std::optional<libei_t> lib;
      std::call_once(once, [] {
        void *handle = dlopen("libei.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
          BOOST_LOG(warning) << "[eis input] cannot load libei.so.1: "sv << dlerror();
          return;
        }
        libei_t l {};
        bool ok = resolve(handle, l.new_sender, "ei_new_sender") &&
                  resolve(handle, l.configure_name, "ei_configure_name") &&
                  resolve(handle, l.setup_backend_socket, "ei_setup_backend_socket") &&
                  resolve(handle, l.get_fd, "ei_get_fd") &&
                  resolve(handle, l.dispatch, "ei_dispatch") &&
                  resolve(handle, l.get_event, "ei_get_event") &&
                  resolve(handle, l.event_get_type, "ei_event_get_type") &&
                  resolve(handle, l.event_get_seat, "ei_event_get_seat") &&
                  resolve(handle, l.event_get_device, "ei_event_get_device") &&
                  resolve(handle, l.event_unref, "ei_event_unref") &&
                  resolve(handle, l.seat_bind_capabilities, "ei_seat_bind_capabilities") &&
                  resolve(handle, l.device_has_capability, "ei_device_has_capability") &&
                  resolve(handle, l.device_ref, "ei_device_ref") &&
                  resolve(handle, l.device_unref, "ei_device_unref") &&
                  resolve(handle, l.device_start_emulating, "ei_device_start_emulating") &&
                  resolve(handle, l.device_pointer_motion, "ei_device_pointer_motion") &&
                  resolve(handle, l.device_pointer_motion_absolute, "ei_device_pointer_motion_absolute") &&
                  resolve(handle, l.device_button_button, "ei_device_button_button") &&
                  resolve(handle, l.device_scroll_discrete, "ei_device_scroll_discrete") &&
                  resolve(handle, l.device_keyboard_key, "ei_device_keyboard_key") &&
                  resolve(handle, l.device_frame, "ei_device_frame") &&
                  resolve(handle, l.now, "ei_now") &&
                  resolve(handle, l.unref, "ei_unref");
        if (!ok) {
          dlclose(handle);
          return;
        }
        lib = l;
      });
      return lib ? &*lib : nullptr;
    }

    std::string socket_path(const std::string &socket) {
      if (!socket.empty() && socket.front() == '/') {
        return socket;
      }
      const char *runtime = std::getenv("XDG_RUNTIME_DIR");
      return std::string {runtime ? runtime : ""} + '/' + socket;
    }

    class eis_input_t: public session_input_t {
    public:
      eis_input_t(const libei_t *lib, std::string socket):
          _lib {lib},
          _socket {std::move(socket)} {
      }

      ~eis_input_t() override {
        std::lock_guard lg {_mutex};
        disconnect();
      }

      void connect_now() {
        std::lock_guard lg {_mutex};
        ensure_connected();
      }

      void move_mouse(int delta_x, int delta_y) override {
        with_device(EI_DEVICE_CAP_POINTER, [&](ei_device *d) {
          _lib->device_pointer_motion(d, delta_x, delta_y);
        });
      }

      void abs_mouse(const touch_port_t &, float x, float y) override {
        // gamescope maps absolute coordinates onto the focused surface, which is
        // the client-sized output in this setup.
        with_device(EI_DEVICE_CAP_POINTER_ABSOLUTE, [&](ei_device *d) {
          _lib->device_pointer_motion_absolute(d, x, y);
        });
      }

      void button_mouse(int button, bool release) override {
        const int code = session_input::moonlight_button_to_evdev(button);
        if (code < 0) {
          return;
        }
        with_device(EI_DEVICE_CAP_BUTTON, [&](ei_device *d) {
          _lib->device_button_button(d, static_cast<std::uint32_t>(code), !release);
        });
      }

      void scroll(int high_res_distance) override {
        // Both use 120 units per notch. Moonlight: positive = up; libei: positive = down.
        with_device(EI_DEVICE_CAP_SCROLL, [&](ei_device *d) {
          _lib->device_scroll_discrete(d, 0, -high_res_distance);
        });
      }

      void hscroll(int high_res_distance) override {
        with_device(EI_DEVICE_CAP_SCROLL, [&](ei_device *d) {
          _lib->device_scroll_discrete(d, high_res_distance, 0);
        });
      }

      void keyboard_update(std::uint16_t modcode, bool release, std::uint8_t) override {
        const int code = session_input::vk_to_evdev(modcode);
        if (code < 0) {
          BOOST_LOG(debug) << "[eis input] unmapped virtual key 0x"sv << std::hex << modcode << std::dec;
          return;
        }
        with_device(EI_DEVICE_CAP_KEYBOARD, [&](ei_device *d) {
          _lib->device_keyboard_key(d, static_cast<std::uint32_t>(code), !release);
        });
      }

    private:
      template<typename F>
      void with_device(int capability, F &&f) {
        std::lock_guard lg {_mutex};
        if (!ensure_connected()) {
          return;
        }
        pump(0);
        if (!_ei || !_device || !_resumed || !_lib->device_has_capability(_device, capability)) {
          return;
        }
        if (!_emulating) {
          _lib->device_start_emulating(_device, ++_sequence);
          _emulating = true;
        }
        f(_device);
        _lib->device_frame(_device, _lib->now(_ei));
        // libei buffers until the next dispatch; flush the frame now.
        _lib->dispatch(_ei);
      }

      /**
       * @brief Process pending libei events, waiting up to timeout_ms for new ones.
       */
      void pump(int timeout_ms) {
        if (!_ei) {
          return;
        }
        pollfd pfd {_lib->get_fd(_ei), POLLIN, 0};
        if (poll(&pfd, 1, timeout_ms) > 0) {
          _lib->dispatch(_ei);
        }
        while (_ei) {
          ei_event *event = _lib->get_event(_ei);
          if (!event) {
            break;
          }
          handle_event(event);
          _lib->event_unref(event);
        }
      }

      void handle_event(ei_event *event) {
        switch (_lib->event_get_type(event)) {
          case EI_EVENT_SEAT_ADDED:
            _lib->seat_bind_capabilities(
              _lib->event_get_seat(event),
              EI_DEVICE_CAP_POINTER,
              EI_DEVICE_CAP_POINTER_ABSOLUTE,
              EI_DEVICE_CAP_KEYBOARD,
              EI_DEVICE_CAP_SCROLL,
              EI_DEVICE_CAP_BUTTON,
              nullptr
            );
            break;
          case EI_EVENT_DEVICE_ADDED:
            if (!_device) {
              _device = _lib->device_ref(_lib->event_get_device(event));
              _resumed = false;
              _emulating = false;
            }
            break;
          case EI_EVENT_DEVICE_RESUMED:
            if (_lib->event_get_device(event) == _device) {
              _resumed = true;
              _emulating = false;
            }
            break;
          case EI_EVENT_DEVICE_PAUSED:
            if (_lib->event_get_device(event) == _device) {
              _resumed = false;
              _emulating = false;
            }
            break;
          case EI_EVENT_DEVICE_REMOVED:
            if (_lib->event_get_device(event) == _device) {
              _lib->device_unref(_device);
              _device = nullptr;
              _resumed = false;
              _emulating = false;
            }
            break;
          case EI_EVENT_DISCONNECT:
            BOOST_LOG(warning) << "[eis input] "sv << _socket << " disconnected"sv;
            _disconnected = true;
            break;
          default:
            break;
        }
      }

      bool ensure_connected() {
        if (_ei && !_disconnected) {
          return true;
        }
        disconnect();

        const auto now = std::chrono::steady_clock::now();
        if (_last_attempt && now - *_last_attempt < 1s) {
          return false;
        }
        _last_attempt = now;

        _ei = _lib->new_sender(nullptr);
        if (!_ei) {
          return false;
        }
        _lib->configure_name(_ei, "Sunshine");
        const auto path = socket_path(_socket);
        if (const int err = _lib->setup_backend_socket(_ei, path.c_str()); err != 0) {
          BOOST_LOG(warning) << "[eis input] cannot connect to "sv << path << ": "sv << std::strerror(-err);
          disconnect();
          return false;
        }

        // Wait (briefly) for the handshake, seat and a resumed device.
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (_ei && !_disconnected && !_resumed && std::chrono::steady_clock::now() < deadline) {
          pump(50);
        }
        if (_disconnected) {
          BOOST_LOG(warning) << "[eis input] "sv << path << " rejected the connection"sv;
          disconnect();
          return false;
        }
        if (_resumed) {
          BOOST_LOG(info) << "[eis input] injecting session input into "sv << path;
        } else {
          BOOST_LOG(info) << "[eis input] connected to "sv << path << ", waiting for a device"sv;
        }
        return true;
      }

      void disconnect() {
        if (_device) {
          _lib->device_unref(_device);
          _device = nullptr;
        }
        if (_ei) {
          _lib->unref(_ei);
          _ei = nullptr;
        }
        _resumed = false;
        _emulating = false;
        _disconnected = false;
      }

      const libei_t *_lib;
      std::string _socket;
      std::mutex _mutex;
      ei *_ei {};
      ei_device *_device {};
      bool _resumed {};
      bool _emulating {};
      bool _disconnected {};
      std::uint32_t _sequence {};
      std::optional<std::chrono::steady_clock::time_point> _last_attempt;
    };
  }  // namespace

  std::unique_ptr<session_input_t> session_input::alloc_eis_input(const std::string &socket) {
    if (socket.empty()) {
      return nullptr;
    }
    const auto *lib = load_libei();
    if (!lib) {
      return nullptr;
    }
    auto sink = std::make_unique<eis_input_t>(lib, socket);
    sink->connect_now();
    return sink;
  }

  namespace {
    /**
     * @brief Sink that drops everything, used when an app's own input target is unusable.
     */
    class discard_input_t: public session_input_t {
    public:
      void move_mouse(int, int) override {}
      void abs_mouse(const touch_port_t &, float, float) override {}
      void button_mouse(int, bool) override {}
      void scroll(int) override {}
      void hscroll(int) override {}
      void keyboard_update(std::uint16_t, bool, std::uint8_t) override {}
    };
  }  // namespace

  std::unique_ptr<session_input_t> alloc_session_input(const std::string &target) {
    // "eis:<socket>" -> libei; "kwin:<socket>" or a bare socket name -> KWin fake-input.
    std::unique_ptr<session_input_t> sink;
    if (target.starts_with("eis:")) {
      sink = session_input::alloc_eis_input(target.substr(4));
    } else if (target.starts_with("kwin:")) {
      sink = session_input::alloc_kwin_fake_input(target.substr(5));
    } else {
      sink = session_input::alloc_kwin_fake_input(target);
    }
    if (!sink) {
      // Falling back to the shared devices would send this session's input to another session.
      BOOST_LOG(error) << "No input path for ["sv << target << "]; this stream's mouse and keyboard are dropped"sv;
      sink = std::make_unique<discard_input_t>();
    }
    return sink;
  }
}  // namespace platf
