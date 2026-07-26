# STACK — what this release was built and validated on

This documents the ENTIRE stack used to develop and validate this release.
The flatpak bundles these versions wherever bundleable; host-side pieces are
listed as requirements. Items marked *(validate at cut)* are finalized by the
release runbook's Verify step 5 from the actual release build.

## Building from source: the required libraries

The source repo's `build.sh` probes these before configuring (building from
source means a source checkout — that script is not part of the install kit).
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
| **SDL3** | **3.2.6** | **often NO — build from source or vendor repo** |
| **Qt6** (widgets/gui/network/svg/linguist tools) | **6.10.1** | **usually NO — Qt online installer or prefix build** |
| **KDDockWidgets-qt6** | **2.3.0** | **NO — build from source** |
| **ryml (rapidyaml)** | recent | **NO — build from source** |
| **plutovg / plutosvg** | 1.1.0 / 0.0.7 | **NO — build from source** |
| X11/XRandR, Wayland, extra-cmake-modules, libbacktrace | any | yes |

For the newer-than-distro set: build each into one local prefix and pass
`-DCMAKE_PREFIX_PATH=$HOME/deps` to that script (forwarded to cmake). The
**flatpak avoids all of this** — its manifest pins every one of these as a
commit-pinned module; from-source is the enthusiast path.

## Bundled in the flatpak (pinned)
- Runtime: org.kde.Platform **6.10** · SDK: org.kde.Sdk **6.10**
- Toolchain: llvm **20.1.8** (SDK extension) *(re-validate at cut)*
- ffmpeg: the org.kde.Platform runtime provides the libavcodec/libavformat
  the app links against — **nothing extra is required to launch** (validated:
  the sonames resolve from the runtime's own codec set). The optional
  `org.freedesktop.Platform.ffmpeg-full` add-extension only adds extra
  patent-encumbered codecs; if Flathub offers a branch matching your
  freedesktop runtime it will be picked up automatically — never install a
  pinned branch by hand (field finding 2026-07-20: a stale pinned branch
  simply doesn't exist on Flathub and the install errors out).
- Dependency modules: commit-pinned in the manifest in this tree —
  the manifest IS the exact list

## Host-side requirements (documented, never bundled)
- OpenXR runtime: **WiVRn 26.6.x** (26.6.1 validated end-to-end on the
  Ubuntu dev rig; 26.6.2 field-run on SteamOS). WiVRn bundles Monado as its
  runtime. Keep the PC server and the headset's WiVRn app on matching
  versions — mismatches refuse to connect.
  - the runtime runs on the HOST; the flatpak reaches it through the
    active-runtime discovery path (`xdg-config/openxr`) and the WiVRn socket
- GPU drivers: Mesa (AMD/Intel; SteamOS native). NVIDIA hosts: the flatpak
  NVIDIA driver extension auto-matches your host driver.
- Validated hosts this release: Ubuntu dev rig (NVIDIA, full VR chain incl.
  in-headset) — validated; SteamOS / Steam Deck — install chain field-tested,
  in-headset validation in progress (see KNOWN-ISSUES).

## Known-hazard versions
- **WiVRn 26.6.x stop-order hazard:** stopping the PC server while a headset
  client is still connected can crash the server. Disconnect the headset
  first (close its WiVRn app), then stop the server. The launcher never
  stops a running server for exactly this reason.
