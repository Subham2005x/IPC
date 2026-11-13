FROM ubuntu:24.04
RUN apt-get update -qq \
    && apt-get install -y -qq --no-install-recommends gcc make libc6-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY Makefile ./
COPY include/ include/
COPY src/ src/
RUN make && mkdir -p results
ENTRYPOINT ["./ipc_system"]
