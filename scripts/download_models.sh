#!/bin/bash
set -e

MODEL_DIR="$(dirname "$0")/../models"
mkdir -p "$MODEL_DIR"

YUNET_URL="https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx"
SFACE_URL="https://huggingface.co/opencv/face_recognition_sface/resolve/main/face_recognition_sface_2021dec.onnx"

download_with_retry() {
    local url=$1
    local dest=$2
    local max_retries=3
    local delay=5

    for ((i=1; i<=max_retries; i++)); do
        echo "Attempt $i/$max_retries: $(basename "$dest")..."
        if wget -q --show-progress -O "${dest}.tmp" "$url"; then
            mv "${dest}.tmp" "$dest"
            echo "OK"
            return 0
        fi
        rm -f "${dest}.tmp"
        echo "Download failed."
        if [ $i -lt $max_retries ]; then
            echo "Retrying in ${delay}s..."
            sleep $delay
            delay=$((delay * 2))
        fi
    done
    echo "ERROR: Failed to download $(basename "$dest") after $max_retries attempts."
    return 1
}

echo "Downloading Face Detection Model (YuNet)..."
if [ ! -s "$MODEL_DIR/face_detection_yunet_2023mar.onnx" ]; then
    download_with_retry "$YUNET_URL" "$MODEL_DIR/face_detection_yunet_2023mar.onnx"
else
    echo "YuNet model found, skipping."
fi

echo "Downloading Face Recognition Model (SFace)..."
if [ ! -s "$MODEL_DIR/face_recognition_sface_2021dec.onnx" ]; then
    download_with_retry "$SFACE_URL" "$MODEL_DIR/face_recognition_sface_2021dec.onnx"
else
    echo "SFace model found, skipping."
fi

echo "Models downloaded to $MODEL_DIR"
