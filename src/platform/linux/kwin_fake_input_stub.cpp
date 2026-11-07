/**
 * @file src/platform/linux/kwin_fake_input_stub.cpp
 * @brief Fallback when KWin support is not built: no KWin fake-input sink.
 */
#include "src/platform/linux/session_input.h"

namespace platf {
  std::unique_ptr<session_input_t> session_input::alloc_kwin_fake_input(const std::string &) {
    return nullptr;
  }
}  // namespace platf
