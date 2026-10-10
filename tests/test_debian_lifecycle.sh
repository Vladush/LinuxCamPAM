#!/bin/bash
set -e

echo "Building Debian package..."
cd "$(dirname "$0")/../build"
cpack -G DEB

DEB_FILE=$(ls linuxcampam-*.deb | head -n 1)
if [ -z "$DEB_FILE" ]; then
    echo "Failed to find generated .deb package."
    exit 1
fi

echo "Found package: $DEB_FILE"
echo "Extracting control information..."
dpkg-deb -I "$DEB_FILE"

echo "Checking conffiles..."
dpkg-deb -e "$DEB_FILE" deb_control
if [ ! -f "deb_control/conffiles" ]; then
    echo "ERROR: conffiles missing from package control data!"
    exit 1
fi

if ! grep -q "/etc/linuxcampam/config.ini" "deb_control/conffiles"; then
    echo "ERROR: /etc/linuxcampam/config.ini not found in conffiles!"
    cat deb_control/conffiles
    exit 1
fi

echo "Conffiles correctly configured."

echo "Checking executable permissions on maintainer scripts..."
for script in postinst prerm postrm preinst; do
    if [ -f "deb_control/$script" ]; then
        if [ ! -x "deb_control/$script" ]; then
            echo "ERROR: $script is not executable!"
            ls -l "deb_control/$script"
            exit 1
        fi
    fi
done

rm -rf deb_control
echo "Debian lifecycle check passed!"
