/**
 * @file src/platform/linux/gamescopepipewire.cpp
 * @brief Definitions for the Gamescope PipeWire capture backend.
 */
// local includes
#include "pipewire.cpp"

// standard includes
#include <cstdio>

namespace gamescope_pipewire {
  constexpr std::string_view TARGET_OBJECT = "gamescope";
  constexpr int WIDTH = 1920;
  constexpr int HEIGHT = 1080;

  /**
   * @brief Native PipeWire display backend for Gamescope.
   * @details Captures the global Gamescope compositor source published on PipeWire.
   */
  class gamescope_t: public pipewire::pipewire_display_t {
  public:
    /**
     * @brief Skip the Wayland DMA-BUF modifier query.
     *
     * @return 0 on success.
     */
    int get_dmabuf_modifiers() override {
      return 0;
    }

    /**
     * @brief Configure the stream.
     *
     * @param display_name Display name.
     * @param out_pipewire_fd Out pipewire fd.
     * @param out_pipewire_node Out pipewire node.
     * @param out_pipewire_object_serial Out pipewire object serial.
     * @return 0 on success.
     */
    int configure_stream(const std::string &display_name, int &out_pipewire_fd, uint32_t &out_pipewire_node, uint64_t &out_pipewire_object_serial) override {
      boost::ignore_unused(display_name);

      int native_width = WIDTH;
      int native_height = HEIGHT;
      if (auto *probe = popen("pw-dump 2>/dev/null | python3 -c 'import json,sys\nd=json.load(sys.stdin)\nfor o in d:\n  p=o.get(\"info\",{}).get(\"props\",{})\n  if p.get(\"node.name\")==\"gamescope\":\n    e=o[\"info\"][\"params\"][\"EnumFormat\"][0]\n    print(e[\"size\"][\"width\"], e[\"size\"][\"height\"])\n    break'", "r"); probe) {
        if (std::fscanf(probe, "%d %d", &native_width, &native_height) != 2 || native_width <= 0 || native_height <= 0) {
          native_width = WIDTH;
          native_height = HEIGHT;
        }
        if (pclose(probe) != 0) {
          BOOST_LOG(warning) << "[gamescope-pipewire] Could not query the gamescope node's native size, falling back to "sv << WIDTH << 'x' << HEIGHT;
          native_width = WIDTH;
          native_height = HEIGHT;
        }
      }

      pipewire::pipewire_t::target_object_name = TARGET_OBJECT;
      out_pipewire_fd = -1;
      out_pipewire_node = 1;  // Node id is not needed when targeting a named object.
      out_pipewire_object_serial = SPA_ID_INVALID;

      width = native_width;
      height = native_height;
      offset_x = 0;
      offset_y = 0;
      logical_width = native_width;
      logical_height = native_height;
      env_width = native_width;
      env_height = native_height;
      env_logical_width = native_width;
      env_logical_height = native_height;

      BOOST_LOG(info) << "[gamescope-pipewire] Using native size "sv << width << 'x' << height;

      return 0;
    }
  };
}  // namespace gamescope_pipewire

namespace platf {
  /**
   * @brief Enumerate displays available through the Gamescope PipeWire backend.
   *
   * @return Gamescope display names.
   */
  std::vector<std::string> gamescope_pipewire_display_names() {
    return {"gamescope"};
  }

  /**
   * @brief Create the Gamescope PipeWire display backend.
   *
   * @param hwdevice_type Hardware device type requested for capture or encode.
   * @param display_name Display name.
   * @param config Configuration values to apply.
   * @return Display backend, or nullptr when initialization fails.
   */
  std::shared_ptr<display_t> gamescope_pipewire_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (!pipewire::pipewire_display_t::init_pipewire_and_check_hwdevice_type(hwdevice_type)) {
      BOOST_LOG(error) << "[gamescope-pipewire] Could not initialize pipewire-based display with the given hw device type."sv;
      return nullptr;
    }

    auto display = std::make_shared<gamescope_pipewire::gamescope_t>();
    if (display->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }

    return display;
  }
}  // namespace platf
