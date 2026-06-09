#!/bin/bash
set -e

CONFIG_FILE="/etc/linuxcampam/config.ini"

FORCE_LOCK=false
for arg in "$@"; do
    if [ "$arg" == "--lock" ]; then
        FORCE_LOCK=true
    fi
done

# Handle Immutable Flag: Check if file is locked and temporarily unlock it
WAS_IMMUTABLE=false
if [ -f "$CONFIG_FILE" ] && command -v lsattr &> /dev/null; then
    if lsattr "$CONFIG_FILE" 2>/dev/null | cut -c 5 | grep -q "i"; then
        WAS_IMMUTABLE=true
        echo "[Setup] Configuration file is currently locked (immutable). Temporarily unlocking for setup..."
        sudo chattr -i "$CONFIG_FILE" || true
    fi
fi

# Default values if nothing detected
IR_CAM=""
RGB_CAM=""

echo "[Setup] detecting cameras..."

if ! command -v v4l2-ctl &> /dev/null; then
    echo "[Setup] Error: v4l2-ctl could not be found. Please install v4l-utils."
    exit 1
fi

# Iterate over video devices
for dev in /dev/video*; do
    if [ -e "$dev" ]; then
        # Check formats
        formats=$(v4l2-ctl -d "$dev" --list-formats 2>/dev/null || true)
        
        # Simple heuristic:
        # GREY/Y8/Y16 usually indicates IR or specialized monochrome.
        # MJPG/YUYV usually indicates standard RGB webcam.
        
        if echo "$formats" | grep -qE "GREY|Y8|Y10|Y12|Y16"; then
            if [ -z "$IR_CAM" ]; then
                IR_CAM="$dev"
                echo "[Setup] Found IR Candidate: $dev"
            fi
        fi
        
        if echo "$formats" | grep -qE "MJPG|YUYV|H264"; then
            if [ -z "$RGB_CAM" ]; then
                RGB_CAM="$dev"
                echo "[Setup] Found RGB Candidate: $dev"
            fi
        fi
    fi
done

echo "[Setup] Configuration Selection:"
echo "  IR Camera: ${IR_CAM:-None}"
echo "  RGB Camera: ${RGB_CAM:-None}"

if [ -n "$IR_CAM" ]; then
    if ! command -v linux-enable-ir-emitter &> /dev/null; then
        echo ""
        echo "====================================================================="
        echo "[Setup] WARNING: IR Camera detected but linux-enable-ir-emitter is missing!"
        echo "        This tool is often required to turn on the IR LEDs."
        echo "====================================================================="
        
        # Check if we are running interactively (e.g. manual run by user vs apt postinst)
        if [ -t 0 ]; then
            read -p "[Setup] Would you like to download and install it now? [y/N] " -n 1 -r
            echo
            if [[ $REPLY =~ ^[Yy]$ ]]; then
                echo "[Setup] Fetching install script..."
                if [ -f "./scripts/install_ir_emitter.sh" ]; then
                    echo "[Setup] Using local script (./scripts/install_ir_emitter.sh) for installation."
                    cp "./scripts/install_ir_emitter.sh" /tmp/install_ir_emitter.sh
                elif [ -f "/usr/share/linuxcampam/install_ir_emitter.sh" ]; then
                    echo "[Setup] Using package script (/usr/share/linuxcampam/install_ir_emitter.sh) for installation."
                    cp "/usr/share/linuxcampam/install_ir_emitter.sh" /tmp/install_ir_emitter.sh
                else
                    echo "[Setup] Downloading script from GitHub..."
                    curl -sL https://raw.githubusercontent.com/Vladush/LinuxCamPAM/master/scripts/install_ir_emitter.sh -o /tmp/install_ir_emitter.sh
                fi
                chmod +x /tmp/install_ir_emitter.sh
                /tmp/install_ir_emitter.sh
                echo ""
                IRE_VERSION=$(linux-enable-ir-emitter -V 2>/dev/null || echo "linux-enable-ir-emitter")
                echo "[Setup] $IRE_VERSION installed successfully."
                read -p "[Setup] Would you like to configure the IR emitter now? [Y/n] " -n 1 -r
                echo
                if [[ ! $REPLY =~ ^[Nn]$ ]]; then
                    echo "[Setup] Running configuration..."
                    
                    # Version 7.x (Rust) runs a Ratatui TUI. 
                    sudo linux-enable-ir-emitter configure
                    
                    # Clear screen and restore sane terminal state after TUI exit in case it crashed
                    stty sane || true
                    clear
                else
                    echo "[Setup] You can configure it later by running: sudo linux-enable-ir-emitter configure"
                fi
            else
                echo "[Setup] Skipping installation."
            fi
        else
            echo "[Setup] Running non-interactively (e.g. package install)."
            echo "[Setup] Please run 'sudo linuxcampam-setup-config' later if you wish to install it."
        fi
        echo "====================================================================="
        echo ""
    fi
fi

if [ ! -f "$CONFIG_FILE" ]; then
    echo "[Setup] Config file not found at $CONFIG_FILE."
    exit 0
fi

update_ini() {
    local section=$1
    local key=$2
    local value=$3
    local file=$4
    sudo sed -i "s|^$key = .*|$key = $value|" "$file"
}

if [ -n "$IR_CAM" ] && [ -n "$RGB_CAM" ]; then
    echo "[Setup] Mode: IR + RGB (Preferred)"
    update_ini "Hardware" "camera_path_ir" "$IR_CAM" "$CONFIG_FILE"
    update_ini "Hardware" "camera_path_rgb" "$RGB_CAM" "$CONFIG_FILE"
    # Keep min_brightness default (40)
elif [ -n "$IR_CAM" ]; then
    echo "[Setup] Mode: IR Only"
    update_ini "Hardware" "camera_path_ir" "$IR_CAM" "$CONFIG_FILE"
    update_ini "Hardware" "camera_path_rgb" "$IR_CAM" "$CONFIG_FILE"
elif [ -n "$RGB_CAM" ]; then
    echo "[Setup] Mode: RGB Only"
    update_ini "Hardware" "camera_path_ir" "$RGB_CAM" "$CONFIG_FILE"
    update_ini "Hardware" "camera_path_rgb" "$RGB_CAM" "$CONFIG_FILE"
else
    echo ""
    echo "=============================================="
    if [ ! -t 0 ] || [ "$DEBIAN_FRONTEND" = "noninteractive" ]; then
        echo "[Setup] WARNING: No cameras detected!"
        echo "=============================================="
        echo ""
        echo "Running non-interactively. Ignoring missing camera to continue setup."
        echo "Please manually configure cameras later in: $CONFIG_FILE"
        echo ""
    else
        echo "[Setup] FATAL: No cameras detected!"
        echo "=============================================="
        echo ""
        echo "LinuxCamPAM requires at least one camera to function."
        echo ""
        echo "Troubleshooting:"
        echo "  1. Check if cameras are connected: ls -la /dev/video*"
        echo "  2. Check camera details: v4l2-ctl --list-devices"
        echo "  3. Verify permissions: groups \$USER | grep video"
        echo ""
        echo "If you have a camera but it wasn't detected, please"
        echo "manually configure it in: $CONFIG_FILE"
        echo ""
        exit 1
    fi
fi

echo "[Setup] Checking System RAM and CPU..."
TOTAL_MEM_KB=$(grep MemTotal /proc/meminfo | awk '{print $2}')
# 4GB = 4000000 KB roughly
THRESHOLD_KB=4000000

# Check for AVX (Modern x86) or NEON/ASIMD (Modern ARM)
HAS_FAST_CPU=false
if grep -q "avx" /proc/cpuinfo; then
    HAS_FAST_CPU=true
elif grep -qE "asimd|neon|vfpv4" /proc/cpuinfo; then
    HAS_FAST_CPU=true
elif grep -q "rv64.*v" /proc/cpuinfo; then
    HAS_FAST_CPU=true
fi

KEEP_ALIVE=0

if [ "$TOTAL_MEM_KB" -lt "$THRESHOLD_KB" ]; then
    if [ "$HAS_FAST_CPU" = true ]; then
        echo "[Setup] Low RAM ($TOTAL_MEM_KB kB) + Fast CPU. Enabling Hybrid Mode (60s)."
        KEEP_ALIVE=60
    else
        echo "[Setup] Low RAM ($TOTAL_MEM_KB kB) and Slow CPU. Forcing Instant Mode."
        KEEP_ALIVE=0
    fi
else
    echo "[Setup] Sufficient RAM ($TOTAL_MEM_KB kB). Using Instant Mode."
    KEEP_ALIVE=0
fi

# Ensure [Performance] section exists or append it
if ! grep -q "\[Performance\]" "$CONFIG_FILE"; then
    echo "" | sudo tee -a "$CONFIG_FILE"
    echo "[Performance]" | sudo tee -a "$CONFIG_FILE"
    echo "model_keep_alive_sec = $KEEP_ALIVE" | sudo tee -a "$CONFIG_FILE"
else
    # Update existing
    update_ini "Performance" "model_keep_alive_sec" "$KEEP_ALIVE" "$CONFIG_FILE"
fi


echo "[Setup] Checking Init System (Systemd vs Non-Systemd)..."
if command -v systemctl &> /dev/null && [ -d /run/systemd/system ]; then
    echo "[Setup] Systemd detected. Using Journal Logging."
    LOG_FILE=""
else
    echo "[Setup] Non-Systemd environment detected. Using File Logging."
    LOG_FILE="/var/log/linuxcampam.log"
fi

# Ensure [General] section exists
if ! grep -q "\[General\]" "$CONFIG_FILE"; then
    echo "" | sudo tee -a "$CONFIG_FILE"
    echo "[General]" | sudo tee -a "$CONFIG_FILE"
    echo "log_level = info" | sudo tee -a "$CONFIG_FILE"
    if [ -n "$LOG_FILE" ]; then
        echo "log_file = $LOG_FILE" | sudo tee -a "$CONFIG_FILE"
    fi
else
    # Only set log_file if it's missing (respect existing user config)
    if [ -n "$LOG_FILE" ]; then
        if ! grep -q "log_file" "$CONFIG_FILE"; then
            echo "log_file = $LOG_FILE" | sudo tee -a "$CONFIG_FILE"
        fi
    fi
fi

echo "[Setup] Config updated."

if [ "$FORCE_LOCK" = true ]; then
    echo ""
    echo "====================================================================="
    echo "[Setup] --lock flag detected. Locking configuration file..."
    if command -v chattr &> /dev/null; then
        sudo chattr +i "$CONFIG_FILE"
        echo "[Setup] Configuration LOCKED."
        echo "[Setup] Important: Run 'sudo chattr -i $CONFIG_FILE' before manual editing."
    else
        echo "[Setup] 'chattr' command not found. Skipping lock."
    fi
    echo "====================================================================="
elif [ -t 0 ] && [ "$DEBIAN_FRONTEND" != "noninteractive" ]; then
    echo ""
    echo "====================================================================="
    echo "[Setup] For maximum security against malicious background scripts,"
    echo "        it is highly recommended to lock the configuration file."
    read -p "[Setup] Would you like to lock the configuration file now? [Y/n] " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Nn]$ ]]; then
        if command -v chattr &> /dev/null; then
            sudo chattr +i "$CONFIG_FILE"
            echo "[Setup] Configuration LOCKED."
            echo "[Setup] Important: Run 'sudo chattr -i $CONFIG_FILE' before manual editing."
        else
            echo "[Setup] 'chattr' command not found. Skipping lock."
        fi
    else
        echo "[Setup] Configuration left unlocked."
    fi
    echo "====================================================================="
else
    # Non-interactive mode (e.g., automated apt upgrade)
    if [ "$WAS_IMMUTABLE" = true ] && command -v chattr &> /dev/null; then
        echo "[Setup] Restoring immutable lock on configuration file..."
        sudo chattr +i "$CONFIG_FILE" || true
    fi
fi
