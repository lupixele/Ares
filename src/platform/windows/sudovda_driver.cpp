/**
 * @file src/platform/windows/sudovda_driver.cpp
 * @brief SudoVDA virtual display driver client implementation.
 *
 * @details The implementation performs no driver installation, signing or redistribution. It opens an
 * already-installed and already-trusted SudoVDA control device and issues IOCTLs against it. Every
 * failure path is reported to the caller rather than thrown, so the session boundary above can keep
 * streaming on a physical display when no virtual display is available.
 */

// located in src/platform/windows

// standard includes
#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

// platform includes
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>

// local includes
#include "src/logging.h"
#include "third-party/sudovda/sudovda-ioctl.h"
#include "src/platform/windows/sudovda_driver.h"

namespace platf::windows {
  using namespace std::literals;

  namespace {
    static_assert(std::is_same_v<std::uint64_t, unsigned long long> || sizeof(std::uint64_t) == sizeof(unsigned long));
    /** @brief Bound on how often a driver open is retried before giving up. */
    constexpr int MAX_OPEN_ATTEMPTS = 4;
    /** @brief Initial backoff, doubled on each failed open attempt. */
    constexpr std::chrono::milliseconds OPEN_BACKOFF_BASE(20);
    /** @brief Longest backoff used while opening the driver device. */
    constexpr std::chrono::milliseconds OPEN_BACKOFF_MAX(320);
    /** @brief Driver-recorded device name field size. */
    constexpr std::size_t DEVICE_NAME_CHARS = 14;
    /** @brief Maximum serial bytes copied into the driver parameter block. */
    constexpr std::size_t SERIAL_CHARS = 13;
    /**
     * @brief Copy a UTF-8 string into a fixed driver buffer.
     * @param destination Fixed-size buffer to fill.
     * @param source Bytes to copy.
     * @param capacity Number of writable characters, including terminator.
     * @note Always leaves the buffer NUL-terminated and never overruns it.
     */
    void copy_truncated(CHAR *destination, const std::string &source, std::size_t capacity) {
      if (capacity == 0) {
        return;
      }
      std::memset(destination, 0, capacity);
      const auto copyable = std::min(source.size(), capacity - 1);
      if (copyable > 0) {
        std::memcpy(destination, source.data(), copyable);
      }
    }

    /**
     * @brief Issue a buffered IOCTL with separate input and output buffers.
     * @param handle Driver control device.
     * @param control_code IOCTL to send.
     * @param input Input buffer, or nullptr when unused.
     * @param input_size Size of input buffer.
     * @param output Output buffer, or nullptr when unused.
     * @param output_size Size of output buffer.
     * @return True when the driver acknowledged the request.
     */
    bool send_ioctl(HANDLE handle,
                    DWORD control_code,
                    const void *input,
                    DWORD input_size,
                    void *output,
                    DWORD output_size) {
      if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
        return false;
      }
      DWORD returned = 0;
      const BOOL ok = DeviceIoControl(handle, control_code, const_cast<LPVOID>(input), input_size, output, output_size, &returned, nullptr);
      if (!ok) {
        BOOST_LOG(debug) << "SudoVDA IOCTL 0x"sv << std::hex << control_code << std::dec << " failed with error "sv << GetLastError();
      }
      return ok == TRUE;
    }

    /**
     * @brief Open the driver control device interface.
     * @return Driver handle, or INVALID_HANDLE_VALUE when unavailable.
     */
    HANDLE open_device_interface() {
      const GUID *interface_guid = &SUDOVDA::SUVDA_INTERFACE_GUID;
      HDEVINFO device_info = SetupDiGetClassDevs(interface_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
      if (device_info == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
      }

      HANDLE handle = INVALID_HANDLE_VALUE;
      SP_DEVICE_INTERFACE_DATA interface_data {};
      interface_data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
      for (DWORD index = 0; SetupDiEnumDeviceInterfaces(device_info, nullptr, interface_guid, index, &interface_data); ++index) {
        DWORD required = 0;
        if (!SetupDiGetDeviceInterfaceDetailA(device_info, &interface_data, nullptr, 0, &required, nullptr) && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
          continue;
        }
        if (required == 0) {
          continue;
        }

        std::vector<BYTE> detail_buffer(required);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A *>(detail_buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(device_info, &interface_data, detail, required, &required, nullptr)) {
          continue;
        }

        handle = CreateFileA(detail->DevicePath,
                             GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
          break;
        }
      }

      SetupDiDestroyDeviceInfoList(device_info);
      return handle;
    }

    /**
     * @brief Convert a refresh rate to the driver's millihertz unit.
     * @param fps Refresh rate in hertz.
     * @return Rate in millihertz, or 0 when no finite value can represent it.
     */
    std::uint32_t to_refresh_millihz(std::uint32_t fps) {
      if (fps > 1000000) {
        return 0;
      }
      return fps * 1000;
    }

    /**
     * @brief Resolve the GDI device name for a newly added display.
     * @param output Driver-reported adapter and target identifiers.
     * @param device_name Buffer receiving the device name, including terminator.
     * @param capacity Number of writable characters in device_name.
     * @return True when a name was resolved and written.
     * @note The OS may not have published the new display yet, so misses are expected.
     */
    bool resolve_added_display_name(const SUDOVDA::VIRTUAL_DISPLAY_ADD_OUT &output,
                                    wchar_t *device_name,
                                    std::size_t capacity) {
      if (capacity == 0) {
        return false;
      }
      device_name[0] = L'\0';

      UINT path_count = 0;
      UINT mode_count = 0;
      if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
        return false;
      }
      if (path_count == 0 || mode_count == 0) {
        return false;
      }

      std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
      std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
      if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) != ERROR_SUCCESS) {
        return false;
      }

      const auto path = std::find_if(paths.begin(), paths.end(), [&output](const DISPLAYCONFIG_PATH_INFO &entry) {
        return entry.targetInfo.id == output.TargetId;
      });
      if (path == paths.end()) {
        return false;
      }

      DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name {};
      source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
      source_name.header.size = sizeof(DISPLAYCONFIG_SOURCE_DEVICE_NAME);
      source_name.header.adapterId = output.AdapterLuid;
      source_name.header.id = path->sourceInfo.id;
      if (DisplayConfigGetDeviceInfo(&source_name.header) != ERROR_SUCCESS) {
        return false;
      }

      // Copy only up to the end of the GDI device name array.
      const wchar_t *view = source_name.viewGdiDeviceName;
      constexpr std::size_t view_capacity = sizeof(source_name.viewGdiDeviceName) / sizeof(wchar_t);
      std::size_t length = 0;
      while (length < view_capacity - 1 && length < capacity - 1 && view[length] != L'\0') {
        ++length;
      }
      if (length > 0) {
        std::wmemcpy(device_name, view, length);
      }
      device_name[length] = L'\0';
      return length > 0;
    }
  }  // namespace

  bool sudovda_protocol_compatible(const SUDOVDA::SUVDA_PROTOCAL_VERSION &client_version,
                                   const SUDOVDA::SUVDA_PROTOCAL_VERSION &driver_version) {
    // A major change is always incompatible.
    if (client_version.Major != driver_version.Major) {
      return false;
    }
    // A driver older than this client may not implement current IOCTLs.
    if (driver_version.Minor < client_version.Minor) {
      return false;
    }
    // A test build pairing with a release build is still accepted; incremental is informational.
    return true;
  }

  sudovda_driver_t::sudovda_driver_t() = default;

  sudovda_driver_t::~sudovda_driver_t() {
    close();
  }

  sudovda_open_status_e sudovda_driver_t::open() {
    std::lock_guard lock(_mutex);
    if (_handle != INVALID_HANDLE_VALUE) {
      return sudovda_open_status_e::ok;
    }

    std::chrono::milliseconds backoff = OPEN_BACKOFF_BASE;
    sudovda_open_status_e status = sudovda_open_status_e::device_not_found;
    for (int attempt = 0; attempt < MAX_OPEN_ATTEMPTS; ++attempt) {
      _handle = open_device_interface();
      if (_handle != INVALID_HANDLE_VALUE) {
        break;
      }

      if (attempt == 0) {
        // Distinguish "no device" from "device present but not openable".
        const DWORD error = GetLastError();
        status = (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_NO_MORE_ITEMS) ?
                   sudovda_open_status_e::device_not_found :
                   sudovda_open_status_e::open_failed;
      }

      if (status == sudovda_open_status_e::device_not_found && attempt == 0) {
        // A freshly booted system may still be enumerating the device.
        std::this_thread::sleep_for(backoff);
      }
      backoff = std::min(backoff * 2, OPEN_BACKOFF_MAX);
    }

    if (_handle == INVALID_HANDLE_VALUE) {
      return status;
    }

    SUDOVDA::VIRTUAL_DISPLAY_GET_PROTOCOL_VERSION_OUT version {};
    if (!send_ioctl(_handle,
                    IOCTL_GET_PROTOCOL_VERSION,
                    nullptr,
                    0,
                    &version,
                    sizeof(version))) {
      close_locked();
      return sudovda_open_status_e::open_failed;
    }

    if (!sudovda_protocol_compatible(SUDOVDA::VDAProtocolVersion, version.Version)) {
      BOOST_LOG(warning) << "SudoVDA driver protocol "sv << static_cast<int>(version.Version.Major) << '.'
                         << static_cast<int>(version.Version.Minor) << '.' << static_cast<int>(version.Version.Incremental)
                         << " is incompatible with client "sv << static_cast<int>(SUDOVDA::VDAProtocolVersion.Major) << '.'
                         << static_cast<int>(SUDOVDA::VDAProtocolVersion.Minor);
      close_locked();
      return sudovda_open_status_e::version_incompatible;
    }

    SUDOVDA::VIRTUAL_DISPLAY_GET_WATCHDOG_OUT watchdog {};
    if (send_ioctl(_handle, IOCTL_GET_WATCHDOG, nullptr, 0, &watchdog, sizeof(watchdog))) {
      _ping_policy.watchdog_timeout = watchdog.Timeout;
    }

    BOOST_LOG(info) << "SudoVDA driver ready (protocol "sv
                    << static_cast<int>(version.Version.Major) << '.' << static_cast<int>(version.Version.Minor)
                    << ", watchdog "sv << watchdog.Timeout << "s)"sv;
    return sudovda_open_status_e::ok;
  }

  void sudovda_driver_t::close() {
    std::lock_guard lock(_mutex);
    close_locked();
  }

  /**
   * @brief Close the driver handle assuming the mutex is already held.
   * @note Split out so that open() can release a failed handle without re-locking.
   */
  void sudovda_driver_t::close_locked() {
    if (_handle != INVALID_HANDLE_VALUE) {
      CloseHandle(_handle);
      _handle = INVALID_HANDLE_VALUE;
    }
    _ping_policy = ping_policy_t {};
    _consecutive_ping_failures = 0;
  }

  bool sudovda_driver_t::is_driver_ready() {
    std::lock_guard lock(_mutex);
    return _handle != INVALID_HANDLE_VALUE;
  }

  bool sudovda_driver_t::handle_open() const {
    std::lock_guard lock(_mutex);
    return _handle != INVALID_HANDLE_VALUE;
  }

  std::optional<std::wstring> sudovda_driver_t::allocate_display(const session_display_spec_t &spec) {
    if (spec.width == 0 || spec.height == 0 || spec.fps == 0) {
      BOOST_LOG(warning) << "Refusing invalid virtual display geometry"sv;
      return std::nullopt;
    }
    if (spec.width > 7680 || spec.height > 4320 || spec.fps > 1000) {
      BOOST_LOG(warning) << "Requested virtual display exceeds supported bounds"sv;
      return std::nullopt;
    }

    std::lock_guard lock(_mutex);
    if (_handle == INVALID_HANDLE_VALUE) {
      return std::nullopt;
    }

    // Generate a fresh identity for this monitor. A failure to obtain one must not proceed.
    GUID monitor_guid {};
    if (FAILED(CoCreateGuid(&monitor_guid))) {
      BOOST_LOG(error) << "Could not create a monitor identity"sv;
      return std::nullopt;
    }

    SUDOVDA::VIRTUAL_DISPLAY_ADD_PARAMS params {};
    params.Width = spec.width;
    params.Height = spec.height;
    params.RefreshRate = to_refresh_millihz(spec.fps);
    params.MonitorGuid = monitor_guid;
    copy_truncated(params.DeviceName, spec.client_name, sizeof(params.DeviceName));
    copy_truncated(params.SerialNumber, spec.client_uid, sizeof(params.SerialNumber));

    SUDOVDA::VIRTUAL_DISPLAY_ADD_OUT added {};
    if (!send_ioctl(_handle,
                    IOCTL_ADD_VIRTUAL_DISPLAY,
                    &params,
                    sizeof(params),
                    &added,
                    sizeof(added))) {
      BOOST_LOG(warning) << "SudoVDA rejected the virtual display request"sv;
      return std::nullopt;
    }

    // The device name is resolved by the OS, not by the driver, and may lag slightly.
    // Poll with a bounded backoff instead of guessing the path.
    wchar_t device_name[MAX_PATH] {};
    std::chrono::milliseconds interval(20);
    constexpr int MAX_NAME_ATTEMPTS = 8;
    bool resolved = false;
    for (int attempt = 0; attempt < MAX_NAME_ATTEMPTS && !resolved; ++attempt) {
      resolved = resolve_added_display_name(added, device_name, MAX_PATH);
      if (!resolved) {
        std::this_thread::sleep_for(interval);
        interval = std::min(interval * 2, OPEN_BACKOFF_MAX);
      }
    }

    monitor_record_t record;
    record.guid = monitor_guid;
    record.spec = spec;
    if (resolved) {
      record.name.assign(device_name);
    }
    auto stored_name = record.name;
    _monitors.push_back(std::move(record));
    return stored_name;
  }

  bool sudovda_driver_t::release_display(const std::wstring &display_name) {
    std::lock_guard lock(_mutex);
    auto record = find_record(display_name);
    if (record == _monitors.end()) {
      // Unknown identity: do not guess a GUID.
      BOOST_LOG(warning) << "Refusing to release an unknown virtual display"sv;
      return false;
    }

    if (_handle == INVALID_HANDLE_VALUE) {
      // Keep the record so a later open can still retry the removal.
      _retained.push_back(record->name);
      return false;
    }

    SUDOVDA::VIRTUAL_DISPLAY_REMOVE_PARAMS params {};
    params.MonitorGuid = record->guid;
    const bool ok = send_ioctl(_handle, IOCTL_REMOVE_VIRTUAL_DISPLAY, &params, sizeof(params), nullptr, 0);
    if (!ok) {
      // Removal failed; retain the record rather than forgetting the allocation.
      _retained.push_back(record->name);
      return false;
    }

    _monitors.erase(record);
    _retained.erase(std::remove(_retained.begin(), _retained.end(), display_name), _retained.end());
    return true;
  }

  bool sudovda_driver_t::ping_watchdog() {
    std::lock_guard lock(_mutex);
    if (_handle == INVALID_HANDLE_VALUE) {
      return false;
    }

    const bool ok = send_ioctl(_handle, IOCTL_DRIVER_PING, nullptr, 0, nullptr, 0);
    if (!ok) {
      ++_consecutive_ping_failures;
      if (_consecutive_ping_failures >= _max_ping_failures) {
        BOOST_LOG(warning) << "SudoVDA driver stopped responding to watchdog pings"sv;
      }
      return false;
    }

    _consecutive_ping_failures = 0;
    return true;
  }

  void sudovda_driver_t::unresolved_cleanup(const std::wstring &display_name) noexcept {
    try {
      std::lock_guard lock(_mutex);
      // Retain the name for a later retry. Never throw, never silently drop.
      if (std::find(_retained.begin(), _retained.end(), display_name) == _retained.end()) {
        _retained.push_back(display_name);
      }
    } catch (...) {
      // No allocation is permitted here; the caller already owns the obligation.
    }
  }

  std::vector<std::wstring> sudovda_driver_t::retained_monitors() const {
    std::lock_guard lock(_mutex);
    return _retained;
  }

  std::string sudovda_driver_t::monitor_client_name(const std::wstring &display_name) const {
    std::lock_guard lock(_mutex);
    const auto record = std::find_if(_monitors.begin(), _monitors.end(), [&display_name](const monitor_record_t &entry) {
      return entry.name == display_name;
    });
    if (record == _monitors.end()) {
      return {};
    }
    return record->spec.client_name;
  }

  std::vector<sudovda_driver_t::monitor_record_t>::iterator
  sudovda_driver_t::find_record(const std::wstring &display_name) {
    return std::find_if(_monitors.begin(), _monitors.end(), [&display_name](const monitor_record_t &entry) {
      return entry.name == display_name;
    });
  }
}  // namespace platf::windows
