#!/usr/bin/env bash
# Install rReport's build dependencies on Ubuntu 24.04 (developer laptops, CI).
#
# This file is the single source of truth for the apt package list: the README,
# the CI workflow and the Docker build all call it. Libraries that are not
# packaged here (cpp-httplib >= 0.20, jsoncons, reproc, BLAKE3) are fetched at
# pinned versions by CMake (cmake/RrDependencies.cmake, ADR-0012).
#
# Usage: sudo scripts/install-deps-ubuntu.sh [--with-clang] [--minimal]
#   --with-clang  also install Clang 20 + clang-tidy/clang-format 20
#                 (Clang >= 19 is required for std::expected, see ADR-0007)
#   --minimal     only the system-only libraries (libcurl, OpenSSL, SQLite) and
#                 the toolchain; everything else is fetched (CI's ci-fetch job)
set -euo pipefail

with_clang=0
minimal=0
for arg in "$@"; do
    case "$arg" in
        --with-clang) with_clang=1 ;;
        --minimal) minimal=1 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

packages=(
    build-essential cmake ninja-build pkg-config git ca-certificates
    libcurl4-openssl-dev libssl-dev libsqlite3-dev
)

if [[ $minimal -eq 0 ]]; then
    packages+=(
        nlohmann-json3-dev libspdlog-dev libfmt-dev libtomlplusplus-dev libcli11-dev
        libgtest-dev libgmock-dev
    )
fi

if [[ $with_clang -eq 1 ]]; then
    packages+=(clang-20 clang-tidy-20 clang-format-20 libclang-rt-20-dev lld-20)
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends "${packages[@]}"
echo "Installed: ${packages[*]}"
