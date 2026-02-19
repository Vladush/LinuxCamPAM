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
SOURCE_DIR="docs/man"
OUTPUT_DIR="docs/man/generated"

# Create output directory if it doesn't exist
mkdir -p "$OUTPUT_DIR"

echo "Generating man pages..."


# linuxcampam.conf(5)
echo "Generating linuxcampam.conf(5)..."
pandoc man/linuxcampam.conf.5.md -s -t man -o man/generated/linuxcampam.conf.5

echo "Done. Generated files in man/generated/"
