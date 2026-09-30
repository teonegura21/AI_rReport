#!/usr/bin/env bash
# The rReport runtime image's entry point: PID 1 of the pod's container.
#
#   1. Checks what the pod needs before it spends anything: a GPU, and the
#      hub's token.
#   2. `rreport-runtime models sync`: brings the GPU profile's model files to
#      the /workspace volume. What is missing or wrong is downloaded
#      (resumably, and verified); the rest is checked.
#   3. `rreport-runtime serve`, until the pod is stopped.
# Arguments are passed on to both commands (e.g. --set log.level=debug).
#
# Why a failure stops the pod (ADR-0016). RunPod restarts a container whose
# command exits, so a pod that cannot start would restart, and bill, in a
# loop. Instead the pod stops itself through RunPod's API
# (`rreport-runtime stop-pod`), and this script waits to be taken down, with
# the reason on the pod's log. Outside RunPod it simply exits.
#
# Why bash stays PID 1 instead of exec'ing the runtime: to see how the runtime
# ended. The kernel gives PID 1 no default signal handling, so every step runs
# as a child, and a stop signal is passed on to it.
set -uo pipefail

log() { printf 'entrypoint: %s\n' "$*" >&2; }

child=0        # the step running now
stopping=0     # a stop signal arrived: RunPod (or docker) is taking the pod down
interrupted=0  # a stop signal arrived during the current `wait`

# shellcheck disable=SC2317  # reached through the trap below
on_stop() {
    stopping=1
    interrupted=1
    if ((child > 0)); then
        kill -TERM "$child" 2>/dev/null
    fi
}
trap on_stop TERM INT HUP

# Runs "$@" as a child, passing stop signals on, and returns its exit status.
# A trapped signal ends `wait` early, while the child may still be running:
# then wait again, until the child itself has ended.
run() {
    "$@" &
    child=$!
    if ((stopping)); then  # the signal came before $child was known
        kill -TERM "$child" 2>/dev/null
    fi
    local status
    while true; do
        interrupted=0
        wait "$child"
        status=$?
        ((interrupted)) || break
    done
    child=0
    return "$status"
}

# Gives up: on RunPod, stops the pod and waits to be taken down.
give_up() {
    log "$1"
    if [[ -z "${RUNPOD_POD_ID:-}" ]]; then
        exit 1  # nothing restarts us in a loop here
    fi
    log "stopping pod ${RUNPOD_POD_ID}: a restart would fail the same way, and bill"
    if ! run rreport-runtime stop-pod; then
        ((stopping)) && exit 0
        log "the pod could not stop itself: stop it in RunPod's console, or it restarts in a loop"
    fi
    ((stopping)) && exit 0
    run sleep infinity  # exiting would restart the container: wait for the stop instead
    exit 0
}

# Runs one step; a stop signal during it ends the script.
step() {
    run "$@"
    local status=$?
    ((stopping)) && exit 0
    return "$status"
}

log "$(rreport-runtime version 2>&1)"

# NVIDIA's container runtime mounts nvidia-smi into a GPU container: without it
# there is no GPU to use.
if ! command -v nvidia-smi >/dev/null \
    || ! gpus=$(nvidia-smi --query-gpu=name,memory.total --format=csv,noheader 2>&1) || [[ -z "$gpus" ]]; then
    give_up "no GPU is visible (nvidia-smi: ${gpus:-not found}): is this a GPU pod?"
fi
log "GPUs: ${gpus//$'\n'/; }"

if [[ -z "${RR_RUNTIME_TOKEN:-}${RR_RUNTIME_SERVER__TOKEN:-}${RR_RUNTIME_CONFIG:-}" ]]; then
    give_up "RR_RUNTIME_TOKEN is not set: give the pod the token the hub sends (the hub's runtime.token)"
fi

step rreport-runtime models sync "$@" || give_up "the model files could not be brought up to date (exit $?): see above"

step rreport-runtime serve "$@"
status=$?
((status == 0)) && exit 0
give_up "the runtime stopped with exit code ${status}: see above"
