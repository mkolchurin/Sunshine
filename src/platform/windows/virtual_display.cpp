/**
 * @file src/platform/windows/virtual_display.cpp
 * @brief SudoVDA driver bind: open, protocol check, watchdog ping.
 */
// local includes
#include "virtual_display.h"

// standard includes
#include <thread>

// lib includes
#include <sudovda/sudovda.h>

// local includes
#include "src/logging.h"

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
}  // namespace VDISPLAY
