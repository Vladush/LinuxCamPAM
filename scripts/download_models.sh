#!/bin/bash
set -e

MODEL_DIR="$(dirname "$0")/../models"
mkdir -p "$MODEL_DIR"

YUNET_URL="https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2022mar.onnx"
SFACE_URL="https://huggingface.co/opencv/face_recognition_sface/resolve/main/face_recognition_sface_2021dec.onnx"

echo "Downloading Face Detection Model (YuNet)..."
if [ ! -s "$MODEL_DIR/face_detection_yunet_2022mar.onnx" ]; then
    wget -q --show-progress -O "$MODEL_DIR/face_detection_yunet_2022mar.onnx" "$YUNET_URL"
else
    echo "YuNet model found, skipping."
fi

echo "Downloading Face Recognition Model (SFace)..."
if [ ! -s "$MODEL_DIR/face_recognition_sface_2021dec.onnx" ]; then
    wget -q --show-progress -O "$MODEL_DIR/face_recognition_sface_2021dec.onnx" "$SFACE_URL"
else
    echo "SFace model found, skipping."
fi

echo "Models downloaded to $MODEL_DIR"
