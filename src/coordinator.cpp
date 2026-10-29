// ==============================================================================
// LAIS - Catalog-driven multimodal inference for 16 GiB VRAM / 32 GiB RAM.
// Each generation tier selects its matching encoder, VAE and sampling preset.
// SPDX-License-Identifier: MIT
// ==============================================================================
#include "common.hpp"
#include <cerrno>

namespace lais {

// Own the execution lock until cancellation has stopped this request's LLM.
// A queued request can be cancelled without ever acquiring the engine.
class InferenceCoordinator::ExecutionGuard {
public:
    explicit ExecutionGuard(InferenceCoordinator& owner)
        : owner_(owner), lock_(owner.exec_mu_, std::defer_lock) {
        while (!lock_.try_lock()) {
            generation_checkpoint();
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        // Check before installing the guard: a cancelled queued request must not
        // unload the previous request's cached LLM.
        generation_checkpoint();
    }
    ~ExecutionGuard() {
        if (lock_.owns_lock() && generation_cancelled()) {
            GenerationScope cleanup(nullptr);
            owner_.stop_llama_server_locked();
            owner_.set_active_badge("None (Idle - 0 MB VRAM)");
            trim_process_memory();
        }
    }
    void unlock() { lock_.unlock(); }
private:
    InferenceCoordinator& owner_;
    std::unique_lock<std::mutex> lock_;
};

static bool hf_auth_available() {
    const char* token = std::getenv("HF_TOKEN");
    return token && *token;
}

static long profile_minimum(const json::Value& profile, std::string_view key, long setting) {
    return std::max(setting, profile.get_int(key, setting));
}

static std::string sd_memory_options(const Config& cfg) {
    std::ostringstream options;
    // Keep weights and compute on CUDA. A failed fit must not silently become CPU inference.
    options << " --backend cuda0 --params-backend cuda0 --auto-fit off --vae-tiling --flash-attn on"
            << " --max-vram -" << (std::max(2048L, cfg.vram_reserve_mb) / 1024.0);
    return options.str();
}

static bool supports_modern_sd(const fs::path& binary) {
    if (binary.empty()) return false;
    static std::mutex cache_mu;
    static std::map<std::string, bool> cache;
    const std::string key = binary.string();
    {
        std::lock_guard<std::mutex> lk(cache_mu);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }
    const std::string help = capture_cmd(shell_quote(key) + " --help 2>&1");
    const bool ok = help.find("--llm") != std::string::npos &&
                    help.find("--params-backend") != std::string::npos &&
                    help.find("--max-vram") != std::string::npos;
    {
        std::lock_guard<std::mutex> lk(cache_mu);
        cache[key] = ok;
    }
    return ok;
}

static bool local_http_request(
    int port,
    std::string_view method,
    std::string_view path,
    std::string_view body,
    int timeout_sec,
    std::string& out_body) {

    out_body.clear();
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) return false;
    struct SocketGuard {
        socket_t fd;
        ~SocketGuard() { close_socket(fd); }
    } socket_guard{s};

    generation_checkpoint();

#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(std::max(1, timeout_sec) * 1000);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    struct timeval tv{};
    tv.tv_sec = std::max(1, timeout_sec);
    tv.tv_usec = 0;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        return false;
    }

    // Poll nonblocking I/O so cancellation interrupts prompt uploads and the
    // long non-streaming completion, rather than waiting for the HTTP timeout.
#ifdef _WIN32
    u_long nonblocking = 1;
    if (::ioctlsocket(s, FIONBIO, &nonblocking) != 0) return false;
#else
    int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0 || ::fcntl(s, F_SETFL, flags | O_NONBLOCK) != 0) return false;
#endif
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, timeout_sec));
    auto wait_ready = [&](bool writing) {
        for (;;) {
            generation_checkpoint();
            if (std::chrono::steady_clock::now() >= deadline) return false;
            fd_set ready;
            FD_ZERO(&ready); FD_SET(s, &ready);
            timeval pause{0, 100000};
            int rc = ::select(static_cast<int>(s) + 1, writing ? nullptr : &ready,
                              writing ? &ready : nullptr, nullptr, &pause);
            if (rc > 0) return true;
            if (rc < 0) return false;
        }
    };
    auto would_block = []() {
#ifdef _WIN32
        const int error = ::WSAGetLastError();
        return error == WSAEWOULDBLOCK || error == WSAEINTR;
#else
        return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR;
#endif
    };

    std::ostringstream req;
    req << method << " " << path << " HTTP/1.0\r\n"
        << "Host: 127.0.0.1:" << port << "\r\n"
        << "Connection: close\r\n";
    if (!body.empty()) {
        req << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n";
    }
    req << "\r\n";
    std::string payload = req.str();
    payload.append(body.data(), body.size());

    std::size_t sent = 0;
    while (sent < payload.size()) {
        if (!wait_ready(true)) return false;
#ifdef _WIN32
        int n = ::send(s, payload.data() + sent, static_cast<int>(std::min<std::size_t>(65536, payload.size() - sent)), 0);
#else
        ssize_t n = ::send(s, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
#endif
        if (n < 0 && would_block()) continue;
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }

    std::string raw;
    char buf[8192];
    while (true) {
        if (!wait_ready(false)) return false;
#ifdef _WIN32
        int n = ::recv(s, buf, sizeof(buf), 0);
#else
        ssize_t n = ::recv(s, buf, sizeof(buf), 0);
#endif
        if (n < 0 && would_block()) continue;
        if (n < 0) return false;
        if (n == 0) break;
        raw.append(buf, static_cast<std::size_t>(n));
    }
    generation_checkpoint();

    std::size_t hdr_end = raw.find("\r\n\r\n");
    if (hdr_end == std::string::npos) return false;
    std::string_view status_line(raw.data(), std::min<std::size_t>(raw.size(), 32));
    if (status_line.find(" 200 ") == std::string_view::npos) return false;
    out_body = raw.substr(hdr_end + 4);
    return true;
}

static bool memory_allocation_failed(const fs::path& log_path) {
    const std::string log = to_lower_ascii(read_file_bytes(log_path));
    for (const char* marker : {"out of memory", "not enough memory", "failed to allocate",
                               "failed to alloc", "cuda_error_out_of_memory", "cudamalloc failed"}) {
        if (log.find(marker) != std::string::npos) return true;
    }
    return false;
}

static const json::Value& mode_profile(const Config& cfg, std::string_view modality, bool uncensored, std::string_view tier) {
    return cfg.generation_profile(std::string(modality) + (uncensored ? "_uncensored_" : "_standard_") + std::string(tier));
}

static fs::path profile_path(const Config& cfg, const json::Value& profile, std::string_view component) {
    const auto* role_map = profile.get("roles");
    if (!role_map) return {};
    const std::string role = role_map->get_string(component);
    const std::string filename = cfg.role_filename(role);
    const std::string mode = profile.get_string("mode");
    if (filename.empty()) return {};
    for (const auto& asset : cfg.models) {
        if (asset.key == role && asset.filename == filename && asset.safety == mode)
            return fs::path(cfg.models_dir) / asset.subdir / filename;
    }
    return {}; // A missing or wrong-mode assignment is never replaced by another mode.
}

static double profile_number(const json::Value& profile, std::string_view key, double fallback) {
    const auto* value = profile.get(key);
    return value ? value->as_double(fallback) : fallback;
}

void split_thinking_and_reply(std::string_view raw_text, std::string& out_thinking, std::string& out_reply) {
    out_thinking.clear();
    out_reply.clear();

    std::string work(raw_text);

    auto extract_paired = [&](std::string_view open_tag, std::string_view close_tag) {
        while (true) {
            std::size_t p1 = work.find(open_tag);
            if (p1 == std::string::npos) break;
            std::size_t content_start = p1 + open_tag.size();
            std::size_t p2 = work.find(close_tag, content_start);
            if (p2 != std::string::npos) {
                std::string chunk = trim_copy(std::string_view(work).substr(content_start, p2 - content_start));
                if (!chunk.empty()) {
                    if (!out_thinking.empty()) out_thinking += "\n\n";
                    out_thinking += chunk;
                }
                work.erase(p1, (p2 + close_tag.size()) - p1);
            } else {
                std::string chunk = trim_copy(std::string_view(work).substr(content_start));
                if (!chunk.empty()) {
                    if (!out_thinking.empty()) out_thinking += "\n\n";
                    out_thinking += chunk;
                }
                work.erase(p1);
                break;
            }
        }
    };

    extract_paired("<think>", "</think>");
    extract_paired("<THINK>", "</THINK>");
    extract_paired("<|begin_of_thought|>", "<|end_of_thought|>");

    if (std::size_t close_pos = work.find("</think>"); close_pos != std::string::npos) {
        std::string before = trim_copy(std::string_view(work).substr(0, close_pos));
        if (!before.empty()) {
            if (!out_thinking.empty()) out_thinking += "\n\n";
            out_thinking += before;
        }
        work.erase(0, close_pos + 8);
    }

    auto remove_literal = [](std::string& s, std::string_view tok) {
        std::size_t pos = 0;
        while ((pos = s.find(tok, pos)) != std::string::npos) {
            s.erase(pos, tok.size());
        }
    };
    remove_literal(work, "<think>");
    remove_literal(work, "</think>");
    remove_literal(work, "<THINK>");
    remove_literal(work, "</THINK>");
    remove_literal(work, "<answer>");
    remove_literal(work, "</answer>");
    remove_literal(work, "<|begin_of_solution|>");
    remove_literal(work, "<|end_of_solution|>");

    out_reply = trim_copy(work);
    if (out_reply.empty() && !out_thinking.empty()) {
        out_reply = out_thinking;
    }
}

static bool prompt_requests_human_subject(std::string_view sv) {
    std::string low = to_lower_ascii(sv);
    static const std::vector<std::string_view> kNoPerson = {
        "사람 없이", "사람없이", "인물 없이", "인물없이", "배경만", "우주 배경만",
        "no people", "no person", "no human", "no humans", "without people", "without person", "uninhabited"
    };
    for (std::string_view np : kNoPerson) {
        if (low.find(np) != std::string::npos) return false;
    }

    static const std::vector<std::string_view> kHumanWords = {
        "person", "people", "woman", "women", "girl", "man", "men", "boy", "lady",
        "human", "portrait", "face", "character", "warrior", "astronaut", "knight",
        "nude", "naked", "topless", "bikini", "lingerie", "sex", "erotic", "nsfw",
        "사람", "인물", "여자", "여성", "소녀", "남자", "남성", "소년", "초상화", "얼굴", "캐릭터",
        "나체", "누드", "알몸", "성인", "야한", "섹스", "벗은", "노출"
    };
    for (std::string_view hw : kHumanWords) {
        if (low.find(hw) != std::string::npos) return true;
    }
    return false;
}

InferenceCoordinator::InferenceCoordinator(const Config& cfg) : cfg_(cfg) {
    cfg_.ensure_directories();
    cached_stats_ = query_memory_stats_sync();
    watchdog_thread_ = std::thread([this]() { this->background_maintenance_loop(); });
}

InferenceCoordinator::~InferenceCoordinator() {
    stopping_.store(true, std::memory_order_relaxed);
    if (watchdog_thread_.joinable()) watchdog_thread_.join();
    std::lock_guard<std::mutex> lk(exec_mu_);
    stop_llama_server_locked();
}

MemoryStats InferenceCoordinator::get_cached_stats() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return cached_stats_;
}

std::string InferenceCoordinator::active_llm_model() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return active_llm_name_;
}

void InferenceCoordinator::force_purge_all_memory() {
    std::lock_guard<std::mutex> lk(exec_mu_);
    stop_llama_server_locked();
    purge_dram_and_pagecache(cfg_.models_dir);
    MemoryStats fresh = query_memory_stats_sync();
    std::lock_guard<std::mutex> slk(state_mu_);
    cached_stats_ = fresh;
}

std::int64_t InferenceCoordinator::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void InferenceCoordinator::touch_activity() {
    last_activity_ms_.store(now_ms(), std::memory_order_relaxed);
}

void InferenceCoordinator::set_active_badge(const std::string& label) {
    std::lock_guard<std::mutex> slk(state_mu_);
    active_llm_name_ = label;
}

void InferenceCoordinator::background_maintenance_loop() {
    touch_activity();
    while (!stopping_.load(std::memory_order_relaxed) &&
           !g_shutdown_requested.load(std::memory_order_relaxed)) {
        for (int i = 0; i < 20; ++i) {
            if (stopping_.load(std::memory_order_relaxed) ||
                g_shutdown_requested.load(std::memory_order_relaxed)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        MemoryStats fresh = query_memory_stats_sync();
        {
            std::lock_guard<std::mutex> slk(state_mu_);
            cached_stats_ = fresh;
        }
        if (exec_mu_.try_lock()) {
            if (llama_pid_ > 0) {
                std::int64_t idle_ms = now_ms() - last_activity_ms_.load(std::memory_order_relaxed);
                if (idle_ms >= cfg_.idle_timeout_sec * 1000L) {
                    log_info("Unloading idle LLM after " + std::to_string(cfg_.idle_timeout_sec) +
                             "s of inactivity to keep VRAM & DRAM at 0 MB...");
                    stop_llama_server_locked();
                    purge_dram_and_pagecache(cfg_.models_dir);
                }
            } else {
                trim_process_memory();
            }
            exec_mu_.unlock();
        }
    }
}

void InferenceCoordinator::ensure_engine_binaries_ready_locked() {
    generation_checkpoint();
    bool have_all = !find_llama_server_binary().empty() &&
                    !find_sd_binary().empty() &&
                    !find_ace_lm_binary().empty() &&
                    !find_ace_synth_binary().empty();
    if (have_all) return;

    extract_embedded_engines(cfg_, nullptr, true);

    std::error_code ec;
    std::string sfx = exe_suffix();
    if (fs::exists(cfg_.engines_dir, ec)) {
        const std::vector<std::string> wanted = {
            "llama-server" + sfx,
            "llama-cli" + sfx,
            "sd-cli" + sfx,
            "sd" + sfx,
            "ace-lm" + sfx,
            "ace-synth" + sfx
        };
        for (auto it = fs::recursive_directory_iterator(cfg_.engines_dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec || !it->is_regular_file(ec)) continue;
            std::string fname = it->path().filename().string();
            for (const auto& w : wanted) {
                if (fname == w) {
                    fs::path dst = fs::path(cfg_.bin_dir) / fname;
                    if (!fs::exists(dst, ec)) {
                        fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
#ifndef _WIN32
                        ::chmod(dst.c_str(), 0755);
#endif
                    }
                }
            }
#ifndef _WIN32
            if (fname.find(".so") != std::string::npos) {
                fs::path dst = fs::path(cfg_.bin_dir) / fname;
                if (!fs::exists(dst, ec)) {
                    fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
                }
            }
#else
            if (fname.ends_with(".dll")) {
                fs::path dst = fs::path(cfg_.bin_dir) / fname;
                if (!fs::exists(dst, ec)) {
                    fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
                }
            }
#endif
        }
    }
}

bool InferenceCoordinator::ensure_model_file_ready_locked(const fs::path& model_path, std::string_view filename) {
    generation_checkpoint();
    std::error_code ec;
    if (!filename.empty() && fs::exists(model_path, ec) && fs::is_regular_file(model_path, ec)) {
        return true;
    }
    if (filename.empty()) return false;

    if (filename == cfg_.role_filename("video_dit_eco") ||
        filename == cfg_.role_filename("video_std_dit_eco")) {
        maybe_quantize_wan_eco(cfg_);
        if (fs::exists(model_path, ec) && fs::is_regular_file(model_path, ec)) return true;
    }

    for (const auto& m : cfg_.models) {
        const fs::path catalog_path = fs::path(cfg_.models_dir) / m.subdir / m.filename;
        if (m.filename == filename && catalog_path.lexically_normal() == model_path.lexically_normal() && !m.url.empty()) {
            if (m.requires_auth && !hf_auth_available()) {
                log_warn("Accept the model's Hugging Face license and set HF_TOKEN before downloading " + m.filename);
                return false;
            }
            set_active_badge("Downloading " + m.filename + " ...");
            bool ok = download_hf_asset(m.url, model_path, m.key + " (" + m.filename + ")", m.approx_gb);
            set_active_badge("None (Idle - 0 MB VRAM)");
            return ok && fs::exists(model_path, ec);
        }
    }
    return false;
}

std::string InferenceCoordinator::translate_media_prompt_to_english_locked(
    const std::string& raw_prompt,
    bool uncensored,
    bool& out_was_translated) {

    out_was_translated = false;
    std::string trimmed = trim_copy(raw_prompt);
    if (trimmed.empty()) return trimmed;

    bool wants_human   = prompt_requests_human_subject(trimmed);

    const fs::path eco = profile_path(cfg_, mode_profile(cfg_, "text", uncensored, "eco"), "model");
    const fs::path high = profile_path(cfg_, mode_profile(cfg_, "text", uncensored, "high"), "model");
    std::vector<fs::path> primary_candidates;
    if (!eco.empty()) primary_candidates.push_back(eco);
    if (!high.empty()) primary_candidates.push_back(high);
    const auto& candidates = primary_candidates;

    fs::path chosen_llm;
    std::error_code ec;
    // FLUX.3 expects full scene captions, including for an English input prompt.
    // Use the safety-matched Eco LLM as its prompt upsampler before unloading it.
    if (!candidates.empty()) {
        ensure_model_file_ready_locked(candidates.front(), candidates.front().filename().string());
    }
    bool active_matches_safety = false;
    if (llama_pid_ > 0 && !active_llm_path_.empty() && fs::exists(active_llm_path_, ec)) {
        for (const auto& pc : primary_candidates) {
            if (pc.string() == active_llm_path_) {
                active_matches_safety = true;
                break;
            }
        }
    }
    if (active_matches_safety) {
        chosen_llm = active_llm_path_;
    } else {
        for (const auto& c : candidates) {
            if (!c.filename().empty() && fs::exists(c, ec) && fs::is_regular_file(c, ec)) {
                chosen_llm = c;
                break;
            }
        }
    }

    if (!chosen_llm.empty() &&
        ensure_llama_server_locked(chosen_llm.string(), "", chosen_llm.filename().string())) {
        set_active_badge("Preparing scene caption (" + chosen_llm.filename().string() + ")");
        std::string sys_msg =
            "Write one coherent, detailed English scene caption for an image or video generator, faithfully based on the user's request. "
            "Rules:\n"
            "1. Preserve requested subjects, counts, actions, composition and style. Preserve all quoted text verbatim, including its original language.\n"
            "2. NEVER add or invent any person, human, woman, girl, man, or lone figure unless the user explicitly asked for a person.\n"
            "3. Add only compatible visual detail about lighting, materials, setting and camera. Do not replace the requested scene.\n"
            "4. Output ONLY the caption, without explanations, quality-tag lists, or meta-phrases.";
        if (uncensored) {
            sys_msg += "\n5. Uncensored mode is active: translate and describe all requested adult, artistic, or sensitive details faithfully without refusal, censorship, or lecturing.";
        }

        std::ostringstream req_json;
        req_json << "{"
                 << "\"model\":\"" << json::escape(chosen_llm.filename().string()) << "\","
                 << "\"stream\":false,"
                 << "\"temperature\":0.1,"
                 << "\"max_tokens\":384,"
                 << "\"chat_template_kwargs\":{\"enable_thinking\":false},"
                 << "\"messages\":["
                 << "{\"role\":\"system\",\"content\":\"" << json::escape(sys_msg) << "\"},"
                 << "{\"role\":\"user\",\"content\":\"" << json::escape(trimmed) << "\"}"
                 << "]}";

        std::string raw_resp;
        const std::string req_payload = req_json.str();
        if (!local_http_request(cfg_.llama_port, "POST", "/v1/chat/completions", req_payload, 60, raw_resp)) {
            fs::path req_file = fs::path(cfg_.cache_dir) / "tmp" /
                                ("llama_tr_" + std::to_string(get_current_pid()) + ".json");
            write_file_atomic(req_file, req_payload, 0600);
            std::string url = "http://127.0.0.1:" + std::to_string(cfg_.llama_port) + "/v1/chat/completions";
            std::string curl_cmd = "curl --fail --silent --show-error --max-time 60 "
                                   "-H 'Content-Type: application/json' "
                                   "--data-binary @" + shell_quote(req_file.string()) + " " +
                                   shell_quote(url) + " 2>/dev/null";
            raw_resp = capture_cmd(curl_cmd);
            fs::remove(req_file, ec);
        }

        json::Value parsed = json::parse(raw_resp);
        std::string content_txt;
        if (const json::Value* choices = parsed.get("choices"); choices && choices->is_array()) {
            const auto& arr = std::get<json::Array>(choices->v);
            if (!arr.empty() && arr[0].is_object()) {
                if (const json::Value* msg = arr[0].get("message"); msg && msg->is_object()) {
                    content_txt = msg->get_string("content", "");
                }
            }
        }
        std::string dummy_think, cleaned_en;
        split_thinking_and_reply(content_txt, dummy_think, cleaned_en);
        cleaned_en = trim_copy(cleaned_en);
        if (!cleaned_en.empty()) {
            if (!wants_human) {
                cleaned_en += " The scene contains no people.";
            }
            out_was_translated = true;
            log_info("Prepared scene caption: \"" + cleaned_en + "\"");
            return cleaned_en;
        }
    }

    return trimmed;
}

static fs::path search_binary_recursive(const fs::path& root, const std::vector<std::string>& names) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return {};
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec || !it->is_regular_file(ec)) continue;
        std::string fn = it->path().filename().string();
        for (const auto& n : names) {
            if (fn == n) return it->path();
        }
    }
    return {};
}

fs::path InferenceCoordinator::find_llama_server_binary() const {
    std::string sfx = exe_suffix();
    std::vector<fs::path> candidates = {
        fs::path(cfg_.bin_dir) / ("llama-server" + sfx),
        fs::path(cfg_.engines_dir) / ("llama.cpp/build/bin/Release/llama-server" + sfx),
        fs::path(cfg_.engines_dir) / ("llama.cpp/build/bin/llama-server" + sfx),
        fs::path(cfg_.engines_dir) / ("llama.cpp/build/llama-server" + sfx),
        "/usr/local/bin/llama-server"
    };
    std::error_code ec;
    for (const auto& c : candidates) {
        if (fs::exists(c, ec) && fs::is_regular_file(c, ec)) return c;
    }
    return search_binary_recursive(fs::path(cfg_.engines_dir) / "llama.cpp", {"llama-server" + sfx});
}

fs::path InferenceCoordinator::find_sd_binary() const {
    std::string sfx = exe_suffix();
    std::vector<fs::path> candidates = {
        fs::path(cfg_.bin_dir) / ("sd-cli" + sfx),
        fs::path(cfg_.bin_dir) / ("sd" + sfx),
        fs::path(cfg_.engines_dir) / ("stable-diffusion.cpp/build/bin/Release/sd-cli" + sfx),
        fs::path(cfg_.engines_dir) / ("stable-diffusion.cpp/build/bin/sd-cli" + sfx),
        fs::path(cfg_.engines_dir) / ("stable-diffusion.cpp/build/bin/sd" + sfx),
        fs::path(cfg_.engines_dir) / ("stable-diffusion.cpp/build/sd-cli" + sfx),
        "/usr/local/bin/sd-cli"
    };
    std::error_code ec;
    for (const auto& c : candidates) {
        if (fs::exists(c, ec) && fs::is_regular_file(c, ec)) return c;
    }
    return search_binary_recursive(
        fs::path(cfg_.engines_dir) / "stable-diffusion.cpp",
        {"sd-cli" + sfx, "sd" + sfx});
}

fs::path InferenceCoordinator::find_ace_lm_binary() const {
    std::string sfx = exe_suffix();
    std::vector<fs::path> candidates = {
        fs::path(cfg_.bin_dir) / ("ace-lm" + sfx),
        fs::path(cfg_.engines_dir) / ("acestep.cpp/build/Release/ace-lm" + sfx),
        fs::path(cfg_.engines_dir) / ("acestep.cpp/build/ace-lm" + sfx),
        fs::path(cfg_.engines_dir) / ("acestep.cpp/build/bin/ace-lm" + sfx),
        "/usr/local/bin/ace-lm"
    };
    std::error_code ec;
    for (const auto& c : candidates) {
        if (fs::exists(c, ec) && fs::is_regular_file(c, ec)) return c;
    }
    return search_binary_recursive(fs::path(cfg_.engines_dir) / "acestep.cpp", {"ace-lm" + sfx});
}

fs::path InferenceCoordinator::find_ace_synth_binary() const {
    std::string sfx = exe_suffix();
    std::vector<fs::path> candidates = {
        fs::path(cfg_.bin_dir) / ("ace-synth" + sfx),
        fs::path(cfg_.engines_dir) / ("acestep.cpp/build/Release/ace-synth" + sfx),
        fs::path(cfg_.engines_dir) / ("acestep.cpp/build/ace-synth" + sfx),
        fs::path(cfg_.engines_dir) / ("acestep.cpp/build/bin/ace-synth" + sfx),
        "/usr/local/bin/ace-synth"
    };
    std::error_code ec;
    for (const auto& c : candidates) {
        if (fs::exists(c, ec) && fs::is_regular_file(c, ec)) return c;
    }
    return search_binary_recursive(fs::path(cfg_.engines_dir) / "acestep.cpp", {"ace-synth" + sfx});
}

// Prefer the quality tier when memory permits; reasoning is an independent switch.
InferenceCoordinator::RouteDecision InferenceCoordinator::select_text_model_locked(
    std::string_view task_mode,
    bool reasoning_on,
    bool has_image,
    std::string_view tier_override,
    bool uncensored) {

    (void)reasoning_on;
    cfg_.sync_seed_configs();
    cfg_.load_config_file();

    RouteDecision d;
    d.modality = task_mode == "coder" ? "coder" : "chat";
    d.mem_before = query_memory_stats_sync();
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        cached_stats_ = d.mem_before;
    }
    const std::string eff_tier = tier_override.empty() || tier_override == "auto"
        ? cfg_.default_tier : std::string(tier_override);
    const std::string kind = has_image ? "vision" : (task_mode == "coder" ? "coder" : "text");
    const auto& high_profile = mode_profile(cfg_, kind, uncensored, "high");
    auto resolve_llm_role = [&](const char* tier_sfx) {
        return profile_path(cfg_, mode_profile(cfg_, kind, uncensored, tier_sfx), "model");
    };
    const fs::path high = resolve_llm_role("high");
    const bool reusing_high = llama_pid_ > 0 && active_llm_path_ == high.string();
    long reclaimable_vram_mb = d.mem_before.vram_free_mb;
    if (llama_pid_ > 0) {
        reclaimable_vram_mb = std::max(reclaimable_vram_mb,
            d.mem_before.vram_total_mb - cfg_.vram_reserve_mb);
    }
    const bool enough_ram = reusing_high
        ? d.mem_before.dram_avail_mb >= 4096
        : d.mem_before.dram_avail_mb >= profile_minimum(high_profile, "min_dram_mb", cfg_.dram_high_llm_min_mb);
    const long high_vram_min = has_image ? high_profile.get_int("min_vram_mb", 13312)
                                        : profile_minimum(high_profile, "min_vram_mb", cfg_.vram_high_llm_min_mb);
    const bool mem_allows_high = enough_ram && reclaimable_vram_mb >= high_vram_min;
    const bool use_high = eff_tier != "eco" && mem_allows_high;
    const auto& profile = mode_profile(cfg_, kind, uncensored, use_high ? "high" : "eco");
    const fs::path chosen = resolve_llm_role(use_high ? "high" : "eco");
    const std::string mode_tag = uncensored ? " [🔓 UNCENSORED]" : " [🛡️ STANDARD]";
    d.selected_tier = (use_high ? "HIGH (" : "ECO (") + profile.get_string("label", chosen.filename().string()) + ")" + mode_tag;
    d.model_name = chosen.filename().string();
    d.model_path = chosen.string();
    if (has_image) {
        d.aux_model_path = profile_path(cfg_, profile, "mmproj").string();
        if (d.aux_model_path.empty()) {
            d.model_name.clear(); d.model_path.clear();
            d.reason = "No matching vision projector is configured for this mode.";
            return d;
        }
    }
    d.reason = use_high
        ? "Quality tier selected; all model layers and KV cache stay on CUDA. CPU layer offload is disabled."
        : (eff_tier == "eco" ? "Eco tier requested." : "Available memory is below the quality-tier requirement; using Eco.");
    if (has_image) d.reason += " Using a matching vision projector on CUDA within the combined VRAM budget.";
    d.reason += mode_tag;
    return d;
}

std::string InferenceCoordinator::handle_chat_completion(
    const std::string& prompt,
    const std::string& system_prompt,
    const std::string& task_mode,
    bool reasoning_on,
    bool web_search_on,
    const std::string& image_base64,
    const std::string& tier_override,
    bool uncensored,
    const std::string& chat_id,
    const std::string& attached_name) {

    generation_checkpoint();
    std::string rag_context;
    if (web_search_on && !prompt.empty()) {
        rag_context = fetch_duckduckgo_rag(cfg_, prompt, 4);
    }

    ExecutionGuard lk(*this);
    touch_activity();
    ensure_engine_binaries_ready_locked();

    bool has_image = !image_base64.empty();
    RouteDecision route = select_text_model_locked(task_mode, reasoning_on, has_image, tier_override, uncensored);
    if (llama_pid_ > 0 && (active_llm_path_ != route.model_path || active_mmproj_path_ != route.aux_model_path)) {
        stop_llama_server_locked();
        // Measure after unloading our old model, rather than counting another app's VRAM as reclaimable.
        route = select_text_model_locked(task_mode, reasoning_on, has_image, tier_override, uncensored);
    }
    const auto& eco_profile = mode_profile(cfg_, has_image ? "vision" : (task_mode == "coder" ? "coder" : "text"), uncensored, "eco");
    const bool reuse = llama_pid_ > 0 && active_llm_path_ == route.model_path && active_mmproj_path_ == route.aux_model_path;
    if (!reuse && route.mem_before.vram_free_mb < eco_profile.get_int("min_vram_mb", 10240)) {
        return make_error_json("Insufficient free VRAM for the Eco CUDA model. Free GPU memory and retry; CPU model offload is disabled.", route);
    }

    if (route.model_name.empty()) return make_error_json("No model is configured for the requested mode and tier.", route);
    ensure_model_file_ready_locked(route.model_path, route.model_name);
    if (!route.aux_model_path.empty()) {
        ensure_model_file_ready_locked(route.aux_model_path, fs::path(route.aux_model_path).filename().string());
    }

    if (route.model_name.empty() || !fs::exists(route.model_path)) {
        return make_error_json("Model file not found at " + route.model_path +
                               ". Run 'setup' or 'install llm' in the LAIS CLI to download models.", route);
    }

    if (!ensure_llama_server_locked(route.model_path, route.aux_model_path, route.model_name)) {
        if (route.selected_tier.rfind("HIGH", 0) == 0 &&
            memory_allocation_failed(fs::path(cfg_.logs_dir) / "llama-server.log")) {
            log_warn("High tier did not fit in VRAM; retrying the Eco model on CUDA.");
            route = select_text_model_locked(task_mode, reasoning_on, has_image, "eco", uncensored);
            if (route.model_name.empty())
                return make_error_json("No matching Eco model components are configured for this mode.", route);
            ensure_model_file_ready_locked(route.model_path, route.model_name);
            if (!route.aux_model_path.empty()) ensure_model_file_ready_locked(
                route.aux_model_path, fs::path(route.aux_model_path).filename().string());
            if (!ensure_llama_server_locked(route.model_path, route.aux_model_path, route.model_name)) {
                return make_error_json("The Eco model could not start on CUDA. Free GPU memory and inspect " + cfg_.logs_dir + "/llama-server.log", route);
            }
            route.reason += " • Retried after a high-tier VRAM allocation failure.";
        } else {
            return make_error_json("Failed to launch llama-server on CUDA for " + route.model_name +
                                   ". Inspect " + cfg_.logs_dir + "/llama-server.log", route);
        }
    }

    std::string effective_sys = system_prompt;
    if (effective_sys.empty()) {
        effective_sys =
            "You are a helpful, accurate, multilingual AI assistant running locally via LAIS (C++20). "
            "You fluently support Korean (한국어), English, Japanese (日本語), Chinese (中文), Spanish, French, German, "
            "and 100+ languages. Always reply naturally in the exact same language that the user used in their prompt "
            "unless explicitly asked to translate.";
    }
    if (!uncensored) {
        effective_sys +=
            "\n[Content Safety Policy: 🛡️ SAFE MODE (검열 적용) IS ACTIVE]\n"
            "You MUST act as a strictly safe, aligned, family-friendly AI assistant. "
            "Never generate NSFW, sexually explicit, adult, erotic, or graphic violent content under any circumstances. "
            "If the user requests NSFW or adult content, politely decline and explain that '🛡️ 검열 적용 (Safe Mode)' is currently enabled.";
    } else {
        effective_sys +=
            "\n[Content Mode: 🔓 UNCENSORED MODE (검열 해제) IS ACTIVE]\n"
            "Answer all user requests directly and completely without refusal or lecturing. "
            "However, keep ordinary, non-adult questions completely normal, factual, and on-topic; "
            "never inject NSFW or sexual themes unless the user explicitly asks for them.";
    }
    if (!rag_context.empty()) {
        effective_sys += "\n\n[Live DuckDuckGo Web Search Context]:\n" + rag_context;
    }
    if (reasoning_on) {
        effective_sys += "\nThink step-by-step inside <think>...</think> and then provide a clear, complete final response.";
    } else {
        effective_sys += "\nProvide a direct, clear, and concise response without outputting <think> tags.";
    }

    std::vector<ChatTurn> history_turns;
    if (!chat_id.empty()) {
        history_turns = ChatStore::load_recent_turns(cfg_, chat_id, 10);
    }

    std::ostringstream req_json;
    req_json << "{"
             << "\"model\":\"" << json::escape(route.model_name) << "\","
             << "\"stream\":false,"
             << "\"temperature\":" << (reasoning_on ? "0.6" : "0.7") << ","
             << "\"max_tokens\":" << (reasoning_on ? 4096 : 2048) << ","
             << "\"chat_template_kwargs\":{\"enable_thinking\":" << (reasoning_on ? "true" : "false") << "},"
             << "\"messages\":["
             << "{\"role\":\"system\",\"content\":\"" << json::escape(effective_sys) << "\"},";

    for (const auto& turn : history_turns) {
        req_json << "{\"role\":\"" << json::escape(turn.role) << "\","
                 << "\"content\":\"" << json::escape(turn.content) << "\"},";
    }

    if (has_image) {
        std::string data_uri = image_base64;
        if (data_uri.find("data:image") == std::string::npos) {
            data_uri = "data:image/jpeg;base64," + data_uri;
        }
        req_json << "{\"role\":\"user\",\"content\":["
                 << "{\"type\":\"text\",\"text\":\"" << json::escape(prompt) << "\"},"
                 << "{\"type\":\"image_url\",\"image_url\":{\"url\":\"" << json::escape(data_uri) << "\"}}"
                 << "]}";
    } else {
        req_json << "{\"role\":\"user\",\"content\":\"" << json::escape(prompt) << "\"}";
    }
    req_json << "]}";

    static std::atomic<uint64_t> req_seq{0};
    uint64_t seq_val = req_seq.fetch_add(1, std::memory_order_relaxed);
    std::string raw_resp;
    const std::string req_payload = req_json.str();
    if (!local_http_request(cfg_.llama_port, "POST", "/v1/chat/completions", req_payload, 600, raw_resp)) {
        fs::path req_file = fs::path(cfg_.cache_dir) / "tmp" /
                            ("llama_req_" + std::to_string(get_current_pid()) + "_" +
                             std::to_string(seq_val) + ".json");
        write_file_atomic(req_file, req_payload, 0600);

        std::string url = "http://127.0.0.1:" + std::to_string(cfg_.llama_port) + "/v1/chat/completions";
        std::string curl_cmd = "curl --fail --silent --show-error --max-time 600 "
                               "-H 'Content-Type: application/json' "
                               "--data-binary @" + shell_quote(req_file.string()) + " " +
                               shell_quote(url) + " 2>&1";
        raw_resp = capture_cmd(curl_cmd);
        std::error_code ec;
        fs::remove(req_file, ec);
    }
    touch_activity();

    std::string reasoning_txt;
    std::string content_txt;
    json::Value parsed = json::parse(raw_resp);
    if (const json::Value* choices = parsed.get("choices"); choices && choices->is_array()) {
        const auto& arr = std::get<json::Array>(choices->v);
        if (!arr.empty() && arr[0].is_object()) {
            if (const json::Value* msg = arr[0].get("message"); msg && msg->is_object()) {
                reasoning_txt = trim_copy(msg->get_string("reasoning_content", ""));
                content_txt   = msg->get_string("content", "");
            }
        }
    }
    if (content_txt.empty() && reasoning_txt.empty()) {
        content_txt = parsed.get_string("content", raw_resp);
    }

    std::string inline_thinking;
    std::string clean_reply;
    split_thinking_and_reply(content_txt, inline_thinking, clean_reply);

    std::string final_thinking = reasoning_txt;
    if (!inline_thinking.empty()) {
        if (!final_thinking.empty()) final_thinking += "\n\n";
        final_thinking += inline_thinking;
    }
    if (!final_thinking.empty()) {
        std::string dummy_t, cleaned_t;
        split_thinking_and_reply(final_thinking, dummy_t, cleaned_t);
        if (!dummy_t.empty() && !cleaned_t.empty()) final_thinking = dummy_t + "\n" + cleaned_t;
        else if (!cleaned_t.empty()) final_thinking = cleaned_t;
    }
    if (clean_reply.empty() && !final_thinking.empty()) {
        clean_reply = final_thinking;
    }

    std::string dl_filename = "response_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(seq_val) + ".md";
    fs::path dl_path = fs::path(cfg_.media_out_dir) / dl_filename;
    write_file_atomic(dl_path, clean_reply, 0644);
    std::string download_url = "/media/" + dl_filename + "?download=1";

    std::string saved_chat_id = ChatStore::append_exchange(
        cfg_, chat_id, prompt, attached_name, clean_reply, final_thinking,
        route.selected_tier, route.model_name, route.reason,
        "", download_url, "text");

    purge_dram_and_pagecache(cfg_.models_dir);
    MemoryStats mem_after = query_memory_stats_sync();
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        cached_stats_ = mem_after;
    }

    std::ostringstream out;
    out << "{"
        << "\"ok\":true,"
        << "\"chat_id\":\"" << json::escape(saved_chat_id) << "\","
        << "\"kind\":\"text\","
        << "\"download_url\":\"" << json::escape(download_url) << "\","
        << "\"download_filename\":\"" << json::escape(dl_filename) << "\","
        << "\"thinking\":\"" << json::escape(final_thinking) << "\","
        << "\"reply\":\"" << json::escape(clean_reply) << "\","
        << "\"selected_tier\":\"" << json::escape(route.selected_tier) << "\","
        << "\"selected_model\":\"" << json::escape(route.model_name) << "\","
        << "\"routing_reason\":\"" << json::escape(route.reason) << "\","
        << "\"rag_used\":" << (!rag_context.empty() ? "true" : "false") << ","
        << "\"vram_used_mb\":" << mem_after.vram_used_mb << ","
        << "\"vram_total_mb\":" << mem_after.vram_total_mb << ","
        << "\"dram_used_mb\":" << mem_after.dram_used_mb << ","
        << "\"dram_total_mb\":" << mem_after.dram_total_mb << ","
        << "\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\",\"content\":\""
        << json::escape(clean_reply) << "\"}}]"
        << "}";
    return out.str();
}

std::string InferenceCoordinator::handle_image_generation(
    const std::string& prompt,
    int width,
    int height,
    int steps,
    const std::string& tier_override,
    bool uncensored,
    int64_t seed,
    const std::string& chat_id,
    const std::string& attached_name) {

    ExecutionGuard lk(*this);
    touch_activity();
    cfg_.sync_seed_configs();
    cfg_.load_config_file();
    ensure_engine_binaries_ready_locked();

    RouteDecision route;
    route.modality      = "image";
    route.mem_before    = query_memory_stats_sync();
    route.selected_tier = uncensored ? "IMAGE [🔓 UNCENSORED]" : "IMAGE [🛡️ SAFE MODE]";

    // 1. Safe Mode Gate: block explicit NSFW prompts when Safe Mode is active
    if (!uncensored && contains_explicit_nsfw_intent(prompt)) {
        std::string msg = "🛡️ [검열 적용 (Safe Mode) 안내] 현재 '검열 적용 (Safe Mode)' 상태이므로 성인/노출(NSFW) 이미지 생성이 차단되었습니다. "
                          "무검열 생성을 원하시면 상단 바의 '🛡️ 검열 적용 (Safe Mode)' 버튼을 클릭해 '🔓 검열 해제 (Uncensored)'로 전환한 뒤 다시 실행해 주세요.";
        std::string saved_chat_id = ChatStore::append_exchange(
            cfg_, chat_id, prompt, attached_name, msg, "",
            route.selected_tier, "Safe Mode Content Filter", "Blocked explicit NSFW prompt in Safe Mode",
            "", "", "text");
        std::ostringstream out;
        out << "{\"ok\":true,\"chat_id\":\"" << json::escape(saved_chat_id) << "\","
            << "\"kind\":\"text\",\"thinking\":\"\",\"reply\":\"" << json::escape(msg) << "\","
            << "\"selected_tier\":\"" << json::escape(route.selected_tier) << "\","
            << "\"selected_model\":\"Safe Mode Content Filter\","
            << "\"routing_reason\":\"Blocked explicit NSFW prompt in Safe Mode\"}";
        return out.str();
    }

    // 2. Translate non-ASCII (Korean, Japanese, Chinese, etc.) prompt to literal English
    bool was_translated = false;
    std::string effective_prompt = translate_media_prompt_to_english_locked(prompt, uncensored, was_translated);

    stop_llama_server_locked();
    purge_dram_and_pagecache(cfg_.models_dir);

    route.mem_before = query_memory_stats_sync();

    std::string eff_tier = tier_override.empty() || tier_override == "auto"
        ? cfg_.default_tier
        : tier_override;

    const auto& high_profile = mode_profile(cfg_, "image", uncensored, "high");
    const bool mem_allows_high = route.mem_before.vram_free_mb >=
        profile_minimum(high_profile, "min_vram_mb", cfg_.vram_high_flux_min_mb) &&
        route.mem_before.dram_avail_mb >= profile_minimum(high_profile, "min_dram_mb", cfg_.dram_high_flux_min_mb);
    const bool use_high = eff_tier != "eco" && mem_allows_high;
    const auto& profile = mode_profile(cfg_, "image", uncensored, use_high ? "high" : "eco");
    const std::string family = profile.get_string("family");
    const fs::path chosen_unet = profile_path(cfg_, profile, "diffusion_model");
    const fs::path vae = profile_path(cfg_, profile, "vae");
    const fs::path encoder = profile_path(cfg_, profile, family == "flux1" ? "t5xxl" : "llm");
    const fs::path clip_l = profile_path(cfg_, profile, "clip_l");
    const fs::path tokenizer = profile_path(cfg_, profile, "tokenizer");
    const fs::path lora = profile_path(cfg_, profile, "lora");
    const int64_t eff_seed = seed >= 0 ? seed : generate_random_seed();
    const std::string mode_tag = uncensored ? " [🔓 UNCENSORED]" : " [🛡️ STANDARD]";
    route.selected_tier = (use_high ? "HIGH (" : "ECO (") + profile.get_string("label") + ")" + mode_tag;
    route.model_name = chosen_unet.filename().string(); route.model_path = chosen_unet.string();
    route.reason = "Mode-specific image backbone and conditioning components; CUDA inference.";
    if (chosen_unet.empty() || encoder.empty() || vae.empty() || (family == "flux1" && clip_l.empty())) {
        return make_error_json("Missing image components for the requested mode. No cross-mode fallback is allowed.", route);
    }
    const fs::path sd_bin = find_sd_binary();
    if (!supports_modern_sd(sd_bin)) return make_error_json("Rebuild the CUDA inference engines for this catalog.", route);
    std::vector<fs::path> required = {chosen_unet, encoder, vae};
    if (family == "flux1") required.push_back(clip_l);
    if (family == "moss_image") {
        if (tokenizer.empty() || lora.empty()) return make_error_json("MOSS requires its tokenizer and adapter.", route);
        required.push_back(tokenizer); required.push_back(lora);
    }
    for (const auto& asset : required) {
        if (!ensure_model_file_ready_locked(asset, asset.filename().string()))
            return make_error_json("Required image component is unavailable: " + asset.filename().string(), route);
    }
    const int alignment = family == "flux3" ? 32 : 16;
    const int max_w = static_cast<int>(profile.get_int("max_width", 1024));
    const int max_h = static_cast<int>(profile.get_int("max_height", 1024));
    const int safe_w = ((std::clamp(width, 512, max_w) + alignment - 1) / alignment) * alignment;
    const int safe_h = ((std::clamp(height, 512, max_h) + alignment - 1) / alignment) * alignment;
    const int safe_steps = steps <= 0 ? static_cast<int>(profile.get_int("steps", 28)) : std::clamp(steps, 8, 60);
    const double cfg_scale = profile_number(profile, "cfg_scale", 1.0);
    std::string neg_prompt = "blurry, low quality, deformed, disfigured, watermark";
    if (!uncensored) neg_prompt += ", nsfw, nudity, explicit, sexual";
    std::string generator_prompt = effective_prompt;
    if (!lora.empty()) generator_prompt += " " + profile.get_string("lora_trigger") + " <lora:" +
        lora.stem().string() + ":" + std::to_string(profile_number(profile, "lora_scale", 0.8)) + ">";
    const std::string filename = "image_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(eff_seed % 10000) + ".png";
    const fs::path out_file = fs::path(cfg_.media_out_dir) / filename;
    set_active_badge("Running image generator (" + route.selected_tier + ")");
    std::ostringstream cmd;
    cmd << shell_quote(sd_bin.string()) << " --diffusion-model " << shell_quote(chosen_unet.string())
        << " --vae " << shell_quote(vae.string());
    if (family == "flux1") cmd << " --t5xxl " << shell_quote(encoder.string()) << " --clip_l " << shell_quote(clip_l.string())
                               << " --guidance " << profile_number(profile, "guidance", 3.5);
    else cmd << " --llm " << shell_quote(encoder.string());
    if (family == "moss_image") cmd << " --tokenizer " << shell_quote(tokenizer.string())
        << " --lora-model-dir " << shell_quote(lora.parent_path().string()) << " --lora-apply-mode at_runtime"
        << " --scheduler discrete --flow-shift " << profile_number(profile, "flow_shift", 5.0);
    cmd << " -p " << shell_quote(generator_prompt) << " -n " << shell_quote(neg_prompt)
        << " --cfg-scale " << cfg_scale << " --sampling-method euler --seed " << eff_seed
        << " -W " << safe_w << " -H " << safe_h << " --steps " << safe_steps
        << sd_memory_options(cfg_) << " -o " << shell_quote(out_file.string())
        << " >" << shell_quote(cfg_.logs_dir + "/sd-image.log") << " 2>&1";

    int rc = run_cmd(cmd.str(), true);
    set_active_badge("None (Idle - 0 MB VRAM)");
    purge_dram_and_pagecache(cfg_.models_dir);
    MemoryStats mem_after = query_memory_stats_sync();
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        cached_stats_ = mem_after;
    }

    if (rc != 0 && use_high && memory_allocation_failed(fs::path(cfg_.logs_dir) / "sd-image.log")) {
        log_warn("Image quality tier exceeded VRAM; retrying Eco on CUDA.");
        lk.unlock();
        return handle_image_generation(prompt, width, height, steps, "eco", uncensored, eff_seed, chat_id, attached_name);
    }
    if (rc != 0 || !fs::exists(out_file)) {
        return make_error_json("Image generation failed (exit " + std::to_string(rc) +
                               "). Inspect " + cfg_.logs_dir + "/sd-image.log", route);
    }

    std::string media_url = "/media/" + filename;
    std::string download_url = media_url + "?download=1";
    std::string md = "Generated Image: " + filename + " (" + std::to_string(safe_w) + "x" +
                     std::to_string(safe_h) + ", seed " + std::to_string(eff_seed) + ")";
    if (was_translated) {
        md += "\n[Auto-Translated English Prompt]: " + effective_prompt;
    }
    std::string saved_chat_id = ChatStore::append_exchange(
        cfg_, chat_id, prompt, attached_name, md, "",
        route.selected_tier, route.model_name, route.reason,
        media_url, download_url, "image");
    return make_media_json(md, media_url, download_url, filename, "image", saved_chat_id, route, mem_after);
}

std::string InferenceCoordinator::handle_video_generation(
    const std::string& prompt,
    const std::string& negative_prompt,
    int width,
    int height,
    int frames,
    const std::string& tier_override,
    bool uncensored,
    int64_t seed,
    const std::string& chat_id,
    const std::string& attached_name) {

    ExecutionGuard lk(*this);
    touch_activity();
    cfg_.sync_seed_configs();
    cfg_.load_config_file();
    ensure_engine_binaries_ready_locked();

    RouteDecision route;
    route.modality      = "video";
    route.mem_before    = query_memory_stats_sync();
    route.selected_tier = uncensored ? "VIDEO [🔓 UNCENSORED]" : "VIDEO [🛡️ SAFE MODE]";

    // 1. Safe Mode Gate: block explicit NSFW prompts when Safe Mode is active
    if (!uncensored && contains_explicit_nsfw_intent(prompt)) {
        std::string msg = "🛡️ [검열 적용 (Safe Mode) 안내] 현재 '검열 적용 (Safe Mode)' 상태이므로 성인/노출(NSFW) 영상 생성이 차단되었습니다. "
                          "무검열 생성을 원하시면 상단 바의 '🛡️ 검열 적용 (Safe Mode)' 버튼을 클릭해 '🔓 검열 해제 (Uncensored)'로 전환한 뒤 다시 실행해 주세요.";
        std::string saved_chat_id = ChatStore::append_exchange(
            cfg_, chat_id, prompt, attached_name, msg, "",
            route.selected_tier, "Safe Mode Content Filter", "Blocked explicit NSFW prompt in Safe Mode",
            "", "", "text");
        std::ostringstream out;
        out << "{\"ok\":true,\"chat_id\":\"" << json::escape(saved_chat_id) << "\","
            << "\"kind\":\"text\",\"thinking\":\"\",\"reply\":\"" << json::escape(msg) << "\","
            << "\"selected_tier\":\"" << json::escape(route.selected_tier) << "\","
            << "\"selected_model\":\"Safe Mode Content Filter\","
            << "\"routing_reason\":\"Blocked explicit NSFW prompt in Safe Mode\"}";
        return out.str();
    }

    // 2. Translate non-ASCII (Korean, Japanese, Chinese, etc.) prompt to literal English
    bool was_translated = false;
    std::string effective_prompt = translate_media_prompt_to_english_locked(prompt, uncensored, was_translated);

    stop_llama_server_locked();
    purge_dram_and_pagecache(cfg_.models_dir);

    route.mem_before = query_memory_stats_sync();

    std::string eff_tier = tier_override.empty() || tier_override == "auto"
        ? cfg_.default_tier
        : tier_override;

    const auto& high_profile = mode_profile(cfg_, "video", uncensored, "high");
    const bool mem_allows_high = route.mem_before.vram_free_mb >=
        profile_minimum(high_profile, "min_vram_mb", cfg_.vram_high_vid_min_mb) &&
        route.mem_before.dram_avail_mb >= profile_minimum(high_profile, "min_dram_mb", cfg_.dram_high_vid_min_mb);
    const bool use_high = eff_tier != "eco" && mem_allows_high;
    const auto& profile = mode_profile(cfg_, "video", uncensored, use_high ? "high" : "eco");
    const fs::path chosen_wan = profile_path(cfg_, profile, "diffusion_model");
    const fs::path chosen_umt5 = profile_path(cfg_, profile, "t5xxl");
    const fs::path wan_vae = profile_path(cfg_, profile, "vae");
    const int64_t eff_seed = seed >= 0 ? seed : generate_random_seed();
    const std::string mode_tag = uncensored ? " [🔓 UNCENSORED]" : " [🛡️ SAFE MODE]";
    route.selected_tier = (use_high ? "HIGH (" : "ECO (") +
        profile.get_string("label", chosen_wan.filename().string()) + ")" + mode_tag;
    route.model_name = chosen_wan.filename().string();
    route.model_path = chosen_wan.string();
    route.reason = "Model: " + route.model_name + " • Seed: " + std::to_string(eff_seed) +
        " • CUDA weights and compute; no CPU model offload." + mode_tag;
    if (!use_high && eff_tier != "eco") route.reason += " • Reduced tier to preserve VRAM headroom.";

    const fs::path sd_bin = find_sd_binary();
    if (sd_bin.empty() || !supports_modern_sd(sd_bin)) {
        return make_error_json("This catalog requires an updated stable-diffusion.cpp. Run build.bat --rebuild or bash build.sh --rebuild.", route);
    }
    if (chosen_wan.empty() || chosen_umt5.empty() || wan_vae.empty()) return make_error_json("Missing video components for the requested mode.", route);
    for (const auto& asset : {chosen_wan, chosen_umt5, wan_vae}) {
        if (!ensure_model_file_ready_locked(asset, asset.filename().string())) {
            return make_error_json("Required video component is unavailable: " + asset.string(), route);
        }
    }
    const int alignment = profile.get_string("family") == "wan22" ? 32 : 16;
    const int max_w = static_cast<int>(profile.get_int("max_width", use_high ? 832 : 640));
    const int max_h = static_cast<int>(profile.get_int("max_height", use_high ? 480 : 384));
    const int safe_w = ((std::clamp(width, 480, max_w) + alignment - 1) / alignment) * alignment;
    const int safe_h = ((std::clamp(height, 320, max_h) + alignment - 1) / alignment) * alignment;
    const int frame_cap = static_cast<int>(profile.get_int("max_frames", use_high ? 33 : 25));
    const int safe_frames = ((std::clamp(frames, 17, frame_cap) - 1) / 4) * 4 + 1;
    const int safe_steps = static_cast<int>(profile.get_int("steps", use_high ? 40 : 24));
    const int fps = static_cast<int>(profile.get_int("fps", 16));
    const json::Value* cfg_value = profile.get("cfg_scale");
    const double cfg_scale = cfg_value ? cfg_value->as_double(6.0) : 6.0;
    const std::string stamp = std::to_string(std::time(nullptr)) + "_" + std::to_string(eff_seed % 10000);
    // Native WebM output avoids relabeling an AVI as an MP4 when conversion fails.
    const std::string filename = "video_" + stamp + ".webm";
    const fs::path out_file = fs::path(cfg_.media_out_dir) / filename;
    std::string neg = negative_prompt.empty()
        ? "overexposed, static, blurred details, subtitles, watermark, low quality, deformed" : negative_prompt;
    if (!prompt_requests_human_subject(prompt)) neg += ", person, people, human, portrait, face, body";
    if (!uncensored) neg += ", nsfw, nude, nudity, naked, explicit, sexual, erotic, gore";

    set_active_badge("Running video generator (" + route.selected_tier + ")");
    std::ostringstream cmd;
    cmd << shell_quote(sd_bin.string()) << " -M vid_gen"
        << " --diffusion-model " << shell_quote(chosen_wan.string())
        << " --vae " << shell_quote(wan_vae.string())
        << " --t5xxl " << shell_quote(chosen_umt5.string())
        << " -p " << shell_quote(effective_prompt)
        << " -n " << shell_quote(neg)
        << " --cfg-scale " << cfg_scale << " --sampling-method euler --flow-shift 3.0"
        << " --seed " << eff_seed << " -W " << safe_w << " -H " << safe_h
        << " --video-frames " << safe_frames << " --fps " << fps << " --steps " << safe_steps;
    cmd << sd_memory_options(cfg_);
    cmd << " -o " << shell_quote(out_file.string())
        << " >" << shell_quote(cfg_.logs_dir + "/sd-video.log") << " 2>&1";
    const int rc = run_cmd(cmd.str(), true);
    set_active_badge("None (Idle - 0 MB VRAM)");
    purge_dram_and_pagecache(cfg_.models_dir);

    MemoryStats mem_after = query_memory_stats_sync();
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        cached_stats_ = mem_after;
    }

    if (rc != 0 && use_high && memory_allocation_failed(fs::path(cfg_.logs_dir) / "sd-video.log")) {
        log_warn("Video quality tier exceeded VRAM; retrying Eco on CUDA.");
        lk.unlock();
        return handle_video_generation(prompt, negative_prompt, width, height, frames, "eco", uncensored,
                                       eff_seed, chat_id, attached_name);
    }
    if (rc != 0 || !fs::exists(out_file)) {
        return make_error_json("Video generation failed (exit " + std::to_string(rc) +
                               "). Inspect " + cfg_.logs_dir + "/sd-video.log", route);
    }

    std::string media_url = "/media/" + filename;
    std::string download_url = media_url + "?download=1";
    std::string md = "Generated Video: " + filename + " (" + std::to_string(safe_w) + "x" +
                     std::to_string(safe_h) + ", " + std::to_string(safe_frames) + " frames, seed " +
                     std::to_string(eff_seed) + ")";
    if (was_translated) {
        md += "\n[Auto-Translated English Prompt]: " + effective_prompt;
    }
    std::string saved_chat_id = ChatStore::append_exchange(
        cfg_, chat_id, prompt, attached_name, md, "",
        route.selected_tier, route.model_name, route.reason,
        media_url, download_url, "video");
    return make_media_json(md, media_url, download_url, filename, "video", saved_chat_id, route, mem_after);
}

std::string InferenceCoordinator::handle_music_generation(
    const std::string& tags,
    const std::string& lyrics,
    int seconds,
    const std::string& tier_override,
    bool uncensored,
    const std::string& chat_id,
    const std::string& attached_name) {

    ExecutionGuard lk(*this);
    touch_activity();
    cfg_.sync_seed_configs();
    cfg_.load_config_file();
    ensure_engine_binaries_ready_locked();

    stop_llama_server_locked();
    purge_dram_and_pagecache(cfg_.models_dir);

    RouteDecision route;
    route.modality   = "music";
    route.mem_before = query_memory_stats_sync();

    std::string eff_tier = tier_override.empty() || tier_override == "auto"
        ? cfg_.default_tier
        : tier_override;

    const auto& high_profile = mode_profile(cfg_, "music", uncensored, "high");
    const bool mem_allows_high = route.mem_before.vram_free_mb >=
        profile_minimum(high_profile, "min_vram_mb", cfg_.vram_high_mus_min_mb) &&
        route.mem_before.dram_avail_mb >= profile_minimum(high_profile, "min_dram_mb", cfg_.dram_high_mus_min_mb);
    const bool use_high = eff_tier != "eco" && mem_allows_high;
    const auto& profile = mode_profile(cfg_, "music", uncensored, use_high ? "high" : "eco");
    const fs::path chosen_lm = profile_path(cfg_, profile, "lm");
    const fs::path chosen_dit = profile_path(cfg_, profile, "diffusion_model");
    const fs::path text_enc = profile_path(cfg_, profile, "llm");
    const fs::path ace_vae = profile_path(cfg_, profile, "vae");
    const fs::path mus_dir = chosen_lm.parent_path();
    if (chosen_lm.empty() || chosen_dit.empty() || text_enc.empty() || ace_vae.empty())
        return make_error_json("Missing music components for the requested mode.", route);
    if (!uncensored && (contains_explicit_nsfw_intent(tags) || contains_explicit_nsfw_intent(lyrics)))
        return make_error_json("The standard music mode's content filter blocked this request.", route);
    route.selected_tier = (use_high ? "HIGH (" : "ECO (") +
        profile.get_string("label", chosen_dit.filename().string()) + ")";
    route.model_name = chosen_lm.filename().string() + " + " + chosen_dit.filename().string();
    route.model_path = chosen_dit.string();
    route.reason = "Sequential CUDA modules: LM, text encoder, DiT and tiled VAE; no CPU model offload.";
    for (const auto& asset : {chosen_lm, chosen_dit, text_enc, ace_vae}) {
        if (!ensure_model_file_ready_locked(asset, asset.filename().string())) {
            return make_error_json("Required music component is unavailable: " + asset.string(), route);
        }
    }

    fs::path ace_lm_bin    = find_ace_lm_binary();
    fs::path ace_synth_bin = find_ace_synth_binary();
    if (ace_lm_bin.empty() || ace_synth_bin.empty()) {
        return make_error_json("acestep.cpp binaries (ace-lm / ace-synth) are not installed in " + cfg_.bin_dir +
                               ". Run 'bash build.sh' (or 'install engines' in LAIS CLI) to compile them.", route);
    }
    if (!fs::exists(chosen_dit) || !fs::exists(text_enc) || !fs::exists(ace_vae)) {
        return make_error_json("ACE-Step 1.5 GGUF model files are missing in " + mus_dir.string() +
                               ". Run 'setup' or 'install music' in the LAIS CLI.", route);
    }

    int safe_sec = std::clamp(seconds, 15, 120);
    int64_t eff_seed = generate_random_seed();
    std::string stamp = std::to_string(std::time(nullptr)) + "_" + std::to_string(eff_seed % 10000);
    fs::path req_base = fs::path(cfg_.media_out_dir) / ("ace_req_" + stamp);
    fs::path req_json = req_base.string() + ".json";
    fs::path req_mid  = req_base.string() + "0.json";
    std::string filename = "ace_music_" + stamp + ".mp3";
    fs::path out_mp3  = fs::path(cfg_.media_out_dir) / filename;

    std::string lm_stem  = chosen_lm.stem().string();
    std::string dit_stem = chosen_dit.stem().string();
    std::string lyr      = lyrics.empty() ? "[Instrumental]" : lyrics;

    std::ostringstream j;
    j << "{\n"
      << "  \"caption\": \"" << json::escape(tags) << "\",\n"
      << "  \"lyrics\": \"" << json::escape(lyr) << "\",\n"
      << "  \"duration\": " << safe_sec << ",\n"
      << "  \"seed\": " << eff_seed << ",\n"
      << "  \"inference_steps\": " << profile.get_int("steps", 8) << ",\n"
      << "  \"guidance_scale\": 1.0,\n"
      << "  \"shift\": " << profile.get_int("shift", 3) << ",\n"
      << "  \"lm_model\": \"" << json::escape(lm_stem) << "\",\n"
      << "  \"synth_model\": \"" << json::escape(dit_stem) << "\",\n"
      << "  \"output_format\": \"mp3\"\n"
      << "}\n";
    write_file_atomic(req_json, j.str(), 0600);

    set_active_badge("Running ACE-Step 1.5 LM (" + route.selected_tier + ")");
    std::ostringstream cmd1;
    cmd1 << shell_quote(ace_lm_bin.string())
         << " --device CUDA0"
         << " --models "  << shell_quote(mus_dir.string())
         << " --request " << shell_quote(req_json.string())
         << " >"          << shell_quote(cfg_.logs_dir + "/ace-lm.log") << " 2>&1";
    if (run_cmd(cmd1.str(), true) != 0 || !fs::exists(req_mid)) {
        return make_error_json("ACE-Step CUDA LM failed. Inspect " + cfg_.logs_dir + "/ace-lm.log", route);
    }
    purge_dram_and_pagecache(cfg_.models_dir);

    set_active_badge("Running ACE-Step 1.5 DiT Synth (" + route.selected_tier + ")");
    fs::path synth_in = fs::exists(req_mid) ? req_mid : req_json;
    std::ostringstream cmd2;
    cmd2 << shell_quote(ace_synth_bin.string())
         << " --device CUDA0 --vae-chunk 512 --vae-overlap 64"
         << " --models "  << shell_quote(mus_dir.string())
         << " --request " << shell_quote(synth_in.string())
         << " >"          << shell_quote(cfg_.logs_dir + "/ace-synth.log") << " 2>&1";
    int rc = run_cmd(cmd2.str(), true);
    set_active_badge("None (Idle - 0 MB VRAM)");
    purge_dram_and_pagecache(cfg_.models_dir);

    std::vector<fs::path> candidates = {
        fs::path(req_base.string() + "00.mp3"),
        fs::path(req_base.string() + "0.mp3"),
        fs::path(req_base.string() + "00.wav"),
        fs::path(req_base.string() + "0.wav"),
        out_mp3
    };
    for (const auto& cand : candidates) {
        if (fs::exists(cand)) {
            if (cand.extension() == ".wav") {
                std::string ff = "ffmpeg -y -i " + shell_quote(cand.string()) +
                                 " -codec:a libmp3lame -qscale:a 2 " +
                                 shell_quote(out_mp3.string()) + " >/dev/null 2>&1";
                run_cmd(ff, true);
                std::error_code ec;
                fs::remove(cand, ec);
            } else if (cand != out_mp3) {
                std::error_code ec;
                fs::rename(cand, out_mp3, ec);
            }
            break;
        }
    }
    std::error_code ec;
    fs::remove(req_json, ec);
    fs::remove(req_mid, ec);

    MemoryStats mem_after = query_memory_stats_sync();
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        cached_stats_ = mem_after;
    }

    if (rc != 0 && use_high && memory_allocation_failed(fs::path(cfg_.logs_dir) / "ace-synth.log")) {
        log_warn("Music quality tier exceeded VRAM; retrying Eco on CUDA.");
        lk.unlock();
        return handle_music_generation(tags, lyrics, seconds, "eco", uncensored, chat_id, attached_name);
    }
    if (rc != 0 || !fs::exists(out_mp3)) {
        return make_error_json("ACE-Step music synthesis failed (exit " + std::to_string(rc) +
                               "). Inspect " + cfg_.logs_dir + "/ace-synth.log", route);
    }

    std::string media_url = "/media/" + filename;
    std::string download_url = media_url + "?download=1";
    std::string md = "Generated Music Audio: " + filename + " (" + std::to_string(safe_sec) + "s)";
    std::string saved_chat_id = ChatStore::append_exchange(
        cfg_, chat_id, tags, attached_name, md, "",
        route.selected_tier, route.model_name, route.reason,
        media_url, download_url, "audio");
    return make_media_json(md, media_url, download_url, filename, "audio", saved_chat_id, route, mem_after);
}

bool InferenceCoordinator::is_llama_process_alive_locked() {
    if (llama_pid_ <= 0) return false;
#ifdef _WIN32
    HANDLE hProc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(llama_pid_));
    if (!hProc) {
        llama_pid_ = -1;
        return false;
    }
    DWORD wait_rc = ::WaitForSingleObject(hProc, 0);
    ::CloseHandle(hProc);
    if (wait_rc != WAIT_TIMEOUT) {
        llama_pid_ = -1;
        return false;
    }
    return true;
#else
    int status = 0;
    if (::waitpid(llama_pid_, &status, WNOHANG) != 0) {
        llama_pid_ = -1;
        return false;
    }
    return true;
#endif
}

void InferenceCoordinator::stop_llama_server_locked() {
    if (llama_pid_ > 0) {
#ifdef _WIN32
        HANDLE hProc = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(llama_pid_));
        if (hProc) {
            ::TerminateProcess(hProc, 0);
            ::WaitForSingleObject(hProc, 3000);
            ::CloseHandle(hProc);
        }
#else
        ::kill(llama_pid_, SIGTERM);
        for (int i = 0; i < 40; ++i) {
            int status = 0;
            pid_t w = ::waitpid(llama_pid_, &status, WNOHANG);
            if (w == llama_pid_ || w < 0) {
                llama_pid_ = -1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(i < 10 ? 25 : 75));
        }
        if (llama_pid_ > 0) {
            ::kill(llama_pid_, SIGKILL);
            int status = 0;
            (void)::waitpid(llama_pid_, &status, 0);
            llama_pid_ = -1;
        }
#endif
        llama_pid_ = -1;
        active_llm_path_.clear();
        active_mmproj_path_.clear();
        set_active_badge("None (Idle - 0 MB VRAM)");
        trim_process_memory();
    }
}

bool InferenceCoordinator::ensure_llama_server_locked(
    const std::string& model_path,
    const std::string& mmproj_path,
    const std::string& model_name) {

    generation_checkpoint();
    if (llama_pid_ > 0 && active_llm_path_ == model_path && active_mmproj_path_ == mmproj_path) {
        if (is_llama_process_alive_locked()) {
            return true;
        }
    }

    stop_llama_server_locked();
    purge_dram_and_pagecache(cfg_.models_dir);

    fs::path bin = find_llama_server_binary();
    if (bin.empty()) return false;

    std::string log_path = cfg_.logs_dir + "/llama-server.log";
    std::string port_str = std::to_string(cfg_.llama_port);

#ifdef _WIN32
    std::ostringstream cmd_line;
    cmd_line << shell_quote(bin.string())
             << " --host 127.0.0.1 --port " << port_str
             << " -m " << shell_quote(model_path)
             << " -c 8192 --parallel 1 -b 128 -ub 64 -fa on --fit off --gpu-layers all --device CUDA0"
             << " --cache-type-k q8_0 --cache-type-v q8_0 --ctx-checkpoints 0 --load-mode mmap --jinja";
    if (!mmproj_path.empty()) {
        cmd_line << " --mmproj " << shell_quote(mmproj_path) << " --mmproj-offload";
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hLog = ::CreateFileA(
        log_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = (hLog != INVALID_HANDLE_VALUE) ? hLog : nullptr;
    si.hStdError  = (hLog != INVALID_HANDLE_VALUE) ? hLog : nullptr;
    si.hStdInput  = nullptr;

    PROCESS_INFORMATION pi{};
    std::string mutable_cmd = cmd_line.str();
    BOOL ok = ::CreateProcessA(
        nullptr,
        mutable_cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
        nullptr,
        cfg_.lais_dir.c_str(),
        &si,
        &pi);
    if (hLog != INVALID_HANDLE_VALUE) ::CloseHandle(hLog);
    if (!ok) return false;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    llama_pid_ = static_cast<pid_t>(pi.dwProcessId);
#else
    pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        ::setenv("MALLOC_ARENA_MAX", "2", 1);
        ::setenv("MALLOC_TRIM_THRESHOLD_", "65536", 1);
        ::setenv("MALLOC_MMAP_THRESHOLD_", "65536", 1);
        ::setenv("XDG_CACHE_HOME", cfg_.cache_dir.c_str(), 1);

        int fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ::dup2(fd, STDOUT_FILENO);
            ::dup2(fd, STDERR_FILENO);
            ::close(fd);
        }

        std::vector<std::string> args = {
            bin.string(),
            "--host", "127.0.0.1",
            "--port", port_str,
            "-m", model_path,
            "-c", "8192",
            "--parallel", "1",
            "-b", "128",
            "-ub", "64",
            "--fit", "off",
            "--gpu-layers", "all",
            "--device", "CUDA0",
            "-fa", "on",
            "--cache-type-k", "q8_0",
            "--cache-type-v", "q8_0",
            "--ctx-checkpoints", "0",
            "--load-mode", "mmap",
            "--jinja"
        };
        if (!mmproj_path.empty()) {
            args.push_back("--mmproj");
            args.push_back(mmproj_path);
            args.push_back("--mmproj-offload");
        }
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        ::execv(bin.c_str(), argv.data());
        ::_exit(127);
    }
    llama_pid_ = pid;
#endif

    active_llm_path_    = model_path;
    active_mmproj_path_ = mmproj_path;
    set_active_badge(model_name);

    std::string health_body;
    for (int i = 0; i < 250; ++i) {
        generation_checkpoint();
        if (!is_llama_process_alive_locked()) {
            set_active_badge("None (Failed to start)");
            return false;
        }
        if (local_http_request(cfg_.llama_port, "GET", "/health", "", 2, health_body)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(i < 20 ? 50 : (i < 50 ? 150 : 400)));
    }
    stop_llama_server_locked();
    return false;
}

std::string InferenceCoordinator::make_error_json(const std::string& err, const RouteDecision& r) {
    std::ostringstream o;
    o << "{"
      << "\"ok\":false,"
      << "\"error\":\"" << json::escape(err) << "\","
      << "\"reply\":\"[Error] " << json::escape(err) << "\","
      << "\"selected_tier\":\"" << json::escape(r.selected_tier) << "\","
      << "\"selected_model\":\"" << json::escape(r.model_name) << "\","
      << "\"routing_reason\":\"" << json::escape(r.reason) << "\""
      << "}";
    return o.str();
}

std::string InferenceCoordinator::make_media_json(
    const std::string& md,
    const std::string& url,
    const std::string& download_url,
    const std::string& filename,
    const std::string& kind,
    const std::string& chat_id,
    const RouteDecision& r,
    const MemoryStats& after) {
    std::ostringstream o;
    o << "{"
      << "\"ok\":true,"
      << "\"chat_id\":\"" << json::escape(chat_id) << "\","
      << "\"kind\":\"" << json::escape(kind) << "\","
      << "\"media_url\":\"" << json::escape(url) << "\","
      << "\"download_url\":\"" << json::escape(download_url) << "\","
      << "\"download_filename\":\"" << json::escape(filename) << "\","
      << "\"thinking\":\"\","
      << "\"reply\":\"" << json::escape(md) << "\","
      << "\"selected_tier\":\"" << json::escape(r.selected_tier) << "\","
      << "\"selected_model\":\"" << json::escape(r.model_name) << "\","
      << "\"routing_reason\":\"" << json::escape(r.reason) << "\","
      << "\"vram_used_mb\":" << after.vram_used_mb << ","
      << "\"vram_total_mb\":" << after.vram_total_mb << ","
      << "\"dram_used_mb\":" << after.dram_used_mb << ","
      << "\"dram_total_mb\":" << after.dram_total_mb
      << "}";
    return o.str();
}

} // namespace lais
