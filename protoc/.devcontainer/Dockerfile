FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN sed -i 's|http://|https://|g' /etc/apt/sources.list.d/ubuntu.sources

RUN apt-get update && apt-get install -y \
    gcc-12 \
    g++-12 \
    cmake \
    python3 \
    python3-pip \
    git \
    && rm -rf /var/lib/apt/lists/*

RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-12 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-12 100 \
    && update-alternatives --install /usr/bin/cc cc /usr/bin/gcc-12 100 \
    && update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++-12 100

RUN pip3 install --break-system-packages conan==2.27.1

# --- Ubuntu 22.04 variant ---
# FROM ubuntu:22.04

# ENV DEBIAN_FRONTEND=noninteractive

# RUN apt-get update && apt-get install -y \
#     gcc-12 \
#     g++-12 \
#     cmake \
#     python3 \
#     python3-pip \
#     git \
#     && rm -rf /var/lib/apt/lists/*

# RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-12 100 \
#     && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-12 100 \
#     && update-alternatives --install /usr/bin/cc cc /usr/bin/gcc-12 100 \
#     && update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++-12 100

# RUN pip3 install conan==2.27.1
