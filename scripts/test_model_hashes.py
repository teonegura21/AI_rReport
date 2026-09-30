#!/usr/bin/env python3
"""Tests for model-hashes.py, run against a fake Hugging Face Hub.

The fake answers the two endpoints the script uses, as the Hub does
(`/revision/<rev>` and `/paths-info/<commit>`), and records what it was sent.
The script runs as a separate process, exactly as an operator runs it.

    python3 -m unittest discover -s scripts -p 'test_*.py' -v
"""
from __future__ import annotations

import http.server
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import threading
import tomllib
import unittest
import urllib.parse

SCRIPT = pathlib.Path(__file__).with_name("model-hashes.py")
COMMIT_MAIN = "1" * 40  # what org/big-GGUF's main branch points at
COMMIT_SMALL = "2" * 40  # the commit the catalog already pins other/small-GGUF to
SHA = {name: (digit * 64) for name, digit in
       {"big-Q4": "a", "big-Q8": "b", "mmproj": "c", "small": "d", "new-mmproj": "e"}.items()}

CATALOG = f"""# A catalog in the runtime's format. This comment must survive a pin.
[[model]]
id = "big-q4"
role = "commander"
file = "big/big-Q4.gguf"
sha256 = "{SHA['big-Q4']}"
url = "https://huggingface.co/org/big-GGUF/resolve/main/big-Q4.gguf"
projector = {{ file = "big/mmproj.gguf", sha256 = "{SHA['mmproj']}", url = "https://huggingface.co/org/big-GGUF/resolve/main/mmproj.gguf" }}

[[model]]
id = "big-q8"
role = "commander"
file = "big/big-Q8.gguf"  # shares the projector above
sha256 = "{SHA['big-Q8']}"
url = "https://huggingface.co/org/big-GGUF/resolve/main/big-Q8.gguf"
projector = {{ file = "big/mmproj.gguf", sha256 = "{SHA['mmproj']}", url = "https://huggingface.co/org/big-GGUF/resolve/main/mmproj.gguf" }}

[[model]]
id = "small"
role = "infantry"
file = "small/small.gguf"
sha256 = "{SHA['small']}"
url = "https://huggingface.co/other/small-GGUF/resolve/{COMMIT_SMALL}/small.gguf"
"""


class FakeHub:
    """A Hub with two repositories, served on a free local port."""

    def __init__(self):
        self.branches = {"org/big-GGUF": {"main": COMMIT_MAIN}, "other/small-GGUF": {}}
        self.files = {
            ("org/big-GGUF", COMMIT_MAIN): {"big-Q4.gguf": SHA["big-Q4"], "big-Q8.gguf": SHA["big-Q8"],
                                            "mmproj.gguf": SHA["mmproj"]},
            ("other/small-GGUF", COMMIT_SMALL): {"small.gguf": SHA["small"]},
        }
        self.not_in_lfs: set[str] = set()
        self.busy_answers = 0  # the next N requests get 503
        self.authorizations: list[str | None] = []
        hub = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):  # keep the test output clean
                pass

            def answer(self, status: int, document: object, headers: dict[str, str] | None = None):
                body = json.dumps(document).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                for name, value in (headers or {}).items():
                    self.send_header(name, value)
                self.end_headers()
                self.wfile.write(body)

            def route(self, pattern: str):
                return re.fullmatch(pattern, urllib.parse.unquote(self.path))

            def do_GET(self):
                hub.authorizations.append(self.headers.get("Authorization"))
                if hub.busy_answers > 0:
                    hub.busy_answers -= 1
                    return self.answer(503, {"error": "busy"}, {"Retry-After": "0"})
                match = self.route(r"/api/models/([^/]+/[^/]+)/revision/(.+)")
                if match is None or match[1] not in hub.branches:
                    return self.answer(404, {"error": "Repository not found"})
                repo, revision = match[1], match[2]
                commit = hub.branches[repo].get(revision, revision)
                if (repo, commit) not in hub.files:
                    return self.answer(404, {"error": f"Invalid rev id: {revision}"})
                self.answer(200, {"id": repo, "sha": commit})

            def do_POST(self):
                hub.authorizations.append(self.headers.get("Authorization"))
                length = int(self.headers.get("Content-Length", "0"))
                form = urllib.parse.parse_qs(self.rfile.read(length).decode())
                match = self.route(r"/api/models/([^/]+/[^/]+)/paths-info/([0-9a-f]{40})")
                files = hub.files.get((match[1], match[2])) if match else None
                if files is None:
                    return self.answer(404, {"error": "Revision not found"})
                entries = []
                for path in form.get("paths", []):
                    if path in files:
                        entry = {"type": "file", "oid": "f" * 40, "size": 1234, "path": path}
                        if path not in hub.not_in_lfs:
                            entry["lfs"] = {"oid": files[path], "size": 1234, "pointerSize": 134}
                        entries.append(entry)
                self.answer(200, entries)

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def endpoint(self) -> str:
        return f"http://127.0.0.1:{self.server.server_address[1]}"

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class ModelHashesTest(unittest.TestCase):
    def setUp(self):
        self.hub = FakeHub()
        self.addCleanup(self.hub.close)
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.catalog = pathlib.Path(directory.name) / "catalog.toml"
        self.catalog.write_text(CATALOG)

    def run_script(self, command: str, token: str | None = None) -> subprocess.CompletedProcess:
        environment = {key: value for key, value in os.environ.items()
                       if key not in ("HF_TOKEN", "HF_ENDPOINT")}
        environment["NO_PROXY"] = "127.0.0.1,localhost"
        if token is not None:
            environment["HF_TOKEN"] = token
        return subprocess.run(
            [sys.executable, str(SCRIPT), command, str(self.catalog), "--endpoint", self.hub.endpoint],
            capture_output=True, text=True, env=environment, timeout=60)

    def test_check_agrees_with_the_hub_and_notes_unpinned_urls(self):
        result = self.run_script("check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.count("\nok ") + result.stdout.startswith("ok "), 5)
        self.assertIn("4 URL(s) follow a branch", result.stdout, "the small model is pinned already")

    def test_check_names_a_mismatch_and_fails(self):
        self.hub.files[("org/big-GGUF", COMMIT_MAIN)]["big-Q8.gguf"] = SHA["new-mmproj"]
        result = self.run_script("check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("MISMATCH  big-q8: big/big-Q8.gguf", result.stdout)
        self.assertIn(f"the catalog says {SHA['big-Q8']}", result.stdout)
        self.assertIn(f"the Hub says     {SHA['new-mmproj']}", result.stdout)

    def test_pin_fixes_each_url_to_a_commit_and_each_checksum_to_the_hubs(self):
        # The projector was re-uploaded: the Hub now has another file under the same name.
        self.hub.files[("org/big-GGUF", COMMIT_MAIN)]["mmproj.gguf"] = SHA["new-mmproj"]
        result = self.run_script("pin")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        text = self.catalog.read_text()
        self.assertIn("# A catalog in the runtime's format. This comment must survive a pin.", text)
        self.assertIn("# shares the projector above", text)
        self.assertNotIn("/resolve/main/", text)
        self.assertEqual(text.count(f"/resolve/{COMMIT_MAIN}/"), 4, "two weights, the shared projector twice")
        self.assertEqual(text.count(SHA["new-mmproj"]), 2, "both copies of the shared projector")
        self.assertNotIn(SHA["mmproj"], text)
        document = tomllib.loads(text)
        self.assertEqual(document["model"][2]["url"],
                         f"https://huggingface.co/other/small-GGUF/resolve/{COMMIT_SMALL}/small.gguf")

        # The branch moves on; the pinned catalog does not.
        self.hub.branches["org/big-GGUF"]["main"] = "3" * 40
        again = self.run_script("check")
        self.assertEqual(again.returncode, 0, again.stdout)
        self.assertNotIn("follow a branch", again.stdout)

    def test_pin_writes_nothing_when_a_file_cannot_be_checked(self):
        del self.hub.files[("other/small-GGUF", COMMIT_SMALL)]["small.gguf"]
        result = self.run_script("pin")
        self.assertEqual(result.returncode, 1)
        self.assertIn("has no file 'small.gguf'", result.stdout)
        self.assertEqual(self.catalog.read_text(), CATALOG)

    def test_a_file_outside_lfs_has_no_checksum_to_compare(self):
        self.hub.not_in_lfs.add("big-Q4.gguf")
        result = self.run_script("check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("not stored in LFS", result.stdout)

    def test_the_token_goes_to_the_hub_only_when_set(self):
        self.assertEqual(self.run_script("check", token="hf_secret").returncode, 0)
        self.assertTrue(self.hub.authorizations)
        self.assertEqual(set(self.hub.authorizations), {"Bearer hf_secret"})
        self.hub.authorizations.clear()
        self.assertEqual(self.run_script("check").returncode, 0)
        self.assertEqual(set(self.hub.authorizations), {None})

    def test_a_busy_hub_is_asked_again(self):
        self.hub.busy_answers = 2
        result = self.run_script("check")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_an_unreadable_catalog_is_a_usage_error(self):
        self.catalog.write_text("this is = = not toml")
        self.assertEqual(self.run_script("check").returncode, 2)


if __name__ == "__main__":
    unittest.main()
