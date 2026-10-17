/**
 * @file third-party/sudovda/sudovda-ioctl.h
 * @brief Vendored SudoVDA virtual display driver IOCTL contract.
 *
 * @details This header contains only the stable user-mode IOCTL contract shared between Ares and the
 * SudoVDA virtual display driver: control codes, parameter and output layouts, the protocol version
 * tuple, and the device interface GUIDs. It intentionally performs no I/O so that it can be compiled
 * and unit-tested on hosts where the driver is not installed.
 *
 * The values below describe the wire format between Ares and the driver and must not be changed
 * unilaterally. Changing any control code or struct layout requires a matching driver.
 */

// located in third-party/sudovda

#pragma once

#include <cstdint>

#include <windows.h>

// CTL_CODE, FILE_DEVICE_* and METHOD_* constants live in the WDK's devioctl.h,
// which is not available in the MinGW UCRT64 headers. winioctl.h defines them but
// redefines the device interface GUIDs when initguid.h is also in use, so the few
// building blocks this driver contract needs are defined here instead.
#ifndef FILE_DEVICE_UNKNOWN
#define FILE_DEVICE_UNKNOWN 0x00000022
#endif

#ifndef METHOD_BUFFERED
#define METHOD_BUFFERED 0
#endif

#ifndef FILE_ANY_ACCESS
#define FILE_ANY_ACCESS 0
#endif

#ifndef CTL_CODE
#define CTL_CODE(DeviceType, Function, Method, Access) (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#endif

#ifdef __cplusplus
namespace SUDOVDA {
#endif

/**
 * @brief Allocate a new virtual display.
 */
#define IOCTL_ADD_VIRTUAL_DISPLAY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

/**
 * @brief Release a previously allocated virtual display.
 */
#define IOCTL_REMOVE_VIRTUAL_DISPLAY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

/**
 * @brief Select the GPU adapter used to render a virtual display.
 */
#define IOCTL_SET_RENDER_ADAPTER CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

/**
 * @brief Query the driver watchdog timeout and current countdown.
 */
#define IOCTL_GET_WATCHDOG CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

/**
 * @brief Keep the watchdog alive.
 */
#define IOCTL_DRIVER_PING CTL_CODE(FILE_DEVICE_UNKNOWN, 0x888, METHOD_BUFFERED, FILE_ANY_ACCESS)

/**
 * @brief Report the driver's IOCTL protocol version.
 */
#define IOCTL_GET_PROTOCOL_VERSION CTL_CODE(FILE_DEVICE_UNKNOWN, 0x8FF, METHOD_BUFFERED, FILE_ANY_ACCESS)

/**
 * @brief Protocol version supported by this client.
 *
 * @details Bump this only when the IOCTL contract changes.
 */
typedef struct _SUVDA_PROTOCAL_VERSION {
  uint8_t Major;         ///< Major protocol version.
  uint8_t Minor;         ///< Minor protocol version.
  uint8_t Incremental;   ///< Incremental protocol version.
  bool TestBuild;        ///< Whether the driver identifies itself as a test build.
} SUVDA_PROTOCAL_VERSION, *PSUVDA_PROTOCAL_VERSION;

/**
 * @brief Protocol version this build was compiled against.
 */
static const SUVDA_PROTOCAL_VERSION VDAProtocolVersion = {0, 2, 1, true};

/**
 * @brief Hardware identifier reported by the virtual display device.
 */
[[maybe_unused]] static const char *SUVDA_HARDWARE_ID = "root\\sudomaker\\sudovda";

/**
 * @brief Display class GUID, {4d36e968-e325-11ce-bfc1-08002be10318}.
 */
static const GUID SUVDA_CLASS_GUID = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

/**
 * @brief Driver interface GUID used to open the control device.
 */
static const GUID SUVDA_INTERFACE_GUID = {0xe5bcc234, 0x1e0c, 0x418a, {0xa0, 0xd4, 0xef, 0x8b, 0x75, 0x01, 0x41, 0x4d}};

/**
 * @brief Input parameters for adding a virtual display.
 */
typedef struct _VIRTUAL_DISPLAY_ADD_PARAMS {
  UINT Width;              ///< Requested horizontal resolution in pixels.
  UINT Height;             ///< Requested vertical resolution in pixels.
  UINT RefreshRate;        ///< Requested refresh rate in millihertz.
  GUID MonitorGuid;        ///< Stable identity of the display, used later for removal.
  CHAR DeviceName[14];     ///< Friendly device name recorded by the driver.
  CHAR SerialNumber[14];   ///< Serial number recorded by the driver.
} VIRTUAL_DISPLAY_ADD_PARAMS, *PVIRTUAL_DISPLAY_ADD_PARAMS;

/**
 * @brief Input parameters for removing a virtual display.
 */
typedef struct _VIRTUAL_DISPLAY_REMOVE_PARAMS {
  GUID MonitorGuid;        ///< Identity of the display to remove.
} VIRTUAL_DISPLAY_REMOVE_PARAMS, *PVIRTUAL_DISPLAY_REMOVE_PARAMS;

/**
 * @brief Output data returned when a virtual display is added.
 */
typedef struct _VIRTUAL_DISPLAY_ADD_OUT {
  LUID AdapterLuid;        ///< Adapter the display was attached to.
  UINT TargetId;           ///< Display target identifier on the adapter.
} VIRTUAL_DISPLAY_ADD_OUT, *PVIRTUAL_DISPLAY_ADD_OUT;

/**
 * @brief Input parameters for selecting the render adapter.
 */
typedef struct _VIRTUAL_DISPLAY_SET_RENDER_ADAPTER_PARAMS {
  LUID AdapterLuid;        ///< Adapter to render virtual displays on.
} VIRTUAL_DISPLAY_SET_RENDER_ADAPTER_PARAMS, *PVIRTUAL_DISPLAY_SET_RENDER_ADAPTER_PARAMS;

/**
 * @brief Output data describing the watchdog state.
 */
typedef struct _VIRTUAL_DISPLAY_GET_WATCHDOG_OUT {
  UINT Timeout;            ///< Watchdog timeout in seconds.
  UINT Countdown;          ///< Remaining watchdog countdown in seconds.
} VIRTUAL_DISPLAY_GET_WATCHDOG_OUT, *PVIRTUAL_DISPLAY_GET_WATCHDOG_OUT;

/**
 * @brief Output data describing the driver protocol version.
 */
typedef struct _VIRTUAL_DISPLAY_GET_PROTOCOL_VERSION_OUT {
  SUVDA_PROTOCAL_VERSION Version;  ///< Version reported by the driver.
} VIRTUAL_DISPLAY_GET_PROTOCOL_VERSION_OUT, *PVIRTUAL_DISPLAY_GET_PROTOCOL_VERSION_OUT;

#ifdef __cplusplus
}  // namespace SUDOVDA
#endif
