/**
 * @file src/platform/linux/input/wlr_virtual_input.cpp
 * @brief Definitions for keyboard and pointer injection through Sway's virtual input protocols.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>

// platform includes
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <virtual-keyboard-unstable-v1.h>
#include <wayland-client.h>
#include <wlr-virtual-pointer-unstable-v1.h>
#include <xkbcommon/xkbcommon.h>

// local includes
#include "src/logging.h"
#include "wlr_virtual_input.h"

using namespace std::literals;

namespace platf::wlr_input {
  namespace {
    // Matches libinput's scroll distance for one wheel detent.
    constexpr double scroll_per_detent = 15.0;
    constexpr int high_res_per_detent = 120;

    std::atomic<bool> routed_through_compositor = false;

    std::uint32_t now_ms() {
      timespec ts {};
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (std::uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    }
  }  // namespace

  struct connection_t {
    ~connection_t() {
      if (pointer) zwlr_virtual_pointer_v1_destroy(pointer);
      if (keyboard) zwp_virtual_keyboard_v1_destroy(keyboard);
      if (pointer_manager) zwlr_virtual_pointer_manager_v1_destroy(pointer_manager);
      if (keyboard_manager) zwp_virtual_keyboard_manager_v1_destroy(keyboard_manager);
      if (seat) wl_seat_destroy(seat);
      if (registry) wl_registry_destroy(registry);
      if (xkb_state) xkb_state_unref(xkb_state);
      if (xkb_keymap) xkb_keymap_unref(xkb_keymap);
      if (xkb_context) xkb_context_unref(xkb_context);
      if (display) wl_display_disconnect(display);
    }

    static void global(void *data, wl_registry *registry, std::uint32_t name, const char *interface, std::uint32_t) {
      auto self = (connection_t *) data;
      std::string_view iface {interface};
      if (iface == wl_seat_interface.name && !self->seat) {
        self->seat = (wl_seat *) wl_registry_bind(registry, name, &wl_seat_interface, 1);
      } else if (iface == zwp_virtual_keyboard_manager_v1_interface.name) {
        self->keyboard_manager = (zwp_virtual_keyboard_manager_v1 *)
          wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
      } else if (iface == zwlr_virtual_pointer_manager_v1_interface.name) {
        self->pointer_manager = (zwlr_virtual_pointer_manager_v1 *)
          wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
      }
    }

    static void global_remove(void *, wl_registry *, std::uint32_t) {}

    bool upload_keymap() {
      xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      if (!xkb_context) return false;
      // Null names honour XKB_DEFAULT_* and fall back to the evdev/us defaults.
      xkb_keymap = xkb_keymap_new_from_names(xkb_context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
      if (!xkb_keymap) return false;
      xkb_state = xkb_state_new(xkb_keymap);
      if (!xkb_state) return false;

      char *text = xkb_keymap_get_as_string(xkb_keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
      if (!text) return false;
      const auto size = std::strlen(text) + 1;
      int fd = memfd_create("helios-keymap", MFD_CLOEXEC);
      bool written = fd >= 0 && write(fd, text, size) == (ssize_t) size;
      std::free(text);
      if (written) {
        // libwayland duplicates the descriptor while marshalling the request.
        zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
      }
      if (fd >= 0) close(fd);
      return written;
    }

    /**
     * @brief Read pending compositor events and send queued requests.
     *
     * Sway announces every output hotplug to all clients; reading them keeps
     * the socket from filling up on long sessions.
     */
    bool sync() {
      while (wl_display_prepare_read(display) != 0) {
        if (wl_display_dispatch_pending(display) < 0) return false;
      }
      pollfd fd {wl_display_get_fd(display), POLLIN, 0};
      if (poll(&fd, 1, 0) > 0) {
        wl_display_read_events(display);
      } else {
        wl_display_cancel_read(display);
      }
      if (wl_display_dispatch_pending(display) < 0) return false;
      if (wl_display_flush(display) < 0 && errno != EAGAIN) return false;
      return wl_display_get_error(display) == 0;
    }

    void pointer_axis(std::uint32_t axis, int high_res_distance) {
      const auto time = now_ms();
      const auto value = wl_fixed_from_double(high_res_distance * scroll_per_detent / high_res_per_detent);
      zwlr_virtual_pointer_v1_axis_source(pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
      if (high_res_distance % high_res_per_detent == 0) {
        zwlr_virtual_pointer_v1_axis_discrete(pointer, time, axis, value, high_res_distance / high_res_per_detent);
      } else {
        zwlr_virtual_pointer_v1_axis(pointer, time, axis, value);
      }
      zwlr_virtual_pointer_v1_frame(pointer);
    }

    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_seat *seat = nullptr;
    zwp_virtual_keyboard_manager_v1 *keyboard_manager = nullptr;
    zwlr_virtual_pointer_manager_v1 *pointer_manager = nullptr;
    zwp_virtual_keyboard_v1 *keyboard = nullptr;
    zwlr_virtual_pointer_v1 *pointer = nullptr;

    struct xkb_context *xkb_context = nullptr;
    struct xkb_keymap *xkb_keymap = nullptr;
    struct xkb_state *xkb_state = nullptr;
    std::uint32_t depressed = 0, latched = 0, locked = 0, group = 0;
    std::bitset<KEY_MAX + 1> pressed;
  };

  namespace {
    const wl_registry_listener registry_listener {&connection_t::global, &connection_t::global_remove};

    std::unique_ptr<connection_t> open_connection() {
      const char *swaysock = std::getenv("SWAYSOCK");
      const char *wayland_display = std::getenv("WAYLAND_DISPLAY");
      if (!swaysock || !*swaysock || !wayland_display || !*wayland_display) return nullptr;

      auto connection = std::make_unique<connection_t>();
      connection->display = wl_display_connect(nullptr);
      if (!connection->display) {
        BOOST_LOG(warning) << "Could not connect to Sway at "sv << wayland_display << " for virtual input"sv;
        return nullptr;
      }
      connection->registry = wl_display_get_registry(connection->display);
      wl_registry_add_listener(connection->registry, &registry_listener, connection.get());
      wl_display_roundtrip(connection->display);

      if (!connection->seat || !connection->keyboard_manager || !connection->pointer_manager) {
        BOOST_LOG(warning) << "Sway at "sv << wayland_display
                           << " lacks a seat or the virtual keyboard/pointer protocols; using uinput"sv;
        return nullptr;
      }

      connection->keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
        connection->keyboard_manager, connection->seat);
      connection->pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
        connection->pointer_manager, connection->seat);
      if (!connection->upload_keymap()) {
        BOOST_LOG(warning) << "Could not build an XKB keymap for the virtual keyboard; using uinput"sv;
        return nullptr;
      }
      if (wl_display_roundtrip(connection->display) < 0) {
        BOOST_LOG(warning) << "Sway rejected the virtual input devices; using uinput"sv;
        return nullptr;
      }
      return connection;
    }
  }  // namespace

  std::unique_ptr<virtual_input_t> virtual_input_t::connect() {
    auto connection = open_connection();
    if (!connection) return nullptr;
    BOOST_LOG(info) << "Routing keyboard and mouse into Sway at "sv << std::getenv("WAYLAND_DISPLAY")
                    << " through virtual input protocols"sv;
    routed_through_compositor = true;
    return std::unique_ptr<virtual_input_t>(new virtual_input_t(std::move(connection)));
  }

  virtual_input_t::virtual_input_t(std::unique_ptr<connection_t> connection):
      connection_(std::move(connection)) {}

  virtual_input_t::~virtual_input_t() = default;

  template<typename F>
  void virtual_input_t::with_connection(F &&f) {
    std::lock_guard lock(mutex_);
    if (!connection_) {
      // Sway restarted or dropped us; never fall back to uinput, which would
      // leak streamed input into the host's other desktop.
      auto now = std::chrono::steady_clock::now();
      if (now < next_reconnect_) return;
      next_reconnect_ = now + 1s;
      connection_ = open_connection();
      if (!connection_) return;
      BOOST_LOG(info) << "Reconnected virtual input to Sway"sv;
    }

    f(*connection_);
    if (!connection_->sync()) {
      BOOST_LOG(warning) << "Lost the Sway virtual input connection: "sv
                         << std::strerror(wl_display_get_error(connection_->display));
      connection_.reset();
    }
  }

  void virtual_input_t::key(int linux_code, bool pressed) {
    if (linux_code <= 0 || linux_code > KEY_MAX) return;
    with_connection([&](connection_t &c) {
      // Wayland clients repeat held keys themselves, so a client's repeated
      // key-down must not reach the compositor as a second press.
      if (c.pressed.test(linux_code) == pressed) return;
      c.pressed.set(linux_code, pressed);

      zwp_virtual_keyboard_v1_key(c.keyboard, now_ms(), linux_code,
                                  pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);

      // XKB keycodes are evdev codes offset by 8.
      xkb_state_update_key(c.xkb_state, linux_code + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
      const auto depressed = xkb_state_serialize_mods(c.xkb_state, XKB_STATE_MODS_DEPRESSED);
      const auto latched = xkb_state_serialize_mods(c.xkb_state, XKB_STATE_MODS_LATCHED);
      const auto locked = xkb_state_serialize_mods(c.xkb_state, XKB_STATE_MODS_LOCKED);
      const auto group = xkb_state_serialize_layout(c.xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
      if (depressed != c.depressed || latched != c.latched || locked != c.locked || group != c.group) {
        c.depressed = depressed;
        c.latched = latched;
        c.locked = locked;
        c.group = group;
        zwp_virtual_keyboard_v1_modifiers(c.keyboard, depressed, latched, locked, group);
      }
    });
  }

  void virtual_input_t::move(int delta_x, int delta_y) {
    with_connection([&](connection_t &c) {
      zwlr_virtual_pointer_v1_motion(c.pointer, now_ms(), wl_fixed_from_int(delta_x), wl_fixed_from_int(delta_y));
      zwlr_virtual_pointer_v1_frame(c.pointer);
    });
  }

  void virtual_input_t::move_abs(int x, int y, int width, int height) {
    if (width <= 0 || height <= 0) return;
    with_connection([&](connection_t &c) {
      // The extent spans Sway's whole output layout, so x and y are layout coordinates.
      zwlr_virtual_pointer_v1_motion_absolute(c.pointer, now_ms(),
                                              std::clamp(x, 0, width - 1), std::clamp(y, 0, height - 1),
                                              width, height);
      zwlr_virtual_pointer_v1_frame(c.pointer);
    });
  }

  void virtual_input_t::button(int linux_button, bool pressed) {
    with_connection([&](connection_t &c) {
      zwlr_virtual_pointer_v1_button(c.pointer, now_ms(), linux_button,
                                     pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
      zwlr_virtual_pointer_v1_frame(c.pointer);
    });
  }

  void virtual_input_t::scroll(int high_res_distance) {
    // Moonlight scrolls up for positive distances; Wayland scrolls down.
    with_connection([&](connection_t &c) {
      c.pointer_axis(WL_POINTER_AXIS_VERTICAL_SCROLL, -high_res_distance);
    });
  }

  void virtual_input_t::hscroll(int high_res_distance) {
    with_connection([&](connection_t &c) {
      c.pointer_axis(WL_POINTER_AXIS_HORIZONTAL_SCROLL, high_res_distance);
    });
  }

  bool active() {
    return routed_through_compositor;
  }
}  // namespace platf::wlr_input
