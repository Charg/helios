#include "src/platform/common.h"

#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include <nlohmann/json.hpp>

#include "src/config.h"
#include "src/logging.h"
#include "src/platform/linux/sway_hdr.h"

using namespace std::literals;

namespace platf {
  namespace {
    using namespace std::chrono_literals;

    std::optional<std::string> run_swaymsg(const std::vector<std::string> &arguments,
                                           std::string_view socket = {}) {
      int pipefd[2];
      if (pipe(pipefd) != 0) return std::nullopt;
      auto pid = fork();
      if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return std::nullopt;
      }
      if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        std::vector<char *> argv;
        argv.reserve(arguments.size() + (socket.empty() ? 2 : 4));
        argv.push_back(const_cast<char *>("swaymsg"));
        std::string socket_arg;
        if (!socket.empty()) {
          socket_arg = std::string(socket);
          argv.push_back(const_cast<char *>("-s"));
          argv.push_back(socket_arg.data());
        }
        for (const auto &argument : arguments) {
          argv.push_back(const_cast<char *>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execvp("swaymsg", argv.data());
        _exit(127);
      }
      close(pipefd[1]);
      std::string output;
      std::array<char, 4096> buffer;
      for (;;) {
        auto count = read(pipefd[0], buffer.data(), buffer.size());
        if (count > 0) output.append(buffer.data(), (std::size_t) count);
        else if (count < 0 && errno == EINTR) continue;
        else break;
      }
      close(pipefd[0]);
      int status = 0;
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
      if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::nullopt;
      return output;
    }

    std::vector<sway_hdr::render_node_t> render_nodes() {
      std::vector<sway_hdr::render_node_t> nodes;
      std::error_code error;
      for (std::filesystem::directory_iterator it("/sys/class/drm", error), end;
           !error && it != end; it.increment(error)) {
        const auto name = it->path().filename().string();
        if (!name.starts_with("renderD")) continue;
        std::error_code link_error;
        auto driver = std::filesystem::read_symlink(it->path() / "device/driver", link_error);
        if (link_error) continue;
        nodes.push_back({"/dev/dri/" + name, driver.filename().string()});
      }
      return nodes;
    }

    std::string env_or_empty(const char *name) {
      const char *value = std::getenv(name);
      return value ? value : "";
    }

    std::set<std::string> wayland_sockets(const std::filesystem::path &runtime_dir) {
      std::set<std::string> sockets;
      std::error_code error;
      for (std::filesystem::directory_iterator it(runtime_dir, error), end;
           !error && it != end; it.increment(error)) {
        const auto name = it->path().filename().string();
        auto status = it->symlink_status(error);
        if (error) break;
        if (name.starts_with("wayland-") && !name.ends_with(".lock") &&
            status.type() == std::filesystem::file_type::socket) {
          sockets.insert(name);
        }
      }
      return sockets;
    }

    class dedicated_sway_t {
    public:
      dedicated_sway_t() = default;
      dedicated_sway_t(const dedicated_sway_t &) = delete;
      dedicated_sway_t &operator=(const dedicated_sway_t &) = delete;

      ~dedicated_sway_t() {
        if (pid_ <= 0) return;
        run_swaymsg({"exit"}, socket_);
        for (int attempt = 0; attempt < 50; ++attempt) {
          int status = 0;
          if (waitpid(pid_, &status, WNOHANG) == pid_) {
            pid_ = -1;
            return;
          }
          std::this_thread::sleep_for(10ms);
        }
        kill(pid_, SIGTERM);
        while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        pid_ = -1;
      }

      bool start() {
        // HDR outputs need the Vulkan renderer; fall back to GLES2 (SDR only)
        // if the compositor cannot start with it.
        if (start(false)) return true;
        if (!env_or_empty("WLR_RENDERER").empty()) return false;
        BOOST_LOG(warning) << "Sway failed to start with the Vulkan renderer, retrying with GLES2; HDR virtual displays are unavailable"sv;
        return start(true);
      }

      const std::string &socket() const { return socket_; }

    private:
      bool start(bool gles_fallback) {
        const char *runtime = std::getenv("XDG_RUNTIME_DIR");
        if (!runtime || !*runtime) return false;
        const std::filesystem::path runtime_dir(runtime);
        std::error_code error;
        if (!std::filesystem::is_directory(runtime_dir, error) || error ||
            access(runtime_dir.c_str(), W_OK | X_OK) != 0) {
          return false;
        }
        const auto before = wayland_sockets(runtime_dir);
        const auto compositor_env = sway_hdr::select_compositor_env(
          env_or_empty("WLR_RENDERER"), env_or_empty("WLR_RENDER_DRM_DEVICE"),
          config::video.adapter_name, config::video.encoder, render_nodes(), gles_fallback);

        pid_ = fork();
        if (pid_ < 0) return false;
        if (pid_ == 0) {
          setsid();
          setenv("WLR_BACKENDS", "headless", 1);
          setenv("WLR_HEADLESS_OUTPUTS", "1", 1);
          setenv("WLR_LIBINPUT_NO_DEVICES", "1", 1);
          for (const auto &[name, value] : compositor_env.vars) {
            setenv(name.c_str(), value.c_str(), 1);
          }
          int null_fd = open("/dev/null", O_RDWR);
          if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) close(null_fd);
          }
          execlp("sway", "sway", "--unsupported-gpu", "--config", "/dev/null",
                 static_cast<char *>(nullptr));
          _exit(127);
        }

        socket_ = (runtime_dir /
                   std::format("sway-ipc.{}.{}.sock", getuid(), pid_)).string();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
          int status = 0;
          if (waitpid(pid_, &status, WNOHANG) == pid_) {
            pid_ = -1;
            return false;
          }
          const auto after = wayland_sockets(runtime_dir);
          std::vector<std::string> created;
          std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                              std::back_inserter(created));
          if (std::filesystem::exists(socket_) && created.size() == 1 &&
              run_swaymsg({"-r", "-t", "get_version"}, socket_)) {
            wayland_display_ = std::move(created.front());
            setenv("SWAYSOCK", socket_.c_str(), 1);
            setenv("WAYLAND_DISPLAY", wayland_display_.c_str(), 1);
            run_swaymsg({"output", "*", "disable"}, socket_);
            return true;
          }
          std::this_thread::sleep_for(20ms);
        }
        kill(pid_, SIGTERM);
        while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        pid_ = -1;
        return false;
      }

      pid_t pid_ = -1;
      std::string socket_;
      std::string wayland_display_;
    };

    std::mutex dedicated_sway_mutex;
    std::unique_ptr<dedicated_sway_t> dedicated_sway;

    std::optional<std::string> sway_socket() {
      if (const char *socket = std::getenv("SWAYSOCK"); socket && *socket) {
        return std::string(socket);
      }
      std::lock_guard lock(dedicated_sway_mutex);
      if (!dedicated_sway) {
        auto instance = std::make_unique<dedicated_sway_t>();
        if (!instance->start()) return std::nullopt;
        dedicated_sway = std::move(instance);
        BOOST_LOG(info) << "Started an isolated headless Sway session for client displays"sv;
      }
      return dedicated_sway->socket();
    }

    std::set<std::string> headless_outputs(std::string_view socket) {
      auto output = run_swaymsg({"-r", "-t", "get_outputs"}, socket);
      if (!output) return {};
      try {
        auto json = nlohmann::json::parse(*output);
        std::set<std::string> names;
        for (const auto &item : json) {
          auto name = item.value("name", "");
          if (name.starts_with("HEADLESS-")) names.insert(std::move(name));
        }
        return names;
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Could not parse sway outputs for virtual displays: "sv << e.what();
        return {};
      }
    }

    bool command_succeeded(const std::vector<std::string> &arguments,
                           std::string_view socket) {
      auto output = run_swaymsg(arguments, socket);
      if (!output) return false;
      try {
        auto json = nlohmann::json::parse(*output);
        return json.is_array() && !json.empty() && json[0].value("success", false);
      } catch (...) {
        return false;
      }
    }

    // Ask sway to switch HDR on or off for an output. HDR is best effort: stock
    // wlroots headless outputs, the GLES2 renderer and older sway builds cannot
    // do it, in which case the output stays SDR.
    void apply_hdr(std::string_view socket, const std::string &output, bool enable) {
      command_succeeded(sway_hdr::hdr_command(output, enable), socket);
      if (!enable) return;
      auto outputs = run_swaymsg({"-r", "-t", "get_outputs"}, socket);
      if (outputs && sway_hdr::output_hdr_enabled(*outputs, output).value_or(false)) return;
      static std::once_flag warned;
      std::call_once(warned, [&] {
        BOOST_LOG(info) << "Client requested HDR for "sv << output
                        << " but sway cannot enable it (needs the Vulkan renderer and a wlroots with HDR headless outputs); continuing in SDR"sv;
      });
    }

    class sway_topology_t final: public virtual_display_topology_t {
    public:
      explicit sway_topology_t(std::string socket): socket_(std::move(socket)) {}

      ~sway_topology_t() override {
        for (const auto &name : owned_) {
          command_succeeded({"output", name, "disable"}, socket_);
        }
      }

      bool apply(const std::vector<client_display_t> &displays) override {
        if (displays.empty()) return false;
        while (owned_.size() < displays.size()) {
          auto before = headless_outputs(socket_);
          if (!command_succeeded({"create_output"}, socket_)) return false;
          auto after = headless_outputs(socket_);
          std::vector<std::string> created;
          std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                              std::back_inserter(created));
          if (created.size() != 1) return false;
          owned_.push_back(std::move(created.front()));
        }
        for (std::size_t i = 0; i < owned_.size(); ++i) {
          if (i >= displays.size()) {
            command_succeeded({"output", owned_[i], "disable"}, socket_);
            continue;
          }
          const auto &display = displays[i];
          auto mode = std::format("{}x{}@{:.3f}Hz", display.width, display.height,
                                  display.refresh_millihz / 1000.0);
          auto scale = std::format("{:.3f}", display.scale_milli / 1000.0);
          if (!command_succeeded({"output", owned_[i], "enable", "mode", mode,
                                  "pos", std::to_string(display.x), std::to_string(display.y),
                                  "scale", scale}, socket_)) {
            return false;
          }
          apply_hdr(socket_, owned_[i], display.hdr);
        }
        active_count_ = displays.size();
        return true;
      }

      std::vector<std::string> display_names() const override {
        return std::vector<std::string>(owned_.begin(),
                                        owned_.begin() + (std::ptrdiff_t) active_count_);
      }

    private:
      std::string socket_;
      std::vector<std::string> owned_;
      std::size_t active_count_ = 0;
    };
  }

  bool virtual_display_topology_available() {
    return sway_socket().has_value();
  }

  std::unique_ptr<virtual_display_topology_t> virtual_display_topology() {
    auto socket = sway_socket();
    if (!socket) return nullptr;
    return std::make_unique<sway_topology_t>(std::move(*socket));
  }
}
