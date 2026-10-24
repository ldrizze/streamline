#!/usr/bin/env bash
# Writes <dist>/THIRD_PARTY_NOTICES.txt and copies license files for every MSYS2
# package that owns a DLL bundled in <dist>. Run from an MSYS2 UCRT64 shell after
# `cmake --install build --prefix dist`.
#
# usage: tools/collect-licenses.sh dist
set -euo pipefail
export LC_ALL=C  # pacman field names are localized otherwise

dist="${1:?usage: collect-licenses.sh <dist-dir>}"
prefix="mingw-w64-ucrt-x86_64-"
notices="$dist/THIRD_PARTY_NOTICES.txt"
mkdir -p "$dist/licenses"

# Map bundled DLLs back to the files they were copied from, then to packages.
mapfile -t owners < <(
  {
    for f in "$dist"/bin/*.dll; do echo "/ucrt64/bin/$(basename "$f")"; done
    for f in "$dist"/lib/gstreamer-1.0/*.dll; do echo "/ucrt64/lib/gstreamer-1.0/$(basename "$f")"; done
  } | xargs pacman -Qqo | sort -u
)

{
  echo "Streamline bundles the following third-party libraries, unmodified, as"
  echo "dynamically linked DLLs taken from MSYS2 (https://www.msys2.org)."
  echo "Full license texts: licenses/ (GPL-3.0, LGPL-2.1, LGPL-3.0 and per-package files)."
  echo "Source code for each package: https://packages.msys2.org/base/<name>"
  echo
  for pkg in "${owners[@]}"; do
    name="${pkg#"$prefix"}"
    info="$(pacman -Qi "$pkg")"
    field() { sed -n "s/^$1 *: //p" <<<"$info" | head -1; }
    printf '%s %s\n  License: %s\n  URL:     %s\n\n' \
      "$name" "$(field Version)" "$(field Licenses)" "$(field URL)"
    if [[ -d "/ucrt64/share/licenses/$name" ]]; then
      mkdir -p "$dist/licenses/$name"
      cp -r "/ucrt64/share/licenses/$name/." "$dist/licenses/$name/"
    fi
  done
} > "$notices"

echo "collect-licenses: ${#owners[@]} packages -> $notices"
