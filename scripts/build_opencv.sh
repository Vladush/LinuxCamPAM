#!/bin/bash
set -e

OPENCV_VER="4.10.0"
INSTALL_DIR="/usr/local"
CORES=$(nproc)

echo "=== Building OpenCV $OPENCV_VER form Source ==="
echo "Target: $INSTALL_DIR"

# Install Deps
sudo apt-get update
sudo apt-get install -y build-essential cmake git pkg-config libjpeg-dev libpng-dev libtiff-dev libavcodec-dev libavformat-dev libswscale-dev libv4l-dev libxvidcore-dev libx264-dev libgtk-3-dev libatlas-base-dev gfortran

# Workspace
mkdir -p opencv_build
cd opencv_build

# Download
if [ ! -d "opencv-$OPENCV_VER" ]; then
    echo "Downloading OpenCV..."
    wget -O opencv.zip https://github.com/opencv/opencv/archive/$OPENCV_VER.zip
    unzip opencv.zip
fi

if [ ! -d "opencv_contrib-$OPENCV_VER" ]; then
    echo "Downloading OpenCV Contrib..."
    wget -O opencv_contrib.zip https://github.com/opencv/opencv_contrib/archive/$OPENCV_VER.zip
    unzip opencv_contrib.zip
fi

# Build
mkdir -p build
cd build

echo "Configuring CMake..."
cmake -D CMAKE_BUILD_TYPE=RELEASE \
    -D CMAKE_INSTALL_PREFIX=$INSTALL_DIR \
    -D OPENCV_EXTRA_MODULES_PATH=../../opencv_contrib-$OPENCV_VER/modules \
    -D WITH_GTK=ON \
    -D WITH_V4L=ON \
    -D WITH_FFMPEG=ON \
    -D WITH_OPENGL=ON \
    -D BUILD_EXAMPLES=OFF \
    -D BUILD_TESTS=OFF \
    -D BUILD_PERF_TESTS=OFF \
    -D BUILD_JAVA=OFF \
    -D BUILD_PYTHON3=OFF \
    -D BUILD_PYTHON_BINDINGS_GENERATOR=OFF \
    -D DO_INSTALL_EXAMPLES=OFF \
    -D DO_INSTALL_TESTS=OFF \
    ../../opencv-$OPENCV_VER

echo "Compiling with $CORES cores..."
make -j$CORES

echo "Installing..."
sudo make install
sudo ldconfig

echo "OpenCV $OPENCV_VER Installed successfully."
