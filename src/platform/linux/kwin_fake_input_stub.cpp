/**
 * @file src/platform/linux/kwin_fake_input_stub.cpp
 * @brief Fallback when KWin support is not built: no per-session compositor input.
 */
#include "src/platform/common.h"

namespace platf {
  std::unique_ptr<session_input_t> alloc_session_input(const std::string &) {
    return nullptr;
  }
}  // namespace platf
