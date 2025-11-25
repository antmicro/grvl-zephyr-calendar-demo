#!/usr/bin/env bash
set -eo pipefail

if [ "$#" -ne 3 ]; then
  echo "Usage: $0 <image_size> <romfs_dir> <build_dir>"
  exit 1
fi

this_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)

IMAGE_SIZE="$1"
ROMFS_DIR="${2:-${this_dir}/../romfs}"
BUILD_DIR="${3:-${this_dir}/../build}"

BLOCK_SIZE=1024
BLOCK_COUNT=$((1024 * IMAGE_SIZE))
BYTES_PER_INODE=128

IMAGE_NAME="zephyr.sdcard.img"
OUTPUT_DIR="${BUILD_DIR}/zephyr"
OUTPUT_IMAGE="${OUTPUT_DIR}/${IMAGE_NAME}"

if [ ! -d "${ROMFS_DIR}" ]; then
  echo "'${ROMFS_DIR}' not found"
  exit 1
fi
echo "Found romfs at '${ROMFS_DIR}'"

echo "Creating SD Card image at '${OUTPUT_IMAGE}'"

mkdir -p "${OUTPUT_DIR}"

truncate -s "${IMAGE_SIZE}M" "${OUTPUT_IMAGE}"
mke2fs \
  -t ext2 \
  -b ${BLOCK_SIZE} \
  -I ${BYTES_PER_INODE} \
  -d "${ROMFS_DIR}" \
  -O filetype,-ext_attr,-resize_inode,-dir_index,-sparse_super,-large_file \
  "${OUTPUT_IMAGE}" \
  ${BLOCK_COUNT}
