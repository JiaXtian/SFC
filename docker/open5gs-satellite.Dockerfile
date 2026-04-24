FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       open5gs \
       iproute2 \
       iputils-ping \
       net-tools \
       procps \
       ca-certificates \
       curl \
       jq \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/sfc

CMD ["sh", "-lc", "while true; do sleep 3600; done"]
