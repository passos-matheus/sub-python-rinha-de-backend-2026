FROM python:3.12-slim AS trainer
WORKDIR /train
RUN pip install --no-cache-dir numpy
COPY train.py references.json ./
RUN python train.py references.json


FROM alpine:3.20 AS builder
RUN apk add --no-cache g++ wget rapidjson-dev
WORKDIR /build
RUN wget -q https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.1/httplib.h
COPY api.cpp .
RUN g++ -std=c++17 -O3 -flto -funroll-loops -pthread \
    -DCPPHTTPLIB_LISTEN_BACKLOG=4096 \
    -o api api.cpp \
    && strip api


FROM alpine:3.20
RUN apk add --no-cache libstdc++ libgcc
COPY --from=builder /build/api /api
COPY --from=trainer /train/refs.bin /refs.bin
COPY --from=trainer /train/labels.bin /labels.bin
EXPOSE 3000
ENTRYPOINT ["/api"]
