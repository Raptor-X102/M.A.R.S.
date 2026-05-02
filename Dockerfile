FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    git \
    ca-certificates \
    libcli11-dev \
    libspdlog-dev \
    libyaml-cpp-dev \
    libasmjit-dev \
    libpfm4-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/mars

COPY . .

RUN cmake -S . -B build -G Ninja \
 && cmake --build build

ENTRYPOINT ["/opt/mars/build/src/cli/mars"]
CMD ["--config", "config/mars_example.yaml"]
