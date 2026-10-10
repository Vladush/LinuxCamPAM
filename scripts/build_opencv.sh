#!/bin/bash
set -e

OPENCV_VER="${OPENCV_VER:-4.12.0}"
# Install to a local directory for static linking
# This ensures we don't pollute /usr/local and makes packaging predictable
INSTALL_DIR="${INSTALL_DIR:-$(pwd)/opencv_static}"
CORES="${MAKE_JOBS:-$(nproc)}"
# Packagers can supply an already verified, extracted OpenCV source tree.
if [[ -n "${OPENCV_SOURCE_DIR:-}" ]]; then
    OPENCV_SOURCE_DIR="$(cd "$OPENCV_SOURCE_DIR" && pwd)"
    test -f "$OPENCV_SOURCE_DIR/CMakeLists.txt"
fi

# Keep the established Debian/Ubuntu defaults. Arch's OpenBLAS linkage needs
# a separate configuration; callers may override these flags with CMake args.
OPENCV_PLATFORM_FLAGS=(-D WITH_OPENMP=OFF -D WITH_LAPACK=ON)
if [ -f /etc/arch-release ]; then
    OPENCV_PLATFORM_FLAGS=(-D WITH_OPENMP=ON -D WITH_LAPACK=OFF)
fi

echo "=== Building OpenCV $OPENCV_VER (Static) from Source ==="
echo "Target: $INSTALL_DIR"

# Install Deps (Minimal for Headless Service)
if [[ -z "${SKIP_DEPS:-}" ]]; then
    echo "Installing Dependencies..."
    SUDO=""
    if [ "$EUID" -ne 0 ] && command -v sudo >/dev/null; then
        SUDO="sudo"
    fi

    if [ -f /etc/arch-release ]; then
        echo "Arch users: run a full system upgrade (pacman -Syu) before installing dependencies."
        $SUDO pacman -S --needed base-devel cmake git pkgconf \
            libjpeg-turbo libpng libtiff \
            v4l-utils python unzip wget
    elif command -v apt-get >/dev/null 2>&1; then
        $SUDO apt-get update
        # Note: libgtk-3-dev removed as we are building headless
        # libatlas-base-dev for linear algebra optimizations
        $SUDO apt-get install -y build-essential cmake git pkg-config \
            libjpeg-dev libpng-dev libtiff-dev \
            libavcodec-dev libavformat-dev libswscale-dev libv4l-dev \
            libxvidcore-dev libx264-dev \
            libatlas-base-dev gfortran python3-dev unzip wget
    else
        echo "Unsupported package manager. Please install dependencies manually."
        exit 1
    fi
fi

# Workspace
mkdir -p opencv_build
cd opencv_build

# Download
if [[ -n "${OPENCV_SOURCE_DIR:-}" ]]; then
    SOURCE_DIR="$OPENCV_SOURCE_DIR"
else
    SOURCE_DIR="$(pwd)/opencv-$OPENCV_VER"
    if [ ! -d "$SOURCE_DIR" ]; then
        echo "Downloading OpenCV..."
        wget -O opencv.zip "https://github.com/opencv/opencv/archive/$OPENCV_VER.zip"
        unzip -q opencv.zip
    fi
fi

# Build
mkdir -p build
cd build

echo "Configuring CMake..."
# Key flags for Static Headless Build:
# - BUILD_SHARED_LIBS=OFF: Static libraries (.a)
# - BUILD_opencv_highgui=OFF: No GUI window support needed
# - WITH_GTK=OFF: No GTK dependency
# - WITH_V4L=ON: Camera support is essential
# - WITH_FFMPEG=OFF: Capture directly through V4L2 without FFmpeg
cmake -D CMAKE_BUILD_TYPE=Release \
    -D CMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -D BUILD_SHARED_LIBS=OFF \
    -D OPENCV_GENERATE_PKGCONFIG=ON \
    -D WITH_FFMPEG=OFF \
    -D WITH_V4L=ON \
    -D WITH_GTK=OFF \
    -D WITH_QT=OFF \
    -D WITH_GSTREAMER=OFF \
    -D WITH_ADE=OFF \
    -D WITH_EIGEN=OFF \
    -D WITH_OPENEXR=OFF \
    -D BUILD_EXAMPLES=OFF \
    "${OPENCV_PLATFORM_FLAGS[@]}" \
    -D BUILD_TESTS=OFF \
    -D BUILD_PERF_TESTS=OFF \
    -D BUILD_JAVA=OFF \
    -D BUILD_PYTHON3=OFF \
    -D BUILD_opencv_python3=OFF \
    -D BUILD_opencv_apps=OFF \
    -D BUILD_opencv_gapi=OFF \
    -D BUILD_opencv_highgui=OFF \
    -D BUILD_opencv_stitching=OFF \
    -D BUILD_opencv_ts=OFF \
    -D BUILD_opencv_python_bindings_generator=OFF \
    -D BUILD_opencv_ml=OFF \
    -D BUILD_opencv_video=OFF \
    -D OPENCV_DNN_OPENCL=ON \
    "$@" \
    "$SOURCE_DIR"

echo "Compiling with $CORES cores..."
make -j"$CORES"

echo "Installing to $INSTALL_DIR..."
make install

echo "OpenCV $OPENCV_VER Static Build Complete."
echo "Artifacts are in: $INSTALL_DIR"
