#!/usr/bin/env python3
"""Tests for mutate.py: reading specs, making edits, and judging a run.

Building and running a suite is left to the real thing (tests/mutants/ is run
by hand, before a milestone). What is tested here is everything that decides
what a run means: a spec read strictly, an edit made only where it is
unambiguous, GoogleTest's failures read, and the verdict.

    python3 -m unittest discover -s scripts -p 'test_*.py' -v
"""
from __future__ import annotations

import importlib.util
import pathlib
import sys
import unittest

SCRIPT = pathlib.Path(__file__).with_name("mutate.py")
_spec = importlib.util.spec_from_file_location("mutate", SCRIPT)
mutate = importlib.util.module_from_spec(_spec)
sys.modules["mutate"] = mutate  # dataclasses look their module up there
_spec.loader.exec_module(mutate)

SPEC = """
target = "rr_agents_tests"
binary = "build/dev/tests/unit/agents/rr_agents_tests"
timeout = 90

[[mutant]]
name = "answers-share-one-slot"
file = "src/agents/army.cpp"
expect = "Army.EachAnswerTakesASlot"
  [[mutant.edit]]
  from = '''Slots{static_cast<std::size_t>(choices)}'''
  to = '''Slots{1}'''

[[mutant]]
name = "two-lines"
file = "src/agents/ladder.cpp"
expect = "Ladder.NeverClimbsARungTwice"
  [[mutant.edit]]
  from = '''
    if (a) {
        return;'''
  to = '''
    if (false) {
        return;'''
"""


class LoadSpec(unittest.TestCase):
    def test_reads_every_field(self) -> None:
        spec = mutate.load_spec(SPEC)
        self.assertEqual(spec.target, "rr_agents_tests")
        self.assertEqual(spec.binary, "build/dev/tests/unit/agents/rr_agents_tests")
        self.assertEqual(spec.timeout, 90)
        self.assertEqual([mutant.name for mutant in spec.mutants], ["answers-share-one-slot", "two-lines"])
        first = spec.mutants[0]
        self.assertEqual(first.file, "src/agents/army.cpp")
        self.assertEqual(first.expect, "Army.EachAnswerTakesASlot")
        self.assertEqual(first.edits, (mutate.Edit("Slots{static_cast<std::size_t>(choices)}", "Slots{1}"),))

    def test_keeps_code_exactly_as_written(self) -> None:
        # Literal strings: no escapes are read, and lines stay lines (TOML
        # drops only the newline right after the opening quotes).
        edit = mutate.load_spec(SPEC).mutants[1].edits[0]
        self.assertEqual(edit.old, "    if (a) {\n        return;")

    def test_refuses_what_it_cannot_run(self) -> None:
        broken = {
            "not TOML": "target = ",
            "no target": 'binary = "b"',
            "no binary": 'target = "t"',
            "a bad timeout": 'target = "t"\nbinary = "b"\ntimeout = 0',
            "a mutant without a test": 'target = "t"\nbinary = "b"\n[[mutant]]\nname = "m"\nfile = "f"\n'
            '  [[mutant.edit]]\n  from = "x"\n  to = "y"',
            "a mutant without an edit": 'target = "t"\nbinary = "b"\n[[mutant]]\nname = "m"\nfile = "f"\nexpect = "T.t"',
            "an edit that changes nothing": 'target = "t"\nbinary = "b"\n[[mutant]]\nname = "m"\nfile = "f"\n'
            'expect = "T.t"\n  [[mutant.edit]]\n  from = "x"\n  to = "x"',
            "a name used twice": 'target = "t"\nbinary = "b"\n'
            + '[[mutant]]\nname = "m"\nfile = "f"\nexpect = "T.t"\n  [[mutant.edit]]\n  from = "x"\n  to = "y"\n' * 2,
        }
        for why, text in broken.items():
            with self.subTest(why), self.assertRaises(mutate.SpecError):
                mutate.load_spec(text)


class ApplyEdits(unittest.TestCase):
    def test_makes_each_edit_in_order(self) -> None:
        text, why = mutate.apply_edits("a = 1; b = 2;", (mutate.Edit("a = 1", "a = 3"), mutate.Edit("a = 3; b", "c")))
        self.assertEqual(text, "c = 2;")
        self.assertEqual(why, "")

    def test_refuses_an_edit_that_could_land_elsewhere(self) -> None:
        text, why = mutate.apply_edits("x x", (mutate.Edit("x", "y"),))
        self.assertIsNone(text)
        self.assertTrue(why.startswith("AMBIGUOUS (2x)"))
        text, why = mutate.apply_edits("x", (mutate.Edit("z", "y"),))
        self.assertIsNone(text)
        self.assertTrue(why.startswith("PATTERN NOT FOUND"))


class Judging(unittest.TestCase):
    OUTPUT = """[==========] 3 tests from 1 test suite ran.
[  PASSED  ] 1 test.
[  FAILED  ] 2 tests, listed below:
[  FAILED  ] Army.EachAnswerTakesASlot (102 ms)
[  FAILED  ] Army.ASharedPoolRunsOnlyWhatItsTokensHold (12 ms)

 2 FAILED TESTS
"""

    def test_reads_the_failed_tests(self) -> None:
        self.assertEqual(
            mutate.failed_tests(self.OUTPUT),
            ["Army.ASharedPoolRunsOnlyWhatItsTokensHold", "Army.EachAnswerTakesASlot"],
        )
        self.assertEqual(mutate.failed_tests("[  PASSED  ] 3 tests."), [])

    def test_the_test_a_suite_died_in_failed(self) -> None:
        crashed = (
            "[ RUN      ] Ledger.A\n[       OK ] Ledger.A (0 ms)\n"
            "[ RUN      ] Ledger.B\n[  SKIPPED ] Ledger.B (0 ms)\n"
            "[ RUN      ] Ledger.C\n/usr/include/c++/14/bits/stl_vector.h:1130: Assertion failed\n"
        )
        self.assertEqual(mutate.failed_tests(crashed), ["Ledger.C"])
        finished = "[ RUN      ] Ledger.A\n[  FAILED  ] Ledger.A (3 ms)\n" + self.OUTPUT
        self.assertEqual(
            mutate.failed_tests(finished),
            ["Army.ASharedPoolRunsOnlyWhatItsTokensHold", "Army.EachAnswerTakesASlot", "Ledger.A"],
        )

    def test_judges_by_the_test_that_claims_the_rule(self) -> None:
        failed = mutate.failed_tests(self.OUTPUT)
        self.assertEqual(mutate.verdict(1, failed, "Army.EachAnswerTakesASlot"), "KILLED")
        self.assertEqual(mutate.verdict(1, failed, "Army.SomethingElse"), "KILLED (other tests)")
        self.assertEqual(mutate.verdict(0, [], "Army.EachAnswerTakesASlot"), "SURVIVED")


class TheSpecsInTheTree(unittest.TestCase):
    def test_every_spec_reads(self) -> None:
        specs = sorted((SCRIPT.parent.parent / "tests" / "mutants").glob("*.toml"))
        self.assertTrue(specs, "tests/mutants/ has specs")
        for path in specs:
            with self.subTest(path.name):
                spec = mutate.load_spec(path.read_text())
                self.assertTrue(spec.mutants)
                for mutant in spec.mutants:
                    self.assertTrue((SCRIPT.parent.parent / mutant.file).is_file(), mutant.file)

    def test_every_edit_finds_its_text_exactly_once(self) -> None:
        # A spec that drifted from the code it mutates would only say so when
        # run; here it says so on every push.
        for path in sorted((SCRIPT.parent.parent / "tests" / "mutants").glob("*.toml")):
            for mutant in mutate.load_spec(path.read_text()).mutants:
                with self.subTest(f"{path.name}: {mutant.name}"):
                    source = (SCRIPT.parent.parent / mutant.file).read_text()
                    text, why = mutate.apply_edits(source, mutant.edits)
                    self.assertIsNotNone(text, why)


if __name__ == "__main__":
    unittest.main()
