<!-- Approved plan, copied verbatim from the planning session (2026-09-29).
     This is the product and architecture baseline; changes are recorded as ADRs in docs/adr/. -->

# rReport — understand any video completely, then go beyond it

> Greenfield plan for `teonegura21/AI_rReport` (branch `claude/busy-dirac-1q39ea`; the repo is empty today). Working name: **rReport**.
>
> Binaries:
> - `rreport`: the hub + CLI, on your Linux laptop.
> - `rreport-runtime`: a CUDA-free supervisor on the GPU pod.
> - `rr-speech-worker`: the GPU speech batch worker.

---

## 0. Context

**The problem.** Today's video AI tools (Gemini/NotebookLM, Eightify, Glasp, NoteGPT, …) *compress*.
- They read only the captions.
- They garble jargon and names, and cut asides as filler.
- They ignore what's on screen and soften blunt content.
- They stop exactly where the video stops.

You want the opposite. First, **understand the whole video**, with nothing dropped. Then **complete its story**:
- fill the gaps it assumes;
- verify and update its claims;
- follow its open threads;
- rebuild its narrative;
- keep researching over time.

**Decisions from our Q&A**

| Topic | Decision | Consequence for the design |
|---|---|---|
| Hardware / library | Rented GPU pod only; laptop hub (Linux) | Library, UI and MCP live on your laptop. The pod is rented only while working |
| "Lost words" | All four: jargon garbled · asides cut · on-screen ignored · blunt topics sanitized | The Fidelity Engine (§3.1–3.4) |
| "Complete the story" | All four: fill gaps · verify & update · follow threads · rebuild narrative | Light enrichment automatically; deep research on demand |
| Stack | C++ core + TypeScript at the edges | C++23 engine. TypeScript only for web UI, extension and a thin MCP adapter |
| **Commander** (big model) | **Qwen3.8**, for its price/quality | Qwen3.8-27B on the pod; a hosted Qwen3.8 API is a drop-in. Claude / GPT / Gemini are optional fallbacks |
| **Infantry** (small models) | **Quantized Gemma 4, "an army"** | A native sub-agent environment inside the C++ engine (§3.7) |
| Pod GPU | Decided by measurement | Capacity benchmark in M0 |
| Video types | All four | Visual track, narrative rebuild and fact-checking are all first-class |
| First surface | Web app + MCP | *For your own use while building:* MCP arrives early (M3) as a chat interface, then the web UI (M7). *For customers:* all three doors launch together with the hosted SaaS (v3) |
| Business | **Hosted SaaS** (subscription) | One account, one library, three doors: web app, browser extension, MCP connector (§14). The code is multi-tenant-ready. The hosted backend must **never** fetch YouTube itself |
| Flagship | Retold article | Transcript, idea map and Q&A are other views of the same knowledge model |
| Language | English only | English-first speech stack |

**Intended outcome.** Paste a YouTube URL. Tens of minutes later (the exact figure comes from the benchmark) you get:
- a **Retold Article** with a *measured* completeness score;
- a corrected, speaker-labelled transcript;
- the on-screen content;
- an **Idea Map** you can question branch by branch;
- automatic "complete the story" panels;
- **research threads** you can continue at any time.

It is available in a local web app and inside Claude, ChatGPT and Cursor via MCP.

---

## 1. Tools you might have missed (research as of 2026-09-28)

| Tool | What it does well | Why it isn't what you want |
|---|---|---|
| **Gemini Notebook** (NotebookLM, renamed Jul 2026) + **Gemini agentic video** (1 Sep 2026) | Huge reach. Clickable mind maps, Deep Research, 1M-token chat. Agentic video looks at frames via the API and is coming to the Gemini app and YouTube "Ask" | Summary-first. The notebook reads transcripts only. Mind maps are topic-level. Answers are policy-filtered and "generated, not quoted" |
| **Recall** (getrecall.ai) | Knowledge graph that links itself, spaced repetition, extension | A graph of *summaries*: no reconstruction, visuals or fact-checks |
| **BibiGPT / Lilys AI / HoverNotes** | Actually look at frames (slides, code) with timestamped citations | Cloud-only summaries and notes; no idea graph or research threads |
| **Twelve Labs** (Pegasus 1.5) / **VideoDB** | Strong video-understanding APIs, both with MCP servers | Building blocks; cloud-only |
| **BiliSum** (open source, MIT) | Local visual notes, mind maps, search across videos | Bilibili-first; no research or fact-checking |
| **mcptube** (open source, MIT) | MCP server + keyframes + vision LLM + a wiki that grows per video | Early (~160★); cloud APIs for vision; no research layer |
| **steipete/summarize** (open source, MIT) | Slides via scene change + OCR; Chrome side panel talking to a local daemon | Summaries |
| **STORM / Co-STORM**, **Local Deep Research**, **GPT Researcher** | Serious open-source research agents | They don't take video as input |
| **Fabric** `extract_wisdom` | Popular YouTube prompt pattern | Deliberately compresses (16-word bullets): the opposite of your goal |

More than 40 YouTube MCP servers exist. Almost all just fetch transcripts, and the "visual" ones hand the work to Gemini.

**The gaps nobody fills. These are our product:**
1. A complete reconstruction with measured coverage.
2. Speech and on-screen content fused, using open models.
3. An idea graph at the level of individual claims, with Q&A scoped to one branch.
4. Research attached to each claim, including "what changed since".
5. Persistent research threads you own.
6. Non-sanitized reporting.
7. An MCP server that exposes structured video knowledge.

Google is closing the "it looks at the screen" gap fast. Our moat is fidelity, completeness, research threads, ownership and model choice.

**Worth studying:**
- steipete/summarize: its capture split between extension and daemon.
- mcptube: MCP tool design.
- Co-STORM: research planning plus a mind map.
- BiliSum: visual notes.
- HKUDS VideoRAG: study the ideas only; its code is non-commercial because of ImageBind.

---

## 2. What you get for every video

1. **Retold Article (flagship).** A long-form piece that retells *everything* in order: every argument, example, number, name and aside.
   - On-screen content is woven in: code as code blocks, slides as lists, charts as numbers.
   - Strong statements appear as verbatim quotes.
   - Every sentence is clickable and seeks the embedded player.
   - Typical length is 40–80% of the transcript. A summary is about 5%.
2. **Fidelity Report.** Coverage, everything labelled filler (click to audit), transcript corrections, `[unclear]` spans, and any unresolved findings.
3. **Transcript & Visuals.** Corrected, speaker-labelled transcript. A gallery of slides, code and charts with their text.
4. **Idea Map.** Thesis → ideas → claims → evidence and examples, with cross-links (supports, contradicts, causes, example-of).
   - Click a node to ask about that branch.
   - Research attaches as dashed "research nodes", so the map grows beyond the video.
5. **Complete the story. Light, runs automatically:**
   - "What this video assumes you know" primer;
   - glossary and entity cards (Wikipedia/Wikidata);
   - interesting facts, ranked partly by YouTube's "most replayed" heatmap;
   - claims to check, with time-sensitive ones and predictions whose date has passed flagged;
   - loose ends and open questions;
   - timeline and cast (for narrative videos);
   - suggested research threads.
6. **Go deeper. Runs on demand:**
   - fact-checks (verdict, sources, counter-arguments, "what changed since");
   - research threads you can resume and branch;
   - a "Complete Story" document with five parts: before, the video, after, disputed, still open.
7. **Conversations** scoped to a video, an idea branch, a research thread or your whole library.
   - Modes: *video-only* (strictly grounded), *with research*, *with web*.
   - Citations point to timestamps and sources.

---

## 3. The core ideas, and why they work

### 3.1 Fidelity Ledger: bookkeeping instead of trust
Summaries lose information because nothing *forces* completeness: the model decides what matters. We turn completeness into bookkeeping that code checks.

- **Closed-world atomization (Ledger A).**
  - The transcript becomes numbered sentences `S1…Sn` and the on-screen items become `V1…Vm`.
  - An infantry mission must return **exactly one entry per ID**. The entry is either:
    - typed atoms: claim, fact, number, example, anecdote, aside, joke, hedge, opinion, question, instruction, definition, reference, visual; or
    - a filler label from a fixed list: disfluency, greeting, repetition, sponsor, decorative.
  - Missing IDs are detected by code and re-dispatched.
  - *Why:* small models are reliable at "label every row" and unreliable at "find everything important", so we only ever ask them the former.
- **Atoms carry the nuance that summarizers destroy:**
  - `speaker` vs `attributed_to`: who *says* it vs who is *quoted*;
  - `modality`: asserted / hedged / hypothetical / reported / sarcastic / question;
  - `register`: main / aside / joke / anecdote / example;
  - `intensity`;
  - `must_quote`: strong, controversial, profane or precise statements that must appear verbatim.
- **Hard rule.** A "filler" sentence containing a number, a named entity or a lexicon term is automatically re-examined.
- **Ledger B (atoms → article).**
  - The article is Markdown with inline markers: `…data curation [A41]. He hedges: "I could be wrong" [A42]`.
  - Models never output timestamps or IDs that code can compute; timestamps are rendered from atom spans.
- **Deterministic checks run first.** They are free and complete:
  - every marker is valid;
  - every atom is cited;
  - each atom's numbers, names and terms appear in the paragraph that cites it;
  - quotes are snapped to the exact transcript words (Smith–Waterman alignment);
  - `must_quote` atoms appear verbatim;
  - order stays chronological within each section.
- **Then one judge per paragraph.** An infantry mission reasons first, then gives its verdict. It flags sentences that are unsupported, softened, intensified, misattributed or have a wrong number.
  - Only failing paragraphs are repaired, at most 2 rounds.
  - Leftovers are **published in the Fidelity Report, never hidden**.
  - Expected overhead: about 15–25% of LLM tokens.

### 3.2 Lexicon-first transcription (fixes *jargon/names garbled*)
1. **Captions first.**
   - Creator-uploaded captions are the text backbone; speech recognition only adds timing.
   - Auto-captions are re-punctuated by the LLM, and code checks that no word changed.
2. **Lexicon-0 (rules, no LLM).** Built from the title, description, chapters, tags and captions. It biases the recognizer: Whisper's prompt, Parakeet's hotwords.
3. **M4 upgrade: three readings.** Parakeet, Whisper and YouTube captions are aligned **ROVER-style**, a NIST technique that votes word by word.
   - Disagreements and near-misses of lexicon terms become infantry missions.
   - Each mission may only choose one of the aligned alternatives, a lexicon term, or `[unclear]`. **It cannot invent words.**
   - On-screen text at that second is evidence.
   - Every correction is logged. The upgrade ships only if it measurably lowers the *term error rate*.

### 3.3 On-screen content is information (fixes *on-screen ignored*)
- **Keyframes on the laptop:** change detection at 1–2 fps, perceptual-hash dedupe, and a heartbeat frame every 30 s. The raw video is deleted afterwards.
- **Infantry missions (Gemma 4 vision)** return, per frame: its kind (slide/code/chart/diagram/scene/UI), the verbatim text, a description, and chart values. Code changes between frames are computed by a line diff in C++.
- **Ledger.** Every frame enters Ledger A as a `V#` item, which becomes visual atoms or is labelled "decorative". The article must weave the visual atoms in.

### 3.4 Report, don't restate (fixes *blunt topics sanitized*)
- **Framing.** Models soften when they *assert*. They are faithful when they *report with attribution* ("X argues that…").
- **Mostly deterministic.** `must_quote` atoms are checked verbatim by code, so most anti-sanitization is not a model's opinion. The judges catch the rest (softening and intensifying).
- **Fallbacks.** A refusal or a softened answer is automatically re-run on a per-role fallback model.
- **Known risks, measured.** Qwen (the commander) censors China-related topics, and Gemma has safety tuning. The eval has a dedicated blunt/controversial slice.
- **Separation.** *The article says what the video says; research says what the world says.* They live in separate panels.

### 3.5 Structure first, prose second: the Video Knowledge Model (VKM)
- **What.** Sections → atoms → idea tree → entities and terms → claims → timeline and narrative.
- **Views.** Every view (article, transcript, map, Q&A, MCP) is a projection of the VKM.
- **Provenance.** Every article sentence knows its atoms, and therefore its seconds of video.

### 3.6 Commander and infantry: who does what
| | Commander: Qwen3.8-27B (pod) or a Qwen3.8 API | Infantry: quantized Gemma 4, many parallel slots |
|---|---|---|
| Strength | Whole-picture judgment and writing | Narrow, schema-constrained, independently checkable work |
| Missions | Sectioning + video brief · global idea tree · narrative · **writing the article** · chat · research planning and synthesis · fact-check verdicts · triaging escalations | Frame reading · disagreement spans · atomization · claims/entities/timeline · paragraph judges · page reading and evidence extraction · citation checks |
| Thinking | Off for writing and chat; on, with a budget, for planning. Qwen3.8 over-thinks by default, so this is set per role | Off |

**Writing the article:** a global **article plan** (through-line and the transition at each section boundary), then **section drafts in parallel**, then a **seam pass** that rewrites only the boundary paragraphs. The seams are then re-verified.

Every role-to-model binding lives in config. Claude, GPT or Gemini can be enabled per role, for example as fallbacks.

### 3.7 The native sub-agent environment ("the army")
Small models are *very* reliable when each one receives a **mission order**:
- one objective;
- only the context it needs (2–8K tokens): its own section, plus a ~1.5K-token **video brief** (speakers, genre, lexicon, section gists);
- a strict output schema;
- a deterministic check on the result.

The engine implements this natively in C++ (`rr_agents`):

| Concept | What it is |
|---|---|
| **Mission** | A typed task: role, prompt template, input packet, output schema, validators, budget, priority, retry policy |
| **Squad** | Fan-out/fan-in. Run N missions in parallel, validate each result, retry failures with the validator's error message, gather results in order |
| **Dispatcher** | A priority queue per model endpoint, sized to its parallel slots. llama-server's continuous batching means one copy of the weights serves many independent conversations. Handles backpressure and cancellation |
| **Escalation** | Two failed validations → the commander. A refusal or softening → the fallback provider. Nothing fails silently |
| **Commander tools** | During research the commander calls `dispatch_squad(mission, inputs[])`, e.g. "read these 12 pages and extract evidence about claim C" |
| **Battle log** | Every mission is recorded: input hash, model, tokens, latency, validation result, retries. It feeds the UI timeline, cost accounting and the eval |

### 3.8 Capture at the edge, compute on the GPU
YouTube blocks datacenter IPs, and 2026 court rulings (*Cordova v. Huneault*; the Udio case) target circumventing YouTube's protections.

- YouTube is fetched only on *your* side: the laptop hub now, the browser extension in the hosted product.
- The pod only ever receives audio, frames and text.
- yt-dlp (plus the Deno runtime it now requires) is a **user-installed, opt-in** dependency, never bundled. `rreport doctor` checks yt-dlp, Deno and ffmpeg.
- Local files and podcasts are first-class inputs too.

### 3.9 Artifacts are the truth; the timeline is the anchor
- **Artifacts.** Every stage writes **one immutable, schema-validated JSON artifact** (zstd-compressed in SQLite).
  - Its cache key is `BLAKE3(stage@version, prompt + schema hashes, model id + quant, params, input artifact hashes)`.
  - SQL tables are *projections* that can be rebuilt from the artifacts.
- **Why:**
  - Reproducibility.
  - Crash consistency: an artifact, its status and its projections commit in one transaction.
  - Cheap experiments: change the composition prompt and every upstream stage is a cache hit.
- **Analysis.** An *analysis* is the config snapshot of one run. A video can have many; the UI shows the current one.
- **Anchors.** Research, chats and claim verdicts are anchored to the **video timeline** `(video, t0_ms, t1_ms, quote)`, not to atom IDs.
  - So re-analysis never orphans your research.
  - Verdicts are append-only, so "what changed since" keeps its history.

### 3.10 Models are configuration; an eval harness decides
Open models turn over monthly (Qwen 3.5 → 3.6 → 3.8 happened within 6 months). Roles map to models in config; the eval harness (§10) picks the winners on *our* metrics.

---

## 4. Architecture

```
 YOUR LINUX LAPTOP (CPU)                                  GPU POD (RunPod; rented while working)
┌────────────────────────────────────────────┐  HTTPS  ┌─────────────────────────────────────────────┐
│ rreport — C++ hub (the source of truth)    │ bearer  │ rreport-runtime — CUDA-free supervisor      │
│  capture: yt-dlp(+Deno) · ffmpeg · frames  │ token   │  auth · blobs · jobs · OpenAI-compat proxy  │
│  pipeline (artifacts) · Fidelity Ledger    │────────▶│  VRAM policy · idle stop · dead-man switch  │
│  agents: missions · squads · dispatcher    │ blobs   │  engines = child processes on 127.0.0.1:    │
│  library: SQLite + FTS5 + vectors          │ jobs    │   llama-server  COMMANDER Qwen3.8-27B       │
│  retrieval · chat · research               │ chat    │   llama-server  INFANTRY  Gemma 4 × N slots │
│  PodManager (RunPod API)                   │◀────────│   llama-server  embedder · reranker         │
│  REST + SSE ─┬─ web UI (React + Vite)      │ (SSE)   │   rr-speech-worker  Parakeet·Whisper·diar.  │
│              └─ MCP adapter (TS)           │         └─────────────────────────────────────────────┘
└──────────────┬─────────────────────────────┘
               │ optional
               ▼
  Hosted Qwen3.8 API · Claude · GPT · Gemini (fallbacks)     Research: one web-search API · Wikipedia/Wikidata (scholarly APIs later)
```

### 4.1 Hub ↔ runtime protocol: async, safe to retry, always streamed
- **Blobs.** `HEAD/PUT /v1/blobs/{library}/{blake3}`: content-addressed and hash-verified on arrival. Each file is uploaded once and reused by many calls.
- **Jobs.** `POST /v1/jobs` with `idempotency_key` = the stage's cache key, so resubmitting returns the same job or result. Also `GET /v1/jobs/{id}`, `GET …/events` (SSE) and `DELETE` (cancel). Used for speech and batch work.
- **Model calls.** `/v1/chat/completions | embeddings | rerank | models` go through an OpenAI-compatible proxy.
  - Images are passed as `rr-blob://…` references and inlined by the supervisor.
  - The hub **always streams**, because RunPod's HTTP proxy cuts silent requests at about 100 s (verify in M0).
- **What travels:**
  - original audio bytes (~60 MB/h, decoded on the pod, with no lossy re-encoding);
  - deduplicated JPEG frames (~5–30 MB/h);
  - text.
  - **Never video.**

### 4.2 Runtime: a CUDA-free supervisor, with GPU engines as child processes
**Why processes:**
- VRAM is guaranteed to be released when a process exits. ONNX Runtime's and ggml's memory pools don't reliably give memory back.
- A CUDA crash can't take down the API.
- whisper.cpp and llama.cpp each vendor their own copy of ggml, so they never share a process.
- Upstream binaries run unmodified.

**The engines:**
- **One `llama-server` per resident model:** the commander, the infantry (`-np N` slots), and the embedder/reranker.
  - The supervisor routes by `model`.
  - It decides which models stay loaded from the GPU profile: phases swap on small GPUs; everything stays loaded on big ones.
- **`rr-speech-worker`:** a one-shot batch CLI (manifest in → JSON out → exit) linking sherpa-onnx (Parakeet + diarization) and whisper.cpp. M1 uses upstream `whisper-cli` instead.

**Build and storage:**
- Build for CUDA 12.8+ with architectures `80;86;89;90;120`, covering A100, 3090, 4090/L40S, H100 and 5090.
- Verify that ONNX Runtime's CUDA provider works on Blackwell; if not, diarization runs on CPU, which is cheap.
- Models sit on a RunPod network volume with a sha256 manifest. The image holds only binaries. Engines bind to localhost; the supervisor is the only exposed port.

### 4.3 Pod lifecycle and cost control
- **`PodManager`** (RunPod API) starts the pod when you submit a job, so boot and model loading overlap capture on your laptop. It stops the pod when the queue is empty.
- **Dead-man's switch.** No hub heartbeat for N minutes and no running job → the pod stops itself. `runpodctl` comes pre-installed with a pod-scoped key: `runpodctl pod stop <id>`. Stopping keeps the disk.
- **Metering.** Every GPU-minute and token is recorded (`gpu_usage`, `llm_calls`) and shown per job in the UI.

### 4.4 Model roster (the eval and the benchmark confirm each choice)
| Role | Default | Alternatives |
|---|---|---|
| Speech (M1) | **Whisper large-v3-turbo** via upstream `whisper-cli`: built-in Silero voice detection, lexicon-0 as prompt, JSON output with word timing | large-v3 |
| Speech (M4) | **Parakeet-TDT-0.6B-v2 via sherpa-onnx.** Its source code confirms hotword biasing (a "context graph" during beam search) and token timestamps for this model family. **+ Whisper** as the second reading | whisper.cpp ≥ 1.9 Parakeet (v3), Cohere Transcribe, IBM Granite Speech 4.1 |
| Diarization (M4) | **sherpa-onnx** C++ `OfflineSpeakerDiarization`: pyannote segmentation + speaker embeddings + clustering | NVIDIA Nemotron 3 Diarization (ONNX) |
| **Infantry** (text + vision) | **Gemma 4 26B-A4B** at 4-bit: mixture-of-experts with 3.8B parameters active per token, so it's fast under parallel load. **Gemma 4 12B / E4B** as "light infantry" for simple missions | Gemma 4 31B (strong, but needs a lot of memory per token of context), Qwen3.6-35B-A3B |
| **Commander** | **Qwen3.8-27B** (dense, 262K context, image + video input) at 4–8-bit depending on the GPU | Hosted Qwen3.8 API, gpt-oss-120b (80 GB pod), Claude / GPT / Gemini |
| OCR for dense text | Infantry (Gemma 4 vision) | PaddleOCR-VL-1.6, GLM-OCR (both run in llama.cpp) |
| Embeddings / reranking | Qwen3-Embedding-0.6B / Qwen3-Reranker-0.6B. The hub keeps a CPU copy of the embedder so search works with the pod off | bge-m3 / bge-reranker-v2-m3 |

### 4.5 GPU options the M0 capacity benchmark chooses between
Sizes are approximate 4-bit weights. The number of infantry slots (conversations running at once) is limited by attention-cache memory, so it is **measured, not assumed**.

| Pod | Commander | Infantry | Loaded together? |
|---|---|---|---|
| 24 GB (RTX 4090, ~$0.34–0.74/h) | Qwen3.8-27B Q4 (~17 GB) | Gemma 4 12B Q4 (~7 GB) or 26B-A4B Q4 (~15 GB) | No: infantry and commander phases alternate |
| 32 GB (RTX 5090, ~$0.69–0.99/h) | Qwen3.8-27B Q4 | Gemma 4 12B Q4, ~6–8 slots | Yes, tight. 26B-A4B needs phase swapping |
| 48 GB (L40S, ~$1.09/h) | Qwen3.8-27B Q4–Q6 | Gemma 4 26B-A4B Q4, ~8–16 slots | Yes, and the speech models fit too |
| 80 GB (A100/H100, ~$1.59–3.49/h) | Qwen3.8-27B Q8 (~29 GB) | Gemma 4 26B-A4B Q4–Q6, ~16–32 slots | Yes, with generous context |

**`rreport bench` measures:**
- commander tokens/s;
- infantry *total* tokens/s at 1/4/8/16/32 slots;
- peak VRAM with an 8-bit attention cache;
- cold start;
- minutes and $ per video-hour on the eval set.

The cheapest profile that meets the quality bar becomes the default. On the biggest pods, SGLang or vLLM are optional infantry engines: they share a common prompt prefix across slots, and they speak the same protocol.

**Early estimate, replaced by the benchmark:** about 15–30 min per video-hour on a 24–32 GB pod, roughly $0.10–0.50 of pod time. No API costs unless an optional cloud specialist is enabled.

### 4.6 Optional cloud specialists
The provider layer speaks two wire protocols:
- **OpenAI-compatible:** llama-server, hosted Qwen3.8, GPT, and Gemini's compatible endpoint.
- **Anthropic Messages:** Claude, from M8.

When Claude is enabled, we call it over raw HTTPS, since C++ has no SDK, following the claude-api skill's cURL reference:
- **Defaults.** `claude-opus-5`, with `thinking:{type:"adaptive"}` + `output_config.effort` per role.
- **Constraints.** No assistant prefill (current models reject it with a 400). JSON output via `output_config.format`.
- **Refusals.** Always check `stop_reason`. Server-side fallbacks are on (`server-side-fallback-2026-07-01`, `fallbacks:"default"`); these are unavailable in the Batch API.
- **Prompt caching.**
  - The video's "knowledge pack" is a stable prefix with `cache_control`; reads cost about 0.1× the input price.
  - Scope switches are mid-conversation `system` messages.
  - Prompt building is byte-deterministic.
  - A test asserts `cache_read_input_tokens > 0`.

---

## 5. The per-video pipeline

Every stage is idempotent and resumable, and emits one artifact (§3.9). Progress streams to the UI over SSE.

| # | Stage | Who | Output | Failure / long videos |
|---|---|---|---|---|
| 1 | resolve | hub (`yt-dlp -J` / ffprobe) | Metadata: description, chapters, date, caption list, "most replayed" heatmap | Bot check → `needs_user` (`rreport doctor`; cookies strictly opt-in) |
| 2 | capture | hub | Captions (manual > auto, json3), original audio bytes, 720p video-only (visual mode) | Resumable downloads; atomic blob writes (tmp → fsync → rename) |
| 3 | frames | hub (ffmpeg) | Deduplicated keyframes; raw video deleted | Adaptive threshold; frame cap per hour |
| 4 | lexicon-0 | hub, rules only | Ranked terms → Whisper prompt (≤224 tokens) + hotword list | — |
| 5 | speech | runtime job | Word-timed transcript(s), plus diarization in M4 | Idempotent job, resubmitted if the pod dies; if one engine fails, the other alone |
| 6 | visuals | infantry squad (vision) | Per frame: kind, verbatim text, description, chart values; lexicon-1 | 1 repair, else the item is flagged |
| 7 | transcript | hub + infantry | `S#` sentences, speakers, corrections log, `[unclear]` | Failed adjudication → keep the vote winner, flagged |
| 8 | sections + brief | chapters, or the commander | Sections ≤8 min + the video brief | Fallback: windows cut at pauses |
| 9 | atomize | infantry squad, one mission per section | `A#` atoms, filler labels, visual accounting (closed world) | ≤2 targeted re-prompts, then flagged |
| 10 | structure | commander (idea tree, narrative) + infantry (claims, entities, timeline) | Ideas + edges, claims, timeline, narrative arc, references, assumed knowledge | Optional: the report ships without the map |
| 11 | compose | commander: plan → parallel section drafts → seam pass | Markdown with `[A#]` markers | Per-section retry |
| 12 | verify | code checks + infantry judges (per paragraph); the commander repairs | Findings, coverage, fidelity tier | ≤2 repair rounds; leftovers published |
| 13 | enrich (light) | infantry + commander (+ Wikipedia/Wikidata) | Glossary, entity cards, primer, claims to check, interesting facts, loose ends, suggested threads | Each item fails independently |
| 14 | index | runtime embedder (CPU fallback on the hub) | Chunks, FTS5, vectors | Keyword search only if embeddings are unavailable |

**Required stages:** 1, 2, 5 (or manual captions), 7, 8, 9, 11, 12. The report header shows its fidelity tier: captions-only · audio · audio + visual.

**Phases and overlap.**
- The pod runs three phases in order: speech → infantry → commander. They swap on small GPUs and stay co-resident on big ones.
- Frames are extracted on the laptop while the pod transcribes.
- Visuals run before transcript reconciliation, because on-screen text helps adjudication.

**Long videos (2–4 h).**
- Infantry never sees more than a section plus the brief.
- The commander's global passes see the whole transcript: about 50K tokens for 4 hours, well within 262K.
- Composition is parallel by section, followed by the seam pass.

---

## 6. Data model

`library.db`: one per library, which in hosted mode means one per tenant. WAL mode, `STRICT` tables, times in integer ms.

- **Truth:**
  - `artifacts` (hash → zstd JSON), `stage_cache` (cache key → artifact);
  - `analyses` (config snapshot, fidelity tier, status), `stage_runs`, `jobs`.
- **Projections (rebuildable from artifacts):**
  - `sentences`, `corrections`, `speakers`, `visuals`, `sections`;
  - `atoms` (+ `attributed_to`, `modality`, `register`, `intensity`, `must_quote`), `fillers`;
  - `ideas` + `idea_atoms` + `idea_edges`, `entities` (library-wide) + `entity_mentions`;
  - `claims`, `timeline_events`;
  - `reports` + `report_blocks` + `report_sentences` + `findings`.
- **Anchored to the timeline (survives re-analysis):**
  - `research_threads` (parent/fork), `research_steps`, `sources`, `research_notes`;
  - `claim_verdicts` (append-only, "as of" dates);
  - `conversations` (forks) + `messages`.
- **Search:** `chunks` + `chunks_fts` (FTS5) + `chunks_vec` (sqlite-vec, partitioned by video, int8).
- **Accounting:**
  - `missions` (the battle log);
  - `llm_calls` + compressed payloads (configurable retention);
  - `gpu_usage`.

A branch scope is a recursive query over `ideas` that collects the subtree's time spans. Migrations are numbered SQL files, each applied in one transaction and tracked with `PRAGMA user_version`.

---

## 7. C++ architecture and repository layout

```
AI_rReport/
├── CMakeLists.txt · CMakePresets.json · vcpkg.json · vcpkg-configuration.json · cmake/ · .clang-format · .clang-tidy
├── src/<module>/include/rr/<module>/*.hpp  +  src/<module>/*.cpp        # one CMake target per module
├── apps/      rreport/ · rreport-runtime/ · rr-speech-worker/ · rreport-eval/
├── prompts/   inja templates + manifest.toml      schemas/{llm,artifacts,api,runtime}/      migrations/      config/default.toml
├── web/       React + Vite + TS (Cytoscape.js idea map, YouTube IFrame player)
├── mcp/       TS, official MCP SDK v2 (`@modelcontextprotocol/server`, spec 2026-07-28), compiled to one executable
├── packages/api-types/   TypeScript types generated from schemas/      extension/ (v2, WXT)
├── docker/runtime/ · deploy/runpod/ · eval/ · tests/{unit,component,replay,fixtures}/ · docs/{adr,design,runbooks}/
```

| Target | Role | Depends on |
|---|---|---|
| `rr_core` | `Result`/`Error`, strong IDs, time spans, hashing, `Secret`, thread pools, `Limiter` | std, fmt, blake3 |
| `rr_log` `rr_config` `rr_json` `rr_text` | logging; layered TOML config; JSON + schema validation; normalization and alignment | spdlog, toml++, nlohmann + jsoncons, utf8proc |
| `rr_db` `rr_blob` `rr_proc` `rr_net` | own SQLite wrapper (no exceptions), blob store, subprocesses, HTTP client/server + SSE | sqlite3[fts5] + vendored sqlite-vec, reproc, cpr, cpp-httplib |
| `rr_llm` | providers, routing/fallback, accounting, record/replay, `structured<T>()` | core, json, net, inja |
| `rr_agents` | missions, squads, dispatcher, escalation, battle log | llm, db |
| `rr_media` `rr_transcript` | capture sources, caption parsers, frames, ROVER | proc, blob, text |
| `rr_knowledge` `rr_fidelity` `rr_report` | VKM types + repositories, ledger checks, rendering | db, agents, inja |
| `rr_pipeline` `rr_research` `rr_hub_api` | stages + executor, research tools, REST/SSE | all of the above, lexbor |
| `rr_runtime_api` `rr_runtime_client` `rr_supervisor` | protocol types, hub-side client + `PodManager`, pod supervisor (**CUDA-free**) | json, net, proc, blob |
| `rr_speech` (+ `rr-speech-worker`) | Parakeet, diarization and Whisper engines: **the only CUDA code** | sherpa-onnx (+ ONNX Runtime), whisper.cpp |

Each target exposes only its own `include/` directory, so the compiler enforces the layering. Third-party types stay hidden behind pimpl.

**Key interfaces (sketch)**
```cpp
enum class Errc : std::uint8_t { invalid_argument, not_found, conflict, io, network, timeout, unavailable,
  rate_limited, cancelled, refused, model_output_invalid, budget_exceeded, needs_user, internal };
struct Error { Errc code; std::string message; std::vector<std::string> context;
  std::optional<std::chrono::milliseconds> retry_after; std::source_location where;
  [[nodiscard]] bool transient() const noexcept; };
template <class T> using Result = std::expected<T, Error>;

class LlmProvider {            // llama-server, hosted Qwen API, Claude, … (caps: schema? images? caching?)
 public: virtual ~LlmProvider() = default;
  [[nodiscard]] virtual ProviderCaps caps() const noexcept = 0;
  [[nodiscard]] virtual Result<ChatResponse> chat(const ChatRequest&, StreamSink*, std::stop_token) = 0; };

template <class Out> struct Mission {  // one order for one soldier
  RoleId role; PromptRef prompt; json packet; SchemaRef schema; std::vector<Validator<Out>> validators;
  Budget budget; Priority priority; RetryPolicy retry; };
class Squad {                  // fan-out/fan-in over a Dispatcher (slot-aware queues per endpoint)
 public: template <class Out> Result<std::vector<Result<Out>>> run(std::span<const Mission<Out>>, std::stop_token); };

class CaptureSource {          // YtDlp, LocalFile, ExtensionUpload (v2)
 public: virtual ~CaptureSource() = default;
  virtual Result<VideoMetadata> resolve(const SourceRef&, std::stop_token) = 0;
  virtual Result<CaptureManifest> capture(const CaptureRequest&, ProgressSink&, std::stop_token) = 0; };

class Stage {                  // pure w.r.t. its inputs; the executor handles caching, retries, transactions
 public: virtual ~Stage() = default;
  virtual const StageInfo& info() const noexcept = 0;
  virtual Result<CacheKey> cache_key(const StageInputs&, const StageEnv&) const = 0;
  virtual Result<Artifact> run(const StageInputs&, StageEnv&, std::stop_token) = 0; };

class Check {                  // Fidelity Ledger checks: deterministic or judge-backed
 public: virtual ~Check() = default;
  virtual Result<std::vector<Finding>> run(const LedgerView&, CheckEnv&, std::stop_token) = 0; };
```

**Conventions:**
- **Errors.**
  - `Result` everywhere a failure is expected; exceptions exist only inside adapters that wrap throwing libraries.
  - A catch-all at each thread entry turns an escaped exception into `Errc::internal`, so a bug fails one stage, not the process.
  - `[[nodiscard]]` on everything returning `Result`; `RR_TRY` macros; an always-on `RR_ASSERT`.
  - Retry policy by error class: transient → backoff with jitter; `model_output_invalid` → one repair, then a fallback model; `refused` → fallback provider; budget or needs-user → stop.
- **Concurrency.**
  - A thread pool per resource class: hub CPU = cores/2, so the laptop stays usable; network I/O = 16 threads.
  - A `Limiter` per model endpoint, sized to its slots.
  - `std::jthread` + `std::stop_token` everywhere; a stop callback kills subprocesses and aborts HTTP transfers.
  - v1 runs one analysis at a time, with its missions in parallel.
  - No coroutines or asio (recorded in an ADR; revisit for hosted).
- **Config.**
  - Layers: built-in `default.toml` < user file < `RREPORT_*` environment variables < CLI flags.
  - `rreport config show --origin` shows where each value came from.
  - Role → model routing is a table.
  - Secrets live in the environment or a 0600 `secrets.toml`; a `Secret` type never prints.
- **Observability.**
  - spdlog JSONL with job/analysis/stage/mission context.
  - Per-stage timeline and cost in the CLI and UI.
  - `/metrics` on the runtime.

---

## 8. Engineering standards (the "extraordinary quality" bar)

- **Build.**
  - C++23, CMake presets, vcpkg manifest mode with a pinned baseline.
  - Warnings `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast -Wnull-dereference`, with `-Werror` in CI.
  - clang-format and clang-tidy enforced.
- **Test layers:**

  | Layer | What runs |
  |---|---|
  | L0 static | Every prompt renders with fixtures; schema lint |
  | L1 unit | GoogleTest under ASan+UBSan |
  | L2 component | Temp SQLite; fake subprocesses; fault injection (`kill -9` mid-stage → resume) under TSan |
  | L3 replay | Whole pipeline on CC-licensed fixtures with the record/replay provider + a fake runtime; golden artifacts |
  | L4 nightly integration | CPU runtime with a tiny GGUF + whisper tiny.en |
  | L5 eval on the pod | Gates every model or prompt change |

  libFuzzer runs on the caption and marker parsers.
- **Contracts.**
  - JSON Schemas are the single source of truth: TypeScript types are generated from them, and C++ types have conformance tests.
  - **Portable schema subset**, so one schema works for llama-server's constrained decoding *and* Claude's `output_config.format`:
    - no recursion (trees become flat lists with `parent_id`);
    - `additionalProperties:false` everywhere;
    - no numeric or length limits in the model-facing schema; those are checked in C++ afterwards.
- **Prompts are data.**
  - A prompt's identity is its content hash. `prompts/manifest.toml` carries human-readable versions, and CI requires a bump when the content changes.
  - Each prompt file documents its purpose, inputs, schema and known failure modes.
- **Documentation.**
  - One ADR per decision in §3 (context, decision, consequences, alternatives).
  - Every header opens with "why this exists, invariants, thread-safety".
  - `docs/design/*.md` per subsystem, updated at the end of each milestone.
- **Security.**
  - The hub binds to localhost, checks the Host header (defends against DNS rebinding), and exchanges a token for an HttpOnly cookie.
  - Runtime: bearer token over HTTPS, engines on localhost.
  - Transcripts and web pages are always delimited *data*, never instructions. Research tools only make read-only GET requests.
- **Licenses.**
  - A model license register covers Parakeet CC-BY-4.0 (attribution), Qwen3.8 (Apache-2.0), and Gemma 4 (reported Apache-2.0, verify).
  - No non-commercial models; FFmpeg only as an LGPL subprocess; no GPL linking.

---

## 9. Milestones: one end-to-end path first, then measure, then breadth

| # | Milestone | Deliverables | Acceptance criteria |
|---|---|---|---|
| **M0** | Foundations + pod + benchmark | Skeleton, build, CI, core libraries, `rreport doctor`; runtime image (supervisor + llama-server serving Qwen3.8-27B and Gemma 4); `PodManager`; idle stop + dead-man's switch; `rreport bench` | CI green with sanitizers. Pod boots; `/health` reports GPU/VRAM; 401 without a token; models sha256-verified; self-stop works; **benchmark table → you pick the default GPU** |
| **M1** | **Vertical slice** | `rr_agents` core; URL → captions / whisper-cli → sections → closed-world atoms (infantry squad) → article (commander: plan, parallel drafts, seams) → deterministic ledger → HTML (TOC, timestamp links, coverage panel, "not included and why" appendix). `rreport serve` shows the reports | On 3 videos (20-min talk with manual captions, 60-min lecture with auto captions only, 90-min podcast): 100% of sentences accounted for; atom coverage ≥97%; 0 invalid markers; must-quote 100%; numbers ≥99%; `kill -9` + `resume` repeats 0 successful LLM calls; you read the articles and agree nothing important is missing |
| **M2** | Measurement | Eval v0: 8 videos, 2 per type. The commander drafts "silver" annotations and you correct them into gold. Paragraph judges. Model bake-off: infantry Gemma 4 26B-A4B vs 12B vs E4B vs 31B; commander quants; sanitization slice | Defaults fixed by numbers. Hand-annotated key-point recall ≥90% |
| **M3** | MCP first | TS adapter (stdio): `ingest_video`, `get_job`, `get_report` (paged by section), `retrieve` (grounded passages, so the *client's* model does the thinking), `list_videos`; tool annotations | Ingest from Claude Desktop/Code and discuss with timestamp citations, months before the web UI |
| **M4** | Audio fidelity | `rr-speech-worker`: Parakeet + Whisper + diarization; ROVER + constrained adjudication; speaker naming (self-introductions + metadata, user-overridable) | Ships only if term error rate improves on the gold set; VRAM/time measured on 4-hour audio |
| **M5** | Visual fidelity | Frame sampler, vision/OCR missions, visual atoms, code blocks and slides woven in, gallery | Visual-item inclusion ≥90% of gold items |
| **M6** | Structure | Commander sectioning, idea tree + edges, claims, timeline, narrative arc, reference resolution | Flow score up with no coverage regression |
| **M7** | Web UI v1 | React reader with a synced YouTube player (click a sentence → seek); ledger view; annotated transcript; idea map; gallery; glossary; job timeline with cost | Manual UX pass on the eval videos |
| **M8** | Enrichment + Q&A | Light enrichment; indexing. Scoped chat:<br>• video = long context;<br>• branch = subtree spans + notes;<br>• library = hybrid search + rerank.<br>Conversation forks; MCP `ask`; optional Anthropic provider | Citation accuracy ≥ target on eval questions |
| **M9** | Deep research | The commander plans, `dispatch_squad` sends infantry to read and extract, the commander synthesizes, infantry verify citations. Threads resume and branch; claim verdicts; research nodes on the map; MCP `fact_check`, `start_research`, `continue_research` | Every research statement cites a fetched source; budgets enforced |
| **M10** | v1 release | One hub binary with the UI embedded; MCP as one executable; pod templates; backup/export; security review | — |
| **v2** | Extension | WXT, Manifest V3 side panel reusing the React components; captions captured from the player you're watching; opt-in tab audio | Ingest works with no yt-dlp installed |
| **v3** | Hosted | Tenant-sharded hubs (SQLite per tenant + Litestream replication), object storage, accounts (OIDC), GPU pool behind a queue, metering → Paddle / Stripe Managed Payments, remote MCP (streamable HTTP + OAuth) + MCP Apps UI; extension as the only YouTube capture path; privacy, retention, DMCA agent | Legal review done; zero server-side YouTube requests (egress policy) |

---

## 10. Evaluation harness: how "complete and faithful" becomes a number

- **Dataset.** 8 videos in M2, growing to ~24 English videos, spread over the 4 video types.
  - Deliberately jargon-heavy (ML, medicine, law), multi-speaker, slide/code-dense and blunt, including China-sensitive topics.
  - The manifest stores IDs and gold annotations only; Creative Commons videos preferred.
- **Gold annotations:**
  - domain terms;
  - key atoms, including asides;
  - on-screen items;
  - `must_quote` statements;
  - checkable claims with known verdicts.
- **Metrics:**

  | Area | Metrics |
  |---|---|
  | Transcript | WER; **term error rate** |
  | Coverage | Key-point recall; aside retention; visual inclusion |
  | Faithfulness | Supported sentences; quote exactness; misattribution rate |
  | Anti-sanitization | Intensity preservation |
  | Form | Flow/readability rubric; length ratio |
  | Research | Citation accuracy; fact-check accuracy |
  | Operations | Minutes and $ per video-hour |

- **Gate.** A model or prompt change merges only if no metric regresses. `rreport eval` writes an HTML report with history.

---

## 11. Verification

- **Every commit (CI, no GPU):** layers L0–L3 on Linux (GCC + Clang), including the ASan/UBSan and TSan jobs.
- **Nightly:** L4, a CPU runtime integration with tiny models.
- **On the pod:**
  - **M0:** health, auth, model sync, self-stop, benchmark.
  - **M1:** a streamed completion longer than 100 s gets through the RunPod proxy; JSON-schema output with thinking off; tokens/s at 1/2/4 slots; peak VRAM; cold start.
  - **Every milestone:** `rreport ingest` + `rreport eval` on the eval set must meet that milestone's criteria.
- **Surfaces:**
  - Web UI: click a sentence → the player seeks to the right second.
  - Claude Code/Desktop via MCP: `ingest_video` → `get_report` → `retrieve`/`ask` on an idea branch.

---

## 12. Top risks and mitigations

| Risk | Mitigation |
|---|---|
| YouTube breaks yt-dlp (tokens, SABR streaming, JS challenges) | Captions-first; `doctor` + update prompts; polite rate limits; `CaptureSource` isolation; file/podcast inputs; extension capture in v2 |
| Legal exposure of the hosted service | Edge capture only; downloader never bundled; private libraries (no public sharing of full articles); no cross-tenant caches; raw media deleted; DMCA agent; legal review before v3 |
| Small-model fidelity ceiling | Closed-world missions; deterministic ledger; targeted repair; escalation to the commander; eval-selected models; honest coverage display |
| Sanitization: Qwen censors China topics, Gemma has safety tuning | `must_quote` verbatim checks; a dedicated eval slice; softening/refusal detection → per-role fallback (another open model, or Claude if enabled) |
| Attention-cache memory limits the army's size | Benchmark; 8-bit cache; small mission packets; SGLang/vLLM on big pods |
| GPU cost runaway | `PodManager` stop-on-empty, dead-man's switch, per-job budgets, cost shown per job |
| Pod proxy timeouts / pod loss mid-job | Always-streamed calls; idempotent jobs; content-addressed blobs; the hub is the only source of truth |
| llama-server image input still marked *experimental* | Pinned llama.cpp build; integration tests on real frames whenever the pin moves; fallback to a dedicated OCR model behind the same interface |
| sqlite-vec is pre-1.0 | Hidden behind a `VectorIndex` interface; usearch is the drop-in |
| MCP spec churn / no official C++ SDK | Thin TS adapter on the official SDK; all logic in C++ |
| Model and engine churn | Models are config; engine versions pinned; record/replay; eval gates |
| Scope explosion / C++ velocity | One end-to-end path first; measurable milestones; mature libraries; generated TS types |
| Google ships "good enough" | Compete on fidelity, completeness, research threads, ownership and openness |

---

## 13. First PRs in detail (M0 → M1)

**Order of work:**
1. **Decisions first.**
   - ADRs in `docs/adr/`:
     - 0001 hub/runtime split
     - 0002 C++ core, TS edges
     - 0003 GPU engines as processes
     - 0004 artifacts are the truth
     - 0005 closed-world atomization
     - 0006 citation markers
     - 0007 errors via `std::expected`
     - 0008 SQLite per library
     - 0009 timeline anchors
     - 0010 threads, not coroutines
     - 0011 commander/infantry
   - Also `docs/architecture.md`, `docs/glossary.md` and `README.md`.
2. **Build.**
   - `CMakeLists.txt`, `CMakePresets.json` (`dev` GCC 14, `dev-clang` with ASan+UBSan, `tsan`, `release`, `runtime-release`, and `speech-cuda` from M4).
   - `vcpkg.json` + `vcpkg-configuration.json`.
   - `cmake/Rr{Warnings,Sanitizers,Module,Embed}.cmake`. `rr_add_module(name DEPS …)` enforces the layering; `RrEmbed` compiles prompts, schemas and migrations into the binary.
3. **Core libraries:**
   - `src/core` (`result`, `error`, `strong_id`, `time`, `hash`, `secret`, `assert`, `thread_pool`, `limiter`);
   - `src/log`, `src/config` + `config/default.toml`;
   - `src/json` (schema registry, canonical serialization);
   - `src/db` (database, statement, migrator) + `migrations/0001_init.sql`;
   - `src/blob`, `src/proc`, `src/net` (HTTP client with retry, SSE).
4. **Runtime + pod:**
   - `src/runtime_api`, `src/supervisor` (engines, jobs, blobs, auth, profiles, routes);
   - `src/runtime_client` (+ `runpod_pod_manager`);
   - `apps/rreport-runtime`;
   - `docker/runtime/{Dockerfile, entrypoint.sh, models.toml, profiles/*.toml}`: CUDA 12.8 base; llama.cpp and whisper.cpp at pinned tags, built with `CMAKE_CUDA_ARCHITECTURES="80;86;89;90;120"`.
   - The entrypoint:
     1. checks `nvidia-smi`;
     2. picks a profile from the detected VRAM;
     3. syncs and verifies models on the `/workspace` volume;
     4. `exec`s the supervisor, which requires `RR_RUNTIME_TOKEN`.
   - `deploy/runpod/README.md`.
5. **LLM + army:**
   - `src/llm` (provider, OpenAI-compatible client, replay, client with `structured<T>`, prompt);
   - `src/agents` (mission, squad, dispatcher, escalation, battle log).
6. **Slice stages:**
   - `src/media` (yt-dlp, ffmpeg, json3/vtt captions, local file source);
   - `src/text` (normalize, sentences, tokens, span match);
   - `src/transcript` (from captions, from whisper JSON);
   - `src/pipeline` (stage, artifact store, executor) + `stages/{resolve,capture,transcribe,sectionize,atomize,compose,verify,render}`;
   - `src/fidelity` (markers, coverage, `checks/*`, repair);
   - `src/report/render_html` + `templates/article.html.inja`.
7. **Prompts and schemas:**
   - `prompts/{atomize,article_plan,compose_section,seam,repair_insert}.md.inja`;
   - `schemas/llm/atomize.json`;
   - `schemas/artifacts/{metadata,transcript,sections,atoms,article}.json`.
8. **CLI:** `apps/rreport` with `doctor`, `ingest`, `resume`, `jobs`, `report`, `serve`, `config` and `bench`.
9. **CI:**
   - `.github/workflows/ci.yml`: lint, gcc, clang with sanitizers, tsan;
   - `runtime-image.yml`: builds and pushes the CUDA image to GHCR on main and tags;
   - CC-licensed fixtures in `tests/`, and `eval/sets/dev-v0.toml`.

---

## 14. How we sell it: a SaaS with three doors

**The business is a SaaS.** Customers pay a monthly subscription, and we run everything: hubs, GPU pods, storage. **MCP is not an alternative to the SaaS; it is one of its doors.** Every door uses the same account, library and subscription:

| Door | What the customer does there | Why it matters |
|---|---|---|
| **Web app** | Reads Retold Articles, explores the Idea Map, runs research threads, manages the library | The flagship reading and research experience can't live in a chat window |
| **Browser extension** | One click on the video they're watching: capture (captions, plus audio and frames while watching), with the report beside the player | The only legal and reliable way to get YouTube content into a hosted service, because our servers never fetch YouTube |
| **MCP connector** (remote, OAuth) | Their library inside Claude, ChatGPT or Cursor: `retrieve`, `ask`, `start_research`, report widgets via MCP Apps | Distribution through the assistants' directories. The user's own model does the reasoning, which is cheap for us |

**Why we don't sell "an MCP" on its own:**
- MCP is a protocol, not a store. Accounts, OAuth, payments and the GPU backend are needed anyway, and together they are the SaaS.
- The directories bring discovery, not payment. Claude's directory lists remote servers that use OAuth; ChatGPT's apps link out to our checkout.
- An MCP-only customer still can't get YouTube content to us without the extension or a local helper.

**Pricing shape.**
- Subscription tiers by video-hours processed per month, at a fidelity tier: captions-only, audio, or audio + visual.
- Research credits on top.
- MCP access included in paid tiers.
- An optional developer plan (MCP/API only) is an easy add-on.
- Actual numbers are set after the M0 benchmark measures the real cost per video-hour.
- Billing goes through a merchant of record (Paddle or Stripe Managed Payments), which handles VAT and sales tax.

**Sequence.**
1. v1: your personal instance, which proves the quality.
2. v2: the extension, which is the capture device.
3. v3: public SaaS launch.
   - **Lead door (decided): the web app + extension.** They are polished first and marketed as the product.
   - The MCP connector ships at launch as a paid-plan feature.

---

## 15. Open decisions (defaults proposed; change any of them at approval)

1. **GPU profile.** Decided from the M0 benchmark table.
2. **Budget per video-hour.** Default target: ≤ $0.50 of pod time, shown per job.
3. **Web-search API for research** (M9). One provider in v1; default **Brave Search API**, behind an interface.
4. **Commander location.** Default: Qwen3.8-27B **on the pod**, for privacy and a flat cost. The hosted Qwen3.8 API is a config switch.
5. **SponsorBlock.** A cheap "sponsor segment" signal, but its data is licensed CC BY-NC-SA. Personal mode only, off in hosted.
