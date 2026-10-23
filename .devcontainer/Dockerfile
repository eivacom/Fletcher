# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 The Fletcher Authors
#
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    gcc-13 \
    g++-13 \
    cmake \
    clang-format-18 \
    python3 \
    python3-pip \
    git \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-18 100

RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100 \
    && update-alternatives --install /usr/bin/cc cc /usr/bin/gcc-13 100 \
    && update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++-13 100

RUN pip3 install --break-system-packages --root-user-action=ignore conan==2.28.1

# Pin Node to an explicit version so the image is reproducible. Bump
# in lock-step with the gateway-client-ts workflow's setup-node.
ARG NODE_VERSION=24.15.0
RUN curl -fsSL https://deb.nodesource.com/setup_24.x | bash - \
    && apt-get install -y "nodejs=${NODE_VERSION}-1nodesource1" \
    && rm -rf /var/lib/apt/lists/*

# Pre-install prettier globally so format-on-save in VS Code works out
# of the box (no per-package npm install required). Pin the version
# to match the format-check-ts.yml workflow so devcontainer and CI
# agree on what "formatted" means.
ARG PRETTIER_VERSION=3.3.3
RUN npm install -g "prettier@${PRETTIER_VERSION}"

# Pin the .NET SDK explicitly, the way Node is pinned above, and for the same
# reason: a developer who does "Reopen in Container" must be able to run
# `dotnet test` on `dotnet/` without installing anything, and CI must not be
# running a different SDK than the container. `dotnet/global.json` pins the same
# band with rollForward: latestFeature, so container, CI and a developer's host
# cannot silently diverge (D-BIND-21). Bump the two together.
#
# Rust, by contrast, is NOT in this image and each Rust CI job installs its own
# pinned toolchain — that asymmetry is deliberate: Rust is exercised by exactly
# one integration test, whereas .NET is a first-class dev dependency from round
# BIND on. The cost is ~200 MB in the image and one fewer install step in every
# .NET job.
#
# libicu is a hard runtime requirement of the SDK on Ubuntu (without it every
# dotnet invocation aborts unless globalization is switched to invariant mode,
# which would change string comparison semantics under the tests).
ARG DOTNET_SDK_VERSION=10.0.400
ENV DOTNET_ROOT=/usr/share/dotnet
ENV PATH="${PATH}:/usr/share/dotnet:/root/.dotnet/tools"
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1
ENV DOTNET_NOLOGO=1
RUN apt-get update && apt-get install -y \
    libicu74 \
    && rm -rf /var/lib/apt/lists/* \
    && curl -fsSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh \
    && bash /tmp/dotnet-install.sh --version "${DOTNET_SDK_VERSION}" --install-dir "${DOTNET_ROOT}" \
    && rm /tmp/dotnet-install.sh \
    && "${DOTNET_ROOT}/dotnet" --info
