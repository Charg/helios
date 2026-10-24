/**
 * @file src/platform/linux/sway_ipc.h
 * @brief Declarations for querying sway over its IPC socket.
 */
#pragma once

// standard includes
#include <optional>
#include <string>
#include <string_view>

namespace sway_ipc {
  /**
   * @brief Find the "hdr" state of a named output in a `get_outputs` reply.
   * @param json The raw JSON reply.
   * @param output_name The sway output name, such as `HEADLESS-1`.
   * @return Whether HDR is enabled, or `std::nullopt` if the reply is malformed or lacks the output or field.
   */
  std::optional<bool> output_hdr_from_json(std::string_view json, std::string_view output_name);

  /**
   * @brief Ask the running sway whether HDR is enabled on an output.
   * @param output_name The sway output name.
   * @return Whether HDR is enabled, or `std::nullopt` if sway can't be queried.
   */
  std::optional<bool> query_output_hdr(std::string_view output_name);
}  // namespace sway_ipc
