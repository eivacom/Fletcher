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
