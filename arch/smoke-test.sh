#!/usr/bin/env bash
# Install a built package in a disposable Arch environment with no build dependencies.
# Usage: bash arch/smoke-test.sh /path/to/*.pkg.tar.zst
set -euo pipefail

if (( EUID != 0 )); then
    echo "Run this check as root inside a disposable Arch container." >&2
    exit 1
fi

package=
for candidate in "$@"; do
    metadata=$(bsdtar -xOf "$candidate" .PKGINFO)
    if grep -qx 'pkgname = linuxcampam-git' <<< "$metadata"; then
        if [[ -n "$package" ]]; then
            echo "Expected one linuxcampam-git package, found multiple." >&2
            exit 1
        fi
        package=$candidate
    fi
done
if [[ -z "$package" ]]; then
    echo "No linuxcampam-git package was supplied." >&2
    exit 1
fi

contents=$(bsdtar -tf "$package" | sed 's|^\./||')
for required in \
    usr/bin/linuxcampam \
    usr/bin/linuxcampamd \
    usr/bin/check_opencl \
    usr/bin/linuxcampam-setup-config \
    usr/bin/detect_opencl.sh \
    usr/lib/security/pam_linuxcampam.so \
    usr/lib/systemd/system/linuxcampam.service \
    etc/linuxcampam/config.ini \
    usr/share/linuxcampam/models/face_detection_yunet_2023mar.onnx \
    usr/share/linuxcampam/models/face_recognition_sface_2021dec.onnx; do
    if ! grep -qxF "$required" <<< "$contents"; then
        echo "Required file is missing from package: $required" >&2
        exit 1
    fi
done
if grep -Eq '^lib(/|$)' <<< "$contents"; then
    echo "Package must not contain /lib; Arch uses /usr/lib." >&2
    exit 1
fi

metadata=$(bsdtar -xOf "$package" .PKGINFO)
if ! grep -qx 'backup = etc/linuxcampam/config.ini' <<< "$metadata"; then
    echo "Package does not preserve configuration during upgrades." >&2
    exit 1
fi

# Check the packaged upgrade hook without starting services in the test container.
# A stopped service must remain stopped after an upgrade.
hook_dir=$(mktemp -d)
trap 'rm -rf "$hook_dir"' EXIT
bsdtar -xOf "$package" .INSTALL > "$hook_dir/install"
(
    systemctl() { printf '%s\n' "$*" >> "$hook_dir/systemctl-calls"; }
    # Simulate a running systemd without changing /run in the container.
    function [ {
        if [[ "$#" == 3 && "$1" == -d && "$2" == /run/systemd/system && "$3" == ']' ]]; then
            return 0
        fi
        builtin [ "$@"
    }
    source "$hook_dir/install"
    post_upgrade test-new-version test-old-version
)
if [[ ! -f "$hook_dir/systemctl-calls" ]] ||
    ! grep -qxE 'try-restart linuxcampam(\.service)?' "$hook_dir/systemctl-calls" ||
    [[ $(wc -l < "$hook_dir/systemctl-calls") -ne 1 ]]; then
    echo "Upgrade hook must use systemctl try-restart to preserve inactive services." >&2
    exit 1
fi

# pacman resolves only dependencies declared by the package, exposing dependencies
# that would otherwise be accidentally supplied by the build environment.
pacman -U --noconfirm "$package"

test -s /usr/share/linuxcampam/models/face_detection_yunet_2023mar.onnx
test -s /usr/share/linuxcampam/models/face_recognition_sface_2021dec.onnx
for binary in \
    /usr/bin/linuxcampam \
    /usr/bin/linuxcampamd \
    /usr/bin/check_opencl \
    /usr/lib/security/pam_linuxcampam.so; do
    if ! links=$(ldd "$binary" 2>&1); then
        printf 'Unable to check shared libraries for %s:\n%s\n' "$binary" "$links" >&2
        exit 1
    fi
    if grep -Fq 'not found' <<< "$links"; then
        printf 'Missing shared libraries for %s:\n%s\n' "$binary" "$links" >&2
        exit 1
    fi
done

linuxcampam help
linuxcampam version
if command -v systemd-analyze >/dev/null 2>&1; then
    systemd-analyze verify /usr/lib/systemd/system/linuxcampam.service
fi

echo "Arch package installation and runtime smoke checks passed."
