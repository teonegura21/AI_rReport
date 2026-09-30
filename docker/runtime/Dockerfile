# syntax=docker/dockerfile:1.7
#
# rReport's pod image: rreport-runtime and llama-server, on CUDA 12.8.
#
#   docker build -f docker/runtime/Dockerfile -t rreport-runtime:dev .
#
# The build context is the repository root. What the image holds, and why:
#   /opt/rreport/rreport-runtime   the supervisor (runtime-release preset: no hub, no tests)
#   /opt/rreport/catalog.toml      which models run on which GPU (the runtime's
#                                  models.catalog default)
#   /opt/rreport/entrypoint.sh     PID 1: checks, model sync, then the runtime
#   /opt/llama.cpp/                llama-server, its libraries and its CUDA backend
# Model files are not in the image. They live on the pod's /workspace volume,
# downloaded once and verified by `rreport-runtime models sync` (design:
# docs/design/runtime.md, "The image").

ARG CUDA_VERSION=12.8.1
ARG UBUNTU_VERSION=24.04

# ---- llama.cpp ----------------------------------------------------------------
FROM nvidia/cuda:${CUDA_VERSION}-devel-ubuntu${UBUNTU_VERSION} AS llama

# A release tag, and the commit it must name: a tag can be moved, a commit cannot.
ARG LLAMA_CPP_TAG=v0.5.0
ARG LLAMA_CPP_COMMIT=7fe450e19305b828c199d602c23a8337aaa1f03b
# sm_80 A100 · sm_86 A40, RTX A6000, RTX 3090 · sm_89 L4, L40S, RTX 4090, RTX 6000 Ada ·
# sm_90 H100, H200 · sm_120 RTX 5090, RTX PRO 6000 (which needs CUDA 12.8 or later).
ARG CUDA_ARCHITECTURES="80;86;89;90;120"

RUN apt-get update -qq \
    && apt-get install -y -qq --no-install-recommends build-essential cmake git ca-certificates libssl-dev \
    && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 --branch "${LLAMA_CPP_TAG}" https://github.com/ggml-org/llama.cpp /src/llama.cpp \
    && actual="$(git -C /src/llama.cpp rev-parse HEAD)" \
    && if [ "${actual}" != "${LLAMA_CPP_COMMIT}" ]; then \
         echo "llama.cpp ${LLAMA_CPP_TAG} is ${actual}, expected ${LLAMA_CPP_COMMIT}" >&2; exit 1; \
       fi
WORKDIR /src/llama.cpp
# Configured as llama.cpp's own CUDA image (.devops/cuda.Dockerfile):
#   GGML_BACKEND_DL + GGML_CPU_ALL_VARIANTS  backends are libraries loaded at
#       run time, from the executable's directory, with a CPU backend for every
#       x86 generation (the pod's CPU is unknown at build time);
#   --allow-shlib-undefined  libcuda.so is the host driver's, mounted into the
#       container at run time, so it cannot be resolved here.
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
        -DGGML_NATIVE=OFF -DGGML_CUDA=ON -DGGML_BACKEND_DL=ON -DGGML_CPU_ALL_VARIANTS=ON \
        -DCMAKE_CUDA_ARCHITECTURES="${CUDA_ARCHITECTURES}" \
        -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF \
        -DCMAKE_EXE_LINKER_FLAGS=-Wl,--allow-shlib-undefined \
    && cmake --build build --config Release -j"$(nproc)" \
    && mkdir -p /opt/llama.cpp \
    && cp build/bin/llama-server /opt/llama.cpp/ \
    && find build -name '*.so*' -exec cp -P {} /opt/llama.cpp/ \;

# ---- rreport-runtime ------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS rreport
# The same package list as developers and CI (the script is its single source).
COPY scripts/install-deps-ubuntu.sh /tmp/install-deps-ubuntu.sh
RUN /tmp/install-deps-ubuntu.sh && rm -rf /var/lib/apt/lists/*
WORKDIR /src/rreport
COPY . .
# The revision names the release (`rreport-runtime version`), and the hub's
# doctor compares it with its own: given by the build, or read from .git.
ARG RR_GIT_REVISION=
RUN cmake --preset runtime-release ${RR_GIT_REVISION:+-DRR_GIT_REVISION=${RR_GIT_REVISION}} \
    && cmake --build --preset runtime-release --target rreport-runtime

# ---- the image ------------------------------------------------------------------
FROM nvidia/cuda:${CUDA_VERSION}-runtime-ubuntu${UBUNTU_VERSION}
# The run-time libraries of both programs: libcurl (OpenSSL) and toml++ for
# rreport-runtime, OpenMP for llama.cpp's CPU backend; CA certificates for
# TLS to Hugging Face and RunPod.
RUN apt-get update -qq \
    && apt-get install -y -qq --no-install-recommends ca-certificates libcurl4t64 libtomlplusplus3t64 libgomp1 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=llama /opt/llama.cpp /opt/llama.cpp
# llama-server's own libraries (libllama, libggml…) are found through ld.so;
# its backends, next to the real executable (the symlink is resolved first).
RUN echo /opt/llama.cpp > /etc/ld.so.conf.d/llama-cpp.conf && ldconfig \
    && ln -s /opt/llama.cpp/llama-server /usr/local/bin/llama-server

COPY --from=rreport /src/rreport/build/runtime-release/apps/rreport-runtime/rreport-runtime /opt/rreport/
COPY docker/runtime/catalog.toml /opt/rreport/catalog.toml
COPY --chmod=0755 docker/runtime/entrypoint.sh /opt/rreport/entrypoint.sh
RUN ln -s /opt/rreport/rreport-runtime /usr/local/bin/rreport-runtime \
    && rreport-runtime version \
    && llama-server --version

# The runtime's one door (server.port). RunPod publishes it through its HTTP
# proxy: https://<pod id>-8080.proxy.runpod.net
EXPOSE 8080
ENTRYPOINT ["/opt/rreport/entrypoint.sh"]
