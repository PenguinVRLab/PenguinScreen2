#!/bin/bash
# PenguinScreen2 — one-stop build. Run it, get a binary. No other steps.
# Verbose docs: README.md + STACK.md (exact validated versions live there).
set -euo pipefail
cd "$(dirname "$0")"

echo "== PenguinScreen2 build =="

missing=()
for t in cmake ninja git c++ pkg-config; do command -v "$t" >/dev/null || missing+=("$t"); done
if [ ${#missing[@]} -gt 0 ]; then
  echo "Missing build tools: ${missing[*]}"
  echo "Debian/Ubuntu:  sudo apt install build-essential cmake ninja-build git pkg-config"
  echo "(Full dependency list + validated versions: STACK.md)"
  exit 1
fi

# Library preflight: probe the required dev libraries BEFORE configure so the
# failure names every missing piece at once instead of dying one find_package
# at a time. pkg-config name -> human name. Versions are the cmake minimums;
# STACK.md carries the exact validated set and where to get the newer-than-
# distro ones (Qt 6.10, SDL3, ryml, KDDockWidgets, plutovg/plutosvg).
declare -A libs=(
  [libpng]="libpng >= 1.6.40"
  [libjpeg]="libjpeg (turbo)"
  [zlib]="zlib"
  [libzstd]="zstd >= 1.5.5"
  [liblz4]="lz4"
  [libwebp]="libwebp"
  [freetype2]="freetype >= 2.10"
  [fontconfig]="fontconfig"
  [libcurl]="curl"
  [libpcap]="libpcap"
  [libavcodec]="ffmpeg (libav*)"
  [shaderc]="shaderc"
  [sdl3]="SDL3 >= 3.2.6"
  [Qt6Core]="Qt6 >= 6.10.1 (widgets, gui, network)"
)
missing_libs=()
for pc in "${!libs[@]}"; do pkg-config --exists "$pc" 2>/dev/null || missing_libs+=("${libs[$pc]}"); done
if [ ${#missing_libs[@]} -gt 0 ]; then
  echo "Missing development libraries:"
  printf '  - %s\n' "${missing_libs[@]}"
  echo
  echo "Debian/Ubuntu baseline:  sudo apt install libpng-dev libjpeg-turbo8-dev \\"
  echo "  zlib1g-dev libzstd-dev liblz4-dev libwebp-dev libfreetype-dev \\"
  echo "  libfontconfig-dev libcurl4-openssl-dev libpcap-dev libavcodec-dev \\"
  echo "  libavformat-dev libavutil-dev libswscale-dev libswresample-dev \\"
  echo "  libshaderc-dev libx11-dev libxrandr-dev libwayland-dev"
  echo
  echo "Some requirements are NEWER than distro packages (Qt 6.10.1, SDL3,"
  echo "ryml, KDDockWidgets 2.3, plutovg/plutosvg) — STACK.md documents how to"
  echo "provide them (prefix build + -DCMAKE_PREFIX_PATH). The flatpak build"
  echo "avoids all of this; this script is the from-source path."
  echo
  echo "Continue anyway (cmake will re-check precisely)? [y/N]"
  read -r yn; [ "${yn,,}" = "y" ] || exit 1
fi

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_VR=ON -DENABLE_TESTS=OFF "$@"
ninja -C build

echo
echo "OK — binary at build/bin/pcsx2-qt"
echo "First run: see README.md (BIOS setup, headset/OpenXR runtime, profiles)."
echo "Per-game VR profiles: resources/vr-profiles/ (shipped) — copy a file into"
echo "your user vrprofiles/ folder to customize; user files override per game."
