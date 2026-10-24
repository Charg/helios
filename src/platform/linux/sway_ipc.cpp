/**
 * @file src/platform/linux/sway_ipc.cpp
 * @brief Definitions for querying sway over its IPC socket.
 */
// standard includes
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// platform includes
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "src/logging.h"
#include "sway_ipc.h"

using namespace std::literals;

namespace sway_ipc {
  namespace {
    constexpr std::string_view IPC_MAGIC = "i3-ipc"sv;
    constexpr std::uint32_t IPC_GET_OUTPUTS = 3;
    constexpr std::size_t IPC_HEADER_SIZE = IPC_MAGIC.size() + 2 * sizeof(std::uint32_t);
    constexpr std::size_t IPC_MAX_REPLY = 16 * 1024 * 1024;
    constexpr auto IPC_TIMEOUT = 2s;

    bool write_all(int fd, const char *data, std::size_t size) {
      while (size > 0) {
        auto written = send(fd, data, size, MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR) {
          continue;
        }
        if (written <= 0) {
          return false;
        }
        data += written;
        size -= written;
      }
      return true;
    }

    bool read_all(int fd, char *data, std::size_t size) {
      auto deadline = std::chrono::steady_clock::now() + IPC_TIMEOUT;
      while (size > 0) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        pollfd pfd {fd, POLLIN, 0};
        if (remaining.count() <= 0 || poll(&pfd, 1, remaining.count()) <= 0) {
          return false;
        }
        auto count = read(fd, data, size);
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count <= 0) {
          return false;
        }
        data += count;
        size -= count;
      }
      return true;
    }

    std::optional<std::string> get_outputs() {
      const char *path = std::getenv("SWAYSOCK");
      if (!path || !*path) {
        BOOST_LOG(debug) << "SWAYSOCK is not set, can't query sway outputs"sv;
        return std::nullopt;
      }

      sockaddr_un addr {};
      addr.sun_family = AF_UNIX;
      if (std::strlen(path) >= sizeof(addr.sun_path)) {
        return std::nullopt;
      }
      std::strcpy(addr.sun_path, path);

      int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd < 0) {
        return std::nullopt;
      }

      std::optional<std::string> reply;
      std::array<char, IPC_HEADER_SIZE> header;
      std::uint32_t length = 0;
      std::uint32_t type = IPC_GET_OUTPUTS;

      // Header is the magic followed by payload length and message type in native byte order
      std::memcpy(header.data(), IPC_MAGIC.data(), IPC_MAGIC.size());
      std::memcpy(header.data() + IPC_MAGIC.size(), &length, sizeof(length));
      std::memcpy(header.data() + IPC_MAGIC.size() + sizeof(length), &type, sizeof(type));

      if (connect(fd, (sockaddr *) &addr, sizeof(addr)) == 0 && write_all(fd, header.data(), header.size()) &&
          read_all(fd, header.data(), header.size()) && std::memcmp(header.data(), IPC_MAGIC.data(), IPC_MAGIC.size()) == 0) {
        std::memcpy(&length, header.data() + IPC_MAGIC.size(), sizeof(length));
        std::memcpy(&type, header.data() + IPC_MAGIC.size() + sizeof(length), sizeof(type));

        if (type == IPC_GET_OUTPUTS && length <= IPC_MAX_REPLY) {
          std::string payload(length, '\0');
          if (read_all(fd, payload.data(), payload.size())) {
            reply = std::move(payload);
          }
        }
      }

      close(fd);
      return reply;
    }
  }  // namespace

  std::optional<bool> output_hdr_from_json(std::string_view json, std::string_view output_name) {
    auto outputs = nlohmann::json::parse(json, nullptr, false);
    if (!outputs.is_array()) {
      return std::nullopt;
    }

    for (const auto &output : outputs) {
      if (!output.is_object() || output.value("name", ""s) != output_name) {
        continue;
      }

      auto hdr = output.find("hdr");
      if (hdr == output.end() || !hdr->is_boolean()) {
        return std::nullopt;
      }
      return hdr->get<bool>();
    }

    return std::nullopt;
  }

  std::optional<bool> query_output_hdr(std::string_view output_name) {
    auto outputs = get_outputs();
    if (!outputs) {
      BOOST_LOG(debug) << "Couldn't query sway outputs over IPC"sv;
      return std::nullopt;
    }

    auto hdr = output_hdr_from_json(*outputs, output_name);
    if (!hdr) {
      BOOST_LOG(debug) << "Sway reported no HDR state for output ["sv << output_name << ']';
    }
    return hdr;
  }
}  // namespace sway_ipc
