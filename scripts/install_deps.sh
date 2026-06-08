#!/bin/bash
set -e

echo "Installing LinuxCamPAM dependencies..."
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

echo "Dependencies installed."
