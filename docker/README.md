# Docker build environment

A containerized build environment for CrossPoint Reader, mirroring the
release-candidate build job (`.github/workflows/release_candidate.yml`). Use
it when you cannot or do not want to install the toolchain on the host.

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

This runs the release-candidate build for the X4 Pro:
`CROSSPOINT_RC_HASH="$(git rev-parse --short=7 HEAD)" pio run -e x4pro-gh_release_rc -j1`,
the same command the workflow's "Build CrossPoint Release Candidate" step
uses. Output lands in `.pio/build/x4pro-gh_release_rc/` in your checkout
(`firmware.bin`, `bootloader.bin`, `firmware.elf`, `firmware.map`,
`partitions.bin` — the same set the workflow uploads as artifacts). The first
run downloads the platform and toolchains into the container's
`/home/dev/.platformio` (~2 GB); later runs reuse the image layer only if you
committed it, otherwise they re-download. To keep the package cache across
runs, create a named volume (one-time ownership fix included — fresh volumes
are root-owned, which PlatformIO rejects):

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

Any PlatformIO environment can be built by overriding `PIO_ENV`, e.g. the
other release-candidate targets:

```sh
docker run --rm -it -v "$PWD":/work -e PIO_ENV=x4c-gh_release_rc crosspoint-build
```

### Arbitrary commands

Everything after the image name replaces the default command:

```sh
# Interactive shell
docker run --rm -it -v "$PWD":/work crosspoint-build bash
```

## Flashing

Serial devices are not available inside the container by default. Pass the
device through and set the upload port:

```sh
docker run --rm -it --device=/dev/ttyUSB0 \
  -v "$PWD":/work -v crosspoint-pio:/home/dev/.platformio \
  -e PIO_ENV=x4pro-gh_release_rc crosspoint-build pio run --target upload
```

On most hosts the device node is owned by `root:dialout`; either add your user
to the `dialout` group or run the container with `--user` matching the node's
owner. If permission errors persist, flash from the host instead — the
container build produces the same `firmware.bin` under `.pio/build/<env>/`.

## Notes

- `platformio.local.ini` is honored as usual (it lives in the mounted tree).
- The nested penv core pin (`pioarduino==6.1.19`) is applied by the
  entrypoint on every run, matching the workflow's "Pin pioarduino core
  inside the platform penv" step; see the comment in
  `docker/entrypoint.sh`.
- The image is not used by CI; it exists purely for local builds.
