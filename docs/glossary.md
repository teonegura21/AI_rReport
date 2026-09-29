# Glossary

| Term | Meaning |
|---|---|
| **Analysis** | One run of the pipeline over a video with a fixed configuration snapshot (models, prompts, parameters). A video can have many; the UI shows the current one. |
| **Anchor** | A reference into a video by time: `{video_id, t0_ms, t1_ms, quote}`. Used by long-lived data (research, chats, verdicts) so it survives re-analysis (ADR-0009). |
| **Artifact** | The immutable, schema-validated JSON output of one stage, addressed by its content hash (ADR-0004). |
| **Atom** | One typed unit of information from the video: a claim, fact, number, example, anecdote, aside, joke, hedge, opinion, question, instruction, definition, reference or visual item. Written `A#`. |
| **Battle log** | The `missions` table: every infantry and commander mission with its inputs hash, model, tokens, latency, validation result and retries. |
| **Blob** | A file stored under its BLAKE3 content hash (audio, frames, payloads). |
| **Cache key** | The BLAKE3 hash of a stage's version, prompt/schema hashes, model id + quantization, parameters and input artifact hashes. Equal keys mean equal work. |
| **Commander** | The big model (Qwen3.8-27B by default): whole-picture judgment, writing, chat, research planning (ADR-0011). |
| **Context card / video brief** | A ~1.5K-token summary of speakers, genre, lexicon and section gists, given to every infantry mission. |
| **Dead-man's switch** | The runtime stops its own pod when the hub's heartbeats stop and no work is running. |
| **Engine** | A GPU process managed by the runtime supervisor (`llama-server`, `rr-speech-worker`). |
| **Filler** | A transcript sentence with no information, labelled from a closed list (disfluency, greeting, repetition, sponsor, decorative) and shown in the Fidelity Report for audit. |
| **Fidelity Ledger** | The bookkeeping that proves completeness. Ledger A maps every sentence and visual item to atoms or a filler label. Ledger B maps every atom to article sentences (ADR-0005). |
| **Fidelity tier** | How much of the video was actually processed: captions-only, audio, or audio + visual. Shown in every report header. |
| **Infantry** | The small parallel models (quantized Gemma 4) that execute narrow missions (ADR-0011). |
| **Lexicon** | The video's vocabulary: jargon, names, acronyms, numbers. Lexicon-0 is built by rules from metadata; lexicon-1 is refined by the LLM with on-screen text. |
| **Mission** | One typed order for one model call: role, prompt, input packet, output schema, validators, budget, priority, retry policy. |
| **`must_quote`** | An atom flag for statements that must appear verbatim (strong, controversial, profane or precise). It is the backbone of anti-sanitization. |
| **Profile (GPU profile)** | Runtime configuration for one GPU class: which engines stay resident, slot counts, quantizations. |
| **Projection** | A relational table derived from artifacts, for querying. It can always be rebuilt. |
| **ROVER** | "Recognizer Output Voting Error Reduction": word-by-word voting across several speech recognizers (NIST). |
| **Seam pass** | After section drafts are written in parallel, the commander rewrites only the boundary paragraphs so the article flows. |
| **Squad** | A fan-out/fan-in group of missions run in parallel with per-result validation and retries. |
| **Stage** | One step of the per-video pipeline, idempotent and resumable, producing one artifact. |
| **Supervisor** | `rreport-runtime`: the CUDA-free process on the pod that authenticates requests, manages engines and proxies model calls. |
| **VKM** | Video Knowledge Model: sections → atoms → idea tree → entities/terms → claims → timeline/narrative. Every view is a projection of it. |
