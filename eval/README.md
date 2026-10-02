# The evaluation set

This directory is the data behind `rreport eval`. ADR-0019 says how it is measured and when a change may merge. It holds only videos licensed CC BY or in the US public domain.

| Path | What | Status |
|---|---|---|
| `sets/candidates.toml` | Proposed videos, four types: talk, conversation, tutorial, narrative | **Waiting for the owner's approval** |
| `sets/<set>.toml` | Approved videos: licence and attribution, type, caption kind, split (`dev` or `test`), the pinned snapshot | After approval |
| `data/<id>/` | Each video's source (captions and description) as `rreport eval fetch` read it once, pinned by hash, so a change on YouTube changes nothing here | `rreport eval fetch`, after approval |
| `gold/<id>.toml` | Facts anchored to the timeline (start, end, quote), must-quotes, terms. Models draft silver (`rreport eval annotate`); a person corrects it into gold with its review sheet (`rreport eval sheet`) | Runbook R2 |
| `labels/` | A person's verdicts on items the judges read, drawn by `rreport eval label`: they calibrate the judges (`rreport eval calibrate`) | Runbook R5 |
| `judges/` | The frozen judge panels (`eval-v1.toml`) | Runbook R5 |
| `gates/` | Each gated metric and the regression it tolerates | Plan P25 |
| `sweeps/` | Variants of the pipeline run against a baseline, each with why (`r6.toml`: what each step of `max` buys) | Runbook R6 |

## Approving the candidates

1. **Check each candidate you want.** Nothing in `candidates.toml` was checked on YouTube itself: the environment that wrote it could not reach the site. On each video's page, check that the uploader is the agency's own channel and that the duration matches. Then list its caption tracks:
   ```sh
   yt-dlp --skip-download --list-subs "https://www.youtube.com/watch?v=<id>"
   ```
   Manual tracks appear under "Available subtitles", machine ones under "Available automatic captions".
2. **Choose.** Keep the `pick = true` videos, or swap any of them. Aim for at least three per type, mostly under an hour.
3. **Choose M1's three acceptance videos:**
   - a talk of about 20 minutes with manual captions;
   - a lecture of about 60 minutes with automatic captions only;
   - a podcast or interview of about 90 minutes.

   The candidates marked `m1` are proposals. No good 20-minute talk was found yet.
4. **Say which,** and they move to `sets/m2.toml` with a dev/test split. Runbook R2 ([docs/runbooks/m1-m2.md](../docs/runbooks/m1-m2.md#r2-the-evaluation-videos-and-their-gold-annotations)) takes it from there: snapshots, drafts, gold. [docs/design/eval.md](../docs/design/eval.md) says how each file is read and checked.
