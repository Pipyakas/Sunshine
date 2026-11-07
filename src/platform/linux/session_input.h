/**
 * @file src/platform/linux/session_input.h
 * @brief Per-session compositor input sinks (KWin fake-input, libei/EIS) and their shared key mapping.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

// lib includes
#include <linux/input-event-codes.h>

// local includes
#include "src/platform/common.h"

namespace platf::session_input {
  /**
   * @brief Map a Windows virtual-key code (as sent by Moonlight) to a Linux evdev key code.
   */
  inline int vk_to_evdev(std::uint16_t vk) {
    static constexpr std::array<std::pair<std::uint16_t, int>, 50> special {{
      {0x08, KEY_BACKSPACE},
      {0x09, KEY_TAB},
      {0x0C, KEY_KP5},
      {0x0D, KEY_ENTER},
      {0x10, KEY_LEFTSHIFT},
      {0xA0, KEY_LEFTSHIFT},
      {0x11, KEY_LEFTCTRL},
      {0xA2, KEY_LEFTCTRL},
      {0x12, KEY_LEFTALT},
      {0xA4, KEY_LEFTALT},
      {0x13, KEY_PAUSE},
      {0x14, KEY_CAPSLOCK},
      {0x1B, KEY_ESC},
      {0x20, KEY_SPACE},
      {0x21, KEY_PAGEUP},
      {0x22, KEY_PAGEDOWN},
      {0x23, KEY_END},
      {0x24, KEY_HOME},
      {0x25, KEY_LEFT},
      {0x26, KEY_UP},
      {0x27, KEY_RIGHT},
      {0x28, KEY_DOWN},
      {0x2C, KEY_SYSRQ},
      {0x2D, KEY_INSERT},
      {0x2E, KEY_DELETE},
      {0x5B, KEY_LEFTMETA},
      {0x5C, KEY_RIGHTMETA},
      {0x5D, KEY_COMPOSE},
      {0x6A, KEY_KPASTERISK},
      {0x6B, KEY_KPPLUS},
      {0x6D, KEY_KPMINUS},
      {0x6E, KEY_KPDOT},
      {0x6F, KEY_KPSLASH},
      {0x90, KEY_NUMLOCK},
      {0x91, KEY_SCROLLLOCK},
      {0xA1, KEY_RIGHTSHIFT},
      {0xA3, KEY_RIGHTCTRL},
      {0xA5, KEY_RIGHTALT},
      {0xBA, KEY_SEMICOLON},
      {0xBB, KEY_EQUAL},
      {0xBC, KEY_COMMA},
      {0xBD, KEY_MINUS},
      {0xBE, KEY_DOT},
      {0xBF, KEY_SLASH},
      {0xC0, KEY_GRAVE},
      {0xDB, KEY_LEFTBRACE},
      {0xDC, KEY_BACKSLASH},
      {0xDD, KEY_RIGHTBRACE},
      {0xDE, KEY_APOSTROPHE},
      {0xE2, KEY_102ND},
    }};

    for (const auto &[from, to] : special) {
      if (from == vk) {
        return to;
      }
    }

    if (vk >= 0x30 && vk <= 0x39) {
      static constexpr std::array digits {KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9};
      return digits[vk - 0x30];
    }
    if (vk >= 0x41 && vk <= 0x5A) {
      static constexpr std::array letters {
        KEY_A,
        KEY_B,
        KEY_C,
        KEY_D,
        KEY_E,
        KEY_F,
        KEY_G,
        KEY_H,
        KEY_I,
        KEY_J,
        KEY_K,
        KEY_L,
        KEY_M,
        KEY_N,
        KEY_O,
        KEY_P,
        KEY_Q,
        KEY_R,
        KEY_S,
        KEY_T,
        KEY_U,
        KEY_V,
        KEY_W,
        KEY_X,
        KEY_Y,
        KEY_Z
      };
      return letters[vk - 0x41];
    }
    if (vk >= 0x60 && vk <= 0x69) {
      static constexpr std::array keypad {KEY_KP0, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP4, KEY_KP5, KEY_KP6, KEY_KP7, KEY_KP8, KEY_KP9};
      return keypad[vk - 0x60];
    }
    if (vk >= 0x70 && vk <= 0x87) {
      static constexpr std::array fkeys {
        KEY_F1,
        KEY_F2,
        KEY_F3,
        KEY_F4,
        KEY_F5,
        KEY_F6,
        KEY_F7,
        KEY_F8,
        KEY_F9,
        KEY_F10,
        KEY_F11,
        KEY_F12,
        KEY_F13,
        KEY_F14,
        KEY_F15,
        KEY_F16,
        KEY_F17,
        KEY_F18,
        KEY_F19,
        KEY_F20,
        KEY_F21,
        KEY_F22,
        KEY_F23,
        KEY_F24
      };
      return fkeys[vk - 0x70];
    }
    return -1;
  }

  /**
   * @brief Map a Moonlight mouse button (BUTTON_LEFT..BUTTON_X2) to a Linux button code.
   */
  inline int moonlight_button_to_evdev(int button) {
    switch (button) {
      case 1:
        return BTN_LEFT;
      case 2:
        return BTN_MIDDLE;
      case 3:
        return BTN_RIGHT;
      case 4:
        return BTN_SIDE;
      case 5:
        return BTN_EXTRA;
      default:
        return -1;
    }
  }

  /**
   * @brief Inject into a KWin compositor through org_kde_kwin_fake_input.
   * @param wayland_display Wayland socket name (e.g. wayland-desk).
   * @return Sink, or nullptr when KWin support is not built.
   */
  std::unique_ptr<session_input_t> alloc_kwin_fake_input(const std::string &wayland_display);

  /**
   * @brief Inject into an EIS server (e.g. gamescope's gamescope-0-ei) through libei.
   * @param socket EIS socket name (relative to XDG_RUNTIME_DIR) or absolute path.
   * @return Sink, or nullptr when libei is unavailable.
   */
  std::unique_ptr<session_input_t> alloc_eis_input(const std::string &socket);
}  // namespace platf::session_input
