#!/usr/bin/env python3
"""Mutation testing: put a bug back on purpose, and check that a test catches it.

A test suite is only as good as the bugs it can catch. Each mutant removes or
bends one rule the code keeps (a check, a bound, an order), and names the
test that must fail without it. This script applies each mutant in turn,
rebuilds the suite, runs it, and reports:

  KILLED                 the named test failed (with any others)
  KILLED (other tests)   tests failed, but not the named one: the rule is
                         guarded, by another test than the one that claims it
  KILLED (timeout)       the suite hung: guarded, but not by a clean failure
  SURVIVED               every test passed: no test guards the rule
  BUILD FAILED           the mutant does not compile; write it another way
  PATTERN NOT FOUND, AMBIGUOUS
                         the edit's text is not in the file exactly once, so
                         it could mutate the wrong place: it is refused

A test the suite died in (a crash, an abort) has failed: the binary never
printed its verdict, so it is named from its "[ RUN      ]" line.

Every source file is restored after each mutant, and checked byte for byte
at the end, whatever happens. The build directory is rebuilt from the
unmutated sources last. Never run two at once, nor while another build reads
the same sources (clang-tidy, a sanitizer build): they would compile mutants.

A spec is a TOML file (tests/mutants/*.toml):

    target  = "rr_agents_tests"                                   # CMake target
    binary  = "build/dev/tests/unit/agents/rr_agents_tests"       # GoogleTest binary
    timeout = 150                                                 # seconds per run

    [[mutant]]
    name   = "answers-share-one-slot"
    file   = "src/agents/army.cpp"
    expect = "Army.EachAnswerTakesASlot"                          # the test that guards it
      [[mutant.edit]]                                             # one or more
      from = '''Slots{static_cast<std::size_t>(choices)}'''
      to   = '''Slots{1}'''

Usage:
    scripts/mutate.py tests/mutants/agents.toml [mutant-name ...] [--preset dev]

Exits 0 when every mutant run was killed by a failing test, 1 otherwise.
"""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys
import tomllib
from dataclasses import dataclass, field

ROOT = pathlib.Path(__file__).resolve().parent.parent


@dataclass(frozen=True)
class Edit:
    old: str
    new: str


@dataclass(frozen=True)
class Mutant:
    name: str
    file: str
    expect: str
    edits: tuple[Edit, ...]


@dataclass(frozen=True)
class Spec:
    target: str
    binary: str
    timeout: int
    mutants: tuple[Mutant, ...] = field(default_factory=tuple)


class SpecError(ValueError):
    """A spec that cannot be run as written."""


def load_spec(text: str) -> Spec:
    """Parses a spec's TOML text. Raises SpecError naming what is wrong."""
    try:
        document = tomllib.loads(text)
    except tomllib.TOMLDecodeError as error:
        raise SpecError(f"not TOML: {error}") from error
    for key in ("target", "binary"):
        if not isinstance(document.get(key), str) or not document[key]:
            raise SpecError(f"'{key}' must be a non-empty string")
    timeout = document.get("timeout", 600)
    if not isinstance(timeout, int) or timeout < 1:
        raise SpecError("'timeout' must be a whole number of seconds, at least 1")
    mutants = []
    names = set()
    for index, entry in enumerate(document.get("mutant", [])):
        where = f"mutant #{index + 1}"
        for key in ("name", "file", "expect"):
            if not isinstance(entry.get(key), str) or not entry[key]:
                raise SpecError(f"{where}: '{key}' must be a non-empty string")
        if entry["name"] in names:
            raise SpecError(f"{where}: the name '{entry['name']}' is used twice")
        names.add(entry["name"])
        edits = entry.get("edit", [])
        if not edits:
            raise SpecError(f"{where} ('{entry['name']}'): no [[mutant.edit]]")
        parsed = []
        for edit in edits:
            if not isinstance(edit.get("from"), str) or not edit["from"] or not isinstance(edit.get("to"), str):
                raise SpecError(f"{where} ('{entry['name']}'): an edit needs a non-empty 'from' and a 'to'")
            if edit["from"] == edit["to"]:
                raise SpecError(f"{where} ('{entry['name']}'): an edit that changes nothing")
            parsed.append(Edit(edit["from"], edit["to"]))
        mutants.append(Mutant(entry["name"], entry["file"], entry["expect"], tuple(parsed)))
    return Spec(document["target"], document["binary"], timeout, tuple(mutants))


def apply_edits(text: str, edits: tuple[Edit, ...]) -> tuple[str | None, str]:
    """The text with each edit made, in order; or None and why not.

    An edit must find its text exactly once: an ambiguous one could mutate a
    copy that is not the code under test.
    """
    for edit in edits:
        count = text.count(edit.old)
        if count != 1:
            verdict = "PATTERN NOT FOUND" if count == 0 else f"AMBIGUOUS ({count}x)"
            return None, f"{verdict}: {edit.old[:70]!r}"
        text = text.replace(edit.old, edit.new, 1)
    return text, ""


def failed_tests(output: str) -> list[str]:
    """The tests GoogleTest's output reports as failed, each once, sorted.

    A test that started and never ended, with "[       OK ]", "[  FAILED  ]" or
    "[  SKIPPED ]", is the one the suite died in: it failed too.
    """
    failed = set()
    running = None
    for line in output.splitlines():
        if line.startswith("[ RUN      ] "):
            running = line.split("] ", 1)[1].strip()
        elif line.startswith(("[       OK ] ", "[  FAILED  ] ", "[  SKIPPED ] ")):
            name = line.split("] ", 1)[1].split(" (", 1)[0].strip()
            if line.startswith("[  FAILED  ] ") and "." in name:
                failed.add(name)
            if name == running:
                running = None
    if running is not None:
        failed.add(running)
    return sorted(failed)


def verdict(returncode: int, failed: list[str], expect: str) -> str:
    """What a mutant's run says about the test that claims to guard it."""
    if returncode == 0:
        return "SURVIVED"
    if expect in failed:
        return "KILLED"
    return "KILLED (other tests)"


def run(command: list[str], timeout: int) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=timeout, check=False)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("spec", type=pathlib.Path)
    parser.add_argument("names", nargs="*", help="run only these mutants")
    parser.add_argument("--preset", default="dev", help="the CMake build preset (default: dev)")
    arguments = parser.parse_args(argv)

    spec = load_spec(arguments.spec.read_text())
    unknown = set(arguments.names) - {mutant.name for mutant in spec.mutants}
    if unknown:
        print(f"no such mutant: {', '.join(sorted(unknown))}", file=sys.stderr)
        return 2
    build = ["cmake", "--build", "--preset", arguments.preset, "--target", spec.target]
    originals: dict[pathlib.Path, bytes] = {}
    results: list[tuple[str, str, str]] = []
    try:
        for mutant in spec.mutants:
            if arguments.names and mutant.name not in arguments.names:
                continue
            path = ROOT / mutant.file
            original = originals.setdefault(path, path.read_bytes())
            mutated, why = apply_edits(original.decode(), mutant.edits)
            if mutated is None:
                results.append((mutant.name, why.split(":", 1)[0], why.split(":", 1)[1].strip()))
                continue
            try:
                path.write_text(mutated)
                built = run(build, 1800)
                if built.returncode != 0:
                    results.append((mutant.name, "BUILD FAILED", built.stdout[-300:]))
                    continue
                try:
                    tested = run([str(ROOT / spec.binary)], spec.timeout)
                except subprocess.TimeoutExpired:
                    results.append((mutant.name, "KILLED (timeout)", ""))
                    continue
                failed = failed_tests(tested.stdout + tested.stderr)
                results.append((mutant.name, verdict(tested.returncode, failed, mutant.expect), ", ".join(failed)))
            finally:
                path.write_bytes(original)
    finally:
        for path, original in originals.items():
            path.write_bytes(original)
        for path, original in originals.items():
            assert path.read_bytes() == original, f"{path} was not restored"
    width = max((len(name) for name, _, _ in results), default=10)
    for name, outcome, detail in results:
        print(f"{name:<{width}}  {outcome:<22} {detail}")
    run(build, 1800)  # the build directory matches the sources again
    survived = [name for name, outcome, _ in results if not outcome.startswith("KILLED")]
    print(f"\n{len(results) - len(survived)} of {len(results)} killed")
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
