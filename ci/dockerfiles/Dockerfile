FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    # Build toolchain
    build-essential \
    software-properties-common \
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
    && add-apt-repository ppa:ubuntu-toolchain-r/test -y \
    && apt-get update && apt-get install -y --no-install-recommends \
    gcc-13 g++-13 \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 \
       --slave /usr/bin/g++ g++ /usr/bin/g++-13 \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

RUN pip3 install --no-cache-dir pyyaml
