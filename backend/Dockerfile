FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    wget \
    libssl-dev \
    libjsoncpp-dev \
    uuid-dev \
    zlib1g-dev \
    libdrogon-dev \
    libspdlog-dev \
    nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

ARG ONNXRUNTIME_VERSION=1.16.0
RUN wget https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}.tgz && \
    tar -xzf onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}.tgz && \
    mv onnxruntime-linux-x64-${ONNXRUNTIME_VERSION} /usr/local/onnxruntime && \
    rm onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}.tgz

ENV ONNXRUNTIME_DIR=/usr/local/onnxruntime

WORKDIR /build
COPY . .

RUN mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc)

FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    libssl3 \
    libjsoncpp25 \
    uuid-runtime \
    libdrogon1.8 \
    libspdlog1 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/onnxruntime /usr/local/onnxruntime
COPY --from=builder /build/build/sfc_server /app/sfc_server
COPY --from=builder /build/config.json /app/config.json

WORKDIR /app
EXPOSE 8080

CMD ["./sfc_server"]
