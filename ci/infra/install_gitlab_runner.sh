#!/usr/bin/env bash
# Install GitLab Runner on a Debian/Ubuntu host.
set -euo pipefail

ARCH="${1:-amd64}"
VERSION="16.6.1"

wget -q "https://gitlab-runner-downloads.s3.amazonaws.com/v${VERSION}/deb/gitlab-runner_${ARCH}.deb" -O /tmp/gitlab-runner.deb
dpkg -i /tmp/gitlab-runner.deb
rm /tmp/gitlab-runner.deb
