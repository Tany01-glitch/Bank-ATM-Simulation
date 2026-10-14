FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ \
    libsqlite3-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY backend ./backend
COPY frontend ./frontend

RUN g++ backend/main.cpp -o bank_atm -std=c++17 -Ibackend -lsqlite3 -pthread

ENV PORT=10000

EXPOSE 10000

CMD ["./bank_atm"]