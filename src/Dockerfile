FROM ghcr.io/userver-framework/ubuntu-22.04-userver-pg:latest AS builder

WORKDIR /app

COPY CMakeLists.txt .
COPY configs configs
COPY src src

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
RUN cmake --build build --config Release


FROM ubuntu:22.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    libpq5 \
    libssl3 \
    libev4 \
    libyaml-cpp0.7 \
    libfmt8 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /app/build/aquarium /app/
COPY --from=builder /app/configs/static_config.yaml /app/configs/
COPY --from=builder /app/configs/config_vars.yaml /app/configs/

EXPOSE 8000 8001

CMD ["/app/aquarium", "--config", "/app/configs/static_config.yaml", "--config_vars", "/app/configs/config_vars.yaml"]
