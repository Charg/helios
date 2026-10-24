/**
 * @file src/platform/linux/input/wlr_virtual_input.h
 * @brief Declarations for keyboard and pointer injection through Sway's virtual input protocols.
 */
#pragma once

// standard includes
#include <chrono>
#include <memory>
#include <mutex>

namespace platf::wlr_input {
  struct connection_t;

  /**
   * @brief Keyboard and pointer devices that live inside one Sway compositor.
   *
   * uinput devices are global to the kernel seat, so a desktop such as GNOME
   * running beside Helios's headless Sway receives them while Sway does not.
   * zwp_virtual_keyboard_v1 and zwlr_virtual_pointer_v1 devices exist only in
   * the compositor that created them, which keeps streamed input private to
   * the streamed desktop.
   *
   * All codes are Linux input event codes (KEY_*, BTN_*). Calls are thread-safe.
   */
  class virtual_input_t {
  public:
    /**
     * @brief Connect to the Sway session named by SWAYSOCK and WAYLAND_DISPLAY.
     * @return The devices, or nullptr when no Sway session is targeted or it lacks the protocols.
     */
    static std::unique_ptr<virtual_input_t> connect();

    ~virtual_input_t();

    void key(int linux_code, bool pressed);
    void move(int delta_x, int delta_y);
    void move_abs(int x, int y, int width, int height);
    void button(int linux_button, bool pressed);
    void scroll(int high_res_distance);
    void hscroll(int high_res_distance);

  private:
    explicit virtual_input_t(std::unique_ptr<connection_t> connection);

    template<typename F>
    void with_connection(F &&f);

    std::mutex mutex_;
    std::unique_ptr<connection_t> connection_;
    std::chrono::steady_clock::time_point next_reconnect_;
  };

  /**
   * @brief Whether keyboard and pointer input is routed through a compositor rather than uinput.
   */
  bool active();
}  // namespace platf::wlr_input
