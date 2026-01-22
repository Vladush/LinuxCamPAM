#!/bin/bash
set -e

# Build function
build_platform() {
    PLATFORM=$1
    DOCKERFILE=$2
    TAG="linuxcampam:$PLATFORM"
    
    echo "=== Building for $PLATFORM using $DOCKERFILE ==="
    docker build -t $TAG -f $DOCKERFILE .
    if [ $? -eq 0 ]; then
        echo "✅ Build and Regression Tests PASSED for $PLATFORM"
    else
        echo "❌ Build FAILED for $PLATFORM"
        exit 1
    fi
}

echo "Starting Cross-Platform Build and Regression Testing..."

# i386 (x86 32-bit)
build_platform "i386" "docker/Dockerfile.i386"

# aarch64 (ARM64)
build_platform "aarch64" "docker/Dockerfile.aarch64"

# riscv64 (RISC-V 64-bit)
build_platform "riscv64" "docker/Dockerfile.riscv64"

echo "🎉 All platforms built and verified successfully!"
