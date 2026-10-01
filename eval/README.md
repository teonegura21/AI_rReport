# The evaluation set

This directory is the data behind `rreport eval`. ADR-0019 says how it is measured and when a change may merge. It holds only videos licensed CC BY or in the US public domain.

| Path | What | Status |
|---|---|---|
| `sets/candidates.toml` | Proposed videos, four types: talk, conversation, tutorial, narrative | **Waiting for the owner's approval** |
| `sets/<set>.toml` | Approved videos: licence and attribution, type, caption kind, split (`dev` or `test`) | After approval |
| `data/<id>/` | Snapshots of each video's captions and metadata, pinned by hash, so a change on YouTube changes nothing here | `rreport eval fetch` (plan P16) |
| `gold/<id>.toml` | Facts anchored to the timeline `(t0_ms, t1_ms, quote)`, must-quotes, terms. The commander drafts silver; a person corrects it into gold | Runbook R2 |
| `labels/` | 30–50 human verdicts and pairwise preferences that calibrate the judges | Runbook R5 |
| `judges/` | The frozen judge panels (`eval-v1.toml`) | Runbook R5 |
| `gates/` | Each gated metric and the regression it tolerates | Plan P25 |

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
4. **Say which,** and they move to `sets/m2.toml` with a dev/test split.
