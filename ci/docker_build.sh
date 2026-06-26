#!/usr/bin/env bash

# SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

# Build and optionally test RPM inside a Docker container.
#
# Usage:
#   bash ci/docker_build.sh              # build the image, then build RPM
#   bash ci/docker_build.sh --shell      # drop into an interactive shell
#   bash ci/docker_build.sh --test       # build RPM + run smoke test
#   bash ci/docker_build.sh --format     # run clang-format check only
#   bash ci/docker_build.sh --image-only # build the Docker image only
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RPM_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
IMAGE_NAME="rpm-dev"

ACTION="${1:-build}"

echo "Building Docker image '${IMAGE_NAME}'..."
docker build -t "$IMAGE_NAME" "$RPM_ROOT/ci/dockerfiles"

if [ "$ACTION" = "--image-only" ]; then
    echo "Image '${IMAGE_NAME}' built."
    exit 0
fi

DOCKER_RUN=(docker run --rm -v "$RPM_ROOT:/workspace" -w /workspace "$IMAGE_NAME")

case "$ACTION" in
    --shell)
        docker run --rm -it -v "$RPM_ROOT:/workspace" -w /workspace "$IMAGE_NAME" /bin/bash
        ;;
    --format)
        "${DOCKER_RUN[@]}" bash ci/check_format.sh
        ;;
    --test)
        "${DOCKER_RUN[@]}" bash -c "\
            git submodule update --init --recursive && \
            bash scripts/build_scripts/build_all.sh && \
            make -C tests run_coremark"
        ;;
    build|*)
        "${DOCKER_RUN[@]}" bash -c "\
            git submodule update --init --recursive && \
            bash scripts/build_scripts/build_all.sh"
        ;;
esac
