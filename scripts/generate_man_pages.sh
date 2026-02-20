#!/bin/bash

# Script to generate man pages from markdown using pandoc

# Exit on error
set -e

# Check for pandoc
if ! command -v pandoc &> /dev/null; then
    echo "Error: pandoc is not installed."
    exit 1
fi

# Define directories
SOURCE_DIR="man"
OUTPUT_DIR="man/generated"

# Create output directory if it doesn't exist
mkdir -p "${OUTPUT_DIR}"

echo "Generating man pages..."

# linuxcampam(1)
echo "Generating linuxcampam(1)..."
pandoc "$SOURCE_DIR/linuxcampam.1.md" -s -t man -o "$OUTPUT_DIR/linuxcampam.1"

# linuxcampamd(8)
echo "Generating linuxcampamd(8)..."
pandoc "$SOURCE_DIR/linuxcampamd.8.md" -s -t man -o "$OUTPUT_DIR/linuxcampamd.8"

# pam_linuxcampam(8)
echo "Generating pam_linuxcampam(8)..."
pandoc "$SOURCE_DIR/pam_linuxcampam.8.md" -s -t man -o "$OUTPUT_DIR/pam_linuxcampam.8"

# linuxcampam.conf(5)
echo "Generating linuxcampam.conf(5)..."
pandoc "$SOURCE_DIR/linuxcampam.conf.5.md" -s -t man -o "$OUTPUT_DIR/linuxcampam.conf.5"

echo "Done. Generated files in $OUTPUT_DIR"
