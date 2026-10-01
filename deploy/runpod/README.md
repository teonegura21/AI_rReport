# Running rReport's pod on RunPod: the M0 runbook

This runbook takes rReport from a green CI to a measured pod. It builds the image, sets up the volume, the pod and the hub, and then runs the M0 checks that only a real pod can settle. The last step, the benchmark, produces the table from which you choose the default GPU (plan §4.5).

Budget about two hours and a few dollars of pod time. Every command below runs on your own machine unless it says "on the pod".

## 0. What you need

- **A RunPod account** with an API key: Settings → API Keys, with read and write access to pods. The hub uses it to start, read and stop your pod.
- **The hub, built from the same commit as the image**, so `rreport doctor` finds one release on both sides:
  ```sh
  sudo scripts/install-deps-ubuntu.sh
  cmake --preset release && cmake --build --preset release
  export PATH="$PWD/build/release/apps/rreport:$PATH"   # rreport
  ```
- **Python 3.11+** and access to `huggingface.co`, for step 1.

## 1. Check the model checksums, and pin them

The catalog's checksums (`docker/runtime/catalog.toml`) were copied from a third party (LocalAI's gallery) and have not been checked against Hugging Face yet. A wrong one would make every pod download 17 GB, refuse it, and stop itself (ADR-0016). So check them first:

```sh
scripts/model-hashes.py check docker/runtime/catalog.toml
```

- **Every line `ok`:** pin each URL to the commit it names today, so a later re-upload cannot change what a pod downloads. Then review the diff and commit it:
  ```sh
  scripts/model-hashes.py pin docker/runtime/catalog.toml
  git diff docker/runtime/catalog.toml
  ```
- **A `MISMATCH`:** the file changed on the Hub since the gallery recorded it. `pin` takes the Hub's value, and that is the one to trust: it comes from the Hub's own LFS record for that exact commit.

Gated repositories need `HF_TOKEN` in the environment. None of the catalog's repositories is gated today.

## 2. Build the image

In GitHub: **Actions → runtime-image → Run workflow**, then pick the branch. The first run takes about an hour, because llama.cpp's CUDA build dominates. Later runs reuse that stage from the cache and take minutes.

The run pushes `ghcr.io/<owner>/rreport-runtime:sha-<commit>` and a tag named after the branch. Use the `sha-` tag, which names exactly one build.

The package is private by default. Either make it public (the package's settings on GitHub), or give RunPod a token that can read it: Settings → Container Registry Auth, user `<your GitHub user>`, password a token with `read:packages`.

## 3. The network volume

Models live on a volume, not in the image. A **network volume** is the right kind:
- it costs about $0.07 per GB per month, where a pod's own volume disk costs more, most of all while the pod is stopped;
- it outlives any one pod. When the machine a stopped pod lives on has no free GPU, you create a new pod on the same volume, with no new download.

A network volume belongs to one data center. Create it (Storage → New Network Volume) in a data center that has the GPU types you want to measure. Size it for the profiles you will run:

| Profiles | Files | Volume |
|---|---|---|
| `16gb` | ~6 GB | 20 GB |
| `24gb` + `24gb-infantry` | ~38 GB | 50 GB |
| `32gb` | ~28 GB | 40 GB |
| `48gb` | ~38 GB | 50 GB |
| `80gb` | ~48 GB | 60 GB |
| All of them | ~95 GB | 120 GB |

## 4. The pod

Create a template (Templates → New Template), then pods from it:

| Setting | Value |
|---|---|
| Container image | `ghcr.io/<owner>/rreport-runtime:sha-<commit>` |
| Container start command | leave empty: the image's entry point runs |
| Container disk | 20 GB |
| Volume mount path | `/workspace` (attach the network volume when deploying) |
| Expose HTTP ports | `8080` |
| Environment | `RR_RUNTIME_TOKEN` = `{{ RUNPOD_SECRET_rr_runtime_token }}` |

- **The token.** Create the secret first: Settings → Secrets, name `rr_runtime_token`, value from `openssl rand -hex 32`. The hub sends the same token (step 5).
- **Optional environment:**
  - `RR_RUNTIME_MODELS__PROFILE`: a profile by name (default `auto`, by the GPU's memory);
  - `RR_RUNTIME_IDLE__STOP_AFTER`: default `15m`;
  - `RR_RUNTIME_LOG__LEVEL`;
  - `HF_TOKEN`, for gated repositories.

Deploy a pod from the template, on the GPU type to measure, with the network volume. Note its ID: the part before `-8080` in its HTTP address.

What the pod does on its own:
1. The entry point checks the GPU and the token.
2. It downloads the profile's model files to `/workspace/models`. Downloads resume if interrupted, and each file is verified.
3. It serves.
4. It stops itself after 15 idle minutes (ADR-0015).
5. If anything fails, it stops itself at once, with the reason on its log (ADR-0016).

The first boot of a profile downloads 17–48 GB; later boots only check the files, in seconds.

## 5. The hub

`~/.config/rreport/config.toml`:
```toml
[runpod]
pod_id = "abc123xyz"   # yours
```

`~/.config/rreport/secrets.toml`, then `chmod 600 ~/.config/rreport/secrets.toml`:
```toml
[runtime]
token = "…"            # the pod's RR_RUNTIME_TOKEN

[runpod]
api_key = "…"          # your RunPod API key
```

Then:
```sh
rreport doctor
```

Every line should be `ok`. The capture tools (`yt-dlp`, `deno`, `ffmpeg`) may be `warn` until M1 needs them. A stopped pod gives `runtime: not checked: the pod is EXITED`: start it with `rreport pod start`.

## 6. The M0 checks

Tick each as you go. Where a check fails, the "If not" column says what it means.

| # | Check | How | Expected | If not |
|---|---|---|---|---|
| 1 | The pod boots, and reports its GPU | `rreport pod start`, then `rreport pod status` | `every engine ready`; then `profile <name>, <GPU> (<total> MiB, <used> used)` and each engine `ready` | The pod's log (RunPod console → the pod → Logs) gives the entry point's reason, just before `stopping pod` |
| 2 | Nothing without the token | `curl -i https://<pod>-8080.proxy.runpod.net/v1/health` | `401`, with `WWW-Authenticate: Bearer` | Any other answer is a bug: stop the pod, then report it |
| 3 | The models are verified | On the pod (its web terminal): `rreport-runtime models verify --full --set models.profile=<name>`, with the profile `pod status` names | `verified` for every file | `mismatch`: the file on the volume is not the catalog's. Delete it and restart the pod |
| 4 | An idle pod stops itself | Leave the pod alone for 15 minutes (or set `RR_RUNTIME_IDLE__STOP_AFTER=5m` on a test pod) | The pod shows `Exited` in the console; its log says `stopping the pod` | The log's error says which API refused which key (below) |
| 5 | A pod that cannot start stops itself | Deploy a test pod from the template with `RR_RUNTIME_TOKEN` removed | The pod stops within a minute; its log says `RR_RUNTIME_TOKEN is not set`, then `stopping pod` | It restarts in a loop: stop it by hand, and report it |
| 6 | Long, silent calls survive RunPod's proxy | On a `24gb-infantry` or larger pod: `rreport bench --parallel 32 --prompt-tokens 1000 --output-tokens 2048` | The table's `failed` column is 0 for every level | `failed` > 0 at the high levels, with `network` errors: the proxy cut queued calls, and ADR-0014's keep-alive needs a look |
| 7 | The benchmark table | Below | One table per GPU type | — |

**If check 4 fails: which key stops a pod.** RunPod does not document what the key it gives each pod may do, and reports disagree (ADR-0015). The runtime tries REST, then GraphQL. If the log shows both refused:
1. create an API key of your own with write access to pods;
2. store it as a RunPod secret, `rr_runpod_api_key`;
3. add `RR_RUNTIME_IDLE__RUNPOD_API_KEY` = `{{ RUNPOD_SECRET_rr_runpod_api_key }}` to the template.

Record which path worked. The ADR asks for it before GraphQL retires (early 2027).

**Check 3's profile.** A terminal session does not see the template's environment, so `auto` there could pick another profile than the running one: name it.

**Check 6, in numbers.** Each request fits its slot's 4096 tokens (1000 in, 2048 out). 32 requests on 8 slots run in four waves. At 2048 tokens a wave lasts about a minute, so the last wave's requests wait several minutes before their first token. RunPod's proxy cuts a connection after about 100 s without a byte. The runtime answers such a request at once and sends a keep-alive every 15 s (ADR-0014), so none should fail.

## 7. The benchmark: choosing the default GPU

On a pod of each candidate type (plan §4.5: RTX 4090, RTX 5090, L40S, A100 or H100), run:

```sh
rreport bench --cold-start
```

It prints a Markdown table and saves JSON to `~/.local/share/rreport/bench/`:
- **The commander:** time to the first token, and the speeds it reads and generates at.
- **The infantry:** *total* tokens per second at 1, 4, 8, 16 and 32 requests at once. Levels beyond the engine's slots are marked "queued".
- **Peak VRAM, and the cold start:** stop → start → every engine ready.
- **Cost:** dollars per million generated tokens, at RunPod's price for the pod.

On a 24 GB card the commander and the infantry do not fit together. Measure the commander with the default profile, then the infantry: redeploy with `RR_RUNTIME_MODELS__PROFILE=24gb-infantry`, and run `rreport bench` again.

Choose the cheapest GPU whose infantry throughput and commander speed meet the job: the plan's estimate is 15–30 minutes of pod per video-hour. Record the choice, with the tables, in the M0 notes.

On 48 and 80 GB pods, the commander runs four slots sharing one pool. [Runbook R1](../../docs/runbooks/m1-m2.md#r1-the-commanders-slots-and-the-prompt-cache) measures what they are worth, and whether the prompt cache serves a shared system message (`--commander-parallel`, `--cache-check`).

## 8. When something goes wrong

| Symptom | Where to look | Likely cause |
|---|---|---|
| `rreport pod start` says `not enough free GPUs` | The error itself | The pod's machine is full. Deploy a new pod from the template, on the same network volume, and update `runpod.pod_id` |
| The pod stops itself right after starting | The pod's log: its last lines | The entry point's reason: no GPU, no token, a model file that failed to download or verify |
| `rreport doctor`: `runtime: … refused the token` | — | The hub's `runtime.token` differs from the pod's `RR_RUNTIME_TOKEN` |
| `rreport doctor`: `versions` warning | — | The hub and the image come from different commits: rebuild one of them |
| Engines stay `starting` | `rreport pod status`; the pod's log (`model=` lines) | A model too large for the GPU fails to load and restarts. Choose a larger profile, or GPU |
| The pod keeps running, idle | The pod's log | See check 4 |

**Costs.** A stopped pod keeps its container image and costs nothing for GPU time. The network volume bills monthly until you delete it. Delete test pods when you are done.
