/**
 * @file src/platform/linux/sway_hdr.h
 * @brief Pure helpers for HDR on Sway headless virtual displays.
 */
#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace platf::sway_hdr {
  struct render_node_t {
    std::string path;  ///< e.g. /dev/dri/renderD128
    std::string driver;  ///< kernel driver name, e.g. nvidia
  };

  struct compositor_env_t {
    std::vector<std::pair<std::string, std::string>> vars;  ///< variables to set in the private sway
  };

  /**
   * @brief Pick the render node wlroots should use for the private compositor.
   * @return The node path, or an empty string to let wlroots choose.
   */
  inline std::string select_render_device(std::string_view adapter_name,
                                          std::string_view encoder,
                                          const std::vector<render_node_t> &nodes) {
    if (adapter_name.starts_with("/dev/dri/")) return std::string(adapter_name);
    // An empty encoder is auto-detection, which tries nvenc first. Other
    // values (vaapi, quicksync, amdvce, software) leave the choice to wlroots.
    if (encoder.empty() || encoder == "nvenc") {
      auto node = std::ranges::find(nodes, "nvidia", &render_node_t::driver);
      if (node != nodes.end()) return node->path;
    }
    return {};
  }

  /**
   * @brief Environment for the private sway. Variables the user already set are left alone.
   * @param user_renderer WLR_RENDERER from the environment, empty when unset.
   * @param user_device WLR_RENDER_DRM_DEVICE from the environment, empty when unset.
   * @param gles_fallback Use GLES2 instead of Vulkan (HDR unavailable).
   */
  inline compositor_env_t select_compositor_env(std::string_view user_renderer,
                                                std::string_view user_device,
                                                std::string_view adapter_name,
                                                std::string_view encoder,
                                                const std::vector<render_node_t> &nodes,
                                                bool gles_fallback) {
    compositor_env_t env;
    if (user_renderer.empty()) {
      env.vars.emplace_back("WLR_RENDERER", gles_fallback ? "gles2" : "vulkan");
    }
    if (user_device.empty()) {
      if (auto device = select_render_device(adapter_name, encoder, nodes); !device.empty()) {
        env.vars.emplace_back("WLR_RENDER_DRM_DEVICE", std::move(device));
      }
    }
    return env;
  }

  /// swaymsg arguments toggling HDR on one output.
  inline std::vector<std::string> hdr_command(std::string_view output, bool enable) {
    return {"output", std::string(output), "hdr", enable ? "on" : "off"};
  }

  /**
   * @brief Read the "hdr" flag of a named output from `swaymsg -t get_outputs`.
   * @return nullopt when the output is missing or the JSON is unusable;
   *         false for outputs without the flag (older sway).
   */
  inline std::optional<bool> output_hdr_enabled(std::string_view get_outputs_json,
                                                std::string_view output) {
    auto json = nlohmann::json::parse(get_outputs_json, nullptr, false);
    if (!json.is_array()) return std::nullopt;
    for (const auto &item : json) {
      if (item.is_object() && item.value("name", "") == output) {
        return item.value("hdr", false);
      }
    }
    return std::nullopt;
  }
}
