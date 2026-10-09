# Docker build environment

A containerized build environment for CrossPoint Reader, mirroring the CI
toolchain (`.github/workflows/ci.yml`). Use it when you cannot or do not want
to install the toolchain on the host.

The image contains **only tooling** — the source tree is bind-mounted, so
build outputs (`.pio/`, `.cache/`, generated i18n files) appear in your normal
checkout, exactly like a native build.

## Build the image

From the repository root:

```sh
docker build -t crosspoint-build -f docker/Dockerfile .
```

To match your host user (avoids root-owned build artifacts in the checkout):

```sh
docker build --build-arg UID="$(id -u)" --build-arg GID="$(id -g)" \
  -t crosspoint-build -f docker/Dockerfile .
```

## Initialize submodules (host, once)

The build requires the `freeink-sdk` submodule; the entrypoint refuses to run
without it:

```sh
git submodule update --init --recursive
```

## Build the firmware

```sh
docker run --rm -it -v "$PWD":/work crosspoint-build
```

This runs `pio run` (the `default` C3 X3/X4 profile). The first run downloads
the platform and toolchains into the container's `/home/dev/.platformio`
(~2 GB); later runs reuse the image layer only if you committed it, otherwise
they re-download. To keep the package cache across runs, create a named volume (one-time
ownership fix included — fresh volumes are root-owned, which PlatformIO
rejects):

```sh
docker volume create crosspoint-pio
docker run --rm -u root -v crosspoint-pio:/home/dev/.platformio \
  alpine chown -R 1000:1000 /home/dev/.platformio
docker run --rm -it -v "$PWD":/work -v crosspoint-pio:/home/dev/.platformio \
  crosspoint-build
```

If you built the image with different `--build-arg UID/GID`, use those values
in the `chown` instead.

### Other profiles

```sh
docker run --rm -it -v "$PWD":/work -e PIO_ENV=sticky crosspoint-build
```

### Arbitrary commands

Everything after the image name replaces the default command:

```sh
# Format check (clang-format 21 is in the image)
docker run --rm -it -v "$PWD":/work crosspoint-build ./bin/clang-format-fix

# Static analysis
docker run --rm -it -v "$PWD":/work crosspoint-build \
  pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high

# Interactive shell
docker run --rm -it -v "$PWD":/work crosspoint-build bash
```

## Flashing

Serial devices are not available inside the container by default. Pass the
device through and set the upload port:

```sh
docker run --rm -it --device=/dev/ttyUSB0 \
  -v "$PWD":/work -v crosspoint-pio:/home/dev/.platformio \
  -e PIO_ENV=default crosspoint-build pio run --target upload
```

On most hosts the device node is owned by `root:dialout`; either add your user
to the `dialout` group or run the container with `--user` matching the node's
owner. If permission errors persist, flash from the host instead — the
container build produces the same `firmware.bin` under `.pio/build/<env>/`.

## Notes

- `platformio.local.ini` is honored as usual (it lives in the mounted tree).
- The nested penv core pin (`pioarduino==6.1.19`) is applied by the
  entrypoint on every run, matching CI; see the comment in
  `docker/entrypoint.sh` and the CI step it mirrors.
- The image is not used by CI; it exists purely for local builds.
