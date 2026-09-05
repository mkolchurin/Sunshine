/**
 * @file src/platform/windows/virtual_display.h
 * @brief SudoVDA driver bind, ADD/REMOVE, render adapter, CDS.
 */
#pragma once

// standard includes
#include <cstdint>
#include <functional>
#include <string>

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

  /**
   * @brief Bind the SudoVDA render adapter by DXGI description.
   * @param adapterName UTF-16 adapter name, e.g. `NVIDIA GeForce RTX 4070 SUPER`.
   *   Empty: first adapter whose description does not contain `Microsoft`.
   * @return `true` when `IOCTL_SET_RENDER_ADAPTER` succeeded.
   */
  bool setRenderAdapterByName(const std::wstring &adapterName);

  /**
   * @brief Create a plug-and-play virtual display.
   * @param s_client_uid Serial / identity string stored on the monitor.
   * @param s_client_name Monitor name (truncated by the driver).
   * @param width Pixel width.
   * @param height Pixel height.
   * @param fps Refresh: Hz if `< 1000`, otherwise millihertz (Apollo ioctl).
   * @param guid Persistent monitor GUID (Moonlight unique_id).
   * @return GDI device name, or empty on failure.
   */
  std::wstring createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const GUID &guid
  );

  /**
   * @brief Remove the virtual display identified by `guid`.
   * @return `true` when the ioctl succeeded.
   */
  bool removeVirtualDisplay(const GUID &guid);

  /**
   * @brief Apply baseline mode via `ChangeDisplaySettingsExW` (Hz and Hz±1).
   * @param deviceName GDI name from `createVirtualDisplay`.
   * @param width Pixel width.
   * @param height Pixel height.
   * @param refresh_rate Millihertz (session path); probe uses Hz via ADD only.
   * @return Win32 status from the last CDS call, or `ERROR_INVALID_PARAMETER`.
   */
  LONG changeDisplaySettings(const wchar_t *deviceName, int width, int height, int refresh_rate);

  /**
   * @brief Whether Advanced Color (HDR) is active on the named GDI output.
   */
  bool getDisplayHDRByName(const wchar_t *displayName);

  /**
   * @brief Enable or disable Advanced Color on the named GDI output.
   * @return `true` when CCD `DisplayConfigSetDeviceInfo` succeeded.
   */
  bool setDisplayHDRByName(const wchar_t *displayName, bool enableAdvancedColor);
}  // namespace VDISPLAY
