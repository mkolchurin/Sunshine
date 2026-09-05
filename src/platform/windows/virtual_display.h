/**
 * @file src/platform/windows/virtual_display.h
 * @brief SudoVDA driver bind (open / ping / status). ADD/REMOVE come in later slices.
 */
#pragma once

// standard includes
#include <functional>

// platform includes
#include <windows.h>

namespace VDISPLAY {
  /**
   * @brief Result of opening and talking to the SudoVDA UMDF device.
   *
   * Integer values are part of the Web UI contract (`vdisplayStatus`).
   */
  enum class DRIVER_STATUS : int {
    UNKNOWN = 1,
    OK = 0,
    FAILED = -1,
    VERSION_INCOMPATIBLE = -2,
    WATCHDOG_FAILED = -3
  };

  extern HANDLE SUDOVDA_DRIVER_HANDLE;

  /**
   * @brief Close the SudoVDA device handle if it is open.
   */
  void closeVDisplayDevice();

  /**
   * @brief Open the SudoVDA device and verify ioctl protocol compatibility.
   * @return `OK` when the handle is usable; otherwise a failure status.
   */
  DRIVER_STATUS openVDisplayDevice();

  /**
   * @brief Start a detached watchdog ping thread.
   * @param failCb Invoked after several consecutive ping failures.
   * @return `false` if the handle is invalid or watchdog timeout cannot be read.
   */
  bool startPingThread(std::function<void()> failCb);
}  // namespace VDISPLAY
