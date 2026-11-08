/**
 * @file src/process.cpp
 * @brief Definitions for the startup and shutdown of the apps started by a streaming Session.
 */
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

// lib includes
#include <boost/algorithm/string.hpp>
#include <boost/crc.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/token_functions.hpp>
#include <openssl/evp.h>
#include <openssl/sha.h>

// local includes
#include "config.h"
#include "crypto.h"
#include "display_device.h"
#include "input.h"
#include "logging.h"
#include "platform/common.h"
#include "process.h"
#include "system_tray.h"
#include "utility.h"

#ifdef _WIN32
  // from_utf8() string conversion function
  #include "platform/windows/utf_utils.h"

  // _SH constants for _wfsopen()
  #include <share.h>
#endif

namespace proc {
  using namespace std::literals;
  namespace pt = boost::property_tree;

  proc_t proc;  ///< Global process registry used to track and terminate child processes.

  /**
   * @brief RAII helper that runs shutdown cleanup when destroyed.
   */
  class deinit_t: public platf::deinit_t {
  public:
    /**
     * @brief Destroy the process subsystem deinitializer.
     */
    ~deinit_t() {
      proc.terminate();
    }
  };

  std::unique_ptr<platf::deinit_t> init() {
    return std::make_unique<deinit_t>();
  }

  void terminate_process_group(boost::process::v1::child &proc, boost::process::v1::group &group, std::chrono::seconds exit_timeout) {
    if (group.valid() && platf::process_group_running((std::uintptr_t) group.native_handle())) {
      if (exit_timeout.count() > 0) {
        // Request processes in the group to exit gracefully
        if (platf::request_process_group_exit((std::uintptr_t) group.native_handle())) {
          // If the request was successful, wait for a little while for them to exit.
          BOOST_LOG(info) << "Successfully requested the app to exit. Waiting up to "sv << exit_timeout.count() << " seconds for it to close."sv;

          // group::wait_for() and similar functions are broken and deprecated, so we use a simple polling loop
          while (platf::process_group_running((std::uintptr_t) group.native_handle()) && (--exit_timeout).count() >= 0) {
            std::this_thread::sleep_for(1s);
          }

          if (exit_timeout.count() < 0) {
            BOOST_LOG(warning) << "App did not fully exit within the timeout. Terminating the app's remaining processes."sv;
          } else {
            BOOST_LOG(info) << "All app processes have successfully exited."sv;
          }
        } else {
          BOOST_LOG(info) << "App did not respond to a graceful termination request. Forcefully terminating the app's processes."sv;
        }
      } else {
        BOOST_LOG(info) << "No graceful exit timeout was specified for this app. Forcefully terminating the app's processes."sv;
      }

      // We always call terminate() even if we waited successfully for all processes above.
      // This ensures the process group state is consistent with the OS in boost.
      std::error_code ec;
      group.terminate(ec);
      group.detach();
    }

    if (proc.valid()) {
      // avoid zombie process
      proc.detach();
    }
  }

  /**
   * @brief Resolve the working directory for a configured command.
   *
   * @param cmd Command line to execute or inspect.
   * @param env Environment variables for the child process.
   * @return Directory used to launch the command, falling back to PATH lookup when needed.
   */
  boost::filesystem::path find_working_directory(const std::string &cmd, boost::process::v1::environment &env) {
    // Parse the raw command string into parts to get the actual command portion
    std::vector<std::string> parts;
    try {
#ifdef _WIN32
      parts = boost::program_options::split_winmain(cmd);
#else
      parts = boost::program_options::split_unix(cmd);
#endif
    } catch (boost::escaped_list_error &err) {
      BOOST_LOG(error) << "Boost failed to parse command ["sv << cmd << "] because " << err.what();
      return boost::filesystem::path();
    }
    if (parts.empty()) {
      BOOST_LOG(error) << "Unable to parse command: "sv << cmd;
      return boost::filesystem::path();
    }

    BOOST_LOG(debug) << "Parsed target ["sv << parts.at(0) << "] from command ["sv << cmd << ']';

    // If the target is a URL, don't parse any further here
    if (parts.at(0).find("://") != std::string::npos) {
      return boost::filesystem::path();
    }

    // If the cmd path is not an absolute path, resolve it using our PATH variable
    boost::filesystem::path cmd_path(parts.at(0));
    if (!cmd_path.is_absolute()) {
      cmd_path = boost::process::v1::search_path(parts.at(0));
      if (cmd_path.empty()) {
        BOOST_LOG(error) << "Unable to find executable ["sv << parts.at(0) << "]. Is it in your PATH?"sv;
        return boost::filesystem::path();
      }
    }

    BOOST_LOG(debug) << "Resolved target ["sv << parts.at(0) << "] to path ["sv << cmd_path << ']';

    // Now that we have a complete path, we can just use parent_path()
    return cmd_path.parent_path();
  }

  void proc_t::terminate_entry_locked(std::map<int, running_t>::iterator &it, bool notify) {
    // Runs the entry's undo commands and tears down its process group.
    // Only this app's input state: other apps may still be streaming.
    input::terminate_gamepads_for_app(it->first);
    std::error_code ec;
    terminate_process_group(it->second.process, it->second.process_group, it->second.app.exit_timeout);
    it->second.process = boost::process::v1::child();
    it->second.process_group = boost::process::v1::group();

    for (auto prep_it = it->second.prep_it; prep_it != it->second.prep_begin; --prep_it) {
      auto &cmd = *(prep_it - 1);
      if (cmd.undo_cmd.empty()) continue;
      boost::filesystem::path working_dir = it->second.app.working_dir.empty() ?
                                                find_working_directory(cmd.undo_cmd, it->second.env) :
                                                boost::filesystem::path(it->second.app.working_dir);
      BOOST_LOG(info) << "Executing Undo Cmd: ["sv << cmd.undo_cmd << ']';
      auto child = platf::run_command(cmd.elevated, true, cmd.undo_cmd, working_dir, it->second.env, it->second.pipe.get(), ec, nullptr);
      if (ec) {
        BOOST_LOG(warning) << "System: "sv << ec.message();
      }
      child.wait();
      auto ret = child.exit_code();
      if (ret != 0) BOOST_LOG(warning) << "Return code ["sv << ret << ']';
    }
    it->second.pipe.reset();

    const std::string app_name = it->second.app.name;
    const int ended_id = it->first;
    std::erase(_launch_order, ended_id);
    it = _running.erase(it);
    if (notify && !app_name.empty()) {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
      system_tray::update_tray_stopped(app_name);
#endif
    }
    // Keep legacy single-app view pointing at most recent still-running app.
    if (_running.empty()) {
      _app_id = -1;
    } else if (ended_id == _app_id) {
      _app_id = _launch_order.empty() ? _running.rbegin()->first : _launch_order.back();
      if (_running.find(_app_id) == _running.end()) _app_id = _running.rbegin()->first;
    }
  }

  void proc_t::prune_expired_locked() {
    // Reap zombies (see running() note about waitpid synchronization).
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}

    std::vector<int> dead;
    for (auto &[id, entry] : _running) {
      if (entry.placebo) {
        continue;
      }
      const bool group_alive = entry.app.wait_all && entry.process_group &&
                               platf::process_group_running((std::uintptr_t) entry.process_group.native_handle());
      const bool proc_alive = entry.process.running();
      const bool detached_grace = entry.app.auto_detach &&
                                  entry.process.native_exit_code() == 0 &&
                                  std::chrono::steady_clock::now() - entry.launch_time < 5s;
      const bool alive = group_alive || proc_alive || detached_grace;
      if (!alive) {
        if (detached_grace) {
          BOOST_LOG(info) << "App exited gracefully within 5 seconds of launch. Treating the app as a detached command."sv;
          BOOST_LOG(info) << "Adjust this behavior in the Applications tab or apps.json if this is not what you want."sv;
          entry.placebo = true;
          continue;
        }
        if (entry.process) {
          BOOST_LOG(info) << "App exited with code ["sv << entry.process.native_exit_code() << ']';
        }
        dead.push_back(id);
      }
    }
    for (int id : dead) {
      auto it = _running.find(id);
      if (it != _running.end()) {
        terminate_entry_locked(it, false);
      }
    }
    // Keep launch order consistent with _running.
    std::erase_if(_launch_order, [&](int id) { return _running.find(id) == _running.end(); });
  }

  bool proc_t::is_running(int app_id) {
    std::lock_guard<std::mutex> lock(*_mutex);
    prune_expired_locked();
    return _running.find(app_id) != _running.end();
  }

  int proc_t::running_count() {
    std::lock_guard<std::mutex> lock(*_mutex);
    prune_expired_locked();
    return (int) _running.size();
  }

  namespace {
    /**
     * @brief Add the stream-specific SUNSHINE_* variables for a client launch.
     */
    void fill_stream_env(boost::process::v1::environment &env, int app_id, const ctx_t &app, const rtsp_stream::launch_session_t &launch_session_ref) {
      const auto *launch_session = &launch_session_ref;
      // Add Stream-specific environment variables
      env["SUNSHINE_APP_ID"] = std::to_string(app_id);
      env["SUNSHINE_APP_NAME"] = app.name;
      env["SUNSHINE_CLIENT_NAME"] = launch_session->client_name;
      env["SUNSHINE_CLIENT_WIDTH"] = std::to_string(launch_session->width);
      env["SUNSHINE_CLIENT_HEIGHT"] = std::to_string(launch_session->height);
      env["SUNSHINE_CLIENT_FPS"] = std::to_string(launch_session->fps);
      env["SUNSHINE_SESSION_LAYOUT"] = config::sunshine.session_layout;
      env["SUNSHINE_CLIENT_HDR"] = launch_session->enable_hdr ? "true" : "false";
      env["SUNSHINE_CLIENT_GCMAP"] = std::to_string(launch_session->gcmap);
      env["SUNSHINE_CLIENT_HOST_AUDIO"] = launch_session->host_audio ? "true" : "false";
      env["SUNSHINE_CLIENT_ENABLE_SOPS"] = launch_session->enable_sops ? "true" : "false";
      int channelCount = launch_session->surround_info & 65535;
      switch (channelCount) {
        case 2:
          env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "2.0";
          break;
        case 6:
          env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "5.1";
          break;
        case 8:
          env["SUNSHINE_CLIENT_AUDIO_CONFIGURATION"] = "7.1";
          break;
      }
      env["SUNSHINE_CLIENT_AUDIO_SURROUND_PARAMS"] = launch_session->surround_params;
    }
  }  // namespace

  int proc_t::execute(int app_id, std::shared_ptr<rtsp_stream::launch_session_t> launch_session) {
    std::lock_guard<std::mutex> lock(*_mutex);

    auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto app) {
      return app.id == std::to_string(app_id);
    });

    if (iter == _apps.end()) {
      BOOST_LOG(error) << "Couldn't find app with ID ["sv << app_id << ']';
      return 404;
    }

    // Allow concurrent launches of different apps. A duplicate launch of an
    // already-running app is treated as a no-op instead of tearing peers down.
    if (auto running = _running.find(app_id); running != _running.end()) {
      _app_id = app_id;
      _app = *iter;

      // A new client attached to an app that keeps running: let opted-in apps
      // adapt to it (e.g. switch a virtual display to the client's mode).
      // Failures are logged but never stop the stream or the running app.
      if (iter->prep_on_attach) {
        auto env = running->second.env;
        fill_stream_env(env, app_id, *iter, *launch_session);
        for (const auto &cmd : iter->prep_cmds) {
          if (cmd.do_cmd.empty()) {
            continue;
          }
          std::error_code ec;
          boost::filesystem::path working_dir = iter->working_dir.empty() ?
                                                        find_working_directory(cmd.do_cmd, env) :
                                                        boost::filesystem::path(iter->working_dir);
          BOOST_LOG(info) << "Executing Do Cmd on attach: ["sv << cmd.do_cmd << ']';
          auto child = platf::run_command(cmd.elevated, true, cmd.do_cmd, working_dir, env, running->second.pipe.get(), ec, nullptr);
          if (ec) {
            BOOST_LOG(warning) << "Couldn't run ["sv << cmd.do_cmd << "] on attach: "sv << ec.message();
            continue;
          }
          child.wait(ec);
          if (!ec && child.exit_code() != 0) {
            BOOST_LOG(warning) << '[' << cmd.do_cmd << "] on attach exited with code ["sv << child.exit_code() << ']';
          }
        }
      }
      return 0;
    }

    running_t entry{};
    entry.app_id = app_id;
    entry.app = *iter;
    entry.placebo = false;
    entry.prep_begin = std::begin(entry.app.prep_cmds);
    entry.prep_it = entry.prep_begin;
    entry.env = _env;
    _running.emplace(app_id, std::move(entry));
    _launch_order.push_back(app_id);

    auto &e = _running.at(app_id);
    auto &app = e.app;
    auto &env = e.env;

    _app_id = app_id;
    _app = app;

    fill_stream_env(env, app_id, app, *launch_session);

    if (!app.output.empty() && app.output != "null"sv) {
#ifdef _WIN32
      auto woutput = utf_utils::from_utf8(app.output);
      e.pipe.reset(_wfsopen(woutput.c_str(), L"a", _SH_DENYNO));
#else
      e.pipe.reset(fopen(app.output.c_str(), "a"));
#endif
    }

    std::error_code ec;
    // Executed when returning from function
    auto fg = util::fail_guard([&]() {
      auto it = _running.find(app_id);
      if (it != _running.end()) {
        terminate_entry_locked(it, true);
      }
    });

    for (; e.prep_it != std::end(app.prep_cmds); ++e.prep_it) {
      auto &cmd = *e.prep_it;

      // Skip empty commands
      if (cmd.do_cmd.empty()) {
        continue;
      }

      boost::filesystem::path working_dir = app.working_dir.empty() ?
                                              find_working_directory(cmd.do_cmd, env) :
                                              boost::filesystem::path(app.working_dir);
      BOOST_LOG(info) << "Executing Do Cmd: ["sv << cmd.do_cmd << ']';
      auto child = platf::run_command(cmd.elevated, true, cmd.do_cmd, working_dir, env, e.pipe.get(), ec, nullptr);

      if (ec) {
        BOOST_LOG(error) << "Couldn't run ["sv << cmd.do_cmd << "]: System: "sv << ec.message();
        if (!(app.cmd.empty() && ec == std::errc::permission_denied)) {
          return -1;
        }
      }

      child.wait(ec);
      if (ec) {
        BOOST_LOG(error) << '[' << cmd.do_cmd << "] wait failed with error code ["sv << ec << ']';
        return -1;
      }
      auto ret = child.exit_code();
      if (ret != 0) {
        BOOST_LOG(error) << '[' << cmd.do_cmd << "] exited with code ["sv << ret << ']';
        return -1;
      }
    }

    for (auto &cmd : app.detached) {
      boost::filesystem::path working_dir = app.working_dir.empty() ?
                                              find_working_directory(cmd, env) :
                                              boost::filesystem::path(app.working_dir);
      BOOST_LOG(info) << "Spawning ["sv << cmd << "] in ["sv << working_dir << ']';
      auto child = platf::run_command(app.elevated, true, cmd, working_dir, env, e.pipe.get(), ec, nullptr);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't spawn ["sv << cmd << "]: System: "sv << ec.message();
      } else {
        child.detach();
      }
    }

    if (app.cmd.empty()) {
      BOOST_LOG(info) << "Executing [Desktop]"sv;
      e.placebo = true;
    } else {
      boost::filesystem::path working_dir = app.working_dir.empty() ?
                                          find_working_directory(app.cmd, env) :
                                          boost::filesystem::path(app.working_dir);
      BOOST_LOG(info) << "Executing: ["sv << app.cmd << "] in ["sv << working_dir << ']';
      e.process = platf::run_command(app.elevated, true, app.cmd, working_dir, env, e.pipe.get(), ec, &e.process_group);
      if (ec) {
        BOOST_LOG(warning) << "Couldn't run ["sv << app.cmd << "]: System: "sv << ec.message();
        return -1;
      }
    }

    e.launch_time = std::chrono::steady_clock::now();
    _app_launch_time = e.launch_time;

    fg.disable();

    return 0;
  }

  std::vector<int> proc_t::running_ids() {
    std::lock_guard<std::mutex> lock(*_mutex);
    prune_expired_locked();
    std::vector<int> ids;
    ids.reserve(_running.size());
    for (const auto &[id, entry] : _running) {
      ids.push_back(id);
    }
    return ids;
  }

  int proc_t::running() {
#ifndef _WIN32
    auto reaper = util::fail_guard([]() {
      while (waitpid(-1, nullptr, WNOHANG) > 0);
    });
#endif
    std::lock_guard<std::mutex> lock(*_mutex);
    prune_expired_locked();
    if (_running.empty()) return 0;
    // Most recently launched still-running app (matches previous single-app semantics).
    if (_app_id > 0 && _running.find(_app_id) != _running.end()) return _app_id;
    return _launch_order.empty() ? _running.rbegin()->first : _launch_order.back();
  }

  void proc_t::terminate() {
    std::lock_guard<std::mutex> lock(*_mutex);
    // Terminate all running apps (shutdown/cancel semantics).
    while (!_running.empty()) {
      auto it = _running.begin();
      terminate_entry_locked(it, true);
      (void) it;  // erased by helper
    }
    _launch_order.clear();
    _app_id = -1;
    display_device::revert_configuration();
    (void) placebo;
  }

  void proc_t::terminate(int app_id) {
    std::lock_guard<std::mutex> lock(*_mutex);
    auto it = _running.find(app_id);
    if (it == _running.end()) {
      // No-op if that app is not running (safe for session teardown races).
      return;
    }
    terminate_entry_locked(it, true);
    if (_running.empty()) {
      display_device::revert_configuration();
    }
  }

  const std::vector<ctx_t> &proc_t::get_apps() const {
    return _apps;
  }

  std::vector<ctx_t> &proc_t::get_apps() {
    return _apps;
  }

  // Gets application image from application list.
  // Returns image from assets directory if found there.
  // Returns default image if image configuration is not set.
  // Returns http content-type header compatible image type.
  std::string proc_t::get_app_image(int app_id) {
    auto iter = std::find_if(_apps.begin(), _apps.end(), [&app_id](const auto app) {
      return app.id == std::to_string(app_id);
    });
    auto app_image_path = iter == _apps.end() ? std::string() : iter->image_path;

    return validate_app_image_path(app_image_path);
  }

  std::string proc_t::get_last_run_app_name() {
    std::lock_guard<std::mutex> lock(*_mutex);
    if (!_running.empty()) {
      auto it = _running.find(_app_id);
      if (it != _running.end()) return it->second.app.name;
      return _running.rbegin()->second.app.name;
    }
    return _app.name;
  }

  std::string proc_t::get_app_output(int app_id) {
    std::lock_guard<std::mutex> lock(*_mutex);
    auto it = _running.find(app_id);
    if (it != _running.end()) {
      return it->second.app.output;
    }
    for (const auto &app : _apps) {
      if (app.id == std::to_string(app_id)) {
        return app.output;
      }
    }
    return ""s;
  }

  std::string proc_t::get_app_encoder_adapter(int app_id) {
    std::lock_guard<std::mutex> lock(*_mutex);
    auto it = _running.find(app_id);
    if (it != _running.end()) {
      return it->second.app.encoder_adapter;
    }
    for (const auto &app : _apps) {
      if (app.id == std::to_string(app_id)) {
        return app.encoder_adapter;
      }
    }
    return ""s;
  }

  std::string proc_t::get_app_capture(int app_id) {
    std::lock_guard<std::mutex> lock(*_mutex);
    auto it = _running.find(app_id);
    if (it != _running.end()) {
      return it->second.app.capture;
    }
    for (const auto &app : _apps) {
      if (app.id == std::to_string(app_id)) {
        return app.capture;
      }
    }
    return ""s;
  }

  std::string proc_t::get_app_wayland_display(int app_id) {
    std::lock_guard<std::mutex> lock(*_mutex);
    auto it = _running.find(app_id);
    if (it != _running.end()) {
      return it->second.app.wayland_display;
    }
    for (const auto &app : _apps) {
      if (app.id == std::to_string(app_id)) {
        return app.wayland_display;
      }
    }
    return ""s;
  }

  std::string proc_t::get_app_input_target(int app_id) {
    std::lock_guard<std::mutex> lock(*_mutex);
    auto it = _running.find(app_id);
    if (it != _running.end()) {
      return it->second.app.input_target;
    }
    for (const auto &app : _apps) {
      if (app.id == std::to_string(app_id)) {
        return app.input_target;
      }
    }
    return ""s;
  }

  bool proc_t::get_app_virtual_output(int app_id) {
    std::lock_guard<std::mutex> lock(*_mutex);
    auto it = _running.find(app_id);
    if (it != _running.end()) {
      return it->second.app.virtual_output;
    }
    for (const auto &app : _apps) {
      if (app.id == std::to_string(app_id)) {
        return app.virtual_output;
      }
    }
    return false;
  }

  std::string proc_t::get_app_audio_sink(int app_id) {
    std::lock_guard<std::mutex> lock(*_mutex);
    auto it = _running.find(app_id);
    if (it != _running.end()) {
      return it->second.app.audio_sink;
    }
    for (const auto &app : _apps) {
      if (app.id == std::to_string(app_id)) {
        return app.audio_sink;
      }
    }
    return ""s;
  }

  bool proc_t::get_app_gamepads(int app_id) {
    std::lock_guard<std::mutex> lock(*_mutex);
    auto it = _running.find(app_id);
    if (it != _running.end()) {
      return it->second.app.gamepads;
    }
    for (const auto &app : _apps) {
      if (app.id == std::to_string(app_id)) {
        return app.gamepads;
      }
    }
    return true;
  }

  bool proc_t::get_app_mouse(int app_id) {
    std::lock_guard<std::mutex> lock(*_mutex);
    auto it = _running.find(app_id);
    if (it != _running.end()) {
      return it->second.app.mouse;
    }
    for (const auto &app : _apps) {
      if (app.id == std::to_string(app_id)) {
        return app.mouse;
      }
    }
    return true;
  }

  proc_t::~proc_t() {
    // It's not safe to call terminate() here because our proc_t is a static variable
    // that may be destroyed after the Boost loggers have been destroyed. Instead,
    // we return a deinit_t to main() to handle termination when we're exiting.
    // Once we reach this point here, termination must have already happened.
    // Multi-app: all entries must have been terminated by deinit_t before we get here.
    assert(_running.empty());
  }

  /**
   * @brief Find the closing parenthesis for an environment-variable expression.
   *
   * @param begin Iterator positioned at the opening parenthesis.
   * @param end End iterator for the expression being scanned.
   * @return Iterator for the matching closing parenthesis, or end when unmatched.
   */
  std::string_view::iterator find_match(std::string_view::iterator begin, std::string_view::iterator end) {
    int stack = 0;

    --begin;
    do {
      ++begin;
      switch (*begin) {
        case '(':
          ++stack;
          break;
        case ')':
          --stack;
      }
    } while (begin != end && stack != 0);

    if (begin == end) {
      throw std::out_of_range("Missing closing bracket \')\'");
    }
    return begin;
  }

  /**
   * @brief Parse env val.
   *
   * @param env Environment variables for the child process.
   * @param val_raw Raw value that may contain $(NAME) substitutions.
   * @return Value with recognized environment-variable substitutions expanded.
   */
  std::string parse_env_val(boost::process::v1::native_environment &env, const std::string_view &val_raw) {
    auto pos = std::begin(val_raw);
    auto dollar = std::find(pos, std::end(val_raw), '$');

    std::stringstream ss;

    while (dollar != std::end(val_raw)) {
      auto next = dollar + 1;
      if (next != std::end(val_raw)) {
        switch (*next) {
          case '(':
            {
              ss.write(pos, (dollar - pos));
              auto var_begin = next + 1;
              auto var_end = find_match(next, std::end(val_raw));
              auto var_name = std::string {var_begin, var_end};

#ifdef _WIN32
              // Windows treats environment variable names in a case-insensitive manner,
              // so we look for a case-insensitive match here. This is critical for
              // correctly appending to PATH on Windows.
              auto itr = std::find_if(env.cbegin(), env.cend(), [&](const auto &e) {
                return boost::iequals(e.get_name(), var_name);
              });
              if (itr != env.cend()) {
                // Use an existing case-insensitive match
                var_name = itr->get_name();
              }
#endif

              ss << env[var_name].to_string();

              pos = var_end + 1;
              next = var_end;

              break;
            }
          case '$':
            ss.write(pos, (next - pos));
            pos = next + 1;
            ++next;
            break;
        }

        dollar = std::find(next, std::end(val_raw), '$');
      } else {
        dollar = next;
      }
    }

    ss.write(pos, (dollar - pos));

    return ss.str();
  }

  /**
   * @brief Validates a path whether it is a valid PNG.
   * @param path The path to the PNG file.
   * @return true if the file has a valid PNG signature, false otherwise.
   */
  bool check_valid_png(const std::filesystem::path &path) {
    // PNG signature as defined in PNG specification
    // http://www.libpng.org/pub/png/spec/1.2/PNG-Structure.html
    static constexpr std::array<unsigned char, 8> PNG_SIGNATURE = {
      0x89,
      0x50,
      0x4E,
      0x47,
      0x0D,
      0x0A,
      0x1A,
      0x0A
    };

    std::ifstream file(path, std::ios::binary);
    if (!file) {
      return false;
    }

    std::array<unsigned char, 8> header;
    file.read(reinterpret_cast<char *>(header.data()), 8);

    if (file.gcount() != 8) {
      return false;
    }

    return header == PNG_SIGNATURE;
  }

  /**
   * @brief Validate app image path.
   */
  std::string validate_app_image_path(std::string app_image_path) {
    if (app_image_path.empty()) {
      return DEFAULT_APP_IMAGE_PATH;
    }

    // get the image extension and convert it to lowercase
    auto image_extension = std::filesystem::path(app_image_path).extension().string();
    boost::to_lower(image_extension);

    // return the default box image if the extension is not "png"
    if (image_extension != ".png") {
      return DEFAULT_APP_IMAGE_PATH;
    }

    // check if image is in assets directory
    if (auto full_image_path = std::filesystem::path(SUNSHINE_ASSETS_DIR) / app_image_path; std::filesystem::exists(full_image_path)) {
      // Validate PNG signature
      if (!check_valid_png(full_image_path)) {
        BOOST_LOG(warning) << "Invalid PNG file at path ["sv << full_image_path << ']';
        return DEFAULT_APP_IMAGE_PATH;
      }
      return full_image_path.string();
    }

    if (app_image_path == "./assets/steam.png") {
      // handle old default steam image definition
      return SUNSHINE_ASSETS_DIR "/steam.png";
    }

    // check if specified image exists
    if (std::error_code code; !std::filesystem::exists(app_image_path, code)) {
      // return default box image if image does not exist
      BOOST_LOG(warning) << "Couldn't find app image at path ["sv << app_image_path << ']';
      return DEFAULT_APP_IMAGE_PATH;
    }

    // Validate PNG signature
    if (!check_valid_png(app_image_path)) {
      BOOST_LOG(warning) << "Invalid PNG file at path ["sv << app_image_path << ']';
      return DEFAULT_APP_IMAGE_PATH;
    }

    // image is a png, and not in assets directory
    // return only "content-type" http header compatible image type
    return app_image_path;
  }

  /**
   * @brief Calculate the SHA-256 digest for a file.
   *
   * @param filename File path whose contents should be hashed.
   * @return Lowercase hexadecimal SHA-256 digest, or std::nullopt on read/hash failure.
   */
  std::optional<std::string> calculate_sha256(const std::string &filename) {
    crypto::md_ctx_t ctx {EVP_MD_CTX_create()};
    if (!ctx) {
      return std::nullopt;
    }

    if (!EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr)) {
      return std::nullopt;
    }

    // Read file and update calculated SHA
    char buf[1024 * 16];
    std::ifstream file(filename, std::ifstream::binary);
    while (file.good()) {
      file.read(buf, sizeof(buf));
      if (!EVP_DigestUpdate(ctx.get(), buf, file.gcount())) {
        return std::nullopt;
      }
    }
    file.close();

    unsigned char result[SHA256_DIGEST_LENGTH];
    if (!EVP_DigestFinal_ex(ctx.get(), result, nullptr)) {
      return std::nullopt;
    }

    // Transform byte-array to string
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto &byte : result) {
      ss << std::setw(2) << (int) byte;
    }
    return ss.str();
  }

  /**
   * @brief Calculate the CRC-32 checksum for a string.
   *
   * @param input Bytes to include in the checksum.
   * @return CRC-32 value for the input bytes.
   */
  uint32_t calculate_crc32(const std::string &input) {
    boost::crc_32_type result;
    result.process_bytes(input.data(), input.length());
    return result.checksum();
  }

  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index) {
    // Generate id by hashing name with image data if present
    std::vector<std::string> to_hash;
    to_hash.push_back(app_name);
    auto file_path = validate_app_image_path(app_image_path);
    if (file_path != DEFAULT_APP_IMAGE_PATH) {
      auto file_hash = calculate_sha256(file_path);
      if (file_hash) {
        to_hash.push_back(file_hash.value());
      } else {
        // Fallback to just hashing image path
        to_hash.push_back(file_path);
      }
    }

    // Create combined strings for hash
    std::stringstream ss;
    for_each(to_hash.begin(), to_hash.end(), [&ss](const std::string &s) {
      ss << s;
    });
    auto input_no_index = ss.str();
    ss << index;
    auto input_with_index = ss.str();

    // CRC32 then truncate to signed 32-bit range due to client limitations
    auto id_no_index = std::to_string(abs((int32_t) calculate_crc32(input_no_index)));
    auto id_with_index = std::to_string(abs((int32_t) calculate_crc32(input_with_index)));

    return std::make_tuple(id_no_index, id_with_index);
  }

  /**
   * @brief Parse serialized text into the corresponding runtime representation.
   */
  std::optional<proc::proc_t> parse(const std::string &file_name) {
    pt::ptree tree;

    try {
      pt::read_json(file_name, tree);

      auto &apps_node = tree.get_child("apps"s);
      auto &env_vars = tree.get_child("env"s);

      auto this_env = boost::this_process::environment();

      for (auto &[name, val] : env_vars) {
        this_env[name] = parse_env_val(this_env, val.get_value<std::string>());
      }

      std::set<std::string> ids;
      std::vector<proc::ctx_t> apps;
      int i = 0;
      for (auto &[_, app_node] : apps_node) {
        proc::ctx_t ctx;

        auto prep_nodes_opt = app_node.get_child_optional("prep-cmd"s);
        auto detached_nodes_opt = app_node.get_child_optional("detached"s);
        auto exclude_global_prep = app_node.get_optional<bool>("exclude-global-prep-cmd"s);
        auto output = app_node.get_optional<std::string>("output"s);
        auto encoder_adapter = app_node.get_optional<std::string>("encoder-adapter"s);
        auto capture = app_node.get_optional<std::string>("capture"s);
        auto wayland_display = app_node.get_optional<std::string>("wayland-display"s);
        auto prep_on_attach = app_node.get_optional<bool>("prep-on-attach"s);
        auto input_target = app_node.get_optional<std::string>("input-target"s);
        auto virtual_output = app_node.get_optional<bool>("virtual-output"s);
        auto gamepads = app_node.get_optional<bool>("gamepads"s);
        auto mouse = app_node.get_optional<bool>("mouse"s);
        auto audio_sink = app_node.get_optional<std::string>("audio-sink"s);
        auto name = parse_env_val(this_env, app_node.get<std::string>("name"s));
        auto cmd = app_node.get_optional<std::string>("cmd"s);
        auto image_path = app_node.get_optional<std::string>("image-path"s);
        auto working_dir = app_node.get_optional<std::string>("working-dir"s);
        auto elevated = app_node.get_optional<bool>("elevated"s);
        auto auto_detach = app_node.get_optional<bool>("auto-detach"s);
        auto wait_all = app_node.get_optional<bool>("wait-all"s);
        auto exit_timeout = app_node.get_optional<int>("exit-timeout"s);

        std::vector<proc::cmd_t> prep_cmds;
        if (!exclude_global_prep.value_or(false)) {
          prep_cmds.reserve(config::sunshine.prep_cmds.size());
          for (auto &prep_cmd : config::sunshine.prep_cmds) {
            auto do_cmd = parse_env_val(this_env, prep_cmd.do_cmd);
            auto undo_cmd = parse_env_val(this_env, prep_cmd.undo_cmd);

            prep_cmds.emplace_back(
              std::move(do_cmd),
              std::move(undo_cmd),
              std::move(prep_cmd.elevated)
            );
          }
        }

        if (prep_nodes_opt) {
          auto &prep_nodes = *prep_nodes_opt;

          prep_cmds.reserve(prep_cmds.size() + prep_nodes.size());
          for (auto &[_, prep_node] : prep_nodes) {
            auto do_cmd = prep_node.get_optional<std::string>("do"s);
            auto undo_cmd = prep_node.get_optional<std::string>("undo"s);
            auto elevated = prep_node.get_optional<bool>("elevated");

            prep_cmds.emplace_back(
              parse_env_val(this_env, do_cmd.value_or("")),
              parse_env_val(this_env, undo_cmd.value_or("")),
              std::move(elevated.value_or(false))
            );
          }
        }

        std::vector<std::string> detached;
        if (detached_nodes_opt) {
          auto &detached_nodes = *detached_nodes_opt;

          detached.reserve(detached_nodes.size());
          for (auto &[_, detached_val] : detached_nodes) {
            detached.emplace_back(parse_env_val(this_env, detached_val.get_value<std::string>()));
          }
        }

        if (output) {
          ctx.output = parse_env_val(this_env, *output);
        }
        if (encoder_adapter) {
          ctx.encoder_adapter = parse_env_val(this_env, *encoder_adapter);
        }
        if (capture) {
          ctx.capture = parse_env_val(this_env, *capture);
        }
        ctx.prep_on_attach = prep_on_attach.value_or(false);
        ctx.virtual_output = virtual_output.value_or(false);
        ctx.gamepads = gamepads.value_or(true);
        ctx.mouse = mouse.value_or(true);
        if (audio_sink) {
          ctx.audio_sink = parse_env_val(this_env, *audio_sink);
        }
        if (input_target) {
          ctx.input_target = parse_env_val(this_env, *input_target);
        }
        if (wayland_display) {
          ctx.wayland_display = parse_env_val(this_env, *wayland_display);
        }

        if (cmd) {
          ctx.cmd = parse_env_val(this_env, *cmd);
        }

        if (working_dir) {
          ctx.working_dir = parse_env_val(this_env, *working_dir);
#ifdef _WIN32
          // The working directory, unlike the command itself, should not be quoted
          // when it contains spaces. Unlike POSIX, Windows forbids quotes in paths,
          // so we can safely strip them all out here to avoid confusing the user.
          boost::erase_all(ctx.working_dir, "\"");
#endif
        }

        if (image_path) {
          ctx.image_path = parse_env_val(this_env, *image_path);
        }

        ctx.elevated = elevated.value_or(false);
        ctx.auto_detach = auto_detach.value_or(true);
        ctx.wait_all = wait_all.value_or(true);
        ctx.exit_timeout = std::chrono::seconds {exit_timeout.value_or(5)};

        auto possible_ids = calculate_app_id(name, ctx.image_path, i++);
        if (ids.count(std::get<0>(possible_ids)) == 0) {
          // Avoid using index to generate id if possible
          ctx.id = std::get<0>(possible_ids);
        } else {
          // Fallback to include index on collision
          ctx.id = std::get<1>(possible_ids);
        }
        ids.insert(ctx.id);

        ctx.name = std::move(name);
        ctx.prep_cmds = std::move(prep_cmds);
        ctx.detached = std::move(detached);

        apps.emplace_back(std::move(ctx));
      }

      return proc::proc_t {
        std::move(this_env),
        std::move(apps)
      };
    } catch (std::exception &e) {
      BOOST_LOG(error) << e.what();
    }

    return std::nullopt;
  }

  /**
   * @brief Refresh cached platform state from the operating system.
   */
  void refresh(const std::string &file_name) {
    auto proc_opt = proc::parse(file_name);

    if (proc_opt) {
      proc = std::move(*proc_opt);
    }
  }
}  // namespace proc
