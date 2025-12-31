#!/bin/bash
# OpenCL backend detection wrapper for LinuxCamPAM
# Forces Rusticl on AMD GPUs to prevent Mesa/Clover crashes

is_amd_gpu() {
    # Method 1: sysfs (works on all Linux, no external tools needed)
    for dev in /sys/bus/pci/devices/*/vendor; do
        if [ -f "$dev" ]; then
            vendor=$(cat "$dev" 2>/dev/null)
            # AMD vendor ID: 0x1002
            if [ "$vendor" = "0x1002" ]; then
                return 0
            fi
        fi
    done
    
    # Method 2: lspci fallback (if available)
    if command -v lspci &>/dev/null; then
        if lspci 2>/dev/null | grep -qiE 'AMD|ATI.*VGA|Radeon'; then
            return 0
        fi
    fi
    
    return 1
}

find_rusticl_icd() {
    # Check multiple known ICD paths across different distros
    for icd_path in /etc/OpenCL/vendors/rusticl.icd \
                    /usr/share/OpenCL/vendors/rusticl.icd \
                    /usr/local/etc/OpenCL/vendors/rusticl.icd \
                    /usr/lib/OpenCL/vendors/rusticl.icd; do
        if [ -f "$icd_path" ]; then
            echo "rusticl.icd"
            return 0
        fi
    done
    return 1
}

if is_amd_gpu; then
    RUSTICL_ICD=$(find_rusticl_icd)
    if [ -n "$RUSTICL_ICD" ]; then
        # AMD with Rusticl - use it
        export OCL_ICD_VENDORS="$RUSTICL_ICD"
    else
        # Try Mesa environment variable as fallback
        export RUSTICL_ENABLE=radeonsi
        
        # If still no Rusticl, disable OpenCL entirely to force CPU fallback
        # Mesa/Clover causes GPU crashes on AMD
        if ! find_rusticl_icd >/dev/null 2>&1; then
            export OPENCV_OPENCL_DEVICE=disabled
        fi
    fi
fi

exec "$@"
