#!/bin/bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
mode=${1:---metadata}
if [ "$#" -gt 0 ]; then shift; fi

case "$mode" in
    --metadata)
        build=$(realpath "${1:-$repo/build}")
        temp=$(mktemp -d)
        trap 'rm -rf "$temp"' EXIT
        # An empty directory prevents inspecting an old package by accident.
        cpack --config "$build/CPackConfig.cmake" -G DEB -B "$temp/packages"
        packages=("$temp"/packages/*.deb)
        [ "${#packages[@]}" -eq 1 ] && [ -f "${packages[0]}" ]
        package=${packages[0]}
        dpkg-deb -e "$package" "$temp/control"
        [ "$(grep -cx '/etc/linuxcampam/config.ini' "$temp/control/conffiles")" -eq 1 ]
        [ -z "$(sort "$temp/control/conffiles" | uniq -d)" ]
        for script in preinst postinst prerm postrm; do
            test -x "$temp/control/$script"
        done
        cp "$package" "$build/$(basename "$package")"
        echo "CPack metadata and maintainer-script permissions passed."
        ;;
    --lifecycle)
        [ "$#" -gt 0 ] || { echo "Usage: $0 --lifecycle package.deb [...]" >&2; exit 2; }
        for package in "$@"; do
            package=$(realpath "$package")
            test -f "$package"
            # Maintainer scripts modify PAM: run only in a disposable container.
            docker run --rm --init \
                -e DEBIAN_FRONTEND=noninteractive \
                --mount "type=bind,src=$package,dst=/package.deb,readonly" \
                --mount "type=bind,src=$repo/tests/debian_lifecycle_container.sh,dst=/test.sh,readonly" \
                "${DEBIAN_TEST_IMAGE:-ubuntu:24.04}" bash /test.sh
        done
        ;;
    *) echo "Usage: $0 [--metadata [build-dir] | --lifecycle package.deb [...]]" >&2; exit 2 ;;
esac
