#!/usr/bin/env python3
"""Tests for the runtime image's entry point (entrypoint.sh).

The script runs under bash as it does in the image, with fake `nvidia-smi` and
`rreport-runtime` executables first on PATH. The fakes log every call to a
file, and behave as the test's environment variables ask:

  FAKE_GPU=none          nvidia-smi fails, as on a pod without a GPU
  FAKE_SYNC_EXIT=N       `models sync` exits with N
  FAKE_SYNC_WAITS=1      `models sync` runs until it is sent SIGTERM
  FAKE_SERVE_EXIT=N      `serve` exits with N at once (else it serves until SIGTERM)
  FAKE_STOP_EXIT=N       `stop-pod` exits with N

    python3 -m unittest discover -s docker/runtime -p 'test_*.py' -v
"""
from __future__ import annotations

import os
import pathlib
import signal
import subprocess
import tempfile
import time
import unittest

ENTRYPOINT = pathlib.Path(__file__).with_name("entrypoint.sh")

FAKE_NVIDIA_SMI = """#!/usr/bin/env bash
if [[ "${FAKE_GPU:-}" == none ]]; then
    echo "NVIDIA-SMI has failed because it couldn't communicate with the NVIDIA driver." >&2
    exit 9
fi
echo "NVIDIA GeForce RTX 4090, 24564 MiB"
"""

# Waits in the background, so that bash can run its TERM trap at once.
FAKE_RUNTIME = """#!/usr/bin/env bash
log() { echo "$*" >> "$FAKE_LOG"; }
# On SIGTERM: logs it, takes 0.3 s to shut down (as the runtime stops its
# engines), logs that, and exits with $2.
wait_for_term() {
    sleep 60 &
    local pid=$!
    trap "log '$1: TERM'; kill $pid; sleep 0.3; log '$1: stopped'; exit ${2:-0}" TERM
    wait "$pid"
    log "$1: never stopped"
    exit 99
}
case "$1" in
    version) echo "rreport-runtime 0.1.0 (test)" ;;
    models)
        log "$*"
        if [[ -n "${FAKE_SYNC_WAITS:-}" ]]; then wait_for_term sync 130; fi
        exit "${FAKE_SYNC_EXIT:-0}" ;;
    serve)
        log "$*"
        if [[ -n "${FAKE_SERVE_EXIT:-}" ]]; then exit "$FAKE_SERVE_EXIT"; fi
        wait_for_term serve 0 ;;
    stop-pod)
        log "stop-pod"
        exit "${FAKE_STOP_EXIT:-0}" ;;
    *) log "unexpected: $*"; exit 64 ;;
esac
"""


class Entrypoint:
    """The entry point, running with the fakes, and what they logged."""

    def __init__(self, directory: pathlib.Path, environment: dict[str, str], arguments: list[str]):
        self.log_file = directory / "calls.log"
        self.log_file.touch()
        bin_dir = directory / "bin"
        bin_dir.mkdir()
        for name, text in (("nvidia-smi", FAKE_NVIDIA_SMI), ("rreport-runtime", FAKE_RUNTIME)):
            (bin_dir / name).write_text(text)
            (bin_dir / name).chmod(0o755)
        env = {"PATH": f"{bin_dir}:/usr/bin:/bin", "FAKE_LOG": str(self.log_file), **environment}
        self.process = subprocess.Popen(["bash", str(ENTRYPOINT), *arguments], env=env,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    def calls(self) -> list[str]:
        return self.log_file.read_text().splitlines()

    def wait_for_call(self, prefix: str, timeout: float = 10) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if any(call.startswith(prefix) for call in self.calls()):
                return
            time.sleep(0.02)
        raise AssertionError(f"no call starting with {prefix!r}; calls: {self.calls()}")

    def stop(self) -> None:
        self.process.send_signal(signal.SIGTERM)

    def finish(self, timeout: float = 10) -> tuple[int, str]:
        """Waits for the script to end; `calls_at_exit` is the log at that moment.

        The script's own end, not its pipes': a child still running holds them
        open, and communicate() would wait for it too, hiding a script that
        left before its runtime had stopped."""
        status = self.process.wait(timeout=timeout)
        self.calls_at_exit = self.calls()
        _, stderr = self.process.communicate(timeout=timeout)
        return status, stderr

    def still_running_after(self, seconds: float) -> bool:
        try:
            self.process.wait(timeout=seconds)
            return False
        except subprocess.TimeoutExpired:
            return True


class EntrypointTest(unittest.TestCase):
    def start(self, arguments: list[str] | None = None, **environment: str) -> Entrypoint:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        entrypoint = Entrypoint(pathlib.Path(directory.name), environment, arguments or [])

        def reap():
            if entrypoint.process.poll() is None:
                entrypoint.process.kill()
            entrypoint.process.communicate()
        self.addCleanup(reap)
        return entrypoint

    ON_RUNPOD = {"RUNPOD_POD_ID": "pod123", "RR_RUNTIME_TOKEN": "t"}

    def test_serves_until_stopped_then_ends_cleanly(self):
        pod = self.start(**self.ON_RUNPOD)
        pod.wait_for_call("serve")
        pod.stop()
        status, stderr = pod.finish()
        self.assertEqual(status, 0, stderr)
        self.assertEqual(pod.calls_at_exit, ["models sync", "serve", "serve: TERM", "serve: stopped"],
                         "the stop reached the runtime, and the script waited for it to finish")
        self.assertIn("RTX 4090", stderr, "the GPUs are logged")

    def test_arguments_reach_both_commands(self):
        pod = self.start(["--set", "log.level=debug"], **self.ON_RUNPOD)
        pod.wait_for_call("serve")
        pod.stop()
        pod.finish()
        self.assertEqual(pod.calls()[:2], ["models sync --set log.level=debug", "serve --set log.level=debug"])

    def test_a_pod_without_a_gpu_stops_itself_and_waits_to_be_taken_down(self):
        pod = self.start(FAKE_GPU="none", **self.ON_RUNPOD)
        pod.wait_for_call("stop-pod")
        self.assertTrue(pod.still_running_after(0.5), "an exit would make RunPod restart the container")
        pod.stop()
        status, stderr = pod.finish()
        self.assertEqual(status, 0)
        self.assertIn("no GPU is visible", stderr)
        self.assertNotIn("models sync", pod.calls(), "nothing downloaded for a pod that cannot run")

    def test_outside_runpod_a_failure_just_exits(self):
        pod = self.start(FAKE_GPU="none", RR_RUNTIME_TOKEN="t")
        status, stderr = pod.finish()
        self.assertEqual(status, 1)
        self.assertEqual(pod.calls(), [], "no pod to stop")
        self.assertIn("no GPU is visible", stderr)
        self.assertNotIn("unbound variable", stderr, "an exit of its own, not a crash")

    def test_a_missing_token_stops_the_pod_before_any_download(self):
        pod = self.start(RUNPOD_POD_ID="pod123")
        pod.wait_for_call("stop-pod")
        pod.stop()
        _, stderr = pod.finish()
        self.assertIn("RR_RUNTIME_TOKEN is not set", stderr)
        self.assertEqual(pod.calls(), ["stop-pod"])

    def test_a_failed_sync_stops_the_pod(self):
        pod = self.start(FAKE_SYNC_EXIT="1", **self.ON_RUNPOD)
        pod.wait_for_call("stop-pod")
        pod.stop()
        _, stderr = pod.finish()
        self.assertIn("could not be brought up to date (exit 1)", stderr)
        self.assertEqual(pod.calls(), ["models sync", "stop-pod"])

    def test_a_runtime_that_fails_stops_the_pod(self):
        pod = self.start(FAKE_SERVE_EXIT="2", **self.ON_RUNPOD)
        pod.wait_for_call("stop-pod")
        pod.stop()
        _, stderr = pod.finish()
        self.assertIn("the runtime stopped with exit code 2", stderr)

    def test_a_stop_during_the_sync_is_passed_on_and_ends_the_script(self):
        pod = self.start(FAKE_SYNC_WAITS="1", **self.ON_RUNPOD)
        pod.wait_for_call("models sync")
        pod.stop()
        status, stderr = pod.finish()
        self.assertEqual(status, 0, stderr)
        self.assertEqual(pod.calls_at_exit, ["models sync", "sync: TERM", "sync: stopped"],
                         "neither served nor stopped again")
        self.assertNotIn("could not be brought up to date", stderr, "a stop is not a failure")

    def test_a_pod_that_cannot_stop_itself_still_waits(self):
        pod = self.start(FAKE_GPU="none", FAKE_STOP_EXIT="1", **self.ON_RUNPOD)
        pod.wait_for_call("stop-pod")
        self.assertTrue(pod.still_running_after(0.5))
        pod.stop()
        status, stderr = pod.finish()
        self.assertEqual(status, 0)
        self.assertIn("stop it in RunPod's console", stderr)


if __name__ == "__main__":
    unittest.main()
