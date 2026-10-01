# Text: made comparable

rReport promises an article faithful to its video: every sentence accounted for, every number right, every quote verbatim, no name lost. Each promise is checked, not hoped for, and each check compares texts: the transcript with an atom, an atom with the paragraph that cites it, a quote with what was said. `rr_text` is what those comparisons stand on. It has pure functions only, with no I/O, no model and no threads, so each rule is exact, unit-tested, mutation-tested and fuzzed.

## Pieces

| Where | What it is |
|---|---|
| `rr/text/span.hpp` | Where something is: byte offsets into the text it was found in |
| `rr/text/unicode.hpp` | Valid UTF-8, repair; NFC, the form a text is kept in; `fold()`, the form texts are compared in; code points and their classes |
| `rr/text/tokens.hpp` | Words, numbers, punctuation and symbols, with their places; terms, the folded words and numbers texts are compared by |
| `rr/text/sentences.hpp` | A text cut into sentences, the unit the transcript is accounted in (`S1`, `S2`, …) |
| `rr/text/numbers.hpp` | The numbers a text states, in digits or words, by value |
| `rr/text/entities.hpp` | The names a text gives; where it mentions a term |
| `rr/text/align.hpp` | Where a quote is in a text, and how faithfully (Smith–Waterman) |
| `rr/text/bm25.hpp` | Which passages a query is most about (Okapi BM25) |
| `src/text/lexicon.*` | The words the rules must know by name: abbreviations, function words, name connectors (private) |
| `tests/fuzz/text_fuzz.cpp` | The invariants every input keeps, for libFuzzer |

Built on [utf8proc](https://github.com/JuliaStrings/utf8proc) (ADR-0012): the system's package when there is one (`libutf8proc-dev`), else its pinned tag, built in-tree.

## Everything has a place

Every finding is reported with a `Span`, the bytes `[begin, end)` of the text it was found in, and spans always fall on code point boundaries. So a check that flags a number can highlight it, the report can link a quote to the transcript words it matches, and two findings can be ordered and compared without copying text. Tests state spans as the text they cover (`covered()`), so an expectation reads as what it means.

## Unicode: one text, one form

A text can be written many ways that a reader cannot tell apart, and comparing bytes would call them different:

| Written as | And as | Made equal by |
|---|---|---|
| `é`, one code point | `e` + a combining accent | NFC (`nfc()`), and `fold()` |
| `ﬁ`, a ligature | `fi` | `fold()` (compatibility decomposition, NFKC) |
| `Straße` | `STRASSE` | `fold()` (case folding) |
| `café` | `cafe`: captions often lose accents | `fold()` (accents removed) |
| `don’t` | `don't` | `fold()` (every apostrophe made straight) |
| `３４`, `٣٤` | `34` | numbers are read by value (`digit_value()`) |

So there are two forms. `nfc()` is the canonical form a text is *kept* in: the same characters, the same bytes. `fold()` is the form texts are *compared* in, and never shown. Both refuse invalid UTF-8 (`Errc::invalid_argument`). A file in another encoding is an error to report, not a text to guess at. `repair()` replaces each invalid byte with U+FFFD for text that must be read anyway, and everything else reads an invalid byte as U+FFFD, one byte long, so no function fails on bad input.

## Tokens and terms

A **word** is a run of letters, marks and digits with at least one letter, in any script: `GPT4`, `3D` and `1st` are words. An apostrophe between letters stays in the word (`don't`, `l’été`), and a hyphen splits (`state-of-the-art` is four words). A **number** is a run of digits; a point between digits is a decimal point (`3.14`), and a comma followed by exactly three digits separates thousands (`1,234,567`, but `1,2,3` is three numbers). Punctuation and symbols are one code point each, and spaces and controls separate tokens.

**Terms** are the words and numbers, folded: what texts are compared by. A word of combining marks alone folds to nothing (fold removes accents) and is no term. Fuzzing found that one: an empty term would have matched every other.

## Sentences

The transcript is accounted for sentence by sentence (ADR-0005), so where a sentence ends decides what `S1`, `S2`, … are. A sentence ends after a terminator (`.` `!` `?` `…`, and `‽`, `؟`, `।`, …) and the closing quotes and brackets after it, when a space or the end of the text follows. Ideographic terminators (`。` `！` `？`) need no space.

Not every terminator ends one:

| Rule | Ends | Does not end |
|---|---|---|
| After `!` or `?`, a lowercase word continues | `It worked! Then…` | `"Why?" she asked.` |
| An ellipsis ends one only before a capital | `Later… Nobody knew.` | `and then... we left` |
| An abbreviation that never ends one | | `Dr. Smith`, `e.g. this`, `Roe v. Wade` |
| One that often does (`etc.`, `Inc.`, `U.S.`), and an initial, end one only before a function word | `Apple Inc. It grew`, `plan B. Then` | `U.S. Army`, `John F. Kennedy`, `J. K. Rowling` |
| A numbering abbreviation before a number | | `No. 5`, `p. 12`, `Fig. 3` |
| A period before a lowercase word | | `a 2 lb. bag` |
| A blank line, a paragraph break | always | |

**Why abbreviations and initials lean towards not ending a sentence.** The two errors are not equal. Merging two sentences makes one `S#` coarser, but it is still accounted for. Splitting "the U.S. | Army officials said" cuts a name in two and leaves a sentence that starts mid-phrase, which atomization then reads wrong. So a period that may belong to the words around it ends a sentence only when the next word is a capitalized *function word* ("It", "The", "Then", from a list in `lexicon.cpp`), which almost always starts a sentence.

**Nothing is lost.** Every code point that is not a space or a control lies in exactly one sentence. A fragment with no word or number (`. . .`, `♪♪`) joins the sentence before it, or the one after it at the start of the text. A text of fragments only is one sentence. The fuzzer checks this on every input.

## Numbers, by value

"Every number right" means by value: the speaker says "thirty-seven percent", the article writes "37%", and the check must see one number. `numbers()` reads:

| Form | Examples | Value |
|---|---|---|
| Digits, any script | `37`, `1,234,567`, `3.14`, `.5`, `٣٤` | 37, 1234567, 3.14, 0.5, 34 |
| Words | `thirty-seven`, `one hundred and five`, `twelve hundred`, `two thousand and eight`, `a dozen` | 37, 105, 1200, 2008, 12 |
| Decimals in words | `three point one four`, `two and a half` | 3.14 (the same double as `3.14`), 2.5 |
| Scales | `1.5 million`, `two billion`, `a million and a half` | 1.5e6, 2e9, 1.5e6 |
| Years in pairs, 1500s to 2000s | `nineteen eighty-four`, `twenty twenty`, `nineteen oh five` | 1984, 2020, 1905 |
| Ordinals | `21st`, `third`, `twenty-first`, `hundredth` | marked `ordinal` |
| Signs | `-5`, `−5`, `minus five`, `negative five` | −5 |
| Percent | `%`, `percent`, `per cent` | marked `percent`; the value is the count |

Numbers are written by grammar (`magnitude := hundreds { scale [and] [hundreds] }`, scales strictly decreasing), read greedily with the backtracking that English needs: "one hundred and two hundred" is 100 and 200, and "five thousand three million" is 5,000 and 3,000,000. **A range shares its unit**: in `5-10%`, `5 to 10 percent` and `between 5 and 10 million`, both ends are percentages, or millions. Without this, an atom saying "5–10%" and an article saying "5% to 10%" would disagree on the first number. A scale is shared only while the first end is at most ten times the second's count, so "from 500 to 1 million" starts at 500, while "from 20 to 10 million" (a fall) starts at 20 million.

What is **not** read, and why:
- **Fractions**, skipped whole: "one third", "two thirds", "a third of", "a thousandth of". Reading "one third" as 1 and 3rd would put two numbers into a check that the text never stated.
- **"One" as a pronoun** ("no one", "the one", "each one", "one another") and **"second" as time** ("a second", "per second"). These are judged by the word before or after; "one of them" and "the second time" are read.
- **"Ten thirty"** is a time, and stays 10 and 30. Years are read in pairs only from fifteen to twenty, so clock times (10, 11, 12) are never years.
- **Vague amounts** ("dozens", "millions of"), **multiples** ("twice", "10x"), and **digits glued to letters** ("3D", "GPT4", "5k"), which name things rather than count them.
- A quantity a `double` cannot hold is not read at all, rather than read as a smaller one.

**Comparing.** `same_value()` allows a relative error of 1e-9, which forgives the rounding of decimals and nothing else. `same_number()` also requires both to be percentages or neither, and ordinals or neither: 37% is not 37 people, and the 37th is not 37.

## Names and mentions

`names()` finds runs of capitalized words: "Marie Curie", "NASA", "iPhone". A run may hold one or two lowercase connectors between capitalized words ("Bank of America", "Pirates of the Caribbean"), `&` and joining hyphens ("AT&T", "Jean-Paul Sartre", "COVID-19"), a number after a capitalized word ("Apollo 11"), and the period of a title or an initial ("Dr. Smith", "John F. Kennedy"). Function words at its head are left out ("The Beatles" gives "Beatles"), unless written in capitals ("US", "IT"). A possessive ends a name and is left out. "I" is no name. A sentence written all in capitals gives none, since case says nothing there.

**A single capitalized word that starts a sentence or a quote is not a name**, because grammar capitalizes it anyway: "Paris is lovely" gives no name, while "She saw Paris" does. This is the main limit of finding names by case. Acronyms and inner capitals ("NASA agreed", "iPhones sold") are kept. Checks that need every name should ask the model for the names (atoms carry them), then use `mentions()`.

`mentions(text, term)` finds where a term's words appear in a row, compared folded: "marie curie" mentions "Marie Curie", "Curie's" mentions "Curie", and "state of the art" mentions "state-of-the-art".

## Quotes: alignment

A quotation must be what was said. A writer model softens ("bad" for "terrible"), trims, or blends two sentences into one quote. A string search would only say "not found". `align(quote, text)` is a local alignment (Smith and Waterman, 1981) of the quote's terms against the text's: a match scores +2, a mismatch −1, a term on one side only −1 (`AlignmentScores`). It returns:
- `verbatim()`: every quote term matched, in order, with nothing between. This is the must-quote check (P13).
- `identity()`: the share of quote terms matched.
- `matched`, `edits` and the two spans: the aligned stretch of the quote, and where it lies in the text. The repair message can then quote the transcript's own words.

Where several places align equally well, the first in the text wins. The cost is the product of the lengths: a 50-word quote against a 90-minute transcript (about 15,000 words) takes about 4 ms (measured, `-O2`). Callers aligning many quotes compute the transcript's terms once and use the `Term` overload. Two rows of the matrix are kept, each cell carrying where its best path began and what it matched, so no traceback matrix is stored.

## Retrieval: BM25

A judge asked whether a fact is in an article should not read the whole article for each fact: long contexts lose what is in the middle (Liu et al., 2023), and each call costs time. So the passages most likely to hold it are retrieved first. `Bm25` is Okapi BM25 over folded terms (k1 = 1.2, b = 0.75), with Lucene's idf, ln(1 + (N − n + 0.5)/(n + 0.5)), which is never negative, so a common word adds little instead of subtracting. No stop words are removed: idf already weighs them down, and in a transcript a "not" matters. Each distinct query term counts once. Sums are made in a fixed order, so equal inputs give equal scores to the last bit, and ties go to the earlier passage. The eval's key-fact recall (P24) uses it, and so does the search for where an atom's content went.

## Fuzzing

`rr_text` reads whatever arrives: captions in an unknown encoding, a model's broken output. Its invariants must hold for every input, not only those a test thought of. `tests/fuzz/text_fuzz.cpp` checks them all:
- `repair()` gives valid UTF-8 and changes exactly the invalid texts;
- `nfc()` and `fold()` refuse only invalid text;
- tokens, sentences, numbers and names are non-empty, ordered, apart, and on code point boundaries;
- tokens and sentences cover every code point but spaces and controls, and sentences are trimmed;
- numbers are finite, and no ordinal is a percentage;
- a stretch of a text aligns with it verbatim, scoring every match;
- BM25 finds a passage by its own words and ranks best first.

The target is built twice (`cmake/RrFuzzing.cmake`):
- **Replay, on every push.** `rr_text_fuzz_replay` runs the committed corpus (`tests/fuzz/corpus/text/`) under CTest, as `fuzz.text`, in every build-test job: GCC, Clang with ASan and UBSan, and TSan.
- **Search, Clang only.** With `RR_FUZZ=ON` (preset `fuzz`), `rr_text_fuzzer` is linked with libFuzzer and searches by coverage. CI runs it for two minutes per push (job `fuzz`) and uploads any input that breaks an invariant.

An input that broke an invariant joins the corpus once fixed (`regression-*.bin`), so every later push replays it. To fuzz longer:

```bash
cmake --preset fuzz && cmake --build --preset fuzz --target rr_text_fuzzer
mkdir -p /tmp/new && build/fuzz/tests/fuzz/rr_text_fuzzer -max_total_time=600 /tmp/new tests/fuzz/corpus/text
```

## Limits, on purpose

- **The lists are English** (abbreviations, function words, number words). Other languages fall back to the general rules: sentences still end at terminators, digits are read in any script, and names follow case where the script has it.
- **Decimal commas** (`1.234,56`) read as two numbers, 1.234 and 56. The transcript's language decides the convention; until speech lands (M4), captions are taken as English.
- **Year pairs** are read for the 1500s to the 2000s only, so clock times are never years.
- **Names by case** miss a lone capitalized word at a sentence's start ("Paris is lovely"), and may take a date with a count ("On Monday 5 people came" gives "Monday 5"). The pipeline therefore asks the models for names and uses `mentions()`.

## How the tests were validated

113 unit tests state each rule with exact expectations, and the fuzzer checks the invariants on any input. Each step below found something the one before had missed.

1. **A scratch driver** printed what the rules made of about 40 tricky inputs, before any test was written. Four results changed the design: "U.S. Army" was being split, a possessive did not end a name, "hundredth" was not read, and fractions were read as two numbers.
2. **Tracing each expectation by hand** caught four expectations of mine that were wrong, so the tests state what the rules mean rather than what the code happened to do.
3. **Fuzzing** found the empty term within its first second. Then it ran 151,424 inputs without a finding.
4. **Writing the mutants** (`tests/mutants/text.toml`) showed nine rules no test checked. Each got its test:
   - a comma before two digits mid-text;
   - a word of marks alone being a word;
   - an ellipsis before an opening quote;
   - "a thousand thousand";
   - an ordinal never taking "%";
   - a spaced hyphen not joining "thirty - seven";
   - a number after a number in a name;
   - a hyphen touching one side only ("Rome -Milan");
   - a longer passage listed first. Listed second, the tie-break hid the length rule.
5. **Running them**, 106 of 111 died at once. The 5 survivors were:
   - two equivalent mutants, which exposed redundant code, now gone. The number reader re-checked a scale's order that the group before it already guaranteed. The rule for a name at a clause's start checked acronyms that its inner-capital rule already kept;
   - three untested rules:
     - "I" inside a run ("Tom I think left");
     - a quote or colon mid-sentence starting a clause;
     - local alignment's restart. With the gap's cost ignored, a cell could carry a *negative* score into a later match, and "a b c" against "a x b c" scored 3 instead of 4.

The final run kills all 110 mutants:

| Area | Mutants | The rules they remove |
|---|---|---|
| Unicode | 12 | case folding, accent and ligature removal, apostrophes; NFC; repair; whitespace controls; titlecase; opening and closing quotes; the digits of any script |
| Tokens | 8 | thousands commas (exactly three digits); apostrophes between letters only; decimal points after a letter; marks starting words; empty terms; spaces as tokens |
| Sentences | 21 | each row of the table above; paragraph breaks (CRLF, U+2029); closing and opening quotes; fragments joining before, after, or standing alone; dotted abbreviations |
| Numbers | 37 | years and their centuries; scales and the groups between them; "and" after hundred; dozen; "and a half", "point"; ranges sharing percent and scale, and only when comparable; signs; percent; ordinals and decades; fractions; "one" and "second"; the tolerance; overflow |
| Names and mentions | 18 | "I"; acronyms; capitals-only sentences; possessives; numbers in names; connectors, one or two; "&" and hyphens; titles and initials; function words at the head; clause starts (sentence, quote, colon); possessive mentions; overlaps |
| Alignment | 7 | verbatim (edits; empty quotes); matches counted; gaps counted; the first of equal places; where a path starts; what a gap costs |
| BM25 | 7 | idf; saturation; length; repeated query terms; ties; passages without a term; `top` |
