# The report: the article as a page

A job's article is read as one web page: the article retold, each sentence starting the video at the moment it tells, and the **Fidelity Report** that says what was kept, what was left out and why (plan §2). `rr_report` reads what a job made back from its library (`rr/report/contents.hpp`) and renders it (`rr/report/page.hpp`); `rreport report JOB` writes it.

## A view, not an artifact

The artifacts are the truth (ADR-0004). The page is rendered from them whenever it is asked for, and never stored as a result: a change to how pages look runs no job again. Every figure on it is computed from them:
- **A sentence's moment** is the start of the first sentence of the first atom it cites; a section's, its first sentence's. No model gives a time (ADR-0006).
- **What was kept** is Ledger A and Ledger B checked again, as the stages check them ([fidelity.md](fidelity.md)), the plan's duplicates excused.
- **What was left out** is the ledger's fillers, each with its label and its words, and the plan's duplicates, each with the atom that tells it and why.
- **What the stages could not do** is each stage's findings, as it published them (ADR-0005: never hidden).

**What a job made** is read back from its library, a stage at a time. The library keeps a job's last run of each stage ([pipeline.md](pipeline.md)), so a stage that a resume found done, or ran again after a change, shows what it made last; a run that failed made nothing, even where an earlier run had done the stage. The article is the seams stage's where it ran, since it is the write stage's with the paragraphs where sections meet smoothed; until then, the write stage's is the article so far. A job that has not finished renders what it has and says what is missing: a page of a transcript alone is a page too. An eval annotation has no article, and no page: `rreport eval sheet` shows what it drafted ([eval.md](eval.md)).

## The page

| Part | What |
|---|---|
| The video | its title, channel, length and link; the job and its tier; the player |
| Contents | each section's heading and start |
| The article | each section under its heading and its start; each sentence starts the player at its moment when clicked; its markers as small links to its atoms, shown or hidden by a switch |
| The Fidelity Report | what was kept, against M1's targets (every sentence accounted for, 97% of atoms told or more, no invalid marker, every word to quote kept, 99% of numbers or more); what is not told, and why; what the stages could not do |
| The ledger | every atom: its time, its kind and labels, its words, who said it (folded) |
| The transcript | every sentence: its time, its speaker, its words (folded) |

The page is one file: its styles and its one short script are inline, and it fetches nothing but the video's player, from `youtube-nocookie.com`. A caption file's job has no player: its moments link to the transcript's rows instead. It reads in a dark theme where the reader's system asks for one.

## Safety

Everything on the page came from a video someone else made, so nothing of it is trusted as HTML:
- every text is escaped (`& < > " '`), in elements and in attributes alike;
- the article is rendered from its parse (`rr/knowledge/article.hpp`): headings, paragraphs and sentences, each as text, never passed through;
- the only links built from data are the video's, from an id of YouTube's form (eleven of `A–Z a–z 0–9 _ -`): any other id makes no link at all.

## The command

`rreport report JOB [--out FILE]` writes the page to `FILE`, by default `reports/JOB.html` beside the library's database, and prints where. It needs no pod, and reads only the library. It exits 0 when the page is written, 1 for a job it does not know, an artifact it cannot read or a page it cannot write, and 2 for an eval annotation.

## How the tests were validated

| Suite | What it holds |
|---|---|
| `tests/unit/report/page_test.cpp` (15) | The synthetic lecture's job, done (`tests/unit/pipeline/lecture.hpp`: its artifacts as the oracle makes them), each part of its page checked by its exact HTML: a sentence's moment, its first atom's first sentence, and of a sentence of several atoms, the first's; each heading's start, in the contents too; the video named, linked and played; the Fidelity Report's figures computed again, an atom taken out of the article counted at once; what is not told, with why; the findings, a stage that did all it was asked not listed; the ledger's labels other than the usual; a caption file's moments linking to the transcript's rows; a job at each point short of done; nothing from the video taken as HTML, in a title, a channel, a sentence, a speaker and an id |
| `tests/unit/report/contents_test.cpp` (7) | A job read back from a real library: every stage done, with its findings in order; the seams stage's article over the write stage's; a stage run again shown as it ran last; a failed run's nothing, even where an earlier run had done its stage; a job that ran nothing; an eval annotation refused; an artifact that does not read as its stage's |
| `tests/component/hub_cli_test.cpp` | `rreport report` on a job stopped after its transcript: the page beside the library, its title, transcript and findings; the same page wherever it is written; an unknown job (1); an eval annotation (2) |

They pass under ASan, UBSan and TSan. The page was also read as a reader reads it: the lecture's page rendered in Chromium at a phone's width (390 px), in the light and the dark theme, with every part unfolded. Nothing scrolls sideways, and the Fidelity Report's tables fit.

**The mutants** (`tests/mutants/report.toml`, 21) each remove one rule of the page or of reading a job back, and name the test that must fail without it.

1. **The first run killed 12 of 15.**
   - Two mutants did not compile: each left a parameter unused, which `-Werror` refuses. They were written again to keep it used: a caption file's moment linking to `#`, and the usual labels said.
   - One survived: a stage that did all it was asked was listed among what the stages could not do, under its name, with nothing below. The test gave the page findings only from stages that had some, while the command gives it every stage done, most with none. It now gives a stage with none, and checks it is not listed.
2. **Reading a job back was in the command,** where no unit test reached it: a mutant that showed the write stage's article over the seams stage's would have survived. It moved into `rr_report` (`contents_of()`), with its own tests on a real library.
   - Moving it showed that the library keeps a job's last run of each stage, one row a stage. The command had kept the last run of each stage itself, which the library had already done; that code is gone.
3. **The second run killed all 21:** the 15 of the page and 6 of reading a job back.
4. **clang-tidy found `contents_of()` too complex to read at once**, each artifact read a branch of it. Placing an artifact is a function of its own now (`put()`), and the 6 mutants of reading a job back, two of them moved with the code, were run again: all killed.
