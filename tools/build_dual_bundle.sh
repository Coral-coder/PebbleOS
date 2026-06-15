#!/usr/bin/env bash
# Build a dual-slot firmware PBZ for pblboot boards.
#
# Usage: tools/build_dual_bundle.sh BOARD [extra ./pbl configure args...]
#
# Example:
#   tools/build_dual_bundle.sh getafix@dvt
#   tools/build_dual_bundle.sh getafix@dvt -DCONFIG_RELEASE=y
#
# Produces: build/normal_BOARD_VERSION.pbz  (with slot0/ and slot1/ inside)
set -euo pipefail

BOARD="${1:?Usage: $0 BOARD [extra configure args...]}"
shift

BUILD_DIR="build"
SLOT0_BIN="$BUILD_DIR/src/fw/tintin_fw_slot0.bin"
SLOT1_BIN="$BUILD_DIR/src/fw/tintin_fw_slot1.bin"

echo "==> Building slot 0 for $BOARD"
./pbl configure --board "$BOARD" --slot 0 "$@"
./pbl build
cp "$BUILD_DIR/src/fw/tintin_fw.bin" "$SLOT0_BIN"
echo "    Saved $SLOT0_BIN"

echo "==> Building slot 1 for $BOARD"
./pbl configure --board "$BOARD" --slot 1 "$@"
./pbl build
cp "$BUILD_DIR/src/fw/tintin_fw.bin" "$SLOT1_BIN"
echo "    Saved $SLOT1_BIN"

echo "==> Creating dual-slot bundle"
./pbl bundle
echo "==> Done. Bundle written to $BUILD_DIR/"
