# Captions, into the transcript

The transcript is the closed world everything in rReport answers to: sentences S1…Sn, each with when it was said and, when known, by whom (ADR-0005, [knowledge.md](knowledge.md)). This phase builds the road to it from a video's captions, in two modules:

| Module | What it does |
|---|---|
| `rr_media` | Where captions come from: yt-dlp's description of a video, the captions chosen in the language spoken, yt-dlp run safely |
| `rr_transcript` | Captions of four formats read into timed words; a model's punctuation laid onto automatic captions without changing a word; the words cut into numbered sentences |

Everything here is deterministic code. The model that punctuates, and the stage that strings the steps together, come with the pipeline (P15).

## Where captions come from

### The video

`yt-dlp -J` describes the video behind an address: its title and description, which the outline reads; its chapters, which cut the talk into sections (plan §5, stage 8); its licence, which the eval records; and the captions on offer. Its JSON is large and loose. Fields come and go, and are null when unknown. `video_from_ytdlp()` reads it strictly where a mistake would matter and leniently elsewhere:
- An address must be one video with an ID. A playlist is refused, with what to give instead.
- A chapter that ends before it starts is left out, and so is a date that is no date. Neither stops the job.

### Which captions

The transcript is what was said, so captions are chosen in the language spoken, and never a translation. `choose_captions()` does it in four steps:
1. **The spoken language** is the one YouTube gives the video. Failing that, it is the one YouTube's speech recognition heard: yt-dlp names that track `<language>-orig`, which tells it from the recognition's translations.
2. **The creator's captions** in that language come first, the very language before a regional variant (`en-GB` for `en`).
3. **Else the recognition's own track** (`en-orig`) is used.
4. **When the language is never told,** the creator's captions are used if they offer one track only, which can only be theirs.

| On offer | Spoken | Chosen |
|---|---|---|
| `en` (the creator's), `en-orig` | `en` | `en`, the creator's |
| `en`, `en-GB` (the creator's) | `en-GB` | `en-GB`: the very language first |
| `fr` (the creator's), `en-orig`, `fr` (automatic) | `en` | `en-orig`: the creator's French is a translation |
| `en`, `de` (the creator's), `de-orig` | not told | `de`: the recognition heard German |
| `en` (the creator's), `en` (automatic) | `es` | none: all of it is translation |
| `en` (the creator's) | not told | `en`: one track, which can only be theirs |

Of a track's formats, json3 is read first, then WebVTT, then SubRip. A track rReport cannot read is passed by. When no track is what was said, the result is `Errc::not_found`, naming what is on offer: the video needs speech recognition, which arrives in M4. A live or upcoming stream has no captions yet, and is `Errc::unavailable`, to try again later.

### Running yt-dlp

YouTube changes often and resists downloaders. yt-dlp keeps up with it, so rReport runs yt-dlp rather than imitating it. The user installs and updates it; it is never bundled (plan §4), and `rreport doctor` checks for it and for the Deno runtime it needs. `YtDlp` is the only code that runs it:
- **Through `rr_proc`.** There is no shell, the run has a timeout, and it can be stopped. The address always comes last, after `--`, and only web addresses are accepted, so an address can never be read as an option (`--exec …`).
- **With the user's configuration.** A proxy or a rate limit set in yt-dlp's own configuration is respected. rReport's flags override what it relies on: the output template, the paths, the subtitle format.
- **Reading what it writes.** yt-dlp names the caption file from the template, `captions.<language>.<format>`. One left by an earlier try is removed first, so a run that silently writes nothing is never mistaken for success.

Its failures are told in rReport's terms, so the pipeline knows whether to retry, stop, or ask:

| yt-dlp says | Error | What follows |
|---|---|---|
| "Sign in to confirm you're not a bot", or "your age"; members-only | `needs_user` | the user acts: `rreport doctor` explains, and cookies are opt-in |
| "Private video", "Video unavailable", "removed", "does not exist" | `not_found` | the job stops |
| "HTTP Error 429", "Too Many Requests" | `rate_limited` | retried with backoff |
| "HTTP Error 5xx" | `unavailable` | retried |
| "Unable to download", "urlopen error", "timed out", "connection" | `network` | retried |
| "Unsupported URL" | `invalid_argument` | the address is wrong |
| anything else | `unavailable` | retried, then reported |
| (yt-dlp is not installed) | `needs_user` | `python3 -m pip install -U yt-dlp` |

A message quotes yt-dlp's last `ERROR:` line, not its warnings.

## Reading captions

### The formats

| Format | From | Times |
|---|---|---|
| json3 | YouTube, through yt-dlp | in automatic captions, every word; in a creator's, each line |
| WebVTT | a file | each cue; in YouTube's automatic captions, every word |
| SubRip | a file | each cue |
| Plain text | a file | none: every time is 0 |

A cue whose timing cannot be read is an error naming its line, and so is a time beyond 11.5 days, which no video lasts. Words at the wrong seconds would send the article's links to the wrong place, so a doubtful file is refused, not guessed at.

### Words, and when each was said

Each format is cut into cues, and one reader turns every cue's text into words, whatever the format:
- **Markup** is removed (`<i>`, `<c.yellow>`, `{\an8}`), character references are decoded (`&amp;` is `&`), text is kept in NFC, and invalid bytes become U+FFFD.
- **A word** is what lies between blanks. A blank is a space, or what `rr_text` never reads as text: a control, a format mark, an unassigned code point. So a NUL byte in a caption file is no part of a word. The exception is a zero-width joiner or non-joiner between two characters, which stays: it is part of a Persian word or of an emoji.
- **Times.** A cue's time is shared among its words by their length, unless the format times its words: json3's segments, and the inline timestamps of YouTube's WebVTT. Starts never decrease. A word ends no later than the next one starts, since YouTube's automatic events overlap.
- **Rolling captions.** YouTube's automatic WebVTT shows each line again in the next cue, and a 10 ms cue holds it between. In a file with inline timestamps, a line without one that repeats the line before is read once.

### Who spoke

- A WebVTT voice, `<v Ada>`, names the speaker of the text it wraps.
- A captioner's label at the start of a line, in capitals, names the speaker until the next turn: `ADA LOVELACE: Numbers are poetry.` It needs two capitals at least, so `I: think so` is speech.
- `>>` and a dialogue dash (`- How are you?`) mark a new speaker without naming them.

Speakers are never guessed. A new speaker starts a paragraph, and a sentence never runs across one.

### What is set apart

Sound descriptions are not speech: `[Music]`, `(laughs)`, the notes around lyrics, whose words are kept. They are kept apart, as sounds with their times, never mixed into what was said. A word the captioner missed, `[inaudible]` or `[unclear]`, stays as the word `[unclear]`: a gap in what was heard is not hidden.

## Punctuation by projection

Automatic captions often come without punctuation, in lowercase. Sentences cannot be found in that, and names cannot be told from other words, since `rr_text` finds names by their capitals. A model punctuates well, but a model also rewrites: it smooths grammar, drops a repetition, turns "thirty seven" into "37". A transcript is what was said, so the model's text is never taken. `project()` lays its punctuation and capitals onto the captions' words instead:

| | |
|---|---|
| The captions | `so marie curie won two nobel prizes she called radium the most beautiful element` |
| The model | `So Marie Curie won two Nobel Prizes. She called radium “the most beautiful element.”` |
| Projected | `So Marie Curie won two Nobel Prizes. She called radium “the most beautiful element.”` |
| The captions | `revenue grew thirty seven percent in twenty twenty three` |
| The model | `Revenue grew 37% in 2023.` |
| Projected | `Revenue grew thirty seven percent in twenty twenty three.` |

**What changes.** Every character of the captions stays, in order, and two things change:
- **Capitals.** A word the model wrote in other capitals takes them, and only them: "resume" stays "resume" where the model wrote "Résumé". Accents are not capitals (`to_lower()`, [text.md](text.md)).
- **Punctuation.** The model's punctuation is added around the captions' words. What it wrote after a word (`.`, `,`, `?`, `”`, `)`) goes after it, and what it wrote before one (`“`, `(`) goes before. A word the captions punctuate already keeps its own: `u.s.` stays `u.s.`, however the model wrote it.

**What may be added.** Only punctuation that changes no word and no number is added: the sentence ends of every script (`. ! ? …`, `。`, `؟`, `।`), commas, semicolons, colons, dashes, quotes and brackets. Never a hyphen, which joins words, nor a sign or a percent, which change numbers, nor any symbol.

**How words are matched.** The terms of the captions and of the model's text ([text.md](text.md)) are aligned with the fewest changes. A changed, added or dropped word costs one, and equal folded words cost nothing. Ties go along the diagonal, so a word the model changed stays paired with the word it replaced, and its punctuation lands where the model put it. Words the model added are not taken, but a sentence end it wrote after them still ends the sentence, after the caption word before them. Words it dropped are kept, unpunctuated. The alignment's time and memory grow with the product of the two lengths, so a projection takes at most 2,048 words a side. The transcript stage punctuates windows of a few hundred.

**The guarantee.** A projection only adds punctuation and changes capitals, so the folded words of each projected word are the caption word's. The projection checks this before it returns, which is the plan's "code checks that no word changed" (§3.2). The fuzzer also projects every input it makes onto its own words.

## Sentences

`transcript_of()` joins the words, a space between two and a blank line before a paragraph, and `rr_text` cuts the sentences ([text.md](text.md#sentences)):
- **When.** A sentence is said from its first word's start to its last word's end. One that starts or ends inside a word takes that word's time in proportion. Chinese and Japanese need no spaces, so one caption word may hold two sentences.
- **Length.** A sentence of more than 60 words is cut where its words pause longest, until no part is longer. Ties go to the cut nearest the middle. Unpunctuated captions give such sentences. The cuts keep them in bounds until a model punctuates them, so atomize's windows of 20 to 30 sentences stay a sane size.
- **Who.** A sentence's speaker is its words'.

`punctuated()` tells which captions need projecting: they end a sentence less than once every 40 words.

## What waits

- **The transcript stage (P15).** It holds the model mission that punctuates, a window at a time, and the stage that reads captions, projects them and stores the transcript. `rreport ingest` arrives with it.
- **Local caption files as a source** come with the stage too: their formats are read here.
- **Speech recognition and diarization** arrive in M4.

## How the tests were validated

58 unit tests state the rules. 37 are for `rr_transcript` (captions, projection, sentences), and 21 are for `rr_media`, whose yt-dlp is a shell script each test writes: what it says, what it writes, how it fails. The script records the arguments it was given, so the tests see exactly what rReport asks of yt-dlp. All pass under ASan and UBSan. Then:

1. **Fuzzing.** The caption target (`tests/fuzz/captions_fuzz.cpp`) reads any bytes in each format, cuts the words into sentences, and projects the input onto its own words; its header lists the invariants. It broke two of them within seconds:
   - **A NUL byte kept inside a word.** The reader split words at spaces only. `rr_text` also treats controls and format marks as blank, so the sentences trimmed what the words kept, and the transcript lost characters relative to its words. Words now split at `rr_text`'s blanks, keeping a zero-width joiner between two characters.
   - **`rr_json`'s parser threw an exception** on a number too large for a double (`155e24854602823`), though its contract says it never throws. It caught nlohmann's `parse_error`, and an overflow is an `out_of_range`. Every JSON rReport reads, model answers included, could have crashed the hub. It now catches every nlohmann exception.

   Both inputs joined the corpus as `regression-*.bin`, each with a named unit test. Then 415,076 inputs broke nothing, and 353,525 more after the refactors that clang-tidy asked for.
2. **Writing the mutants** (`tests/mutants/transcript.toml`, 53; `tests/mutants/media.toml`, 24) found rules that no test checked before any mutant ran:
   - a byte order mark;
   - milliseconds written in other than three digits;
   - a word starting before the one before it;
   - a bracket closing on another line;
   - text after a voice in its cue;
   - a bracket written against the word before (`left(quietly)`);
   - a sentence end carried onto a word that ends one already;
   - a caption word's own brackets;
   - a caption file left by an earlier try;
   - the message quoting yt-dlp's last error;
   - the very language before its regional variant.

   Each got its test. One rule was redundant: a 503 matched both "service unavailable" and "http error 5", so the narrower match went.
3. **The first run killed 52 of 53 transcript mutants,** each by the test that claims it. The survivor removed the 11.5-day limit on timings. The test meant to guard it never reached the limit, because a length guard refused its 21-character timing first. That guard existed only because `rr_core`'s `parse_timestamp` could overflow on an absurd number of hours, which is undefined behaviour for every caller. `parse_timestamp` now checks its arithmetic, tested at the exact bound: `2562047788015:12:54` holds, and a second more does not. The guard is gone, and a 300-hour timing tests the limit.

The final runs kill every mutant, each by the test that claims it: 53 of 53 for `rr_transcript` and 24 of 24 for `rr_media`. The same holds for the specs this phase started or extended: `core.toml` (2), `json.toml` (1), and two in `text.toml` for `append_utf8()` and `to_lower()`. Two of those first failed to build, because removing a rule left a variable unused. They became boundary mutants instead: a bound a second wider, and U+D800 itself left uncounted as a surrogate.
