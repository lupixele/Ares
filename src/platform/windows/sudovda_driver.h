/**
 * @file src/platform/windows/sudovda_driver.h
 * @brief SudoVDA virtual display driver client implementing the session driver backend contract.
 *
 * @details This is the concrete driver-backed implementation of
 * platf::windows::idriver_backend_t. It opens the SudoVDA control device, verifies the IOCTL
 * protocol version, and performs add/remove/ping operations against the real driver.
 *
 * Two properties make it safe to use on hosts where the driver is absent or mismatched:
 *
 * - It never installs, signs, patches or redistributes driver files. It only talks to a driver the
 *   operator has already installed and trusted.
 * - Every operation reports failure instead of throwing when the device cannot be opened, so the
 *   boundary above it can keep the session on a physical display.
 *
 * The implementation deliberately keeps no process-global state: each instance owns its own handle
 * and its own monitor identities.
 */

// located in src/platform/windows

#pragma once

// standard includes
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// platform includes
#include <guiddef.h>
#include <windows.h>

// local includes
#include "third-party/sudovda/sudovda-ioctl.h"
#include "src/platform/windows/session_driver_boundary.h"

namespace platf::windows {

  /**
   * @brief Outcome of opening the driver handle.
   */
  enum class sudovda_open_status_e {
    ok,                      ///< Handle opened and protocol version accepted.
    device_not_found,        ///< Driver control device is not present.
    version_incompatible,    ///< Driver protocol version does not match this build.
    open_failed,             ///< Device present but could not be opened.
  };

  /**
   * @brief Protocol compatibility decision between this build and the driver.
   *
   * @param client_version Protocol version this build was compiled against.
   * @param driver_version Protocol version reported by the driver.
   * @returns True when the two versions describe the same IOCTL contract.
   */
  bool sudovda_protocol_compatible(const SUDOVDA::SUVDA_PROTOCAL_VERSION &client_version,
                                   const SUDOVDA::SUVDA_PROTOCAL_VERSION &driver_version);

  /**
   * @brief Concrete SudoVDA-backed driver backend.
   */
  class sudovda_driver_t : public idriver_backend_t {
  public:
    /**
     * @brief Backend pin mask used while the driver handle is open.
     */
    struct ping_policy_t {
      uint32_t watchdog_timeout {0};   ///< Driver-reported watchdog timeout in seconds.
    };

    /**
     * @brief Backend pin interval, in milliseconds, used when the watchdog is disabled.
     */
    static constexpr uint32_t DEFAULT_PING_INTERVAL_MS = 30000;

    /**
     * @brief Number of consecutive failed pings tolerated before reporting a dead driver.
     */
    static constexpr uint32_t DEFAULT_MAX_PING_FAILURES = 3;

    sudovda_driver_t();
    ~sudovda_driver_t() override;

    sudovda_driver_t(const sudovda_driver_t &) = delete;
    sudovda_driver_t &operator=(const sudovda_driver_t &) = delete;
    sudovda_driver_t(sudovda_driver_t &&) = delete;
    sudovda_driver_t &operator=(sudovda_driver_t &&) = delete;

    /**
     * @brief Attempt to open the driver handle and accept its protocol version.
     * @returns Reason describing why the driver is usable or not.
     * @note Insecure failures are reported, never retried indefinitely.
     */
    sudovda_open_status_e open();

    /**
     * @brief Close the driver handle, leaving allocated monitors untouched.
     * @note Closing the handle does not release displays; callers must remove them explicitly.
     */
    void close();

    /**
     * @brief Close the driver handle when the caller already holds the lock.
     * @note Used by open() so a failed probe can drop the handle without re-locking.
     */
    void close_locked();

    /**
     * @brief Check whether the driver handle is open and responsive.
     * @return True when the driver can accept operations.
     */
    bool is_driver_ready() override;

    /**
     * @brief Allocate a virtual display through the driver.
     * @param spec Requested display geometry and client identity.
     * @return Device name of the created display, or nullopt on failure.
     */
    std::optional<std::wstring> allocate_display(const session_display_spec_t &spec) override;

    /**
     * @brief Release a virtual display through the driver.
     * @param display_name Device name previously returned by allocate_display.
     * @return True when the driver accepted the removal.
     */
    bool release_display(const std::wstring &display_name) override;

    /**
     * @brief Keep the driver watchdog alive.
     * @return True when the driver acknowledged the ping.
     */
    bool ping_watchdog() override;

    /**
     * @brief Retain cleanup responsibility for a display whose release failed.
     * @param display_name Device name that could not be removed.
     * @note The name is durably retained for a later retry and never silently dropped.
     */
    void unresolved_cleanup(const std::wstring &display_name) noexcept override;

    /**
     * @brief Monitors currently believed to remain allocated by this instance.
     * @return Device names that must still be released.
     */
    std::vector<std::wstring> retained_monitors() const;

    /**
     * @brief Whether the driver handle is currently open.
     * @return True when a handle is held.
     */
    bool handle_open() const;

    /**
     * @brief Highest resolved client name recorded for a monitor identity.
     * @param display_name Device name previously returned by allocate_display.
     * @return Stored client name, or empty when unknown.
     */
    std::string monitor_client_name(const std::wstring &display_name) const;

  private:
    /**
     * @brief Monitor identity bookkeeping for one allocated display.
     */
    struct monitor_record_t {
      GUID guid {};            ///< Identity passed to the driver for removal.
      std::wstring name;       ///< Resolved system device name.
      session_display_spec_t spec {};  ///< Parameters used to create the display.
    };

    /**
     * @brief Find the record for a device name.
     * @param display_name Device name to look up.
     * @return Iterator to the record, or end() when not tracked.
     */
    std::vector<monitor_record_t>::iterator find_record(const std::wstring &display_name);

    mutable std::mutex _mutex;              ///< Serializes handle and monitor operations.
    HANDLE _handle {INVALID_HANDLE_VALUE};  ///< Driver control device handle.
    ping_policy_t _ping_policy {};          ///< Last observed watchdog policy.
    uint32_t _consecutive_ping_failures {0};  ///< Consecutive unacknowledged pings.
    uint32_t _max_ping_failures {DEFAULT_MAX_PING_FAILURES};  ///< Failure threshold.
    std::vector<monitor_record_t> _monitors;   ///< Displays this instance created.
    std::vector<std::wstring> _retained;       ///< Releases that failed and need retry.
  };

}  // namespace platf::windows
