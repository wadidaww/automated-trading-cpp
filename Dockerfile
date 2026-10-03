# Base image pinned by digest (the index digest of ubuntu:24.04 as of 2026-10-03). Bump deliberately
# and re-run the build; a floating tag would change the trading binary's runtime without a review.
FROM ubuntu:24.04@sha256:a853f94d226358a79c740cfc7bce0c289748f3fe3488d921d038ccd752c61b60 AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git ca-certificates \
    libprotobuf-dev protobuf-compiler libboost-dev libssl-dev libyaml-cpp-dev && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY . .
RUN cmake --preset release && cmake --build --preset release

FROM ubuntu:24.04@sha256:a853f94d226358a79c740cfc7bce0c289748f3fe3488d921d038ccd752c61b60 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends libprotobuf32t64 libssl3t64 libyaml-cpp0.8 && \
    rm -rf /var/lib/apt/lists/* && \
    useradd --system --uid 10001 --home-dir /var/lib/futu_trader --shell /usr/sbin/nologin trader && \
    install -d -o trader -g trader -m 0700 /var/lib/futu_trader
WORKDIR /app
COPY --from=builder /app/build/release/futu_trader /app/futu_trader
# Example configs only: the real config and every secret are mounted at run time, never baked in.
COPY config /app/config
USER 10001:10001
# Ready means: OpenD linked, kill switch clear, no halt pending, engine and WAL healthy.
# Set metrics.port in your config to match (9464 by default).
HEALTHCHECK --interval=15s --timeout=5s --start-period=30s --retries=3 \
  CMD ["/app/futu_trader", "--probe", "9464"]
ENTRYPOINT ["/app/futu_trader"]
CMD ["--config", "/etc/futu_trader/trader.yaml"]
