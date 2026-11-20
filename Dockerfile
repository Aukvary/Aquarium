FROM ghcr.io/userver-framework/ubuntu-24.04-userver:latest

WORKDIR /app

# Disable userfaultfd stack usage monitor inside container environment to prevent seccomp aborts
ENV USERVER_ENABLE_STACK_USAGE_MONITOR=0

# Copy project source and configuration files
COPY CMakeLists.txt ./
COPY proto/ ./proto/
COPY src/ ./src/
COPY configs/ ./configs/
COPY sql/ ./sql/
COPY tests/ ./tests/

# Build the service binary in Release mode
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target aquarium -j$(nproc)

# Expose HTTP (monitoring/testsuite) and gRPC ports
EXPOSE 8080 8081

# Healthcheck checking that gRPC server port is open and accepting TCP connections
HEALTHCHECK --interval=5s --timeout=5s --start-period=10s --retries=5 \
    CMD bash -c 'exec 3<>/dev/tcp/127.0.0.1/8081' || exit 1

# Run Aquarium service with configs
CMD ["./build/aquarium", "--config", "configs/static_config.yaml", "--config-vars", "configs/config_vars.yaml"]
