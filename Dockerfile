FROM --platform=linux/amd64 debian:bookworm-slim AS build
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
	  git build-essential cmake pkg-config ca-certificates \
	  libssl-dev libjsoncpp-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY native/CMakeLists.txt /src/CMakeLists.txt
COPY native/src /src/src

RUN cmake -S /src -B /src/build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /src/build --target edge-runtime --parallel "$(nproc)"

FROM --platform=linux/amd64 debian:bookworm-slim
ARG BUILD_NONCE=dev
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates libssl3 libjsoncpp25 nodejs \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/edge-runtime /usr/local/bin/edge-runtime
RUN chmod 0755 /usr/local/bin/edge-runtime

WORKDIR /app
COPY native/scripts/start.sh /app/start.sh
COPY native/reporter /app/reporter
RUN chmod 0755 /app/start.sh

RUN printf 'build-nonce=%s\n' "${BUILD_NONCE}" > /app/.build-nonce

EXPOSE 8080 8081
CMD ["/app/start.sh"]
