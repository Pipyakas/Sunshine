/**
 * @file src/platform/linux/gamescopepipewire.cpp
 * @brief Definitions for the Gamescope PipeWire capture backend.
 */
// local includes
#include "pipewire.cpp"

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

      pipewire::pipewire_t::target_object_name = TARGET_OBJECT;
      out_pipewire_fd = -1;
      out_pipewire_node = 1;  // Node id is not needed when targeting a named object.
      out_pipewire_object_serial = SPA_ID_INVALID;

      width = WIDTH;
      height = HEIGHT;
      offset_x = 0;
      offset_y = 0;
      logical_width = WIDTH;
      logical_height = HEIGHT;
      env_width = WIDTH;
      env_height = HEIGHT;
      env_logical_width = WIDTH;
      env_logical_height = HEIGHT;

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
