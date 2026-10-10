# CrossPoint Reader local build environment.
#
# Mirrors the release-candidate build job (.github/workflows/release_candidate.yml):
#   - Python 3.13
#   - pioarduino PlatformIO Core 6.1.19 (pinned, both outer and nested penv)
#   - the platform dependencies the workflow installs explicitly
#   - build command: CROSSPOINT_RC_HASH=<short sha> pio run -e x4pro-gh_release_rc -j1
#
# Unlike ci.yml, this workflow runs no clang-format or cppcheck steps, so the
# image ships neither clang-format nor libpcre3.
#
# The image contains only tooling. Source code is bind-mounted at /work, so
# builds read/write the host checkout directly (generated i18n files, .pio,
# .cache and the custom-core scaffold land in the mounted tree, as in a
# native build).
#
# Build (from the repository root):
#   docker build -t crosspoint-build -f docker/Dockerfile .
#
# Usage: see docker/README.md.

FROM python:3.13-slim

# System packages: git (scripts/git_branch.py, the CROSSPOINT_RC_HASH short
# SHA, version flags) and ca-certificates (TLS for package downloads).
RUN apt-get update \
  && apt-get install -y --no-install-recommends git ca-certificates \
  && rm -rf /var/lib/apt/lists/*

# PlatformIO Core, pinned to the same revision as the workflow, plus the
# platform dependencies it installs explicitly (platform-espressif32's
# builder/main.py imports them unconditionally; the platform's own penv
# bootstrap is not reliable).
RUN pip install --no-cache-dir \
  "https://github.com/pioarduino/platformio-core/archive/refs/tags/v6.1.19.zip" \
  "littlefs-python>=0.16.0" "fatfs-ng>=0.1.14" "pyyaml>=6.0.2" \
  "rich-click>=1.8.6" "zopfli>=0.2.2" "intelhex>=2.3.0" "rich>=14.0.0" \
  "urllib3<2" "cryptography>=45.0.3" "certifi>=2025.8.3" "ecdsa>=0.19.1" \
  "bitstring>=4.3.1" "reedsolo>=1.5.3,<1.8" "esp-idf-size>=2.0.0" \
  "esp-coredump>=1.14.0"

# Python packages used by the build pre/post scripts (icon conversion, HTML
# build, etc.).
COPY scripts/requirements.txt /tmp/crosspoint-requirements.txt
RUN pip install --no-cache-dir -r /tmp/crosspoint-requirements.txt \
  && rm /tmp/crosspoint-requirements.txt

# Run as a non-root user so build artifacts in the mounted checkout are not
# owned by root on the host. UID/GID default to 1000; override at build time
# to match your host user (docker compose does this automatically).
ARG UID=1000
ARG GID=1000
RUN groupadd -g "${GID}" dev \
  && useradd -m -u "${UID}" -g "${GID}" -s /bin/bash dev

USER dev
WORKDIR /work

ENV PLATFORMIO_CORE_DIR=/home/dev/.platformio
# The release-candidate matrix entry this image reproduces.
ENV PIO_ENV=x4pro-gh_release_rc

COPY docker/entrypoint.sh /usr/local/bin/crosspoint-entrypoint.sh
ENTRYPOINT ["crosspoint-entrypoint.sh"]
# Same build command as the workflow's "Build CrossPoint Release Candidate"
# step, with the RC version suffix taken from the mounted checkout's HEAD.
CMD CROSSPOINT_RC_HASH="$(git rev-parse --short=7 HEAD)" pio run -e "$PIO_ENV" -j1
