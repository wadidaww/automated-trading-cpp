FROM ubuntu:22.04 AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git ca-certificates && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY . .
RUN cmake --preset release && cmake --build --preset release

FROM ubuntu:22.04 AS runtime
RUN useradd -m -u 10001 trader
WORKDIR /app
COPY --from=builder /app/build/release/futu_trader /app/futu_trader
COPY config /app/config
COPY data/models /app/data/models
USER trader
HEALTHCHECK --interval=30s --timeout=5s CMD ["/app/futu_trader", "--health-check"]
ENTRYPOINT ["/app/futu_trader"]
