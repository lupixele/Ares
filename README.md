# Ares

Ares is an open-source, self-hosted desktop streaming server designed to pair with [Athena](https://github.com/lupixele/Athena) (and compatible Moonlight / Artemis clients). Offering low latency, native client resolution matching, and cloud gaming server capabilities, Ares supports AMD, Intel, and Nvidia hardware encoding alongside software encoding fallback. A modernized web interface is provided for configuration and secure client pairing.

* Repository: [https://github.com/lupixele/Ares](https://github.com/lupixele/Ares)
* Paired Android Client: [Athena](https://github.com/lupixele/Athena)

---

> [!NOTE]
> ### Project Status: Work in Progress (WIP)
> * **Apollo Baseline & Sunshine Migration:** Ares is currently based on an **Apollo baseline** host, with a migration to current upstream **Sunshine** actively in progress.
> * **Interface Modernization:** Ongoing host web interface overhaul and modernization.
> * **Virtual Display & Touch Keyboard:** Integration of Windows touch keyboard docking safeguards and single-display topology fixes (derived from `vda-touch-fix` research) is in active development. Host virtual-display driver assets and automated signing/distribution paths are incomplete and **not yet production-ready**.
> * **No Prebuilt Binaries:** There are currently **no prebuilt binaries, installers, WinGet packages, or official Ares releases**. Do not download third-party binaries expecting a tested Ares release. Ares must currently be built from source.

---

## Major Features

- [x] **Built-in Virtual Display**: Automatic resolution and framerate matching with your streaming client via SudoVDA (Windows).
- [x] **Client Permission Management**: Fine-grained access control (streaming, input control, app execution) per client device.
- [x] **Touch Keyboard Docking Integration (In Progress)**: Integrated virtual display handling to keep the Windows touch keyboard docked on high-resolution virtual displays without manual script patching.
- [x] **Clipboard Synchronization**: Seamless clipboard sharing between host and client.
- [x] **Automated Connection Triggers**: Run custom scripts on client connect and disconnect (see [Auto pause/resume games](https://github.com/ClassicOldSong/Apollo/wiki/Auto-pause-resume-games)).
- [x] **Input-Only Mode**: Control the host without capturing or streaming display video.
- [x] **Seamless Dual-GPU Laptop Support**: Stream directly from dedicated GPUs in headless environments without physical dummy plugs.
- [x] **Modernized Web Management**: Browser-based configuration, client management, and pairing workflow.

---

## Client Pairing & Documentation

* **Paired Client:** Recommended client is [Athena](https://github.com/lupixele/Athena) (Android) or [Artemis](https://github.com/ClassicOldSong/moonlight-android) / [Moonlight](https://moonlight-stream.org/) clients across other platforms.
* **Core Documentation:** For general Sunshine configuration parameters, refer to LizardByte's [Sunshine Documentation](https://docs.lizardbyte.dev/projects/sunshine).
* **Apollo Technical Reference:** For specific features inherited from Apollo, refer to the [Apollo Wiki](https://github.com/ClassicOldSong/Apollo/wiki).

---

## Permission System

For an in-depth overview, see the [Permission System Guide](https://github.com/ClassicOldSong/Apollo/wiki/Permission-System).

> [!NOTE]
> The **FIRST** client paired with Ares is granted FULL permissions. Subsequent newly paired clients are granted only `View Streams` and `List Apps` permissions by default. If a newly paired client cannot launch apps, grant `Launch Apps` in the client permissions panel. If mouse or keyboard input is unresponsive, verify that `Mouse Input` and `Keyboard Input` permissions are enabled for that client.

---

## Virtual Display (Windows)

> [!WARNING]
> ***It is highly recommended to remove any other virtual display solutions from your system and host configuration to avoid device enumeration conflicts.***

> [!NOTE]
> **Concept:** Ares treats your streaming client like a dedicated plug-and-play monitor.

Ares leverages SudoVDA for virtual display handling on Windows:
* **Dynamic Resolution & Refresh Rate Matching:** The virtual display is created automatically upon stream start matching client specifications, and removed when the session terminates.
* **Persistent Display Identity:** Ares assigns a persistent display identifier to each paired client, allowing Windows to remember layout, scale factor, and color profiles natively across sessions.
* **Touch Keyboard & Lifecycle Fixes:** Display lifecycle handling and virtual display touch keyboard docking optimizations are being integrated into Ares host drivers. Driver distribution packaging remains under development.

### Dual-GPU Laptops

Ares supports dual-GPU systems seamlessly:
1. Set the **Adapter Name** to your discrete GPU (dGPU) in configuration.
2. Enable **Headless mode** in the **Audio/Video** tab.
3. Save and restart.

No physical dummy plug is required; the desktop will be rendered and encoded directly from your dGPU.

---

## HDR Support

HDR streaming requires Windows 11 (23H2 or 24H2). Windows 10 and earlier versions do not provide the necessary OS-level HDR display pipeline.

> [!NOTE]
> HDR visual fidelity depends entirely on your client device's tone-mapping capabilities and screen dynamic range. If enabling HDR causes washed-out or dim visuals, verify your client display tone-mapping configuration.

<details>
<summary>HDR Color Space Notice</summary>

Enabling HDR is generally not recommended if your primary workflow requires color-accurate SDR reproduction across non-calibrated displays. Windows Auto HDR will not trigger automatically on virtual displays unless running in native HDR mode.

</details>

---

## System Requirements

> [!NOTE]
> Requirements inherited from upstream Sunshine and Apollo baselines.

### Minimum Requirements

| Component | Description |
|---|---|
| **GPU** | **AMD:** VCE 1.0 or higher ([obs-amd support](https://github.com/obsproject/obs-amd-encoder/wiki/Hardware-Support))<br>**Intel:** Quick Sync / VAAPI-compatible ([VAAPI support](https://www.intel.com/content/www/us/en/developer/articles/technical/linuxmedia-vaapi.html))<br>**Nvidia:** NVENC-enabled cards ([NVENC matrix](https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new)) |
| **CPU** | AMD Ryzen 3 or higher / Intel Core i3 or higher |
| **RAM** | 4 GB or more |
| **OS** | Windows 10+ (Windows 11 23H2+ required for Virtual Display HDR) |
| **Network** | 5 GHz 802.11ac Wi-Fi or Ethernet |

---

## Building from Source

Because official prebuilt Ares packages are not yet published, build from source using the following workflow:

### Prerequisites

* Visual Studio 2022 (MSVC with Desktop C++ workload)
* CMake 3.24 or higher
* Git (with submodule support)
* Node.js (LTS version, for web assets)

### Build Steps (Windows)

```powershell
# 1. Clone repository with all submodules
git clone --recurse-submodules https://github.com/lupixele/Ares.git
cd Ares

# 2. Build modernized Web UI assets
cd src/web
npm install
npm run build
cd ../..

# 3. Configure and compile Ares
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

---

## Legacy Branding & Assets Note

Any remaining Apollo logos or screenshot assets in documentation and packaging are retained temporarily pending updated Ares identity assets.

---

## Lineage & Attribution

Ares is built upon the open-source streaming ecosystem:

* **[Apollo](https://github.com/ClassicOldSong/Apollo):** The immediate baseline architecture for virtual displays, client permission controls, and desktop host enhancements developed by ClassicOldSong and contributors.
* **[Sunshine](https://github.com/LizardByte/Sunshine):** The foundational open-source self-hosted streaming server by LizardByte and contributors.
* **[Moonlight](https://moonlight-stream.org/):** The original open-source client implementation and reverse-engineered protocol.
* **[vda-touch-fix](https://github.com/ClassicOldSong):** Upstream analysis and patching for Windows virtual display touch keyboard docking and display topology persistence.

All original upstream licenses, copyrights, and contributions are preserved and gratefully acknowledged.

---

## License

Ares is free and open-source software licensed under the **GNU General Public License v3.0** ([GPLv3](LICENSE)).
