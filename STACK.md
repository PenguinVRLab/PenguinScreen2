# STACK - what this release was built on

This documents the ENTIRE stack used to develop this release.
The flatpak bundles these versions wherever bundleable; host-side pieces are
listed as requirements.

## Building from source: the required libraries

The source repo's `build.sh` probes these before configuring (building from
source means a source checkout - that script is not part of the install kit).
CMake minimums (from the build system itself):

| Dependency | Minimum | Distro package usually OK? |
|---|---|---|
| libpng | 1.6.40 | recent distros yes |
| libjpeg (turbo) | any | yes |
| zlib, lz4, libwebp, fontconfig, curl, libpcap | any | yes |
| zstd | 1.5.5 | yes |
| freetype | 2.10 | yes |
| ffmpeg (libavcodec/format/util/swscale/swresample) | 4.x+ | yes |
| shaderc | any | yes |
| **SDL3** | **3.2.6** | **often NO - build from source or vendor repo** |
| **Qt6** (widgets/gui/network/svg/linguist tools) | **6.10.1** | **usually NO - Qt online installer or prefix build** |
| **KDDockWidgets-qt6** | **2.3.0** | **NO - build from source** |
| **ryml (rapidyaml)** | recent | **NO - build from source** |
| **plutovg / plutosvg** | 1.1.0 / 0.0.7 | **NO - build from source** |
| X11/XRandR, Wayland, extra-cmake-modules, libbacktrace | any | yes |

For the newer-than-distro set: build each into one local prefix and pass
`-DCMAKE_PREFIX_PATH=$HOME/deps` to that script (forwarded to cmake). The
**flatpak avoids all of this** - its manifest pins every one of these as a
commit-pinned module; from-source is the enthusiast path.

## Bundled in the flatpak (pinned)
- Runtime: org.kde.Platform **6.10** · SDK: org.kde.Sdk **6.10**
- Built with: flatpak-builder **1.4.9** (the upstream release tarball) and flatpak **1.14.6**, inside an Ubuntu 24.04
  build image whose packages are pinned to Ubuntu's snapshot archive of **2026-10-15**
- Toolchain: llvm **20.1.8** (SDK extension)
- ffmpeg: the org.kde.Platform runtime provides the libavcodec/libavformat
  the app links against - **nothing extra is required to launch** (the
  sonames resolve from the runtime's own codec set). The optional
  `org.freedesktop.Platform.ffmpeg-full` add-extension only adds extra
  patent-encumbered codecs; if Flathub offers a branch matching your
  freedesktop runtime it will be picked up automatically - never install a
  pinned branch by hand (a stale pinned branch
  simply doesn't exist on Flathub and the install errors out).
- Dependency modules: commit-pinned in the manifest in this tree -
  the manifest IS the exact list
- **PS VR2 support out of the box: Monado with its PS VR2 driver is vendored
  INTO this flatpak**: Monado `main` at commit `ded98428a572b264eaea6917895c85f7d131bcbe`
  plus two patches in `flatpak/common/` (the headset's DP RX status logged; the bindings
  generator's merge order fixed, so the build is reproducible), its
  service and client library, built by `flatpak/modules/30-monado.json` (Eigen
  3.4.0 as its build-time sub-module). **The Vulkan driver under it is the
  flatpak's own:** `flatpak/modules/29-mesa-radv.json` builds RADV only, from
  Mesa **26.2.2** (with libdrm 2.4.134 for its amdgpu library), and the launcher
  puts the Monado service AND the game on that one driver (`VK_DRIVER_FILES`),
  so the picture the PS VR2 shows does not move with Flathub's Mesa. The
  runtime's own Mesa (the `org.freedesktop.Platform.GL` extension of the pinned
  runtime branch, 24.08 for KDE 6.10) is used instead when the headset's GPU is
  NVIDIA (`PSCREEN2_MESA=auto`, the default, reads the GPU that owns the
  headset's DisplayPort; the sandbox's NVIDIA driver is flatpak's
  `GL.nvidia-<host version>` extension and must match the host's driver, which
  the launcher checks), or on request for an AMD GPU newer than that build
  knows: `PSCREEN2_MESA=runtime`. The launcher prints which driver it uses at
  every start and refuses a runtime Mesa older than 25.3 (the PS VR2 picture is
  wrong on it). A PS VR2 on an NVIDIA GPU is experimental. Its socket is
  `$XDG_RUNTIME_DIR/penguinscreen2-monado/comp_ipc`. Having the driver does not
  mean you have the headset: the launcher offers PS VR2 only when one is plugged
  in or you ask for it. Licences: Monado BSL-1.0, Mesa and libdrm MIT, Eigen
  MPL-2.0; the texts are installed beside the GPL
  (`share/licenses/<app-id>/`).

## Host-side requirements (documented, never bundled)
- OpenXR runtime: **WiVRn, the current Flathub release**. WiVRn bundles Monado as its runtime. The install is
  unpinned (`flatpak install --user flathub io.github.wivrn.wivrn` in
  `setup-configure-deps.sh`), so the PC side updates only with
  `flatpak update --user io.github.wivrn.wivrn`. Keep the PC server and the
  headset's WiVRn app on matching versions - mismatches refuse to connect;
  the launcher prints the installed version at every start (Quickstart §4).
  - the runtime runs on the HOST; the flatpak reaches it through the
    active-runtime discovery path (`xdg-config/openxr`) and the WiVRn socket
  - the PS VR2 is the exception: its Monado is vendored (above). The host
    supplies only the udev rule for the headset's USB node and, on SteamOS,
    a Plasma X11 desktop session (setup steps: `PSVR2.md`)
- GPU drivers: Mesa (AMD/Intel; SteamOS native). NVIDIA hosts: flatpak needs the
  NVIDIA driver extension that matches your host driver exactly. After a host driver update, install the
  matching one (`DEVICES.md`, Troubleshooting).
- Hosts this release runs on: Ubuntu with an NVIDIA GPU (the full VR chain); SteamOS on a
  Steam Machine (AMD), with a Quest 3 through WiVRn and with the PS VR2 through the bundled
  Monado driver (Plasma X11 session); SteamOS on the Steam Deck, over the USB-C cable or
  docked on wired Ethernet (see KNOWN-ISSUES and DEVICES.md). Windows is not listed yet.

## Known-hazard versions
- **WiVRn 26.6.x stop-order hazard:** stopping the PC server while a headset
  client is still connected can crash the server. Disconnect the headset
  first (close its WiVRn app), then stop the server. The launcher never
  stops a running server for exactly this reason.
