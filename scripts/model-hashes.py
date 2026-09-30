#!/usr/bin/env python3
"""Check, and pin, the model files of a runtime catalog against Hugging Face.

The pod verifies every model file against the sha256 its catalog gives
(docker/runtime/catalog.toml), so those checksums must be right, and must stay
right: a repository whose "main" branch is re-uploaded would otherwise change
what a pod downloads. This script asks the Hub itself.

  model-hashes.py check CATALOG   compare each file's sha256 with the Hub's
                                  record, at the revision its URL names
  model-hashes.py pin CATALOG     rewrite CATALOG in place: each URL's revision
                                  becomes the commit it resolves to now, and
                                  each sha256 the Hub's record at that commit

How the Hub answers (the endpoints huggingface_hub itself uses):
  GET  /api/models/<repo>/revision/<revision>     {"sha": "<commit>", ...}
  POST /api/models/<repo>/paths-info/<commit>     form: paths=<file>&expand=false
       -> [{"type": "file", "path": ..., "size": N,
            "lfs": {"oid": "<sha256 of the file>", "size": N}}]
Model weights are stored through LFS (or Xet, which keeps the same record), so
the Hub knows each file's sha256 without anyone downloading it.

Only URLs of the form https://huggingface.co/<owner>/<repo>/resolve/<revision>/<path>
are handled; others are reported and left alone. The catalog is rewritten as
text, so its comments and layout survive: only the quoted URLs and checksums
change.

HF_TOKEN, when set, is sent to the Hub (gated repositories need it), and only
to the Hub. Exit status: 0 all good; 1 a mismatch or an unreachable file; 2
usage errors.
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import os
import re
import sys
import time
import tomllib
import urllib.error
import urllib.parse
import urllib.request

DEFAULT_ENDPOINT = "https://huggingface.co"
COMMIT = re.compile(r"[0-9a-f]{40}")
HUB_URL = re.compile(
    r"https://huggingface\.co/(?P<repo>[^/]+/[^/]+)/resolve/(?P<revision>[^/]+)/(?P<path>.+)")
USER_AGENT = "rreport-model-hashes/1"


@dataclasses.dataclass(frozen=True)
class CatalogFile:
    model: str   # the model's id
    file: str    # path under the models directory
    sha256: str  # as the catalog gives it
    url: str


@dataclasses.dataclass(frozen=True)
class HubRecord:
    commit: str
    sha256: str
    size: int


class HubError(Exception):
    pass


def catalog_files(document: dict) -> list[CatalogFile]:
    """Every file the catalog names, weights and projectors, with a URL."""
    files = []
    for model in document.get("model", []):
        for entry in (model, model.get("projector")):
            if entry is not None and "url" in entry:
                files.append(CatalogFile(model=model["id"], file=entry["file"],
                                         sha256=entry["sha256"], url=entry["url"]))
    return files


class Hub:
    """The few Hub API calls this script needs, with retries for 429 and 5xx."""

    def __init__(self, endpoint: str, token: str | None, attempts: int = 4):
        self.endpoint = endpoint.rstrip("/")
        self.token = token
        self.attempts = attempts
        self._commits: dict[tuple[str, str], str] = {}

    def _request(self, path: str, data: bytes | None = None) -> object:
        request = urllib.request.Request(self.endpoint + path, data=data)
        request.add_header("User-Agent", USER_AGENT)
        request.add_header("Accept", "application/json")
        if self.token:
            # Only ever to the Hub's endpoint: this is the only place a request is made.
            request.add_header("Authorization", f"Bearer {self.token}")
        for attempt in range(1, self.attempts + 1):
            try:
                with urllib.request.urlopen(request, timeout=30) as response:
                    return json.load(response)
            except urllib.error.HTTPError as error:
                retryable = error.code == 429 or error.code >= 500
                if not retryable or attempt == self.attempts:
                    detail = error.read().decode("utf-8", "replace")[:300]
                    raise HubError(f"{error.code} from {path}: {detail}") from None
                delay = error.headers.get("Retry-After", "")
                time.sleep(float(delay) if delay.isdigit() else 2 ** attempt)
            except (urllib.error.URLError, TimeoutError) as error:
                if attempt == self.attempts:
                    raise HubError(f"cannot reach {self.endpoint}: {error}") from None
                time.sleep(2 ** attempt)
        raise AssertionError("unreachable")

    def commit(self, repo: str, revision: str) -> str:
        """The commit `revision` (a branch, tag or commit) names now."""
        key = (repo, revision)
        if key not in self._commits:
            info = self._request(f"/api/models/{repo}/revision/{urllib.parse.quote(revision, safe='')}")
            sha = info.get("sha") if isinstance(info, dict) else None
            if not isinstance(sha, str) or not COMMIT.fullmatch(sha):
                raise HubError(f"{repo}@{revision}: the Hub gave no commit")
            self._commits[key] = sha
        return self._commits[key]

    def record(self, repo: str, commit: str, path: str) -> HubRecord:
        """The file's sha256 and size as the Hub records them at `commit`."""
        body = urllib.parse.urlencode({"paths": path, "expand": "false"}).encode()
        entries = self._request(f"/api/models/{repo}/paths-info/{commit}", data=body)
        entry = next((item for item in entries if isinstance(item, dict) and item.get("path") == path), None)
        if entry is None or entry.get("type") != "file":
            raise HubError(f"{repo}@{commit[:12]} has no file '{path}'")
        lfs = entry.get("lfs")
        if not isinstance(lfs, dict) or not isinstance(lfs.get("oid"), str):
            raise HubError(f"{repo}/{path} is not stored in LFS, so the Hub has no sha256 for it: "
                           "download it and hash it by hand")
        return HubRecord(commit=commit, sha256=lfs["oid"], size=int(lfs.get("size", entry.get("size", 0))))


@dataclasses.dataclass
class Finding:
    file: CatalogFile
    record: HubRecord | None = None
    error: str | None = None

    @property
    def matches(self) -> bool:
        return self.record is not None and self.record.sha256 == self.file.sha256

    @property
    def pinned(self) -> bool:
        match = HUB_URL.fullmatch(self.file.url)
        return match is not None and COMMIT.fullmatch(match["revision"]) is not None

    def pinned_url(self) -> str:
        match = HUB_URL.fullmatch(self.file.url)
        assert match is not None and self.record is not None
        return f"https://huggingface.co/{match['repo']}/resolve/{self.record.commit}/{match['path']}"


def look_up(files: list[CatalogFile], hub: Hub) -> list[Finding]:
    findings = []
    for file in files:
        match = HUB_URL.fullmatch(file.url)
        if match is None:
            findings.append(Finding(file, error="not a Hugging Face 'resolve' URL: checked by hand only"))
            continue
        try:
            commit = hub.commit(match["repo"], match["revision"])
            findings.append(Finding(file, record=hub.record(match["repo"], commit, match["path"])))
        except HubError as error:
            findings.append(Finding(file, error=str(error)))
    return findings


def gib(size: int) -> str:
    return f"{size / 2**30:.1f} GiB"


def report(findings: list[Finding]) -> bool:
    """Prints one line per file; true when every checksum matches the Hub's."""
    all_good = True
    for finding in findings:
        name = f"{finding.file.model}: {finding.file.file}"
        if finding.error is not None:
            print(f"ERROR     {name}\n          {finding.error}")
            all_good = False
        elif not finding.matches:
            print(f"MISMATCH  {name}\n          the catalog says {finding.file.sha256}\n"
                  f"          the Hub says     {finding.record.sha256} (commit {finding.record.commit[:12]})")
            all_good = False
        else:
            pin = "" if finding.pinned else f", but the URL follows a branch (now {finding.record.commit[:12]})"
            print(f"ok        {name} ({gib(finding.record.size)}){pin}")
    unpinned = sum(1 for finding in findings if finding.error is None and not finding.pinned)
    if unpinned:
        print(f"\n{unpinned} URL(s) follow a branch: `pin` fixes them to today's commit.")
    return all_good


def replace_quoted(text: str, old: str, new: str) -> str:
    quoted = f'"{old}"'
    if quoted not in text:
        raise HubError(f"cannot find {quoted} in the catalog's text")
    return text.replace(quoted, f'"{new}"')


def pin(path: str, text: str, findings: list[Finding]) -> str:
    """The catalog's text with every URL at a commit and every checksum the Hub's."""
    urls: dict[str, str] = {}
    shas: dict[str, str] = {}
    for finding in findings:
        if finding.error is not None:
            raise HubError(f"{finding.file.file}: {finding.error}")
        for mapping, old, new in ((urls, finding.file.url, finding.pinned_url()),
                                  (shas, finding.file.sha256, finding.record.sha256)):
            # A value shared by several entries (a projector two models use) must
            # map to one new value, or a text replacement would be wrong.
            if mapping.setdefault(old, new) != new:
                raise HubError(f"'{old}' would become two different values: edit {path} by hand")
    for old, new in [*urls.items(), *shas.items()]:
        if old != new:
            text = replace_quoted(text, old, new)
    return text


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("command", choices=["check", "pin"])
    parser.add_argument("catalog")
    parser.add_argument("--endpoint", default=os.environ.get("HF_ENDPOINT", DEFAULT_ENDPOINT),
                        help="the Hub's address (default: $HF_ENDPOINT, else huggingface.co)")
    arguments = parser.parse_args(argv)

    try:
        with open(arguments.catalog, "rb") as file:
            raw = file.read()
        document = tomllib.loads(raw.decode("utf-8"))
    except (OSError, UnicodeDecodeError, tomllib.TOMLDecodeError) as error:
        print(f"model-hashes: cannot read {arguments.catalog}: {error}", file=sys.stderr)
        return 2
    files = catalog_files(document)
    if not files:
        print(f"model-hashes: {arguments.catalog} names no files with a URL", file=sys.stderr)
        return 2

    hub = Hub(arguments.endpoint, os.environ.get("HF_TOKEN") or None)
    findings = look_up(files, hub)
    if arguments.command == "check":
        return 0 if report(findings) else 1

    try:
        pinned = pin(arguments.catalog, raw.decode("utf-8"), findings)
        tomllib.loads(pinned)  # never write a catalog that no longer parses
    except (HubError, tomllib.TOMLDecodeError) as error:
        report(findings)
        print(f"model-hashes: nothing written: {error}", file=sys.stderr)
        return 1
    changed_checksums = [finding for finding in findings if not finding.matches]
    temporary = f"{arguments.catalog}.tmp"
    with open(temporary, "w", encoding="utf-8") as file:
        file.write(pinned)
    os.replace(temporary, arguments.catalog)
    for finding in changed_checksums:
        print(f"checksum  {finding.file.file}: {finding.file.sha256} -> {finding.record.sha256}")
    print(f"pinned {len(findings)} file(s) in {arguments.catalog}; review the diff before committing it")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
