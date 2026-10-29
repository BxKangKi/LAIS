// ==============================================================================
// LAIS (Local AI Studio) v3.7.0 - Multi-Threaded HTTP/1.1 Server & PAKE Routes
// SPDX-License-Identifier: MIT
// ==============================================================================
#include "common.hpp"
#include "crypto_pake.hpp"
#include "webui.hpp"

namespace lais {

class HttpServer {
public:
    HttpServer(const Config& cfg, InferenceCoordinator& coord)
        : cfg_(cfg), coord_(coord), auth_(cfg) {}

    bool run_forever() {
        socket_t server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd == kInvalidSocket) {
            log_err("Failed to create TCP socket.");
            return false;
        }
        int opt = 1;
        ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#ifdef SO_REUSEPORT
        ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&opt), sizeof(opt));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<uint16_t>(cfg_.bind_port));
        if (cfg_.bind_host == "0.0.0.0" || cfg_.bind_host.empty()) {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else if (::inet_pton(AF_INET, cfg_.bind_host.c_str(), &addr.sin_addr) <= 0) {
            log_err("Invalid bind IP address: " + cfg_.bind_host);
            close_socket(server_fd);
            return false;
        }

        if (::bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            log_err("Failed to bind to " + cfg_.bind_host + ":" + std::to_string(cfg_.bind_port) + ".");
            close_socket(server_fd);
            return false;
        }
        if (::listen(server_fd, 128) < 0) {
            log_err("Failed to listen on TCP socket.");
            close_socket(server_fd);
            return false;
        }

        write_file_atomic(cfg_.pid_file, std::to_string(get_current_pid()) + "\n", 0644);

        log_ok("LAIS Server v" + std::string(LAIS_VERSION) + " listening on http://" +
               cfg_.bind_host + ":" + std::to_string(cfg_.bind_port) +
               " (Data root: " + cfg_.lais_dir + ")");

        while (!g_shutdown_requested.load(std::memory_order_relaxed)) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(server_fd, &readfds);
            timeval tv{1, 0};
            int sel = ::select(static_cast<int>(server_fd) + 1, &readfds, nullptr, nullptr, &tv);
            if (sel <= 0) continue;

            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            socket_t client_fd = ::accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
            if (client_fd == kInvalidSocket) continue;

            bool is_loopback = (ntohl(client_addr.sin_addr.s_addr) == INADDR_LOOPBACK);

            if (active_workers_.load(std::memory_order_relaxed) >= 64) {
                send_response(client_fd, 503, "application/json", "{\"error\":\"Server busy\"}");
                close_socket(client_fd);
                continue;
            }

            active_workers_.fetch_add(1, std::memory_order_relaxed);
            std::thread([this, client_fd, is_loopback]() {
                struct ScopeGuard {
                    std::atomic<int>& counter;
                    socket_t fd;
                    ~ScopeGuard() {
                        close_socket(fd);
                        counter.fetch_sub(1, std::memory_order_relaxed);
                    }
                } guard{active_workers_, client_fd};
                this->handle_client(client_fd, is_loopback);
            }).detach();
        }

        close_socket(server_fd);
        std::error_code ec;
        fs::remove(cfg_.pid_file, ec);
        log_info("LAIS HTTP Server stopped cleanly.");
        return true;
    }

private:
    Config cfg_;
    InferenceCoordinator& coord_;
    PakeAuthManager auth_;
    std::atomic<int> active_workers_{0};
    struct GenerationEntry {
        std::shared_ptr<GenerationState> state = std::make_shared<GenerationState>();
        std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
        bool started = false;
    };
    std::mutex generations_mu_;
    // Session + request ID prevents another browser session from cancelling work.
    std::map<std::pair<std::string, std::string>, GenerationEntry> generations_;

    static bool valid_request_id(const std::string& id) {
        return id.size() >= 8 && id.size() <= 128 &&
            std::all_of(id.begin(), id.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '_';
            });
    }
    void prune_generations_locked() {
        const auto cutoff = std::chrono::steady_clock::now() - std::chrono::minutes(10);
        for (auto it = generations_.begin(); it != generations_.end();) {
            if (it->second.created < cutoff && (!it->second.started || it->second.state->finished.load()))
                it = generations_.erase(it);
            else ++it;
        }
    }

    static void configure_client_socket(socket_t fd) {
        int nodelay = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
#ifdef _WIN32
        DWORD rcv_ms = 30000;
        DWORD snd_ms = 120000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcv_ms), sizeof(rcv_ms));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&snd_ms), sizeof(snd_ms));
#else
        timeval rcv_tv{30, 0};
        timeval snd_tv{120, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_tv, sizeof(snd_tv));
#endif
    }

    static std::size_t parse_content_length_ci(std::string_view headers) {
        std::string lower = to_lower_ascii(headers);
        std::size_t pos = lower.find("content-length:");
        if (pos == std::string::npos) return 0;
        pos += 15;
        while (pos < lower.size() && (lower[pos] == ' ' || lower[pos] == '\t')) ++pos;
        return static_cast<std::size_t>(std::strtoull(lower.c_str() + pos, nullptr, 10));
    }

    static std::string extract_session_token(std::string_view headers) {
        std::istringstream iss{std::string(headers)};
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string low = to_lower_ascii(line);
            if (low.rfind("cookie:", 0) == 0) {
                std::string cookies = line.substr(7);
                std::size_t p = cookies.find("lais_session=");
                if (p != std::string::npos) {
                    p += 13;
                    std::size_t end = cookies.find(';', p);
                    return trim_copy(cookies.substr(p, end == std::string::npos ? std::string::npos : (end - p)));
                }
            } else if (low.rfind("authorization:", 0) == 0) {
                std::string auth = trim_copy(line.substr(14));
                if (to_lower_ascii(auth).rfind("bearer ", 0) == 0) {
                    return trim_copy(auth.substr(7));
                }
            }
        }
        return {};
    }

    static bool has_browser_cookie_or_ua(std::string_view headers) {
        std::string low = to_lower_ascii(headers);
        return low.find("mozilla/") != std::string::npos || low.find("cookie:") != std::string::npos;
    }

    void handle_client(socket_t fd, bool is_loopback) {
        configure_client_socket(fd);

        std::string req;
        req.reserve(8192);
        char buf[8192];
        std::size_t header_end = std::string::npos;
        std::size_t content_len = 0;

        while (true) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, static_cast<std::size_t>(n));
            if (header_end == std::string::npos) {
                header_end = req.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    content_len = parse_content_length_ci(std::string_view(req).substr(0, header_end));
                }
            }
            if (header_end != std::string::npos && req.size() >= header_end + 4 + content_len) {
                break;
            }
            if (req.size() > 32 * 1024 * 1024) break;
        }

        if (req.empty()) return;

        std::size_t line_end = req.find("\r\n");
        std::istringstream iss(req.substr(0, line_end));
        std::string method, raw_path, proto;
        iss >> method >> raw_path >> proto;

        std::string path = raw_path;
        std::string query_str;
        if (std::size_t q = path.find('?'); q != std::string::npos) {
            query_str = path.substr(q + 1);
            path.resize(q);
        }

        std::string headers_str = (header_end != std::string::npos) ? req.substr(0, header_end) : req;
        std::string session_token = extract_session_token(headers_str);

        std::string body = (header_end != std::string::npos && req.size() > header_end + 4)
            ? req.substr(header_end + 4, content_len)
            : "";

        if (method == "OPTIONS") {
            send_response(fd, 200, "text/plain", "");
            return;
        }

        // Public endpoints (WebUI shell, health check, and PAKE authentication handshake)
        if (method == "GET" && (path == "/" || path == "/index.html")) {
            send_response(fd, 200, "text/html; charset=utf-8", WEBUI_HTML);
            return;
        }
        if (method == "GET" && path == "/health") {
            send_response(fd, 200, "application/json",
                std::string("{\"status\":\"ok\",\"service\":\"lais\",\"version\":\"") + LAIS_VERSION + "\"}");
            return;
        }
        if (method == "GET" && path.rfind("/api/locales/", 0) == 0) {
            const std::string locale = path.substr(std::string("/api/locales/").size());
            if (locale != "en-us" && locale != "ko-kr") {
                send_response(fd, 404, "application/json", "{\"error\":\"Unknown interface language\"}");
                return;
            }
            const std::string bundle = read_file_bytes(fs::path(cfg_.configs_dir) / (locale + ".lang"));
            if (bundle.size() > 262144 || !json::parse(bundle).is_object()) {
                send_response(fd, 503, "application/json", "{\"error\":\"Language bundle unavailable\"}");
                return;
            }
            send_response(fd, 200, "application/json; charset=utf-8", bundle);
            return;
        }
        if (method == "GET" && path == "/api/auth/status") {
            send_response(fd, 200, "application/json", auth_.get_auth_status_json(session_token));
            return;
        }
        if (method == "POST" && path == "/api/auth/pake-init") {
            json::Value j = json::parse(body);
            std::string user = j.get_string("username", "");
            std::string c_nonce = j.get_string("client_nonce", "");
            send_response(fd, 200, "application/json", auth_.create_challenge_json(user, c_nonce));
            return;
        }
        if (method == "POST" && path == "/api/auth/pake-verify") {
            json::Value j = json::parse(body);
            std::string cid   = j.get_string("challenge_id", "");
            std::string user  = j.get_string("username", "");
            std::string proof = j.get_string("client_proof", "");
            bool auto_login   = j.get_bool("auto_login", true);
            std::string issued_token;
            std::string resp = auth_.verify_challenge_json(cid, user, proof, auto_login, issued_token);
            std::string set_cookie;
            if (!issued_token.empty()) {
                set_cookie = "lais_session=" + issued_token + "; Path=/; SameSite=Lax; HttpOnly";
                if (auto_login) {
                    set_cookie += "; Max-Age=2592000";
                }
            }
            send_response(fd, 200, "application/json", resp, "", set_cookie);
            return;
        }
        if (method == "POST" && path == "/api/auth/logout") {
            auth_.invalidate_session(session_token);
            std::string clear_cookie = "lais_session=; Path=/; Max-Age=0; SameSite=Lax; HttpOnly";
            send_response(fd, 200, "application/json", "{\"ok\":true}", "", clear_cookie);
            return;
        }

        // Enforce PAKE session authentication on all browser API calls
        bool is_cli_loopback = is_loopback && !has_browser_cookie_or_ua(headers_str) &&
                               (path == "/api/purge-memory" || path == "/v1/chat/completions" || path == "/v1/models");
        if (!is_cli_loopback && !auth_.is_session_valid(session_token)) {
            send_response(fd, 401, "application/json", "{\"ok\":false,\"error\":\"Unauthorized. Please sign in via PAKE.\"}");
            return;
        }

        if (method == "POST" && path == "/api/generation/stop") {
            const std::string id = json::parse(body).get_string("request_id");
            if (!valid_request_id(id)) {
                send_response(fd, 400, "application/json", "{\"ok\":false,\"error\":\"Invalid request_id\"}");
                return;
            }
            std::shared_ptr<GenerationState> state;
            {
                std::lock_guard<std::mutex> lock(generations_mu_);
                prune_generations_locked();
                const auto key = std::make_pair(session_token, id);
                // Retain an early Stop until the matching POST arrives. This
                // handles immediate clicks and independently scheduled workers.
                if (!generations_.count(key) && generations_.size() >= 4096) {
                    send_response(fd, 503, "application/json", "{\"ok\":false,\"error\":\"Too many generation requests\"}");
                    return;
                }
                state = generations_[key].state;
            }
            const bool cancelled = state->request_cancel();
            send_response(fd, 200, "application/json", cancelled
                ? "{\"ok\":true,\"cancelled\":true,\"status\":\"stopping\"}"
                : "{\"ok\":true,\"cancelled\":false,\"status\":\"completed\"}");
            return;
        }

        std::shared_ptr<GenerationState> generation;
        const bool generating = method == "POST" && (path == "/api/chat" || path == "/v1/chat/completions" ||
            path == "/api/generate-image" || path == "/api/generate-video" || path == "/api/generate-music");
        if (generating) {
            const std::string id = json::parse(body).get_string("request_id");
            if (id.empty()) {
                // Keep existing API clients compatible; the WebUI always sends an ID.
                generation = std::make_shared<GenerationState>();
            } else {
                if (!valid_request_id(id)) {
                    send_response(fd, 400, "application/json", "{\"ok\":false,\"error\":\"Invalid request_id\"}");
                    return;
                }
                std::lock_guard<std::mutex> lock(generations_mu_);
                prune_generations_locked();
                const auto key = std::make_pair(session_token, id);
                if (!generations_.count(key) && generations_.size() >= 4096) {
                    send_response(fd, 503, "application/json", "{\"ok\":false,\"error\":\"Too many generation requests\"}");
                    return;
                }
                auto& entry = generations_[key];
                if (entry.started) {
                    send_response(fd, 409, "application/json", "{\"ok\":false,\"error\":\"request_id already used\"}");
                    return;
                }
                entry.started = true;
                generation = entry.state;
            }
        }
        struct GenerationEnd {
            std::shared_ptr<GenerationState> state;
            ~GenerationEnd() { if (state) state->finish(); }
        } generation_end{generation};
        GenerationScope generation_scope(generation);
        try {
        generation_checkpoint();
        if (method == "POST" && path == "/api/auth/change-credentials") {
            json::Value j = json::parse(body);
            std::string resp = auth_.change_credentials_json(
                session_token,
                j.get_string("challenge_id", ""),
                j.get_string("current_client_proof", ""),
                j.get_string("new_username", ""),
                j.get_string("new_salt_hex", ""),
                static_cast<int>(j.get_int("new_iterations", 10000)),
                j.get_string("new_stored_key_hex", ""),
                j.get_string("new_server_key_hex", ""));
            send_response(fd, 200, "application/json", resp);
        } else if (method == "GET" && path == "/v1/models") {
            std::ostringstream mlist;
            mlist << "{\"object\":\"list\",\"data\":["
                  << "{\"id\":\"lais-standard\",\"object\":\"model\",\"owned_by\":\"lais\",\"safety\":\"standard\"},"
                  << "{\"id\":\"lais-uncensored\",\"object\":\"model\",\"owned_by\":\"lais\",\"safety\":\"uncensored\"}";
            for (const auto& m : cfg_.models) {
                mlist << ",{\"id\":\"" << json::escape(m.key) << "\","
                      << "\"filename\":\"" << json::escape(m.filename) << "\","
                      << "\"category\":\"" << json::escape(m.category) << "\","
                      << "\"tiers\":[";
                for (std::size_t i = 0; i < m.tiers.size(); ++i) {
                    if (i) mlist << ",";
                    mlist << "\"" << json::escape(m.tiers[i]) << "\"";
                }
                mlist << "],"
                      << "\"safety\":\"" << json::escape(m.safety) << "\","
                      << "\"object\":\"model\",\"owned_by\":\"lais\"}";
            }
            mlist << "]}";
            send_response(fd, 200, "application/json", mlist.str());
        } else if (method == "GET" && path == "/api/chats") {
            send_response(fd, 200, "application/json", ChatStore::list_sessions_json(cfg_));
        } else if (method == "POST" && path == "/api/chats/branch") {
            json::Value j = json::parse(body);
            std::string src_id = j.get_string("source_chat_id", "");
            int msg_idx        = static_cast<int>(j.get_int("message_index", -1));
            std::string title  = j.get_string("title", "");
            send_response(fd, 200, "application/json", ChatStore::branch_session_json(cfg_, src_id, msg_idx, title));
        } else if (method == "GET" && path.rfind("/api/chats/", 0) == 0) {
            std::string cid = path.substr(11);
            send_response(fd, 200, "application/json", ChatStore::load_session_json(cfg_, cid));
        } else if (method == "DELETE" && path.rfind("/api/chats/", 0) == 0) {
            std::string cid = path.substr(11);
            bool ok = ChatStore::delete_session(cfg_, cid);
            send_response(fd, 200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
        } else if (method == "GET" && path == "/api/system-stats") {
            MemoryStats st = coord_.get_cached_stats();
            std::ostringstream o;
            o << "{"
              << "\"gpu_name\":\"" << json::escape(st.gpu_name) << "\","
              << "\"vram_total_mb\":" << st.vram_total_mb << ","
              << "\"vram_used_mb\":" << st.vram_used_mb << ","
              << "\"vram_free_mb\":" << st.vram_free_mb << ","
              << "\"vram_used_pct\":" << st.vram_used_pct << ","
              << "\"dram_total_mb\":" << st.dram_total_mb << ","
              << "\"dram_used_mb\":" << st.dram_used_mb << ","
              << "\"dram_avail_mb\":" << st.dram_avail_mb << ","
              << "\"dram_cached_mb\":" << st.dram_cached_mb << ","
              << "\"dram_used_pct\":" << st.dram_used_pct << ","
              << "\"active_llm\":\"" << json::escape(coord_.active_llm_model()) << "\","
              << "\"lais_dir\":\"" << json::escape(cfg_.lais_dir) << "\""
              << "}";
            send_response(fd, 200, "application/json", o.str());
        } else if (method == "POST" && path == "/api/purge-memory") {
            coord_.force_purge_all_memory();
            send_response(fd, 200, "application/json", "{\"ok\":true,\"status\":\"VRAM and DRAM page cache purged\"}");
        } else if (method == "POST" && (path == "/api/chat" || path == "/v1/chat/completions")) {
            json::Value j = json::parse(body);
            std::string prompt   = j.get_string("prompt", "");
            std::string sys      = j.get_string("system", "");
            std::string img64    = j.get_string("image_base64", "");
            std::string chat_id  = j.get_string("chat_id", "");
            std::string att_name = j.get_string("attached_name", "");

            if (prompt.empty()) {
                if (const json::Value* msgs = j.get("messages"); msgs && msgs->is_array()) {
                    std::ostringstream conv;
                    const auto& arr = std::get<json::Array>(msgs->v);
                    for (std::size_t idx = 0; idx < arr.size(); ++idx) {
                        const auto& m = arr[idx];
                        std::string role = m.get_string("role", "");
                        std::string content_text;
                        if (const json::Value* cval = m.get("content")) {
                            if (cval->is_string()) {
                                content_text = cval->as_string();
                            } else if (cval->is_array()) {
                                for (const auto& part : std::get<json::Array>(cval->v)) {
                                    std::string ptype = part.get_string("type", "");
                                    if (ptype == "text") {
                                        content_text += part.get_string("text", "");
                                    } else if (ptype == "image_url") {
                                        if (const json::Value* iu = part.get("image_url")) {
                                            std::string u = iu->get_string("url", "");
                                            if (!u.empty()) img64 = u;
                                        }
                                    }
                                }
                            }
                        }
                        if (role == "system" && sys.empty()) {
                            sys = content_text;
                        } else if (!content_text.empty()) {
                            if (arr.size() <= 2 && role == "user") {
                                prompt = content_text;
                            } else {
                                conv << (role == "assistant" ? "Assistant: " : "User: ")
                                     << content_text << "\n";
                            }
                        }
                    }
                    if (prompt.empty()) prompt = trim_copy(conv.str());
                }
            }

            std::string mode = j.get_string("task_mode", "chat");
            std::string req_model = to_lower_ascii(j.get_string("model", ""));
            if (req_model.find("coder") != std::string::npos) mode = "coder";

            bool thinking = j.get_bool("thinking", false) ||
                            j.get_bool("think", false) ||
                            j.get_bool("reasoning", false) ||
                            (req_model.find("thinking") != std::string::npos);
            bool web_search   = j.get_bool("web_search", false);
            bool default_unc  = cfg_.default_uncensored;
            if (req_model.find("uncensored") != std::string::npos ||
                req_model.find("abliterated") != std::string::npos ||
                req_model == "llm_high" || req_model == "llm_eco" ||
                req_model == "coder_high" || req_model == "coder_eco" ||
                req_model == "vision_high" || req_model == "vision_eco") {
                default_unc = true;
            } else if (req_model.find("standard") != std::string::npos ||
                       req_model.find("_std_") != std::string::npos) {
                default_unc = false;
            }
            bool uncensored   = j.get_bool("uncensored", default_unc);
            std::string default_tier = "auto";
            if (req_model.find("27b") != std::string::npos || req_model.find("high") != std::string::npos) {
                default_tier = "high";
            } else if (req_model.find("9b") != std::string::npos || req_model.find("eco") != std::string::npos) {
                default_tier = "eco";
            }
            std::string tier  = j.get_string("tier", default_tier);

            std::string resp = coord_.handle_chat_completion(
                prompt, sys, mode, thinking, web_search, img64, tier, uncensored, chat_id, att_name);
            send_response(fd, 200, "application/json", resp);
        } else if (method == "POST" && path == "/api/generate-image") {
            json::Value j = json::parse(body);
            std::string prompt   = j.get_string("prompt", "");
            int w                = static_cast<int>(j.get_int("width", 1024));
            int h                = static_cast<int>(j.get_int("height", 1024));
            int steps            = static_cast<int>(j.get_int("steps", 0)); // 0 selects the model's preset
            std::string tier     = j.get_string("tier", "auto");
            bool uncensored      = j.get_bool("uncensored", cfg_.default_uncensored);
            int64_t seed         = static_cast<int64_t>(j.get_int("seed", -1));
            std::string chat_id  = j.get_string("chat_id", "");
            std::string att_name = j.get_string("attached_name", "");
            std::string resp = coord_.handle_image_generation(prompt, w, h, steps, tier, uncensored, seed, chat_id, att_name);
            send_response(fd, 200, "application/json", resp);
        } else if (method == "POST" && path == "/api/generate-video") {
            json::Value j = json::parse(body);
            std::string prompt   = j.get_string("prompt", "");
            std::string neg      = j.get_string("negative_prompt", "");
            int w                = static_cast<int>(j.get_int("width", 832));
            int h                = static_cast<int>(j.get_int("height", 480));
            int frames           = static_cast<int>(j.get_int("frames", 33));
            std::string tier     = j.get_string("tier", "auto");
            bool uncensored      = j.get_bool("uncensored", cfg_.default_uncensored);
            int64_t seed         = static_cast<int64_t>(j.get_int("seed", -1));
            std::string chat_id  = j.get_string("chat_id", "");
            std::string att_name = j.get_string("attached_name", "");
            std::string resp = coord_.handle_video_generation(prompt, neg, w, h, frames, tier, uncensored, seed, chat_id, att_name);
            send_response(fd, 200, "application/json", resp);
        } else if (method == "POST" && path == "/api/generate-music") {
            json::Value j = json::parse(body);
            std::string tags     = j.get_string("tags", j.get_string("prompt", ""));
            std::string lyrics   = j.get_string("lyrics", "[instrumental]");
            int seconds          = static_cast<int>(j.get_int("seconds", 60));
            std::string tier     = j.get_string("tier", "auto");
            bool uncensored      = j.get_bool("uncensored", cfg_.default_uncensored);
            std::string chat_id  = j.get_string("chat_id", "");
            std::string att_name = j.get_string("attached_name", "");
            std::string resp = coord_.handle_music_generation(tags, lyrics, seconds, tier, uncensored, chat_id, att_name);
            send_response(fd, 200, "application/json", resp);
        } else if (method == "GET" && path.rfind("/media/", 0) == 0) {
            std::string fname = fs::path(path.substr(7)).filename().string();
            fs::path fpath = fs::path(cfg_.media_out_dir) / fname;
            if (!fname.empty() && fs::exists(fpath) && fs::is_regular_file(fpath)) {
                std::string data = read_file_bytes(fpath);
                std::string mime = "application/octet-stream";
                if (fname.ends_with(".png"))       mime = "image/png";
                else if (fname.ends_with(".mp4"))  mime = "video/mp4";
                else if (fname.ends_with(".webm")) mime = "video/webm";
                else if (fname.ends_with(".mp3"))  mime = "audio/mpeg";
                else if (fname.ends_with(".wav"))  mime = "audio/wav";
                else if (fname.ends_with(".md"))   mime = "text/markdown; charset=utf-8";
                else if (fname.ends_with(".txt"))  mime = "text/plain; charset=utf-8";
                bool force_dl = (query_str.find("download=1") != std::string::npos);
                send_response(fd, 200, mime, data, force_dl ? fname : "");
            } else {
                send_response(fd, 404, "text/plain", "Media file not found");
            }
        } else {
            send_response(fd, 404, "application/json", "{\"error\":\"Endpoint not found\"}");
        }
        } catch (const GenerationCancelled&) {
            send_response(fd, 200, "application/json",
                "{\"ok\":false,\"cancelled\":true,\"status\":\"stopped\"}");
        } catch (const std::exception&) {
            send_response(fd, 500, "application/json", "{\"ok\":false,\"error\":\"Request failed\"}");
        }
    }

    static bool send_all(socket_t fd, const char* data, std::size_t len) {
        std::size_t total = 0;
        while (total < len) {
#ifdef MSG_NOSIGNAL
            ssize_t n = ::send(fd, data + total, len - total, MSG_NOSIGNAL);
#else
            ssize_t n = ::send(fd, data + total, len - total, 0);
#endif
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (n == 0) return false;
            total += static_cast<std::size_t>(n);
        }
        return true;
    }

    static void send_response(
        socket_t fd,
        int status,
        std::string_view content_type,
        std::string_view body,
        std::string_view download_filename = "",
        std::string_view set_cookie = "") {

        std::string_view status_text = (status == 200) ? "OK"
                                     : (status == 400) ? "Bad Request"
                                     : (status == 401) ? "Unauthorized"
                                     : (status == 409) ? "Conflict"
                                     : (status == 404) ? "Not Found"
                                     : (status == 503) ? "Service Unavailable"
                                     : "Error";
        std::ostringstream h;
        h << "HTTP/1.1 " << status << " " << status_text << "\r\n"
          << "Content-Type: " << content_type << "\r\n"
          << "Content-Length: " << body.size() << "\r\n";
        if (!download_filename.empty()) {
            h << "Content-Disposition: attachment; filename=\"" << download_filename << "\"\r\n";
        }
        if (!set_cookie.empty()) {
            h << "Set-Cookie: " << set_cookie << "\r\n";
        }
        h << "Access-Control-Allow-Origin: *\r\n"
          << "Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
          << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
          << "Connection: close\r\n\r\n";
        std::string hdr = h.str();
        if (send_all(fd, hdr.data(), hdr.size()) && !body.empty()) {
            (void)send_all(fd, body.data(), body.size());
        }
    }
};

bool run_http_server_forever(const Config& cfg, InferenceCoordinator& coord) {
    HttpServer server(cfg, coord);
    return server.run_forever();
}

} // namespace lais
