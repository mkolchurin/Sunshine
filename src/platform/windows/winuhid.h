/**
 * @file src/platform/windows/winuhid.h
 * @brief WinUHid relative mouse and DualSense backends (LoadLibrary, MSVC DLLs).
 */
#pragma once

// standard includes
#include <memory>
#include <vector>

// local includes
#include "src/platform/common.h"

namespace platf::winuhid {

  /**
   * @brief Relative mouse injected through WinUHid, with SendInput fallback.
   */
  class mouse_t {
  public:
    mouse_t();
    ~mouse_t();

    mouse_t(mouse_t &&other) noexcept;
    mouse_t &operator=(mouse_t &&other) noexcept;

    mouse_t(const mouse_t &) = delete;
    mouse_t &operator=(const mouse_t &) = delete;

    /**
     * @brief Submit a relative motion report.
     *
     * @param delta_x Horizontal delta in mickeys.
     * @param delta_y Vertical delta in mickeys.
     */
    void move(int delta_x, int delta_y);

    /**
     * @brief Submit a mouse button press or release.
     *
     * @param button Moonlight mouse button identifier.
     * @param release True when the button was released.
     */
    void button(int button, bool release);

    /**
     * @brief Submit a high-resolution scroll report (1/120 detent).
     *
     * @param high_res_distance Scroll distance in 1/120 units.
     * @param horizontal True for horizontal wheel.
     */
    void scroll(int high_res_distance, bool horizontal);

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
  };

  /**
   * @brief Loaded WinUHid DLL and enumerator status (no device create).
   */
  struct status_t {
    bool dlls = false;  ///< `WinUHid.dll` and `WinUHidDevs.dll` loaded.
    bool mouse = false;  ///< Relative-mouse exports resolved.
    bool ps5 = false;  ///< DualSense exports resolved.
    unsigned interface_version = 0;  ///< `WinUHidGetDriverInterfaceVersion`, or 0.
  };

  /**
   * @brief Probe WinUHid without creating a HID device.
   *
   * @return DLL load flags and enumerator interface version.
   */
  status_t status();

  /**
   * @brief Return whether WinUHid DualSense exports are usable.
   *
   * @return True when `WinUHidPS5Create` can be called.
   */
  bool ps5_available();

  /**
   * @brief One virtual DualSense pad.
   */
  class ps5_t {
  public:
    /**
     * @brief Create a DualSense through WinUHid.
     *
     * @param id Sunshine gamepad identifiers.
     * @param feedback_queue Queue used to return rumble/LED/AT to the client.
     * @return Pad, or `nullptr` on failure.
     */
    static std::shared_ptr<ps5_t> create(const gamepad_id_t &id, feedback_queue_t feedback_queue);

    ~ps5_t();

    ps5_t(const ps5_t &) = delete;
    ps5_t &operator=(const ps5_t &) = delete;

    /**
     * @brief Replace the feedback queue after a client resume.
     *
     * @param feedback_queue Queue for the resumed session.
     */
    void rebind(feedback_queue_t feedback_queue);

    /**
     * @brief Submit buttons, sticks, triggers, and hat.
     *
     * @param state Moonlight gamepad state.
     */
    void update(const gamepad_state_t &state);

    /**
     * @brief Submit DualSense touchpad contact.
     *
     * @param touch Moonlight controller touch event.
     */
    void touch(const gamepad_touch_t &touch);

    /**
     * @brief Submit accelerometer or gyroscope.
     *
     * @param motion Moonlight motion event (accel m/s^2, gyro deg/s).
     */
    void motion(const gamepad_motion_t &motion);

    /**
     * @brief Submit battery metadata.
     *
     * @param battery Moonlight battery event.
     */
    void battery(const gamepad_battery_t &battery);

  private:
    ps5_t();

    struct impl;
    std::shared_ptr<impl> impl_;
  };

  /**
   * @brief DualSense slots keyed by global gamepad index.
   */
  class pads_t {
  public:
    pads_t();

    /**
     * @brief Allocate a DualSense in a global slot.
     *
     * @param id Sunshine gamepad identifiers.
     * @param feedback_queue Queue used to return feedback to the client.
     * @return 0 on success, otherwise -1.
     */
    int alloc(const gamepad_id_t &id, feedback_queue_t feedback_queue);

    /**
     * @brief Rebind feedback for an existing DualSense slot.
     *
     * @param id Sunshine gamepad identifiers.
     * @param feedback_queue Queue for the resumed session.
     * @return 0 when the slot exists.
     */
    int rebind(const gamepad_id_t &id, feedback_queue_t feedback_queue);

    /**
     * @brief Return whether a DualSense occupies a slot.
     *
     * @param nr Global gamepad index.
     * @return True when the slot is a WinUHid DualSense.
     */
    bool has(int nr) const;

    /**
     * @brief Destroy the DualSense in a slot.
     *
     * @param nr Global gamepad index.
     */
    void free(int nr);

    /**
     * @brief Return a DualSense slot.
     *
     * @param nr Global gamepad index.
     * @return Pad, or `nullptr`.
     */
    ps5_t *get(int nr);

  private:
    std::vector<std::shared_ptr<ps5_t>> slots;
  };

}  // namespace platf::winuhid
