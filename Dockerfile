# syntax=docker/dockerfile:1.7
#
# z interpreter — multi-stage Docker build.
#
# Default (core only):
#   docker build -t z .
#
# With the optional image module (also installs ImageMagick at runtime):
#   docker build --build-arg IMAGE=1 -t z .
#
# Run a script (mount your working directory at /work):
#   docker run --rm -v "$PWD:/work" z program.z
#
# Or use one of the bundled examples:
#   docker run --rm z /opt/z/examples/adults.z
#
# Drop into the REPL:
#   docker run --rm -it z
#
# Open a shell instead of the interpreter:
#   docker run --rm -it --entrypoint /bin/bash z

# ---------------------------------------------------------------------------
# 1. Build stage — compile z and run the test suite as a sanity check
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy the sources needed for the build. .dockerignore keeps the context lean.
COPY z.c z_img.h Makefile ./
COPY examples ./examples

# `--build-arg IMAGE=1` flips on the optional image module.
ARG IMAGE=0
RUN make IMAGE=${IMAGE}

# Quick smoke test inside the builder so we catch regressions at build time.
# The Makefile drops the binary at dist/<os>_<arch>/z — on this image that's
# dist/linux_x86/z (or dist/linux_arm64/z on Apple Silicon hosts via emulation).
RUN dist/*/z examples/test_suite.z

# ---------------------------------------------------------------------------
# 2. Runtime stage — small image with just z + its dependencies
# ---------------------------------------------------------------------------
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

ARG IMAGE=0
RUN apt-get update && apt-get install -y --no-install-recommends \
        curl \
        ca-certificates \
    && if [ "$IMAGE" = "1" ]; then apt-get install -y --no-install-recommends imagemagick; fi \
    && rm -rf /var/lib/apt/lists/*

# Drop the compiled binary on PATH and ship the example programs alongside.
# The dist directory contains exactly one platform subfolder during a build,
# so the wildcard reliably resolves to it.
COPY --from=builder /src/dist/*/z       /usr/local/bin/z
COPY --from=builder /src/examples       /opt/z/examples

# Per-user history file location for the REPL.
ENV HOME=/root
WORKDIR /work

ENTRYPOINT ["/usr/local/bin/z"]
CMD []
