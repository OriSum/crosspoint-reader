#!/bin/bash
# Container entrypoint: prepares the workspace, then execs the given command.
set -euo pipefail

WORKDIR=/work

if [ ! -d "${WORKDIR}" ] || [ ! -f "${WORKDIR}/platformio.ini" ]; then
  echo "error: mount the repository checkout at ${WORKDIR}" >&2
  echo "  docker run --rm -it -v \"\$PWD\":/work crosspoint-build" >&2
  exit 1
fi

# The freeink-sdk submodule must be initialized on the host before building;
# lib_deps reference it via symlink:// paths.
if [ ! -d "${WORKDIR}/freeink-sdk" ] || [ -z "$(ls -A "${WORKDIR}/freeink-sdk" 2>/dev/null)" ]; then
  echo "error: freeink-sdk submodule is not initialized." >&2
  echo "  Run on the host:  git submodule update --init --recursive" >&2
  exit 1
fi

# First build in a fresh container: the platform bootstraps its nested penv
# with an open-ended "pioarduino>=6.1.19" that can resolve to a newer core
# which conflicts with the pinned platform (see ci.yml, "Pin pioarduino core
# inside the platform penv"). Bootstrap the penv here if missing, then pin
# the nested core to the same version as the outer core, exactly as CI does.
PENV_PIO="${PLATFORMIO_CORE_DIR}/penv/bin/pio"
PENV_PYTHON="${PLATFORMIO_CORE_DIR}/penv/bin/python"
if [ ! -x "${PENV_PIO}" ]; then
  pio pkg install -e "${PIO_ENV}"
fi
"${PENV_PIO}" pkg install -e "${PIO_ENV}"
pip --python "${PENV_PYTHON}" install "pioarduino==6.1.19"

exec "$@"
