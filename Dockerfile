# syntax=docker/dockerfile:1.7
#
# dsco as a persistent, durable agent container.
#
# Build stage mirrors .github/actions/setup-native/action.yml's Ubuntu package
# list exactly — that's the combination CI already builds and tests dsco
# against on every push (.github/workflows/ci.yml, ubuntu-24.04 x {gcc,clang}),
# so this Dockerfile rides on top of an already-verified build lane rather than
# inventing a new one.

# ---------- builder ----------
FROM ubuntu:24.04 AS builder
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential pkg-config clang git ca-certificates \
      libcurl4-openssl-dev libsqlite3-dev libreadline-dev \
      libsodium-dev libuv1-dev libmbedtls-dev libhiredis-dev \
      zlib1g-dev \
      python3 \
    && rm -rf /var/lib/apt/lists/*
# zlib1g-dev isn't in .github/actions/setup-native/action.yml's package list
# because GitHub's hosted ubuntu-24.04 runners ship zlib headers preinstalled
# in the base toolcache image — a bare `ubuntu:24.04` container doesn't get
# that for free, even though the Makefile links -lz unconditionally.

WORKDIR /src
COPY . .

# Ship the clang build: it's the lane CI also runs sanitizers and static
# analysis against, so it's the most scrutinized of the two compilers CI uses.
ENV CC=clang
RUN make -j"$(nproc)" dsco \
    && ./dsco --version \
    && ./dsco --tool-exec cwd '{}' >/dev/null

# ---------- runtime ----------
FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive

# Runtime image pulls in the same -dev packages as the builder rather than
# hand-picking minimal runtime .so package names: Ubuntu 24.04 renamed several
# runtime libs mid-cycle for the time_t64 transition (e.g. mbedTLS/hiredis
# packages gained a "t64" suffix), and getting that wrong silently breaks
# `dsco` at container start rather than at build time. The -dev packages
# always pull their matching runtime libs in as dependencies, so this stays
# correct across Ubuntu point releases at the cost of a few extra MB.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates \
      libcurl4-openssl-dev libsqlite3-dev libreadline-dev \
      libsodium-dev libuv1-dev libmbedtls-dev libhiredis-dev \
      zlib1g-dev \
      tini socat curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --uid 10001 --shell /usr/sbin/nologin dsco

COPY --from=builder /src/dsco /usr/local/bin/dsco
COPY docker/entrypoint.sh /usr/local/bin/dsco-entrypoint.sh
RUN chmod 0755 /usr/local/bin/dsco /usr/local/bin/dsco-entrypoint.sh

# Docker copies a mount point's ownership from the image layer onto a fresh
# named volume the first time it's attached — but only if that directory
# already exists in the image. Without this, Docker creates it as root:root
# on first use regardless of the container's USER, and the non-root `dsco`
# process can never write its own state (sessions, baseline.db, auth/) again.
RUN mkdir -p /home/dsco/.dsco && chown -R dsco:dsco /home/dsco

USER dsco
WORKDIR /home/dsco
ENV HOME=/home/dsco \
    DSCO_MODE=mcp \
    DSCO_TIER=agent

# ~/.dsco holds every piece of state that must outlive a restart: sessions,
# baseline.db (SQLite timeline), auth/, plugins, capsules, chronicle. Mount a
# real volume here in compose/k8s — see docker-compose.yml / k8s/base.
VOLUME ["/home/dsco/.dsco"]

# `dsco --tool-exec cwd '{}'` is the project's own canonical, no-LLM-call
# smoke test (used by CI itself, README.md) — deterministic, fast, and proves
# the binary + its dynamic libs actually work, not just that the process exists.
HEALTHCHECK --interval=30s --timeout=10s --start-period=10s --retries=3 \
    CMD dsco --tool-exec cwd '{}' >/dev/null 2>&1 || exit 1

ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/dsco-entrypoint.sh"]
