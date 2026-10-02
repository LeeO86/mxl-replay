# syntax=docker/dockerfile:1
# Pins: IMPLEMENTATION_PLAN.md. MXL_REF is the single MXL pin.
ARG CUDA_IMAGE=nvidia/cuda:12.8.2-devel-ubuntu24.04

FROM node:22-bookworm AS webui
WORKDIR /web
COPY web/package.json web/package-lock.json ./
RUN npm ci --no-fund --no-audit
COPY web/ ./
RUN npm run build

FROM ${CUDA_IMAGE} AS build
ARG MXL_REF=218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7
ARG NMOS_CPP_REF=fe303849527394b03bdedc8f161f377fe458bb62

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git pkg-config curl zip unzip tar ca-certificates \
        bison flex autoconf automake libtool \
        libjpeg-dev libsqlite3-dev \
        libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev \
        libboost-system-dev libboost-thread-dev libboost-chrono-dev \
        libboost-date-time-dev libboost-regex-dev libboost-random-dev \
        libboost-filesystem-dev libcpprest-dev nlohmann-json3-dev \
        libssl-dev libavahi-compat-libdnssd-dev uuid-dev \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 https://github.com/microsoft/vcpkg /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
ENV VCPKG_ROOT=/opt/vcpkg

RUN git init /tmp/mxl \
    && git -C /tmp/mxl remote add origin https://github.com/dmf-mxl/mxl.git \
    && git -C /tmp/mxl fetch --depth 1 origin "${MXL_REF}" \
    && git -C /tmp/mxl checkout FETCH_HEAD
COPY docker/mxl-vcpkg.json /tmp/mxl-manifest/vcpkg.json
RUN BASELINE="$(git -C /opt/vcpkg rev-parse HEAD)" \
    && sed -i "1a\\  \"builtin-baseline\": \"${BASELINE}\"," /tmp/mxl-manifest/vcpkg.json
RUN cmake -S /tmp/mxl -B /tmp/mxl/build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_MANIFEST_DIR=/tmp/mxl-manifest \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF -DBUILD_UTILS=OFF -DBUILD_DOCS=OFF \
        -DMXL_ENABLE_FABRICS_OFI=OFF \
        -DCMAKE_INSTALL_PREFIX=/opt/mxl \
    && cmake --build /tmp/mxl/build -j"$(nproc)" \
    && cmake --install /tmp/mxl/build

RUN git init /opt/nmos-cpp \
    && git -C /opt/nmos-cpp remote add origin https://github.com/sony/nmos-cpp.git \
    && git -C /opt/nmos-cpp fetch --depth 1 origin "${NMOS_CPP_REF}" \
    && git -C /opt/nmos-cpp checkout FETCH_HEAD

COPY . /src
COPY --from=webui /web/dist /src/web/dist
RUN cmake -S /src -B /src/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="/opt/mxl;/tmp/mxl/build/vcpkg_installed/x64-linux" \
        -DREPLAY_WITH_NMOS=ON -DNMOS_CPP_DIR=/opt/nmos-cpp/Development \
    && cmake --build /src/build -j"$(nproc)" \
    && /src/build/unit-tests

FROM ubuntu:24.04 AS runtime
ARG MXL_REF=218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates curl libjpeg-turbo8 libsqlite3-0 \
        libavcodec60 libavformat60 libavutil58 libswscale7 libswresample4 \
        libcpprest2.10 libboost-chrono1.83.0t64 libboost-thread1.83.0 \
        libssl3t64 libavahi-compat-libdnssd1 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /usr/local/cuda/targets/x86_64-linux/lib/libcudart.so* /usr/local/lib/
COPY --from=build /usr/local/cuda/targets/x86_64-linux/lib/libnvjpeg.so* /usr/local/lib/
COPY --from=build /opt/mxl/lib/ /usr/local/lib/
COPY --from=build /tmp/mxl/build/vcpkg_installed/x64-linux/lib/ /usr/local/lib/
COPY --from=build /src/build/mxl-replay /usr/local/bin/mxl-replay
COPY --from=build /src/build/mxl-replay-harness /usr/local/bin/mxl-replay-harness
RUN ldconfig && useradd --uid 1000 --create-home replay
USER 1000:1000
ENV NVIDIA_DRIVER_CAPABILITIES=compute,video,utility
LABEL io.dmf.mxl.revision="${MXL_REF}"
EXPOSE 8150 3302
ENTRYPOINT ["/usr/local/bin/mxl-replay"]
