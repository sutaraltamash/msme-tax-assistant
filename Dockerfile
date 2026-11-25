# Stage 1: Compile C++17 Crow backend on Linux
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake libcurl4-openssl-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY backend ./backend
COPY frontend ./frontend

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
    && cmake --build build --target tax_assistant_server -j2

# Stage 2: Minimal runtime image
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libcurl4 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /src/build/tax_assistant_server ./tax_assistant_server
COPY frontend ./frontend
COPY config ./config
COPY database/schema.sql ./database/schema.sql

RUN mkdir -p /app/database /tmp/gstnode_uploads

ENV HOST=0.0.0.0 \
    PORT=18080 \
    FRONTEND_DIR=/app/frontend \
    RULES_CONFIG_PATH=/app/config/rules_2026.json \
    GROQ_SCHEMA_PATH=/app/config/groq_schema.json \
    DATABASE_PATH=/app/database/tax_sessions.db

EXPOSE 18080
CMD ["./tax_assistant_server"]
