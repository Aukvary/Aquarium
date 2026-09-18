FROM ghcr.io/userver-framework/ubuntu-22.04-userver-pg:latest AS builder

WORKDIR /app
COPY . .

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
RUN cmake --build build --config Release


FROM ubuntu:22.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    libpq5 \
    libssl3 \
    libev4 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /app/build/your_microservice /app/
COPY --from=builder /app/configs/static_config.yaml /app/configs/

EXPOSE 8080

CMD ["/app/your_microservice", "--config", "/app/configs/static_config.yaml"]
