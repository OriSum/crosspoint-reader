# CrossPoint Reader local build environment.
#
# Mirrors the CI toolchain (.github/workflows/ci.yml):
#   - Python 3.13
#   - pioarduino PlatformIO Core 6.1.19 (pinned, both outer and nested penv)
#   - clang-format 21 (pip wheel; bin/clang-format-fix picks it up from PATH)
#   - libpcre3 (pioarduino's Linux cppcheck binary links against it)
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

# System packages: git (scripts/git_branch.py, version flags), ca-certificates
# (TLS for package downloads). libpcre3 (PCRE1) was removed from Debian
# trixie, but pioarduino's Linux cppcheck binary links against libpcre.so.3
# (see the CI step "Install cppcheck runtime dependency"); fetch the last
# bookworm build directly.
RUN apt-get update \
  && apt-get install -y --no-install-recommends git ca-certificates curl \
  && curl -fsSL -o /tmp/libpcre3.deb http://deb.debian.org/debian/pool/main/p/pcre3/libpcre3_8.39-15_amd64.deb \
  && apt-get install -y --no-install-recommends /tmp/libpcre3.deb \
  && rm -f /tmp/libpcre3.deb \
  && rm -rf /var/lib/apt/lists/*

# PlatformIO Core, pinned to the same revision as CI, plus the platform
# dependencies CI installs explicitly (platform-espressif32's builder/main.py
# imports them unconditionally; the platform's own penv bootstrap is not
# reliable). clang-format comes from its pip wheel so the version is pinned
# with the rest of the toolchain.
RUN pip install --no-cache-dir \
  "https://github.com/pioarduino/platformio-core/archive/refs/tags/v6.1.19.zip" \
  "littlefs-python>=0.16.0" "fatfs-ng>=0.1.14" "pyyaml>=6.0.2" \
  "rich-click>=1.8.6" "zopfli>=0.2.2" "intelhex>=2.3.0" "rich>=14.0.0" \
  "urllib3<2" "cryptography>=45.0.3" "certifi>=2025.8.3" "ecdsa>=0.19.1" \
  "bitstring>=4.3.1" "reedsolo>=1.5.3,<1.8" "esp-idf-size>=2.0.0" \
  "esp-coredump>=1.14.0" "clang-format>=21,<22"

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
ENV PIO_ENV=default

COPY docker/entrypoint.sh /usr/local/bin/crosspoint-entrypoint.sh
ENTRYPOINT ["crosspoint-entrypoint.sh"]
CMD ["pio", "run"]
