#!/bin/bash
set -e

# Default version used to be 6.1.2. Using PR branch for tweaked controls.
DEFAULT_VERSION="feat/tweak-controls"

echo "=== Installing Dependencies ==="
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y git pkg-config libssl-dev libclang-dev libv4l-dev libopencv-dev meson ninja-build libgtk-3-dev libyaml-cpp-dev libargparse-dev

echo "=== Cloning Repository ==="
cd /tmp
rm -rf linux-enable-ir-emitter
# Use Vladush fork to test PR changes
git clone https://github.com/Vladush/linux-enable-ir-emitter.git
cd linux-enable-ir-emitter

echo "=== Switching to Desired Release ==="

TARGET_VERSION="${1:-$DEFAULT_VERSION}"
git fetch --all --tags

if git rev-parse "$TARGET_VERSION" >/dev/null 2>&1 || git rev-parse "origin/$TARGET_VERSION" >/dev/null 2>&1; then
    LATEST_TAG="$TARGET_VERSION"
else
    echo "Error: Version/branch '$TARGET_VERSION' not found."
    exit 1
fi

echo "Checking out release/branch: $LATEST_TAG"
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

    # NOTE: since we are on a custom branch/fork, a release tag might not exist for it.
    API_URL="https://api.github.com/repos/Vladush/linux-enable-ir-emitter/releases/tags/${LATEST_TAG}"
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
    # IMPORTANT: Run `cargo --version` in /tmp where no Cargo.toml exists, or else old cargo versions abort instantly
    CARGO_AVAILABLE=false
    if command -v cargo &>/dev/null; then
        CARGO_AVAILABLE=true
    fi
    
    if [ "$CARGO_AVAILABLE" = true ]; then
        CARGO_VERSION=$(cd /tmp && cargo --version 2>/dev/null | awk '{print $2}' | cut -d'.' -f1,2 | sed 's/\.//')
        # Default to 0 if parsing fails
        CARGO_VERSION=${CARGO_VERSION:-0}
        if [ "$CARGO_VERSION" -lt 185 ]; then
            echo "Cargo version ($CARGO_VERSION) is older than 1.85, which is needed for edition 2024 dependencies."
            CARGO_AVAILABLE=false
        fi
    fi
    
    if [ "$CARGO_AVAILABLE" = false ]; then
        echo "Temporarily installing the latest stable Rust toolchain via rustup..."
        export RUSTUP_HOME=/tmp/rustup
        export CARGO_HOME=/tmp/cargo
        curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path
        export PATH="/tmp/cargo/bin:$PATH"
        echo "Using temporary Rust toolchain: $(cargo --version)"
    fi
    
    # Actually check again in case rustup failed entirely and there's still no cargo
    if ! command -v cargo &>/dev/null; then
        echo ""
        echo "=============================================="
        echo "Rust toolchain not available"
        echo "=============================================="
        echo ""
        echo "Version/Branch $LATEST_TAG requires Rust/Cargo to build from source."
        echo ""
        echo "Auto-installing stable version 6.1.2 instead (no Rust required)..."
        echo ""
        
        # Switch to stable 6.1.2 repository
        cd /tmp
        rm -rf linux-enable-ir-emitter
        git clone https://github.com/EmixamPP/linux-enable-ir-emitter.git
        cd linux-enable-ir-emitter
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
        exit 0
    fi

    # Build using whatever cargo is in PATH (either system or our temp rustup)
    cargo build --release
    
    echo "Installing binary..."
    cp target/release/linux-enable-ir-emitter /usr/local/bin/
    chmod +x /usr/local/bin/linux-enable-ir-emitter
    
    # Cleanup temporary toolchain if we created it
    if [ -n "$RUSTUP_HOME" ]; then
        echo "Cleaning up temporary Rust toolchain..."
        rm -rf /tmp/rustup /tmp/cargo
    fi
else
    echo "=== Building and Installing (Meson) ==="
    rm -rf build
    meson setup build --prefix=/usr/local
    ninja -C build
    ninja -C build install
fi

echo "=== Installation Complete ==="
