FROM ubuntu:22.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    cmake --build . --config Release -j$(nproc)

FROM ubuntu:22.04 AS runner

WORKDIR /app
COPY --from=builder /app/build/rate_limiter /app/rate_limiter
COPY --from=builder /app/config.yaml /app/config.yaml

EXPOSE 8080

ENTRYPOINT ["/app/rate_limiter"]