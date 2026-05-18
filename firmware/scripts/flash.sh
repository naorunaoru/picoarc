#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
firmware_dir="$(cd "${script_dir}/.." && pwd)"
build_dir="${firmware_dir}/build-arm"
uf2="${build_dir}/picoarc.uf2"

cmake -S "${firmware_dir}" -B "${build_dir}" -G Ninja
cmake --build "${build_dir}"

picotool load -f "${uf2}"
picotool reboot
