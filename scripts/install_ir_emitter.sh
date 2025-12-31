#!/bin/bash
set -e

# Default version: 6.1.2 is stable and uses Meson (no Rust required)
# 7.0.0-beta requires Rust toolchain and is only for manual selection
DEFAULT_VERSION="6.1.2"

echo "=== Installing Dependencies ==="
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y git pkg-config libssl-dev libclang-dev libv4l-dev libopencv-dev meson ninja-build libgtk-3-dev libyaml-cpp-dev libargparse-dev

echo "=== Cloning Repository ==="
cd /tmp
rm -rf linux-enable-ir-emitter
git clone https://github.com/EmixamPP/linux-enable-ir-emitter.git
cd linux-enable-ir-emitter

echo "=== Switching to Desired Release ==="

TARGET_VERSION="${1:-$DEFAULT_VERSION}"
git fetch --tags

if git rev-parse "$TARGET_VERSION" >/dev/null 2>&1; then
    LATEST_TAG="$TARGET_VERSION"
else
    echo "Error: Version '$TARGET_VERSION' not found."
    exit 1
fi

echo "Checking out release: $LATEST_TAG"
git checkout "$LATEST_TAG"

if [ -f "Cargo.toml" ]; then
    echo "=== Rust-based version detected (Cargo.toml present) ==="
    
    install_binary() {
        echo "Installing binary to /usr/bin/..."
        cp linux-enable-ir-emitter /usr/bin/
        chmod +x /usr/bin/linux-enable-ir-emitter
        echo "Installation successful (binary)."
        exit 0
    }

    # Try pre-built binary first
    echo "Checking for pre-built binary..."
    if ! command -v curl &> /dev/null; then
        apt-get update && apt-get install -y curl
    fi

    API_URL="https://api.github.com/repos/EmixamPP/linux-enable-ir-emitter/releases/tags/${LATEST_TAG}"
    DOWNLOAD_URL=$(curl -s "$API_URL" | grep "browser_download_url" | grep "linux-enable-ir-emitter" | grep "x86-64" | head -n 1 | cut -d '"' -f 4)

    if [ -n "$DOWNLOAD_URL" ]; then
        echo "Found binary at: $DOWNLOAD_URL"
        echo "Downloading..."
        curl -L -o ir-emitter.tar.gz "$DOWNLOAD_URL"
        
        echo "Extracting..."
        tar -xzf ir-emitter.tar.gz
        
        FOUND_BIN=$(find . -name "linux-enable-ir-emitter" -type f | head -n 1)
        if [ -n "$FOUND_BIN" ]; then
            if [ "$FOUND_BIN" != "./linux-enable-ir-emitter" ]; then
                mv "$FOUND_BIN" .
            fi
            install_binary
        else
            echo "Binary not found in archive."
        fi
    else
        echo "No pre-built binary found for this tag."
    fi

    # No pre-built binary - check for Rust toolchain
    if ! command -v cargo &>/dev/null; then
        echo ""
        echo "=============================================="
        echo "Rust toolchain not available"
        echo "=============================================="
        echo ""
        echo "Version $LATEST_TAG requires Rust/Cargo to build from source."
        echo ""
        echo "Auto-installing stable version 6.1.2 instead (no Rust required)..."
        echo ""
        
        # Switch to stable 6.1.2
        git checkout 6.1.2
        
        echo "=== Building and Installing 6.1.2 (Meson) ==="
        rm -rf build
        meson setup build --prefix=/usr/local
        ninja -C build
        ninja -C build install
        
        echo ""
        echo "=============================================="
        echo "Installed version 6.1.2 (stable, Meson-based)"
        echo "=============================================="
        echo ""
        echo "If you want version $LATEST_TAG, install Rust first:"
        echo "  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh"
        echo "  source \$HOME/.cargo/env"
        echo "  sudo $0 $LATEST_TAG"
        echo ""
        
        echo "=== Installation Complete ==="
        echo "Running configuration..."
        linux-enable-ir-emitter configure
        exit 0
    fi
    
    # Rust is available - build from source
    if grep -q 'edition = "2024"' Cargo.toml; then
        echo "Patching Cargo.toml for edition 2021 compatibility..."
        sed -i 's/edition = "2024"/edition = "2021"/' Cargo.toml
        if [ -f "Cargo.lock" ]; then
            echo "Removing incompatible Cargo.lock..."
            rm Cargo.lock
        fi
    fi

    cargo build --release
    
    echo "Installing binary..."
    cp target/release/linux-enable-ir-emitter /usr/local/bin/
    chmod +x /usr/local/bin/linux-enable-ir-emitter
else
    echo "=== Building and Installing (Meson) ==="
    rm -rf build
    meson setup build --prefix=/usr/local
    ninja -C build
    ninja -C build install
fi

echo "=== Installation Complete ==="
echo "Running configuration..."
linux-enable-ir-emitter configure
