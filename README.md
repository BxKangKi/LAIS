# LAIS — Local AI Studio

LAIS is a C++20 local application for multilingual chat, coding, image understanding,
image generation, video generation and music generation. This configuration targets
an NVIDIA CUDA GPU with **16 GiB VRAM and 32 GiB system RAM**, with GPU model offload
to the CPU disabled.

## Separate model modes

`configs/models.json` uses catalog version **2026103103**, with **45 mode-scoped
assets** and **26 generation profiles**. The only model modes are `standard` and
`uncensored`. Every entry has an explicit `tiers` array containing `eco`, `high`,
or both. Components and installation dependencies belong to one mode; inference
never substitutes the opposite mode when an assignment is absent.

| Task | Standard | Uncensored |
| --- | --- | --- |
| Chat / coding, High | Qwen3.8 27B IQ3_S | Huihui Qwen3.8 27B abliterated IQ3_S |
| Chat / coding / vision, Eco | Qwen3.5 9B Q6_K | HauhauCS Qwen3.5 9B Aggressive Uncensored Q6_K |
| Vision, High | Qwen3.5 9B Q8 + matching F16 projector | HauhauCS Qwen3.5 9B Uncensored Q8 + its publisher's BF16 projector |
| Images, High / Eco | FLUX.3 Klein Base Q8 / Klein Q6, standard Qwen3-VL encoder and FLUX.2 VAE | **T8 FLUX.1-dev Abliterated V2 diffusion weights**, Q6 / Q4, with T5, CLIP and matching VAE |
| Video, High / Eco | Wan 2.2 TI2V 5B Q8 / Q5 + UMT5 + Wan 2.2 VAE | NSFW Wan 1.3B experimental E14 FP16 + NSFW UMT5 + Wan 2.1 VAE; different resolution presets |
| Music, High / Eco | ACE-Step 1.5 XL SFT Q8 + 4B LM / Turbo Q8 + 1.7B LM, application content filter enabled | The same original ACE-Step checkpoints in a separate mode directory, with the application content filter disabled |

The default uncensored image pipeline changes the **diffusion backbone itself**.
An uncensored text encoder by itself does not establish that a diffusion model is
uncensored. The user-provided MOSS SFT + focused NSFW LoRA bundle and additional
FLUX.3 encoders remain available as optional catalog assets. MOSS profiles include
the required tokenizer, runtime adapter and native sampling settings. To activate
one, assign `image_moss_uncensored_{eco,high}` to the corresponding
`image_uncensored_{eco,high}` profile in the installed catalog.

**Music qualification:** no independently verified, separately abliterated ACE-Step
checkpoint was identified. Its uncensored entry means the native, unfiltered
application pipeline, not newly modified music weights. These assets explicitly
record `uncensoring_status: not_a_separately_abliterated_checkpoint`. The two music
modes use `music/standard` and `music/uncensored` directories, so removing one mode
does not delete files belonging to the other. VAEs and tokenizers are numeric
components; their mode membership does not imply that a refusal policy was removed.
Publisher claims of ablation or NSFW fine-tuning do not guarantee unrestricted
outputs for every prompt or identical quality to the standard model.

## GPU memory policy

- llama.cpp: CUDA0, all model layers on the GPU, automatic fitting off, Q8 KV cache,
  one 8192-token context and small prompt batches. Vision uses the matching GPU projector.
- stable-diffusion.cpp: parameters and compute on CUDA, automatic fitting off,
  flash attention and tiled VAE decoding. Each model family gets its own encoders,
  VAE, sampling settings and size limits.
- ACE-Step: sequential CUDA language-model and synthesis processes, with chunked VAE decoding.
- Auto/High select High only when its memory threshold is met. A detected GPU
  allocation failure retries **Eco in the same mode**. If Eco cannot fit, generation
  reports an error; it does not start CPU model offloading.

VRAM thresholds are conservative routing estimates, not measurements on every GPU.
Host RAM is still needed for the program, model loading, file mappings, operating-system
caches and media. A zero-DRAM-use claim or prevention of OS/driver paging is not implied.

## Interface language and command editing

The language selector is available on both the sign-in screen and the main web page.
It switches between **English** and **한국어** and remembers the browser's choice.
`configs/en-us.lang` and `configs/ko-kr.lang` are UTF-8 JSON dictionaries with matching
105-key sets. Edit the installed files to change labels; bundles are read on language
selection. Only the interface changes: conversation content and model language
settings are preserved, and models continue to accept multilingual input.

The interactive CLI supports Left/Right, Home/End, Delete/Backspace, Up/Down history,
insertion in the middle of a command, UTF-8 text, Ctrl+A/E and Ctrl+Left/Right.
Ctrl+W deletes a word, Ctrl+U/K delete before/after the cursor and Ctrl+C clears input.

## Stop generation

While a request is running, **Send** changes to **■ Stop generating**
(**■ 생성 중지** in Korean). Click it to cancel that request. **Stopping…** remains
visible until the server confirms cleanup; then Send becomes available again.
Enter does not submit another request while one is active, and text typed for the
next request is preserved.

Stop works for chat, coding, image analysis, images, videos and music, including
LLM startup and queued requests. It cancels the local LLM request and unloads its
server, or terminates the request's generator/converter process tree. Cancelled
exchanges are not appended to saved conversation history. Since responses are
currently returned as a complete JSON result, stopping does not return a partial
answer. A response already committed to history is kept if completion wins the race.
If the Stop request fails, the UI reports the failure and lets you try again.

Rebuild the C++ application after updating these sources and restart the running
LAIS service. Updating only the language/configuration files does not install the
button or the server cancellation behavior. On Windows, use `build.bat --rebuild`.

API clients may send a unique `request_id` (8–128 ASCII letters, digits, `_` or `-`)
on generation requests, then POST `{"request_id":"<same-id>"}` to
`/api/generation/stop` using the same authenticated session. IDs cannot be reused
within the retention window. The cancellation acknowledgement returns
`status: "stopping"`; the original generation request returns `cancelled: true`
and `status: "stopped"` after the engine has stopped. A late Stop returns
`cancelled: false` and `status: "completed"`. Clients omitting the ID still work.

The new regressions run without downloading models or requiring a GPU:

```bash
python3 tests/run_generation_stop.py  # Linux, Python 3 and g++
node tests/run_generation_ui.js
```

They exercise authenticated HTTP cancellation with mock engines, LLM startup,
queued requests, session isolation, child-process termination, normal requests
after cancellation, late completion and UI state recovery. Actual GPU inference
and the Windows process-job path require validation on the target machine.

## Build and install

Rebuild the application and CUDA engines for this catalog. Windows requires a
Visual Studio x64 developer environment; Linux/WSL requires a C++20 toolchain,
CMake, Ninja, Git, curl and CUDA. The build scripts may install missing dependencies.

```bat
build.bat --rebuild
build\lais.exe models
build\lais.exe install uncensored image high
build\lais.exe serve
```

```bash
bash build.sh --rebuild
./build/lais models
./build/lais install uncensored image high
./build/lais serve
```

Use `install standard`, `install uncensored`, `install standard llm eco`, or an exact
model key to limit downloads. `install all` includes both modes and optional assets.
Compound selectors are combined with AND; required same-mode components are included
automatically. Running without a command opens the editable interactive shell.
Linux should run as the normal user, as required by the existing application guard.

An older catalog is backed up as `models.json.pre-2026103103.bak` during migration.
Existing settings and same-version catalog edits are preserved. The new `.lang`
files are included in seed copying, installation, restore and generated bundles.
Changing only `models.json` is insufficient for this update: the coordinator,
language endpoint and CLI editor also require the new application build.

The delivered source archive includes a Linux application build, source, tests and
configs. It does not include model weights or CUDA inference-engine binaries.
Windows users must build their executable locally.

## Primary sources

Pinned revisions, exact file sizes and hashes are recorded in `configs/models.json`.

- [Standard Qwen3.8 GGUF](https://huggingface.co/unsloth/Qwen3.8-27B-GGUF), [Huihui abliterated GGUF](https://huggingface.co/huihui-ai/Huihui-Qwen3.8-27B-abliterated-GGUF)
- [Standard Qwen3.5 GGUF](https://huggingface.co/unsloth/Qwen3.5-9B-GGUF), [HauhauCS aggressive uncensored checkpoint and projector](https://huggingface.co/HauhauCS/Qwen3.5-9B-Uncensored-HauhauCS-Aggressive)
- [T8 abliterated diffusion GGUFs](https://huggingface.co/t8star/flux.1-dev-abliterated-V2-GGUF), [abliterated FLUX component bundle](https://huggingface.co/georgesung/flux.1-dev-abliterated-merged)
- [Standard FLUX.3 Base GGUF](https://huggingface.co/leejet/flux-3-image-klein-base-GGUF), [FLUX.3 TE modification scope](https://huggingface.co/hellmans/FLUX-3-Image-Klein-TE-Uncensored-GGUF)
- [NSFW Wan E14 checkpoint](https://huggingface.co/NSFW-API/NSFW_Wan_1.3b), [NSFW Wan UMT5](https://huggingface.co/NSFW-API/NSFW-Wan-UMT5-XXL), [Wan 2.1 components](https://huggingface.co/Comfy-Org/Wan_2.1_ComfyUI_repackaged)
- [ACE-Step GGUFs](https://huggingface.co/Serveurperso/ACE-Step-1.5-GGUF), [native ACE-Step engine](https://github.com/ServeurpersoCom/acestep.cpp), [native diffusion engine](https://github.com/leejet/stable-diffusion.cpp)
