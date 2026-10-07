#!/bin/bash
set -e

echo "Installing LinuxCamPAM dependencies..."
if [ -f /etc/arch-release ]; then
    echo "Detected Arch Linux. Using pacman..."
    sudo pacman -Sy --needed \
        base-devel \
        cmake \
        pam \
        jsoncpp \
        nlohmann-json \
        wget \
        clang \
        ninja \
        v4l-utils \
        systemd \
        hidapi \
        gtest \
        pkgconf
elif command -v apt-get >/dev/null 2>&1; then
    echo "Detected Debian/Ubuntu. Using apt-get..."
    sudo apt-get update
    sudo apt-get install -y \
        build-essential \
        cmake \
        libpam0g-dev \
        libjsoncpp-dev \
        nlohmann-json3-dev \
        wget \
        clang \
        ninja-build \
        v4l-utils libudev-dev libhidapi-dev libgmock-dev pkg-config
else
    echo "Unsupported distribution. Please install dependencies manually."
    exit 1
fi

echo "Dependencies installed."
