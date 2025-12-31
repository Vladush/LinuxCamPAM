#!/bin/bash
set -e

echo "Installing LinuxCamPAM dependencies..."
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    libopencv-dev \
    libpam0g-dev \
    libjsoncpp-dev \
    nlohmann-json3-dev \
    wget \
    clang \
    ninja-build \
    v4l-utils

echo "Dependencies installed."
