FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
ARG UBUNTU_MIRROR=http://archive.ubuntu.com/ubuntu
ARG OPEN5GS_REPO=https://github.com/open5gs/open5gs.git
ARG OPEN5GS_REF=v2.7.7

RUN set -eux; \
    cat > /etc/apt/sources.list <<EOF
deb ${UBUNTU_MIRROR} jammy main restricted universe multiverse
deb ${UBUNTU_MIRROR} jammy-updates main restricted universe multiverse
deb ${UBUNTU_MIRROR} jammy-backports main restricted universe multiverse
deb ${UBUNTU_MIRROR} jammy-security main restricted universe multiverse
EOF

RUN set -eux; \
    for i in 1 2 3 4 5; do apt-get update && break || sleep 3; done; \
    apt-get install -y --no-install-recommends \
      ca-certificates \
      git \
      build-essential \
      meson \
      ninja-build \
      pkg-config \
      cmake \
      flex \
      bison \
      libgnutls28-dev \
      libgcrypt20-dev \
      libssl-dev \
      libidn11-dev \
      libyaml-dev \
      libnghttp2-dev \
      libmicrohttpd-dev \
      libcurl4-gnutls-dev \
      libtins-dev \
      libtalloc-dev \
      libsctp-dev \
      libmongoc-dev \
      libbson-dev \
      iproute2 \
      iputils-ping \
      net-tools \
      procps \
      curl \
      jq \
      python3 \
      python3-pip \
    && rm -rf /var/lib/apt/lists/*

RUN set -eux; \
    git clone --depth 1 --branch "${OPEN5GS_REF}" "${OPEN5GS_REPO}" /tmp/open5gs; \
    meson setup /tmp/open5gs/build /tmp/open5gs --prefix=/usr --buildtype=release; \
    ninja -C /tmp/open5gs/build; \
    ninja -C /tmp/open5gs/build install; \
    rm -rf /tmp/open5gs

WORKDIR /opt/sfc

CMD ["sh", "-lc", "while true; do sleep 3600; done"]
