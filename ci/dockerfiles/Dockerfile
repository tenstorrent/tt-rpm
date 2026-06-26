FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    # Build toolchain
    build-essential \
    software-properties-common \
    gpg \
    gpg-agent \
    curl \
    ca-certificates \
    cmake \
    make \
    git \
    pkg-config \
    clang-format \
    python3 \
    python3-pip \
    # RPM library dependencies (see CMakeLists.txt)
    libboost-date-time-dev \
    libboost-iostreams-dev \
    libboost-serialization-dev \
    libboost-timer-dev \
    libboost-program-options-dev \
    libyaml-cpp-dev \
    rapidjson-dev \
    libsqlite3-dev \
    zlib1g-dev \
    libhdf5-dev \
    # Bare-metal RISC-V cross compiler for the tests/ workloads (CoreMark,
    # Dhrystone). This package is compiler-only; the C library is built below.
    gcc-riscv64-unknown-elf \
    && add-apt-repository ppa:ubuntu-toolchain-r/test -y \
    && apt-get update && apt-get install -y --no-install-recommends \
    gcc-13 g++-13 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 \
       --slave /usr/bin/g++ g++ /usr/bin/g++-13 \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

RUN pip3 install --no-cache-dir pyyaml

# Build the C library for the bare-metal cross compiler. gcc-riscv64-unknown-elf
# ships no libc, and Ubuntu has no Newlib package for this target. Rather than
# pull a large prebuilt toolchain, build a minimal Newlib against the apt
# compiler: only the rv64gc/lp64d multilib the workloads use, size-optimized,
# no docs. (Reusing the apt gcc -- instead of also rebuilding it -- keeps this
# to a ~5s build.) Installing into the compiler's sysroot (/usr/riscv64-unknown-
# elf) lets riscv64-unknown-elf-gcc find <stdio.h>/<math.h>/<alloca.h> and
# libc/libm with no extra flags, so the test Makefile works unchanged.
ARG NEWLIB_VERSION=4.4.0.20231231
RUN curl -fsSL "https://sourceware.org/pub/newlib/newlib-${NEWLIB_VERSION}.tar.gz" \
        | tar -xz -C /tmp \
    && mkdir /tmp/newlib-build && cd /tmp/newlib-build \
    && "/tmp/newlib-${NEWLIB_VERSION}/configure" \
        --target=riscv64-unknown-elf --prefix=/usr --disable-multilib \
        --enable-newlib-nano-malloc --enable-newlib-nano-formatted-io \
        --enable-target-optspace --disable-newlib-supplied-syscalls \
        CFLAGS_FOR_TARGET="-O2 -march=rv64gc -mabi=lp64d -mcmodel=medany -ffunction-sections -fdata-sections" \
    && make -j"$(nproc)" MAKEINFO=true \
    && make install MAKEINFO=true \
    && rm -rf /tmp/newlib-build "/tmp/newlib-${NEWLIB_VERSION}"
