# syntax=docker/dockerfile:1
# Build context: repository root.   docker build -f deploy/docker/Dockerfile -t hotpath:dev .

# ---- 1. train the model --------------------------------------------------------------------------
FROM python:3.12-slim AS model
WORKDIR /src
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY python/ python/
RUN PYTHONPATH=python python -m hotpath.train --out /models

# ---- 2. compile the engine -----------------------------------------------------------------------
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake git ca-certificates curl && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt .
COPY cmake/ cmake/
COPY include/ include/
COPY src/ src/
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHOTPATH_BUILD_TESTS=OFF -DHOTPATH_BUILD_BENCH=OFF \
 && cmake --build build -j"$(nproc)" \
 && cmake --install build --prefix /opt/hotpath

# ---- 3. runtime ----------------------------------------------------------------------------------
FROM ubuntu:24.04
RUN useradd --system --uid 10001 --no-create-home hotpath
COPY --from=build /opt/hotpath /opt/hotpath
COPY --from=model /models /opt/hotpath/models
ENV HOTPATH_MODEL=/opt/hotpath/models/anomaly.onnx \
    HOTPATH_METRICS_PORT=9100
USER 10001
EXPOSE 9100 9200
ENTRYPOINT ["/opt/hotpath/bin/hotpath"]
