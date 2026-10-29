// ==============================================================================
// LAIS (Local AI Studio) v3.7.0 - Common Declarations, Config & ChatStore
// SPDX-License-Identifier: MIT
// ==============================================================================
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <process.h>
#include <io.h>
#include <conio.h>
#include <malloc.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <exception>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace lais {

namespace fs = std::filesystem;

inline constexpr const char* LAIS_VERSION = "3.7.0";

#ifdef _WIN32
using socket_t = SOCKET;
using ssize_t  = std::ptrdiff_t;
using pid_t    = int;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
#endif

extern std::atomic<bool> g_shutdown_requested;
extern std::atomic<bool> g_in_interactive_repl;

void close_socket(socket_t s);
void set_env_var(const char* name, const char* value);
pid_t get_current_pid();
FILE* open_pipe_read(const char* cmd);
int close_pipe_read(FILE* pipe);
const char* null_stderr_redirect();
const char* null_all_redirect();
const char* exe_suffix();
fs::path get_self_exe_path(const char* argv0);
void handle_termination_signal(int sig);

void log_info(std::string_view msg);
void log_ok(std::string_view msg);
void log_warn(std::string_view msg);
void log_err(std::string_view msg);

std::string trim_copy(std::string_view sv);
std::string to_lower_ascii(std::string_view sv);
std::string shell_quote(std::string_view arg);
std::vector<std::string> split_command_tokens(std::string_view line);
std::string current_timestamp_iso();
bool contains_non_ascii(std::string_view sv);
bool contains_explicit_nsfw_intent(std::string_view sv);
int64_t generate_random_seed();

// Cancellation belongs to one HTTP request, including its queued work and children.
struct GenerationCancelled : std::exception {
    const char* what() const noexcept override { return "Generation stopped"; }
};
struct GenerationState {
    std::atomic<bool> cancelled{false};
    std::atomic<bool> finished{false};
    std::mutex commit_mu;
    bool request_cancel();
    void finish();
};
std::shared_ptr<GenerationState> current_generation();
bool generation_cancelled();
void generation_checkpoint();
class GenerationScope {
public:
    explicit GenerationScope(std::shared_ptr<GenerationState> state);
    ~GenerationScope();
    GenerationScope(const GenerationScope&) = delete;
    GenerationScope& operator=(const GenerationScope&) = delete;
private:
    std::shared_ptr<GenerationState> previous_;
};

std::string capture_cmd(const std::string& cmd);
int run_cmd(const std::string& cmd, bool quiet = false);
bool command_exists(std::string_view tool);
bool can_run_sudo();

bool write_file_atomic(const fs::path& path, std::string_view content, int mode = 0644);
std::string read_file_bytes(const fs::path& path);
uint64_t directory_size_bytes(const fs::path& root);

// ------------------------------------------------------------------------------
// Minimal RFC 8259 JSON Parser & Serializer
// ------------------------------------------------------------------------------
namespace json {

struct Value;
using Object = std::map<std::string, Value>;
using Array  = std::vector<Value>;

struct Value {
    using Variant = std::variant<std::monostate, bool, double, std::string, Array, Object>;
    Variant v;

    bool is_null()   const { return std::holds_alternative<std::monostate>(v); }
    bool is_bool()   const { return std::holds_alternative<bool>(v); }
    bool is_number() const { return std::holds_alternative<double>(v); }
    bool is_string() const { return std::holds_alternative<std::string>(v); }
    bool is_array()  const { return std::holds_alternative<Array>(v); }
    bool is_object() const { return std::holds_alternative<Object>(v); }

    std::string as_string(std::string_view fallback = "") const;
    long as_int(long fallback = 0) const;
    double as_double(double fallback = 0.0) const;
    bool as_bool(bool fallback = false) const;
    const Value* get(std::string_view key) const;
    std::string get_string(std::string_view key, std::string_view fallback = "") const;
    long get_int(std::string_view key, long fallback = 0) const;
    bool get_bool(std::string_view key, bool fallback = false) const;
};

Value parse(std::string_view input);
std::string escape(std::string_view sv);

} // namespace json

// ------------------------------------------------------------------------------
// Model Catalog Asset & Unified Runtime Configuration
// ------------------------------------------------------------------------------
struct ModelAsset {
    std::string key;
    std::string category;
    std::vector<std::string> tiers;
    std::string safety = "standard";
    std::string filename;
    std::string subdir;
    std::string url;
    double      approx_gb = 0.0;
    std::string description;
    bool        requires_auth = false;
    std::vector<std::string> dependencies;
};

struct Config {
    std::string home_dir;
    std::string cuda_home        = "/usr/local/cuda";
    std::string lais_dir;        // ~/lais
    std::string bin_dir;         // ~/lais/bin
    std::string configs_dir;     // ~/lais/configs
    std::string data_dir;        // ~/lais/data
    std::string chats_dir;       // ~/lais/chats
    std::string engines_dir;     // ~/lais/engines
    std::string models_dir;      // ~/lais/models
    std::string cache_dir;       // ~/lais/cache
    std::string media_out_dir;   // ~/lais/outputs
    std::string logs_dir;        // ~/lais/logs
    std::string config_file;     // ~/lais/configs/settings.json
    std::string sources_file;    // ~/lais/configs/sources.json
    std::string models_file;     // ~/lais/configs/models.json
    std::string user_file;       // ~/lais/data/user.json
    std::string sessions_file;   // ~/lais/data/sessions.json
    std::string history_file;    // ~/lais/history.log
    std::string pid_file;        // ~/lais/cache/lais.pid

    std::string bind_host          = "0.0.0.0";
    int         bind_port          = 3000;
    int         llama_port         = 18080;
    std::string default_tier       = "auto";
    bool        default_uncensored = false; // Default: Safe Mode (Censored / Aligned)
    long        idle_timeout_sec   = 45;

    long vram_reserve_mb         = 2048;
    long vram_high_llm_min_mb    = 14592;
    long dram_high_llm_min_mb    = 4096;
    long vram_high_flux_min_mb   = 14336;
    long dram_high_flux_min_mb   = 4096;
    long vram_high_vid_min_mb    = 14336;
    long dram_high_vid_min_mb    = 4096;
    long vram_high_mus_min_mb    = 11000;
    long dram_high_mus_min_mb    = 4096;

    std::string rag_search_url;
    std::string http_user_agent;

    std::string llama_cpp_repo;
    long        llama_cpp_min_build = 10261;
    std::string stable_diffusion_cpp_repo;
    std::string stable_diffusion_cpp_tag;
    std::string acestep_cpp_repo;
    std::string cuda_keyring_deb_url;
    std::string cuda_apt_package;
    std::string miniforge_linux_x86_64_url;

    std::map<std::string, std::string> roles;
    std::vector<ModelAsset>            models;
    json::Value                       generation_profiles;

    void init_paths();
    void ensure_directories() const;
    void sync_seed_configs(const char* argv0 = nullptr) const;
    bool load_settings_json();
    bool load_sources_json();
    bool load_models_json();
    bool load_config_file();
    bool save_config_file() const;
    std::string role_filename(std::string_view role_key) const;
    const json::Value& generation_profile(std::string_view key) const;
    void append_command_history(std::string_view cmd_line) const;
    std::vector<std::string> load_command_history_lines() const;
    static Config from_env(const char* argv0 = nullptr);
};

std::vector<ModelAsset> match_models_by_selector(const Config& cfg, std::string_view selector);

// ------------------------------------------------------------------------------
// Persistent Chat Session Store (With Thinking Separation & Branching)
// ------------------------------------------------------------------------------
struct ChatTurn {
    std::string role;
    std::string content;
};

class ChatStore {
public:
    static std::string sanitize_id(std::string_view raw);
    static std::string generate_chat_id();
    static std::string derive_title(std::string_view prompt);
    static std::string list_sessions_json(const Config& cfg);
    static std::string load_session_json(const Config& cfg, std::string_view raw_id);
    static std::vector<ChatTurn> load_recent_turns(const Config& cfg, std::string_view raw_id, std::size_t max_turns = 10);
    static bool delete_session(const Config& cfg, std::string_view raw_id);
    static std::string branch_session_json(
        const Config& cfg,
        std::string_view source_chat_id,
        int message_index,
        std::string_view custom_title = "");
    static std::string append_exchange(
        const Config& cfg,
        std::string_view raw_chat_id,
        std::string_view user_prompt,
        std::string_view attached_name,
        std::string_view assistant_reply,
        std::string_view assistant_thinking,
        std::string_view selected_tier,
        std::string_view selected_model,
        std::string_view routing_reason,
        std::string_view media_url,
        std::string_view download_url,
        std::string_view kind);
};

// ------------------------------------------------------------------------------
// Telemetry & Memory Reclamation Declarations
// ------------------------------------------------------------------------------
struct MemoryStats {
    long dram_total_mb  = 32768;
    long dram_avail_mb  = 16384;
    long dram_used_mb   = 16384;
    long dram_cached_mb = 0;
    int  dram_used_pct  = 50;

    long vram_total_mb  = 16384;
    long vram_free_mb   = 14000;
    long vram_used_mb   = 2384;
    int  vram_used_pct  = 15;
    std::string gpu_name = "NVIDIA GPU (16GB VRAM Profile)";
};

MemoryStats query_memory_stats_sync();
void trim_process_memory();
std::size_t purge_dram_and_pagecache(const std::string& models_root);
std::string powershell_encoded_command(std::string_view ascii_script);
std::string url_encode(std::string_view value);
std::string fetch_duckduckgo_rag(const Config& cfg, std::string_view query, int max_results = 4);

// ------------------------------------------------------------------------------
// Engine Manager & Service Declarations
// ------------------------------------------------------------------------------
bool download_hf_asset(const std::string& url, const fs::path& dest, const std::string& label, double approx_gb = 0.0);
void maybe_quantize_wan_eco(const Config& cfg);
bool extract_embedded_engines(Config& cfg, const char* argv0 = nullptr, bool force_extract = false);
bool build_cpp_ai_engines(const Config& cfg, bool force_update = false);
bool install_models_by_selector(Config& cfg, std::string_view selector);
bool uninstall_models_by_selector(Config& cfg, std::string_view selector);
fs::path install_self_binary(const Config& cfg, const char* argv0, bool install_usr_local = true);
void sync_existing_global_binaries(const Config& cfg, const char* argv0);
bool is_service_running(const Config& cfg);
bool start_lais_service(Config& cfg, const char* argv0);
bool stop_lais_service(const Config& cfg);
void print_models_table(Config& cfg);
void print_status(Config& cfg);
void print_help();
bool execute_command(Config& cfg, const std::vector<std::string>& tokens, const char* argv0, bool& should_exit);

// ------------------------------------------------------------------------------
// Thinking Parser & InferenceCoordinator
// ------------------------------------------------------------------------------
void split_thinking_and_reply(std::string_view raw_text, std::string& out_thinking, std::string& out_reply);

class InferenceCoordinator {
public:
    explicit InferenceCoordinator(const Config& cfg);
    ~InferenceCoordinator();

    struct RouteDecision {
        std::string modality;
        std::string selected_tier;
        std::string model_name;
        std::string model_path;
        std::string aux_model_path;
        std::string reason;
        MemoryStats mem_before;
    };

    MemoryStats get_cached_stats() const;
    std::string active_llm_model() const;
    void force_purge_all_memory();

    RouteDecision select_text_model_locked(
        std::string_view task_mode,
        bool reasoning_on,
        bool has_image,
        std::string_view tier_override,
        bool uncensored);

    std::string handle_chat_completion(
        const std::string& prompt,
        const std::string& system_prompt,
        const std::string& task_mode,
        bool reasoning_on,
        bool web_search_on,
        const std::string& image_base64,
        const std::string& tier_override,
        bool uncensored = false,
        const std::string& chat_id = "",
        const std::string& attached_name = "");

    std::string handle_image_generation(
        const std::string& prompt,
        int width,
        int height,
        int steps,
        const std::string& tier_override,
        bool uncensored = false,
        int64_t seed = -1,
        const std::string& chat_id = "",
        const std::string& attached_name = "");

    std::string handle_video_generation(
        const std::string& prompt,
        const std::string& negative_prompt,
        int width,
        int height,
        int frames,
        const std::string& tier_override,
        bool uncensored = false,
        int64_t seed = -1,
        const std::string& chat_id = "",
        const std::string& attached_name = "");

    std::string handle_music_generation(
        const std::string& tags,
        const std::string& lyrics,
        int seconds,
        const std::string& tier_override,
        bool uncensored = false,
        const std::string& chat_id = "",
        const std::string& attached_name = "");

private:
    class ExecutionGuard;
    Config cfg_;
    mutable std::mutex exec_mu_;
    mutable std::mutex state_mu_;
    MemoryStats cached_stats_;
    pid_t llama_pid_ = -1;
    std::string active_llm_path_;
    std::string active_mmproj_path_;
    std::string active_llm_name_ = "None (Idle - 0 MB VRAM)";
    std::atomic<std::int64_t> last_activity_ms_{0};
    std::atomic<bool> stopping_{false};
    std::thread watchdog_thread_;

    static std::int64_t now_ms();
    void touch_activity();
    void set_active_badge(const std::string& label);
    void background_maintenance_loop();

    void ensure_engine_binaries_ready_locked();
    bool ensure_model_file_ready_locked(const fs::path& model_path, std::string_view filename);

    // Translates non-English (Korean, Japanese, Chinese, etc.) prompts into detailed
    // English visual descriptions for Flux.1-dev (whose T5-v1_1-XXL + CLIP-L encoders
    // are English-only) and Wan 2.1 before unloading llama-server.
    std::string translate_media_prompt_to_english_locked(
        const std::string& raw_prompt,
        bool uncensored,
        bool& out_was_translated);

    fs::path find_llama_server_binary() const;
    fs::path find_sd_binary() const;
    fs::path find_ace_lm_binary() const;
    fs::path find_ace_synth_binary() const;

    bool is_llama_process_alive_locked();
    void stop_llama_server_locked();
    bool ensure_llama_server_locked(
        const std::string& model_path,
        const std::string& mmproj_path,
        const std::string& model_name);

    static std::string make_error_json(const std::string& err, const RouteDecision& r);
    static std::string make_media_json(
        const std::string& md,
        const std::string& url,
        const std::string& download_url,
        const std::string& filename,
        const std::string& kind,
        const std::string& chat_id,
        const RouteDecision& r,
        const MemoryStats& after);
};

bool run_http_server_forever(const Config& cfg, InferenceCoordinator& coord);

} // namespace lais
