/**
 * @file src/platform/windows/virtual_display.cpp
 * @brief SudoVDA: open/ping, SET_ADAPTER, ADD/REMOVE, CDS.
 */
// local includes
#include "virtual_display.h"

// standard includes
#include <algorithm>
#include <string_view>
#include <thread>
#include <vector>

// lib includes
#include <dxgi.h>
#include <dxgi1_6.h>
#include <sudovda/sudovda.h>
#include <wrl/client.h>

// local includes
#include "src/logging.h"
#include "src/platform/windows/utf_utils.h"

using namespace SUDOVDA;

namespace VDISPLAY {
  HANDLE SUDOVDA_DRIVER_HANDLE = INVALID_HANDLE_VALUE;

  void closeVDisplayDevice() {
    if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return;
    }

    CloseHandle(SUDOVDA_DRIVER_HANDLE);
    SUDOVDA_DRIVER_HANDLE = INVALID_HANDLE_VALUE;
  }

  DRIVER_STATUS openVDisplayDevice() {
    uint32_t retryInterval = 20;
    while (true) {
      SUDOVDA_DRIVER_HANDLE = OpenDevice(&SUVDA_INTERFACE_GUID);
      if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
        if (retryInterval > 320) {
          BOOST_LOG(error) << "SudoVDA: Open device failed";
          return DRIVER_STATUS::FAILED;
        }
        retryInterval *= 2;
        Sleep(retryInterval);
        continue;
      }

      break;
    }

    if (!CheckProtocolCompatible(SUDOVDA_DRIVER_HANDLE)) {
      BOOST_LOG(error) << "SudoVDA: protocol not compatible with driver";
      closeVDisplayDevice();
      return DRIVER_STATUS::VERSION_INCOMPATIBLE;
    }

    BOOST_LOG(info) << "SudoVDA: device opened, protocol compatible";
    return DRIVER_STATUS::OK;
  }

  bool startPingThread(std::function<void()> failCb) {
    if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return false;
    }

    VIRTUAL_DISPLAY_GET_WATCHDOG_OUT watchdogOut {};
    if (GetWatchdogTimeout(SUDOVDA_DRIVER_HANDLE, watchdogOut)) {
      BOOST_LOG(info) << "SudoVDA: watchdog timeout " << watchdogOut.Timeout << "s, countdown " << watchdogOut.Countdown;
    } else {
      BOOST_LOG(error) << "SudoVDA: watchdog fetch failed";
      return false;
    }

    if (!watchdogOut.Timeout) {
      BOOST_LOG(warning) << "SudoVDA: watchdog timeout is 0, ping thread not started";
      return true;
    }

    const auto sleepInterval = watchdogOut.Timeout * 1000 / 3;
    BOOST_LOG(info) << "SudoVDA: ping thread interval " << sleepInterval << "ms";

    std::thread ping_thread([sleepInterval, failCb = std::move(failCb)]() {
      uint8_t fail_count = 0;
      bool logged_ok = false;
      for (;;) {
        if (!PingDriver(SUDOVDA_DRIVER_HANDLE)) {
          fail_count += 1;
          BOOST_LOG(warning) << "SudoVDA: ping failed (" << static_cast<int>(fail_count) << "/3)";
          if (fail_count > 3) {
            failCb();
            return;
          }
        } else {
          fail_count = 0;
          if (!logged_ok) {
            BOOST_LOG(info) << "SudoVDA: ping ok";
            logged_ok = true;
          }
        }
        Sleep(sleepInterval);
      }
    });

    ping_thread.detach();
    return true;
  }

  bool setRenderAdapterByName(const std::wstring &adapterName) {
    if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return false;
    }

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      BOOST_LOG(error) << "SudoVDA: CreateDXGIFactory1 failed";
      return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc;
    int i = 0;
    while (SUCCEEDED(factory->EnumAdapters(i, &adapter))) {
      i += 1;

      if (!SUCCEEDED(adapter->GetDesc(&desc))) {
        continue;
      }

      const auto description = std::wstring_view(desc.Description);
      if (!adapterName.empty()) {
        if (description != adapterName) {
          continue;
        }
      } else if (description.find(L"Microsoft") != std::wstring_view::npos) {
        continue;
      }

      if (SetRenderAdapter(SUDOVDA_DRIVER_HANDLE, desc.AdapterLuid)) {
        BOOST_LOG(info) << "SudoVDA: render adapter set to " << utf_utils::to_utf8(std::wstring {description});
        return true;
      }

      BOOST_LOG(error) << "SudoVDA: IOCTL_SET_RENDER_ADAPTER failed for " << utf_utils::to_utf8(std::wstring {description});
      return false;
    }

    if (adapterName.empty()) {
      BOOST_LOG(warning) << "SudoVDA: no non-Microsoft render adapter found";
    } else {
      BOOST_LOG(warning) << "SudoVDA: render adapter not found: " << utf_utils::to_utf8(adapterName);
    }
    return false;
  }

  std::wstring createVirtualDisplay(
    const char *s_client_uid,
    const char *s_client_name,
    uint32_t width,
    uint32_t height,
    uint32_t fps,
    const GUID &guid
  ) {
    if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return {};
    }

    VIRTUAL_DISPLAY_ADD_OUT output {};
    if (!AddVirtualDisplay(SUDOVDA_DRIVER_HANDLE, width, height, fps, guid, s_client_name, s_client_uid, output)) {
      BOOST_LOG(error) << "SudoVDA: ADD virtual display failed";
      return {};
    }

    uint32_t retryInterval = 20;
    wchar_t deviceName[CCHDEVICENAME] {};
    while (!GetAddedDisplayName(output, deviceName)) {
      Sleep(retryInterval);
      if (retryInterval > 320) {
        BOOST_LOG(error) << "SudoVDA: cannot get name for newly added virtual display";
        return {};
      }
      retryInterval *= 2;
    }

    BOOST_LOG(info) << "SudoVDA: virtual display added " << utf_utils::to_utf8(deviceName)
                    << " " << width << "x" << height << "@" << fps;

    return {deviceName};
  }

  bool removeVirtualDisplay(const GUID &guid) {
    if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
      return false;
    }

    if (RemoveVirtualDisplay(SUDOVDA_DRIVER_HANDLE, guid)) {
      BOOST_LOG(info) << "SudoVDA: virtual display removed";
      return true;
    }

    BOOST_LOG(warning) << "SudoVDA: REMOVE virtual display failed";
    return false;
  }

  LONG changeDisplaySettings(const wchar_t *deviceName, int width, int height, int refresh_rate) {
    DEVMODEW devMode {};
    devMode.dmSize = sizeof(devMode);

    if (!EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &devMode)) {
      BOOST_LOG(warning) << "SudoVDA: EnumDisplaySettingsW failed for " << utf_utils::to_utf8(deviceName);
      return ERROR_INVALID_PARAMETER;
    }

    DWORD targetRefreshRate = refresh_rate / 1000;
    DWORD altRefreshRate = targetRefreshRate;

    if (refresh_rate % 1000) {
      if (refresh_rate % 1000 >= 900) {
        targetRefreshRate += 1;
      } else {
        altRefreshRate += 1;
      }
    } else {
      altRefreshRate -= 1;
    }

    BOOST_LOG(info) << "SudoVDA: applying mode " << width << "x" << height << "x" << targetRefreshRate
                    << " for " << utf_utils::to_utf8(deviceName);

    devMode.dmPelsWidth = width;
    devMode.dmPelsHeight = height;
    devMode.dmDisplayFrequency = targetRefreshRate;
    devMode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;

    auto res = ChangeDisplaySettingsExW(deviceName, &devMode, nullptr, CDS_UPDATEREGISTRY, nullptr);

    if (res != ERROR_SUCCESS) {
      BOOST_LOG(warning) << "SudoVDA: baseline mode failed, trying alt " << width << "x" << height << "x" << altRefreshRate;
      devMode.dmDisplayFrequency = altRefreshRate;
      res = ChangeDisplaySettingsExW(deviceName, &devMode, nullptr, CDS_UPDATEREGISTRY, nullptr);
      if (res != ERROR_SUCCESS) {
        BOOST_LOG(error) << "SudoVDA: alt baseline mode failed";
      }
    }

    if (res == ERROR_SUCCESS) {
      BOOST_LOG(info) << "SudoVDA: baseline display mode applied";
    }

    return res;
  }

  namespace {
    bool findDisplayIds(const wchar_t *displayName, LUID &adapterId, uint32_t &targetId) {
      UINT32 pathCount = 0;
      UINT32 modeCount = 0;
      if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount)) {
        return false;
      }

      std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
      std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
      if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr)) {
        return false;
      }

      auto path = std::find_if(paths.begin(), paths.end(), [&displayName](const DISPLAYCONFIG_PATH_INFO &candidate) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName {};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = candidate.sourceInfo.adapterId;
        sourceName.header.id = candidate.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
          return false;
        }
        return std::wstring_view(displayName) == sourceName.viewGdiDeviceName;
      });

      if (path == paths.end()) {
        return false;
      }

      adapterId = path->sourceInfo.adapterId;
      targetId = path->targetInfo.id;
      return true;
    }

    bool getDisplayHDR(const LUID &adapterLuid, const wchar_t *displayName) {
      Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
      if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
      }

      for (UINT adapterIdx = 0;; ++adapterIdx) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory->EnumAdapters1(adapterIdx, adapter.ReleaseAndGetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) {
          break;
        }
        if (FAILED(hr)) {
          break;
        }

        DXGI_ADAPTER_DESC1 adapterDesc {};
        if (FAILED(adapter->GetDesc1(&adapterDesc))) {
          continue;
        }
        if (adapterDesc.AdapterLuid.LowPart != adapterLuid.LowPart || adapterDesc.AdapterLuid.HighPart != adapterLuid.HighPart) {
          continue;
        }

        const std::wstring_view displayNameView {displayName};
        for (UINT outputIdx = 0;; ++outputIdx) {
          Microsoft::WRL::ComPtr<IDXGIOutput> output;
          const HRESULT outHr = adapter->EnumOutputs(outputIdx, output.ReleaseAndGetAddressOf());
          if (outHr == DXGI_ERROR_NOT_FOUND) {
            break;
          }
          if (FAILED(outHr) || !output) {
            continue;
          }

          DXGI_OUTPUT_DESC outputDesc {};
          if (FAILED(output->GetDesc(&outputDesc))) {
            continue;
          }

          MONITORINFOEXW monitorInfo {};
          monitorInfo.cbSize = sizeof(monitorInfo);
          if (!GetMonitorInfoW(outputDesc.Monitor, &monitorInfo) || displayNameView != monitorInfo.szDevice) {
            continue;
          }

          Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
          if (FAILED(output.As(&output6)) || !output6) {
            return false;
          }
          DXGI_OUTPUT_DESC1 desc1 {};
          if (FAILED(output6->GetDesc1(&desc1))) {
            return false;
          }
          return desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        }
        return false;
      }
      return false;
    }
  }  // namespace

  bool getDisplayHDRByName(const wchar_t *displayName) {
    LUID adapterId {};
    uint32_t targetId = 0;
    if (!findDisplayIds(displayName, adapterId, targetId)) {
      return false;
    }
    return getDisplayHDR(adapterId, displayName);
  }

  bool setDisplayHDRByName(const wchar_t *displayName, bool enableAdvancedColor) {
    LUID adapterId {};
    uint32_t targetId = 0;
    if (!findDisplayIds(displayName, adapterId, targetId)) {
      return false;
    }

    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE setHdrInfo {};
    setHdrInfo.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
    setHdrInfo.header.size = sizeof(setHdrInfo);
    setHdrInfo.header.adapterId = adapterId;
    setHdrInfo.header.id = targetId;
    setHdrInfo.enableAdvancedColor = enableAdvancedColor;
    return DisplayConfigSetDeviceInfo(&setHdrInfo.header) == ERROR_SUCCESS;
  }
}  // namespace VDISPLAY
