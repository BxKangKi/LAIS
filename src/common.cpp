// ==============================================================================
// LAIS (Local AI Studio) v3.7.3 - Common Utilities, JSON, Config & ChatStore
// SPDX-License-Identifier: MIT
// ==============================================================================
#include "common.hpp"
#ifndef _WIN32
#include <cerrno>
#include <spawn.h>
extern char** environ;
#endif

namespace lais {

std::atomic<bool> g_shutdown_requested{false};
std::atomic<bool> g_in_interactive_repl{false};

static thread_local std::shared_ptr<GenerationState> generation_state;

bool GenerationState::request_cancel() {
    std::lock_guard<std::mutex> lock(commit_mu);
    if (finished.load()) return false;
    cancelled.store(true);
    return true;
}

void GenerationState::finish() {
    std::lock_guard<std::mutex> lock(commit_mu);
    finished.store(true);
}

std::shared_ptr<GenerationState> current_generation() { return generation_state; }
bool generation_cancelled() { return generation_state && generation_state->cancelled.load(); }
void generation_checkpoint() {
    if (generation_cancelled()) throw GenerationCancelled{};
}
GenerationScope::GenerationScope(std::shared_ptr<GenerationState> state)
    : previous_(std::move(generation_state)) { generation_state = std::move(state); }
GenerationScope::~GenerationScope() { generation_state = std::move(previous_); }

void close_socket(socket_t s) {
#ifdef _WIN32
    if (s != INVALID_SOCKET) ::closesocket(s);
#else
    if (s >= 0) ::close(s);
#endif
}

void set_env_var(const char* name, const char* value) {
#ifdef _WIN32
    ::_putenv_s(name, value);
#else
    ::setenv(name, value, 1);
#endif
}

pid_t get_current_pid() {
#ifdef _WIN32
    return static_cast<pid_t>(::GetCurrentProcessId());
#else
    return ::getpid();
#endif
}

FILE* open_pipe_read(const char* cmd) {
#ifdef _WIN32
    return ::_popen(cmd, "r");
#else
    return ::popen(cmd, "r");
#endif
}

int close_pipe_read(FILE* pipe) {
#ifdef _WIN32
    return ::_pclose(pipe);
#else
    return ::pclose(pipe);
#endif
}

const char* null_stderr_redirect() {
#ifdef _WIN32
    return " 2>NUL";
#else
    return " 2>/dev/null";
#endif
}

const char* null_all_redirect() {
#ifdef _WIN32
    return " >NUL 2>&1";
#else
    return " >/dev/null 2>&1";
#endif
}

const char* exe_suffix() {
#ifdef _WIN32
    return ".exe";
#else
    return "";
#endif
}

fs::path get_self_exe_path(const char* argv0) {
    std::error_code ec;
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    DWORD len = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return fs::path(buf);
    }
#else
    if (fs::exists("/proc/self/exe", ec)) {
        fs::path p = fs::read_symlink("/proc/self/exe", ec);
        if (!ec && !p.empty()) return p;
    }
#endif
    if (argv0 && *argv0) {
        fs::path p = fs::absolute(fs::path(argv0), ec);
        if (!ec && fs::exists(p, ec)) return p;
    }
    return {};
}

void handle_termination_signal(int sig) {
    if (sig == SIGINT && g_in_interactive_repl.load(std::memory_order_relaxed)) {
        return;
    }
    g_shutdown_requested.store(true, std::memory_order_relaxed);
}

void log_info(std::string_view msg) {
    std::cout << "\033[1;34m[LAIS INFO]\033[0m " << msg << std::endl;
}
void log_ok(std::string_view msg) {
    std::cout << "\033[1;32m[LAIS  OK ]\033[0m " << msg << std::endl;
}
void log_warn(std::string_view msg) {
    std::cout << "\033[1;33m[LAIS WARN]\033[0m " << msg << std::endl;
}
void log_err(std::string_view msg) {
    std::cerr << "\033[1;31m[LAIS ERR ]\033[0m " << msg << std::endl;
}

std::string trim_copy(std::string_view sv) {
    std::size_t first = 0;
    while (first < sv.size() && std::isspace(static_cast<unsigned char>(sv[first]))) ++first;
    std::size_t last = sv.size();
    while (last > first && std::isspace(static_cast<unsigned char>(sv[last - 1]))) --last;
    return std::string(sv.substr(first, last - first));
}

std::string to_lower_ascii(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string shell_quote(std::string_view arg) {
#ifdef _WIN32
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"') out += "\\\"";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
#else
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
#endif
}

std::vector<std::string> split_command_tokens(std::string_view line) {
    std::vector<std::string> tokens;
    std::string cur;
    bool in_single = false;
    bool in_double = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\'' && !in_double) {
            in_single = !in_single;
        } else if (c == '"' && !in_single) {
            in_double = !in_double;
        } else if (std::isspace(static_cast<unsigned char>(c)) && !in_single && !in_double) {
            if (!cur.empty()) {
                tokens.push_back(std::move(cur));
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) tokens.push_back(std::move(cur));
    return tokens;
}

std::string current_timestamp_iso() {
    std::time_t now = std::time(nullptr);
    std::tm tm_val{};
#ifdef _WIN32
    ::localtime_s(&tm_val, &now);
#else
    ::localtime_r(&now, &tm_val);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_val);
    return std::string(buf);
}

bool contains_non_ascii(std::string_view sv) {
    for (unsigned char c : sv) {
        if (c >= 0x80) return true;
    }
    return false;
}

bool contains_explicit_nsfw_intent(std::string_view sv) {
    std::string low = to_lower_ascii(sv);
    static const std::vector<std::string_view> kKeywords = {
        "nsfw", "nude", "nudity", "naked", "topless", "erotic", "porn", "sex",
        "sexual", "nipple", "breasts", "boobs", "pussy", "penis", "hentai",
        "lewd", "undressed", "strip", "lingerie", "fetish", "bdsm",
        "나체", "누드", "알몸", "전라", "반라", "가슴 노출", "성인", "야한",
        "야동", "섹스", "성교", "에로", "19금", "음란", "벗은", "노출"
    };
    for (std::string_view kw : kKeywords) {
        if (low.find(kw) != std::string::npos) return true;
    }
    return false;
}

int64_t generate_random_seed() {
    static std::mutex s_mu;
    std::lock_guard<std::mutex> lk(s_mu);
    static std::mt19937_64 s_rng(
        static_cast<uint64_t>(std::random_device{}()) ^
        (static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) << 1));
    std::uniform_int_distribution<int64_t> dist(100000, 2147483646);
    return dist(s_rng);
}

// Use a private process group/job so Stop also ends descendants (curl, CUDA
// generators and converters). A temporary file avoids a full stdout pipe blocking
// cancellation. CLI commands outside a generation retain their original behavior.
static int run_generation_command(const std::string& cmd, std::string* output) {
    generation_checkpoint();
    auto close_file = [](FILE* file) { std::fclose(file); };
    std::unique_ptr<FILE, decltype(close_file)> capture(output ? std::tmpfile() : nullptr, close_file);
    if (output && !capture) return -1;
    int result = -1;
    bool cancelled = false;
#ifdef _WIN32
    HANDLE job = ::CreateJobObjectA(nullptr, nullptr);
    if (!job) return -1;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        ::CloseHandle(job);
        return -1;
    }
    HANDLE out_handle = nullptr;
    if (capture) {
        HANDLE original = reinterpret_cast<HANDLE>(::_get_osfhandle(::_fileno(capture.get())));
        if (!::DuplicateHandle(::GetCurrentProcess(), original, ::GetCurrentProcess(),
                               &out_handle, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            ::CloseHandle(job);
            return -1;
        }
    }
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    if (out_handle) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = out_handle;
        si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);
    }
    const char* shell = std::getenv("COMSPEC");
    std::string line = shell_quote(shell && *shell ? shell : "C:\\Windows\\System32\\cmd.exe") +
                       " /D /S /C \"" + cmd + "\"";
    PROCESS_INFORMATION pi{};
    BOOL ok = ::CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE,
                               CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (out_handle) ::CloseHandle(out_handle);
    if (!ok) { ::CloseHandle(job); return -1; }
    if (!::AssignProcessToJobObject(job, pi.hProcess) || ::ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, INFINITE);
    } else {
        while (::WaitForSingleObject(pi.hProcess, 50) == WAIT_TIMEOUT) {
            if (generation_cancelled()) {
                cancelled = true;
                ::TerminateJobObject(job, 1);
                ::WaitForSingleObject(pi.hProcess, INFINITE);
                break;
            }
        }
        DWORD exit_code = 1;
        if (::GetExitCodeProcess(pi.hProcess, &exit_code)) result = static_cast<int>(exit_code);
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(job);
#else
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t actions;
    ::posix_spawnattr_init(&attr);
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    ::posix_spawnattr_setpgroup(&attr, 0);
    if (capture) ::posix_spawn_file_actions_adddup2(&actions, ::fileno(capture.get()), STDOUT_FILENO);
    char* args[] = {const_cast<char*>("sh"), const_cast<char*>("-c"), const_cast<char*>(cmd.c_str()), nullptr};
    pid_t child = -1;
    const int spawn_rc = ::posix_spawn(&child, "/bin/sh", &actions, &attr, args, environ);
    ::posix_spawn_file_actions_destroy(&actions);
    ::posix_spawnattr_destroy(&attr);
    if (spawn_rc != 0) return -1;
    int status = 0;
    for (;;) {
        pid_t done = ::waitpid(child, &status, WNOHANG);
        if (done == child) {
            result = WIFEXITED(status) ? WEXITSTATUS(status) : status;
            break;
        }
        if (done < 0 && errno != EINTR) break;
        if (generation_cancelled()) {
            cancelled = true;
            ::kill(-child, SIGKILL);
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
#endif
    if (cancelled) throw GenerationCancelled{};
    generation_checkpoint();
    if (capture) {
        std::rewind(capture.get());
        char buffer[4096];
        for (std::size_t n; (n = std::fread(buffer, 1, sizeof(buffer), capture.get())) != 0;)
            output->append(buffer, n);
    }
    return result;
}

std::string capture_cmd(const std::string& cmd) {
    if (current_generation()) {
        std::string output;
        run_generation_command(cmd, &output);
        return trim_copy(output);
    }
    FILE* pipe = open_pipe_read(cmd.c_str());
    if (!pipe) return {};
    std::string result;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
        result.append(buf);
    }
    close_pipe_read(pipe);
    return trim_copy(result);
}

int run_cmd(const std::string& cmd, bool quiet) {
    std::string full = quiet ? (cmd + null_stderr_redirect()) : cmd;
    if (current_generation()) return run_generation_command(full, nullptr);
    int rc = std::system(full.c_str());
#ifdef _WIN32
    return rc;
#else
    if (rc == -1) return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : rc;
#endif
}

bool command_exists(std::string_view tool) {
#ifdef _WIN32
    std::string cmd = "where " + shell_quote(tool) + null_all_redirect();
#else
    std::string cmd = "command -v " + shell_quote(tool) + null_all_redirect();
#endif
    return std::system(cmd.c_str()) == 0;
}

bool can_run_sudo() {
#ifdef _WIN32
    return false;
#else
    if (::geteuid() == 0) return true;
    if (!command_exists("sudo")) return false;
    if (std::system("sudo -n true >/dev/null 2>&1") == 0) return true;
    return ::isatty(STDIN_FILENO) == 1;
#endif
}

bool write_file_atomic(const fs::path& path, std::string_view content, int mode) {
    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
    }
    fs::path tmp = path.string() + ".tmp." + std::to_string(get_current_pid());
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        ofs.flush();
        if (!ofs) return false;
    }
#ifndef _WIN32
    ::chmod(tmp.c_str(), static_cast<mode_t>(mode));
#else
    (void)mode;
#endif
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::copy_file(tmp, path, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp, ec);
    }
    return !ec;
}

std::string read_file_bytes(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

uint64_t directory_size_bytes(const fs::path& root) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return 0;
    uint64_t total = 0;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) continue;
        if (it->is_regular_file(ec)) {
            total += it->file_size(ec);
        }
    }
    return total;
}

// ------------------------------------------------------------------------------
// JSON Parser & Serializer Implementation
// ------------------------------------------------------------------------------
namespace json {

std::string Value::as_string(std::string_view fallback) const {
    if (auto* s = std::get_if<std::string>(&v)) return *s;
    if (auto* d = std::get_if<double>(&v)) {
        long iv = static_cast<long>(*d);
        if (std::abs(*d - static_cast<double>(iv)) < 1e-9) return std::to_string(iv);
        return std::to_string(*d);
    }
    if (auto* b = std::get_if<bool>(&v)) return *b ? "true" : "false";
    return std::string(fallback);
}

long Value::as_int(long fallback) const {
    if (auto* d = std::get_if<double>(&v)) return static_cast<long>(*d);
    if (auto* s = std::get_if<std::string>(&v)) {
        try { return std::stol(*s); } catch (...) {}
    }
    return fallback;
}

double Value::as_double(double fallback) const {
    if (auto* d = std::get_if<double>(&v)) return *d;
    if (auto* s = std::get_if<std::string>(&v)) {
        try { return std::stod(*s); } catch (...) {}
    }
    return fallback;
}

bool Value::as_bool(bool fallback) const {
    if (auto* b = std::get_if<bool>(&v)) return *b;
    if (auto* d = std::get_if<double>(&v)) return *d != 0.0;
    if (auto* s = std::get_if<std::string>(&v)) {
        std::string low = to_lower_ascii(*s);
        if (low == "true" || low == "1" || low == "yes" || low == "on") return true;
        if (low == "false" || low == "0" || low == "no" || low == "off") return false;
    }
    return fallback;
}

const Value* Value::get(std::string_view key) const {
    if (auto* o = std::get_if<Object>(&v)) {
        auto it = o->find(std::string(key));
        if (it != o->end()) return &it->second;
    }
    return nullptr;
}

std::string Value::get_string(std::string_view key, std::string_view fallback) const {
    const Value* val = get(key);
    return val ? val->as_string(fallback) : std::string(fallback);
}

long Value::get_int(std::string_view key, long fallback) const {
    const Value* val = get(key);
    return val ? val->as_int(fallback) : fallback;
}

bool Value::get_bool(std::string_view key, bool fallback) const {
    const Value* val = get(key);
    return val ? val->as_bool(fallback) : fallback;
}

class Parser {
public:
    explicit Parser(std::string_view src) : src_(src), pos_(0) {}

    Value parse_root() {
        skip_ws();
        if (pos_ >= src_.size()) return Value{};
        return parse_value();
    }

private:
    std::string_view src_;
    std::size_t pos_;

    void skip_ws() {
        while (pos_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[pos_]))) ++pos_;
    }

    Value parse_value() {
        skip_ws();
        if (pos_ >= src_.size()) return Value{};
        char c = src_[pos_];
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return Value{parse_string()};
        if (c == 't' || c == 'f') return parse_bool();
        if (c == 'n') return parse_null();
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return parse_number();
        return Value{};
    }

    Value parse_object() {
        Object obj;
        ++pos_;
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == '}') {
            ++pos_;
            return Value{std::move(obj)};
        }
        while (pos_ < src_.size()) {
            skip_ws();
            if (pos_ >= src_.size() || src_[pos_] != '"') break;
            std::string key = parse_string();
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == ':') ++pos_;
            Value val = parse_value();
            obj.emplace(std::move(key), std::move(val));
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < src_.size() && src_[pos_] == '}') {
                ++pos_;
                break;
            }
            break;
        }
        return Value{std::move(obj)};
    }

    Value parse_array() {
        Array arr;
        ++pos_;
        skip_ws();
        if (pos_ < src_.size() && src_[pos_] == ']') {
            ++pos_;
            return Value{std::move(arr)};
        }
        while (pos_ < src_.size()) {
            arr.push_back(parse_value());
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < src_.size() && src_[pos_] == ']') {
                ++pos_;
                break;
            }
            break;
        }
        return Value{std::move(arr)};
    }

    static void append_utf8(std::string& out, uint32_t cp) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0x10FFFF) {
            out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    uint32_t parse_hex4() {
        uint32_t val = 0;
        for (int i = 0; i < 4 && pos_ < src_.size(); ++i, ++pos_) {
            char h = src_[pos_];
            val <<= 4;
            if (h >= '0' && h <= '9') val |= static_cast<uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') val |= static_cast<uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') val |= static_cast<uint32_t>(h - 'A' + 10);
        }
        return val;
    }

    std::string parse_string() {
        std::string out;
        if (pos_ < src_.size() && src_[pos_] == '"') ++pos_;
        while (pos_ < src_.size()) {
            char c = src_[pos_++];
            if (c == '"') break;
            if (c == '\\' && pos_ < src_.size()) {
                char esc = src_[pos_++];
                switch (esc) {
                    case '"':  out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/'); break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = parse_hex4();
                        if (cp >= 0xD800 && cp <= 0xDBFF &&
                            pos_ + 1 < src_.size() && src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            uint32_t low = parse_hex4();
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                            }
                        }
                        append_utf8(out, cp);
                        break;
                    }
                    default: out.push_back(esc); break;
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    Value parse_bool() {
        if (src_.substr(pos_, 4) == "true") {
            pos_ += 4;
            return Value{true};
        }
        if (src_.substr(pos_, 5) == "false") {
            pos_ += 5;
            return Value{false};
        }
        ++pos_;
        return Value{false};
    }

    Value parse_null() {
        if (src_.substr(pos_, 4) == "null") pos_ += 4;
        else ++pos_;
        return Value{};
    }

    Value parse_number() {
        std::size_t start = pos_;
        if (src_[pos_] == '-') ++pos_;
        while (pos_ < src_.size() &&
               (std::isdigit(static_cast<unsigned char>(src_[pos_])) ||
                src_[pos_] == '.' || src_[pos_] == 'e' || src_[pos_] == 'E' ||
                src_[pos_] == '+' || src_[pos_] == '-')) {
            ++pos_;
        }
        std::string token(src_.substr(start, pos_ - start));
        try { return Value{std::stod(token)}; } catch (...) { return Value{0.0}; }
    }
};

Value parse(std::string_view input) {
    return Parser(input).parse_root();
}

std::string escape(std::string_view sv) {
    std::string out;
    out.reserve(sv.size() + 16);
    for (unsigned char c : sv) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

} // namespace json

// ------------------------------------------------------------------------------
// Config Implementation
// ------------------------------------------------------------------------------
void Config::init_paths() {
    bin_dir       = lais_dir + "/bin";
    configs_dir   = lais_dir + "/configs";
    data_dir      = lais_dir + "/data";
    chats_dir     = lais_dir + "/chats";
    engines_dir   = lais_dir + "/engines";
    models_dir    = lais_dir + "/models";
    cache_dir     = lais_dir + "/cache";
    media_out_dir = lais_dir + "/outputs";
    logs_dir      = lais_dir + "/logs";
    config_file   = configs_dir + "/settings.json";
    sources_file  = configs_dir + "/sources.json";
    models_file   = configs_dir + "/models.json";
    user_file     = data_dir + "/user.json";
    sessions_file = data_dir + "/sessions.json";
    history_file  = lais_dir + "/history.log";
    pid_file      = cache_dir + "/lais.pid";
}

void Config::ensure_directories() const {
    std::error_code ec;
    fs::create_directories(lais_dir, ec);
    fs::create_directories(bin_dir, ec);
    fs::create_directories(configs_dir, ec);
    fs::create_directories(data_dir, ec);
    fs::create_directories(chats_dir, ec);
    fs::create_directories(engines_dir, ec);
    fs::create_directories(models_dir, ec);
    fs::create_directories(models_dir + "/llm", ec);
    fs::create_directories(models_dir + "/diffusion", ec);
    fs::create_directories(models_dir + "/text_encoders", ec);
    fs::create_directories(models_dir + "/vae", ec);
    fs::create_directories(models_dir + "/music", ec);
    fs::create_directories(cache_dir, ec);
    fs::create_directories(cache_dir + "/tmp", ec);
    fs::create_directories(cache_dir + "/hf", ec);
    fs::create_directories(media_out_dir, ec);
    fs::create_directories(logs_dir, ec);

    set_env_var("XDG_CACHE_HOME", cache_dir.c_str());
    set_env_var("HF_HOME", (cache_dir + "/hf").c_str());
    set_env_var("TMPDIR", (cache_dir + "/tmp").c_str());
}

// Upgrade an older catalog once, preserving both the old file and same-version edits.
static void sync_dual_media_models_catalog(const fs::path& seed_path, const fs::path& dst_path) {
    std::error_code ec;
    if (!fs::exists(seed_path, ec) || !fs::exists(dst_path, ec)) return;
    if (fs::equivalent(seed_path, dst_path, ec)) return;

    const std::string seed_raw = read_file_bytes(seed_path);
    const std::string dst_raw = read_file_bytes(dst_path);
    json::Value seed = json::parse(seed_raw);
    json::Value dst = json::parse(dst_raw);
    const long seed_version = seed.get_int("catalog_version", 0);
    const long dst_version = dst.get_int("catalog_version", 0);
    const json::Value* seed_roles = seed.get("roles");
    const json::Value* dst_roles = dst.get("roles");
    const bool missing_dual_safety_roles =
        seed_version == dst_version &&
        seed_roles && seed_roles->is_object() && !seed_roles->get_string("llm_std_high").empty() &&
        (!dst_roles || !dst_roles->is_object() || dst_roles->get_string("llm_std_high").empty());
    const bool has_stale_draft_urls =
        seed_version == dst_version &&
        seed_raw.find("587cfbf3dc3eb82a594366b0fc9a7d6d4ec1c5b3") == std::string::npos &&
        (dst_raw.find("587cfbf3dc3eb82a594366b0fc9a7d6d4ec1c5b3") != std::string::npos ||
         dst_raw.find("LeeAeron/") != std::string::npos ||
         dst_raw.find("99bca3445fd447946004ca07f66a7742e2ed4099") != std::string::npos);
    if (seed.is_object() && (seed_version > dst_version || missing_dual_safety_roles || has_stale_draft_urls)) {
        fs::path backup = dst_path.string() + ".pre-" + std::to_string(seed_version) + ".bak";
        if (!fs::exists(backup, ec)) {
            fs::copy_file(dst_path, backup, fs::copy_options::none, ec);
            if (ec) return;
        }
        fs::copy_file(seed_path, dst_path, fs::copy_options::overwrite_existing, ec);
    }
}

void Config::sync_seed_configs(const char* argv0) const {
    ensure_directories();
    std::error_code ec;
    std::vector<fs::path> search_roots = { fs::current_path(ec) };
    fs::path self_exe = get_self_exe_path(argv0);
    if (!self_exe.empty()) {
        search_roots.push_back(self_exe.parent_path());
        search_roots.push_back(self_exe.parent_path().parent_path());
    }
    search_roots.push_back(fs::path(bin_dir));

    for (const char* fname : {"settings.json", "sources.json", "models.json", "en-us.lang", "ko-kr.lang"}) {
        fs::path dst = fs::path(configs_dir) / fname;
        if (fs::exists(dst, ec) && fs::file_size(dst, ec) > 10) {
            if (std::string_view(fname) == "models.json") {
                for (const auto& root : search_roots) {
                    fs::path src = root / "configs" / fname;
                    if (fs::exists(src, ec) && fs::file_size(src, ec) > 10 && !fs::equivalent(src, dst, ec)) {
                        sync_dual_media_models_catalog(src, dst);
                        break;
                    }
                }
            }
            continue;
        }
        for (const auto& root : search_roots) {
            fs::path src = root / "configs" / fname;
            if (fs::exists(src, ec) && fs::file_size(src, ec) > 10 && !fs::equivalent(src, dst, ec)) {
                fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
                break;
            }
        }
    }

    fs::path dst_user = fs::path(user_file);
    if (!fs::exists(dst_user, ec) || fs::file_size(dst_user, ec) <= 10) {
        for (const auto& root : search_roots) {
            fs::path src = root / "data" / "user.json";
            if (fs::exists(src, ec) && fs::file_size(src, ec) > 10 && !fs::equivalent(src, dst_user, ec)) {
                fs::copy_file(src, dst_user, fs::copy_options::overwrite_existing, ec);
                break;
            }
        }
    }
}

bool Config::load_settings_json() {
    std::error_code ec;
    fs::path target = fs::exists(config_file, ec) ? fs::path(config_file) : (fs::path("configs") / "settings.json");
    if (!fs::exists(target, ec)) return false;
    std::string content = read_file_bytes(target);
    if (content.empty()) return false;
    json::Value j = json::parse(content);
    if (!j.is_object()) return false;

    bind_host              = j.get_string("bind_host", bind_host);
    bind_port              = static_cast<int>(j.get_int("bind_port", bind_port));
    llama_port             = static_cast<int>(j.get_int("llama_port", llama_port));
    default_tier           = j.get_string("default_tier", default_tier);
    default_uncensored     = j.get_bool("default_uncensored", default_uncensored);
    idle_timeout_sec       = std::max(10L, j.get_int("idle_timeout_sec", idle_timeout_sec));
    vram_reserve_mb        = std::max(512L, j.get_int("vram_reserve_mb", vram_reserve_mb));
    vram_high_llm_min_mb   = j.get_int("vram_high_llm_min_mb", vram_high_llm_min_mb);
    dram_high_llm_min_mb   = j.get_int("dram_high_llm_min_mb", dram_high_llm_min_mb);
    vram_high_flux_min_mb  = j.get_int("vram_high_flux_min_mb", vram_high_flux_min_mb);
    dram_high_flux_min_mb  = j.get_int("dram_high_flux_min_mb", dram_high_flux_min_mb);
    vram_high_vid_min_mb   = j.get_int("vram_high_vid_min_mb", vram_high_vid_min_mb);
    dram_high_vid_min_mb   = j.get_int("dram_high_vid_min_mb", dram_high_vid_min_mb);
    vram_high_mus_min_mb   = j.get_int("vram_high_mus_min_mb", vram_high_mus_min_mb);
    dram_high_mus_min_mb   = j.get_int("dram_high_mus_min_mb", dram_high_mus_min_mb);
    cuda_home              = j.get_string("cuda_home", cuda_home);
    rag_search_url         = j.get_string("rag_search_url", rag_search_url);
    http_user_agent        = j.get_string("http_user_agent", http_user_agent);
    return true;
}

bool Config::load_sources_json() {
    std::error_code ec;
    fs::path target = fs::exists(sources_file, ec) ? fs::path(sources_file) : (fs::path("configs") / "sources.json");
    if (!fs::exists(target, ec)) return false;
    std::string content = read_file_bytes(target);
    if (content.empty()) return false;
    json::Value j = json::parse(content);
    if (!j.is_object()) return false;

    llama_cpp_repo             = j.get_string("llama_cpp_repo", llama_cpp_repo);
    llama_cpp_min_build        = j.get_int("llama_cpp_min_build", llama_cpp_min_build);
    stable_diffusion_cpp_repo  = j.get_string("stable_diffusion_cpp_repo", stable_diffusion_cpp_repo);
    stable_diffusion_cpp_tag   = j.get_string("stable_diffusion_cpp_tag", stable_diffusion_cpp_tag);
    acestep_cpp_repo           = j.get_string("acestep_cpp_repo", acestep_cpp_repo);
    cuda_keyring_deb_url       = j.get_string("cuda_keyring_deb_url", cuda_keyring_deb_url);
    cuda_apt_package           = j.get_string("cuda_apt_package", cuda_apt_package);
    miniforge_linux_x86_64_url = j.get_string("miniforge_linux_x86_64_url", miniforge_linux_x86_64_url);
    return true;
}

bool Config::load_models_json() {
    std::error_code ec;
    fs::path target = fs::exists(models_file, ec) ? fs::path(models_file) : (fs::path("configs") / "models.json");
    if (!fs::exists(target, ec)) return false;
    std::string content = read_file_bytes(target);
    if (content.empty()) return false;
    json::Value j = json::parse(content);
    if (!j.is_object()) return false;

    if (const json::Value* profiles = j.get("generation_profiles")) {
        generation_profiles = *profiles;
    } else {
        generation_profiles = {};
    }
    if (const json::Value* engines = j.get("engine_requirements"); engines &&
        (stable_diffusion_cpp_tag.empty() || stable_diffusion_cpp_tag == "master-1017-be48649")) {
        stable_diffusion_cpp_tag = engines->get_string("stable_diffusion_cpp_tag", stable_diffusion_cpp_tag);
    }
    if (const json::Value* r_obj = j.get("roles"); r_obj && r_obj->is_object()) {
        roles.clear();
        for (const auto& [k, v] : std::get<json::Object>(r_obj->v)) {
            roles[k] = v.as_string();
        }
    }
    if (const json::Value* m_arr = j.get("models"); m_arr && m_arr->is_array()) {
        models.clear();
        for (const auto& item : std::get<json::Array>(m_arr->v)) {
            if (!item.is_object()) continue;
            ModelAsset a;
            a.key         = item.get_string("key", "");
            a.category    = item.get_string("category", "");
            a.safety = item.get_string("safety", "standard");
            if (a.safety != "standard" && a.safety != "uncensored") {
                log_err("Invalid model mode; expected standard or uncensored."); models.clear(); return false;
            }
            if (const auto* tiers = item.get("tiers"); tiers && tiers->is_array()) {
                for (const auto& tier : std::get<json::Array>(tiers->v)) {
                    if (tier.as_string() == "eco" || tier.as_string() == "high") a.tiers.push_back(tier.as_string());
                }
            } else {
                const auto tier = item.get_string("tier", "");
                if (tier == "eco" || tier == "high") a.tiers.push_back(tier);
            }
            if (a.tiers.empty()) { log_err("Model requires explicit eco/high tier membership."); models.clear(); return false; }
            if (const auto* deps = item.get("requires"); deps && deps->is_array()) {
                for (const auto& dependency : std::get<json::Array>(deps->v)) a.dependencies.push_back(dependency.as_string());
            }
            a.filename    = item.get_string("filename", "");
            a.subdir      = item.get_string("subdir", "");
            a.url         = item.get_string("url", "");
            if (const json::Value* gb = item.get("approx_gb"); gb && gb->is_number()) {
                a.approx_gb = std::get<double>(gb->v);
            }
            a.description = item.get_string("description", "");
            a.requires_auth = item.get_bool("requires_auth", false);
            if (!a.key.empty() && !a.filename.empty()) {
                models.push_back(std::move(a));
            }
        }
    }
    return !models.empty();
}

const json::Value& Config::generation_profile(std::string_view key) const {
    static const json::Value empty;
    const json::Value* profile = generation_profiles.get(key);
    return profile ? *profile : empty;
}

bool Config::load_config_file() {
    bool ok_s = load_settings_json();
    load_sources_json();
    load_models_json();
    return ok_s;
}

bool Config::save_config_file() const {
    ensure_directories();
    std::ostringstream o;
    o << "{\n"
      << "  \"bind_host\": \"" << json::escape(bind_host) << "\",\n"
      << "  \"bind_port\": " << bind_port << ",\n"
      << "  \"llama_port\": " << llama_port << ",\n"
      << "  \"default_tier\": \"" << json::escape(default_tier) << "\",\n"
      << "  \"default_uncensored\": " << (default_uncensored ? "true" : "false") << ",\n"
      << "  \"idle_timeout_sec\": " << idle_timeout_sec << ",\n"
      << "  \"vram_reserve_mb\": " << vram_reserve_mb << ",\n"
      << "  \"vram_high_llm_min_mb\": " << vram_high_llm_min_mb << ",\n"
      << "  \"dram_high_llm_min_mb\": " << dram_high_llm_min_mb << ",\n"
      << "  \"vram_high_flux_min_mb\": " << vram_high_flux_min_mb << ",\n"
      << "  \"dram_high_flux_min_mb\": " << dram_high_flux_min_mb << ",\n"
      << "  \"vram_high_vid_min_mb\": " << vram_high_vid_min_mb << ",\n"
      << "  \"dram_high_vid_min_mb\": " << dram_high_vid_min_mb << ",\n"
      << "  \"vram_high_mus_min_mb\": " << vram_high_mus_min_mb << ",\n"
      << "  \"dram_high_mus_min_mb\": " << dram_high_mus_min_mb << ",\n"
      << "  \"cuda_home\": \"" << json::escape(cuda_home) << "\",\n"
      << "  \"rag_search_url\": \"" << json::escape(rag_search_url) << "\",\n"
      << "  \"http_user_agent\": \"" << json::escape(http_user_agent) << "\"\n"
      << "}\n";
    return write_file_atomic(config_file, o.str(), 0644);
}

std::string Config::role_filename(std::string_view role_key) const {
    const auto it = roles.find(std::string(role_key));
    return it == roles.end() ? "" : it->second;
}

void Config::append_command_history(std::string_view cmd_line) const {
    std::string trimmed = trim_copy(cmd_line);
    if (trimmed.empty()) return;
    std::error_code ec;
    fs::create_directories(lais_dir, ec);
    std::ofstream ofs(history_file, std::ios::app);
    if (ofs) {
        ofs << "[" << current_timestamp_iso() << "] " << trimmed << "\n";
    }
}

std::vector<std::string> Config::load_command_history_lines() const {
    std::vector<std::string> list;
    std::ifstream ifs(history_file);
    if (!ifs) return list;
    std::string line;
    while (std::getline(ifs, line)) {
        std::string t = trim_copy(line);
        if (t.empty()) continue;
        if (t.size() > 22 && t.front() == '[' && t[20] == ']' && t[21] == ' ') {
            t = trim_copy(t.substr(22));
        }
        if (!t.empty()) list.push_back(t);
    }
    return list;
}

Config Config::from_env(const char* argv0) {
    Config c;
    const char* h = std::getenv("HOME");
#ifdef _WIN32
    if (!h || !*h) h = std::getenv("USERPROFILE");
    c.home_dir = (h && *h) ? h : "C:\\Temp";
#else
    c.home_dir = (h && *h) ? h : "/tmp";
#endif
    if (const char* ch = std::getenv("CUDA_HOME")) {
        if (*ch) c.cuda_home = ch;
    }
    if (const char* root = std::getenv("LAIS_HOME")) {
        if (*root) c.lais_dir = root;
    } else {
        c.lais_dir = c.home_dir + "/lais";
    }
    c.init_paths();
    c.ensure_directories();
    c.sync_seed_configs(argv0);
    c.load_config_file();

    if (const char* p = std::getenv("LAIS_PORT")) {
        int port = std::atoi(p);
        if (port > 0 && port < 65536) c.bind_port = port;
    }
    if (const char* host = std::getenv("LAIS_HOST")) {
        if (*host) c.bind_host = host;
    }
    return c;
}

std::vector<ModelAsset> match_models_by_selector(const Config& cfg, std::string_view selector) {
    std::string q = to_lower_ascii(trim_copy(selector));
    if (q.empty() || q == "all" || q == "*") return cfg.models;
    for (const auto& m : cfg.models) if (to_lower_ascii(m.key) == q) return {m};
    for (char& c : q) if (c == ':' || c == ',' || c == '/') c = ' ';
    std::istringstream tokens(q);
    std::vector<std::string> filters;
    for (std::string token; tokens >> token;) filters.push_back(token);
    std::vector<ModelAsset> result;
    for (const auto& m : cfg.models) {
        bool match = true;
        for (auto filter : filters) {
            if (filter == "safe" || filter == "censored") filter = "standard";
            if (filter == "abliterated" || filter == "nsfw") filter = "uncensored";
            if (filter == "all" || filter == "*") continue;
            if (filter == "standard" || filter == "uncensored") match = match && m.safety == filter;
            else if (filter == "eco" || filter == "high") match = match &&
                std::find(m.tiers.begin(), m.tiers.end(), filter) != m.tiers.end();
            else if (filter == "coder") match = match && m.category == "llm";
            else if (filter == "llm") match = match && (m.category == "llm" || m.category == "vision");
            else if (filter == "vision") match = match && (m.category == "vision" ||
                m.key == "llm_standard_eco" || m.key == "llm_uncensored_eco");
            else if (filter == "image" || filter == "video" || filter == "music") match = match && m.category == filter;
            else match = match && (to_lower_ascii(m.key).find(filter) != std::string::npos ||
                                    to_lower_ascii(m.filename).find(filter) != std::string::npos);
        }
        if (match) result.push_back(m);
    }
    return result;
}

// ------------------------------------------------------------------------------
// ChatStore Implementation (With Thinking & Branching)
// ------------------------------------------------------------------------------
std::string ChatStore::sanitize_id(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
            out.push_back(c);
        }
    }
    return out;
}

std::string ChatStore::generate_chat_id() {
    static std::atomic<uint64_t> s_seq{1};
    return "chat_" + std::to_string(std::time(nullptr)) + "_" +
           std::to_string(s_seq.fetch_add(1, std::memory_order_relaxed));
}

std::string ChatStore::derive_title(std::string_view prompt) {
    std::string t = trim_copy(prompt);
    for (char& c : t) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    if (t.empty()) return "New Conversation";
    if (t.size() > 48) {
        std::size_t cut = 48;
        while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) --cut;
        t = t.substr(0, cut) + "...";
    }
    return t;
}

std::string ChatStore::list_sessions_json(const Config& cfg) {
    std::error_code ec;
    fs::create_directories(cfg.chats_dir, ec);
    struct Meta {
        std::string id;
        std::string title;
        std::string parent_id;
        std::string created_at;
        std::string updated_at;
        std::size_t count = 0;
    };
    std::vector<Meta> items;
    for (auto it = fs::directory_iterator(cfg.chats_dir, ec); it != fs::directory_iterator(); it.increment(ec)) {
        if (ec || !it->is_regular_file(ec) || it->path().extension() != ".json") continue;
        std::string raw = read_file_bytes(it->path());
        if (raw.empty()) continue;
        json::Value j = json::parse(raw);
        if (!j.is_object()) continue;
        Meta m;
        m.id         = j.get_string("id", it->path().stem().string());
        m.title      = j.get_string("title", "Conversation");
        m.parent_id  = j.get_string("parent_chat_id", "");
        m.created_at = j.get_string("created_at", "");
        m.updated_at = j.get_string("updated_at", m.created_at);
        if (const json::Value* msgs = j.get("messages"); msgs && msgs->is_array()) {
            m.count = std::get<json::Array>(msgs->v).size();
        }
        items.push_back(std::move(m));
    }
    std::sort(items.begin(), items.end(), [](const Meta& a, const Meta& b) {
        if (a.updated_at != b.updated_at) return a.updated_at > b.updated_at;
        return a.id > b.id;
    });

    std::ostringstream o;
    o << "{\"ok\":true,\"chats\":[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) o << ",";
        o << "{"
          << "\"id\":\"" << json::escape(items[i].id) << "\","
          << "\"title\":\"" << json::escape(items[i].title) << "\","
          << "\"parent_chat_id\":\"" << json::escape(items[i].parent_id) << "\","
          << "\"created_at\":\"" << json::escape(items[i].created_at) << "\","
          << "\"updated_at\":\"" << json::escape(items[i].updated_at) << "\","
          << "\"message_count\":" << items[i].count
          << "}";
    }
    o << "]}";
    return o.str();
}

std::string ChatStore::load_session_json(const Config& cfg, std::string_view raw_id) {
    std::string id = sanitize_id(raw_id);
    if (id.empty()) return "{\"ok\":false,\"error\":\"Invalid chat ID\"}";
    fs::path p = fs::path(cfg.chats_dir) / (id + ".json");
    std::error_code ec;
    if (!fs::exists(p, ec)) return "{\"ok\":false,\"error\":\"Chat session not found\"}";
    std::string content = read_file_bytes(p);
    if (content.empty()) return "{\"ok\":false,\"error\":\"Empty chat file\"}";
    return content;
}

std::vector<ChatTurn> ChatStore::load_recent_turns(const Config& cfg, std::string_view raw_id, std::size_t max_turns) {
    std::vector<ChatTurn> turns;
    std::string id = sanitize_id(raw_id);
    if (id.empty()) return turns;
    fs::path p = fs::path(cfg.chats_dir) / (id + ".json");
    std::error_code ec;
    if (!fs::exists(p, ec)) return turns;
    json::Value j = json::parse(read_file_bytes(p));
    if (!j.is_object()) return turns;
    if (const json::Value* arr = j.get("messages"); arr && arr->is_array()) {
        const auto& vec = std::get<json::Array>(arr->v);
        std::size_t start = (vec.size() > max_turns) ? (vec.size() - max_turns) : 0;
        for (std::size_t i = start; i < vec.size(); ++i) {
            if (!vec[i].is_object()) continue;
            std::string role = vec[i].get_string("role", "user");
            std::string content = vec[i].get_string("content", "");
            if (!content.empty()) {
                turns.push_back(ChatTurn{role, content});
            }
        }
    }
    return turns;
}

bool ChatStore::delete_session(const Config& cfg, std::string_view raw_id) {
    std::string id = sanitize_id(raw_id);
    if (id.empty()) return false;
    fs::path p = fs::path(cfg.chats_dir) / (id + ".json");
    std::error_code ec;
    return fs::remove(p, ec);
}

static std::string serialize_msg_object(const json::Value& m) {
    std::string raw_content = m.get_string("content", "");
    std::string raw_thinking = m.get_string("thinking", "");
    if (raw_thinking.empty() && raw_content.find("<think>") != std::string::npos) {
        std::string extracted_think, cleaned_reply;
        split_thinking_and_reply(raw_content, extracted_think, cleaned_reply);
        raw_thinking = extracted_think;
        raw_content  = cleaned_reply;
    }
    std::ostringstream mj;
    mj << "{"
       << "\"role\":\"" << json::escape(m.get_string("role", "user")) << "\","
       << "\"content\":\"" << json::escape(raw_content) << "\","
       << "\"thinking\":\"" << json::escape(raw_thinking) << "\","
       << "\"attached_name\":\"" << json::escape(m.get_string("attached_name", "")) << "\","
       << "\"selected_tier\":\"" << json::escape(m.get_string("selected_tier", "")) << "\","
       << "\"selected_model\":\"" << json::escape(m.get_string("selected_model", "")) << "\","
       << "\"routing_reason\":\"" << json::escape(m.get_string("routing_reason", "")) << "\","
       << "\"media_url\":\"" << json::escape(m.get_string("media_url", "")) << "\","
       << "\"download_url\":\"" << json::escape(m.get_string("download_url", "")) << "\","
       << "\"kind\":\"" << json::escape(m.get_string("kind", "")) << "\","
       << "\"timestamp\":\"" << json::escape(m.get_string("timestamp", "")) << "\""
       << "}";
    return mj.str();
}

std::string ChatStore::branch_session_json(
    const Config& cfg,
    std::string_view source_chat_id,
    int message_index,
    std::string_view custom_title) {

    std::error_code ec;
    fs::create_directories(cfg.chats_dir, ec);
    std::string src_id = sanitize_id(source_chat_id);
    if (src_id.empty()) {
        return "{\"ok\":false,\"error\":\"Source chat session is not saved yet.\"}";
    }
    fs::path src_path = fs::path(cfg.chats_dir) / (src_id + ".json");
    if (!fs::exists(src_path, ec)) {
        return "{\"ok\":false,\"error\":\"Source chat session file not found.\"}";
    }

    json::Value existing = json::parse(read_file_bytes(src_path));
    if (!existing.is_object()) {
        return "{\"ok\":false,\"error\":\"Invalid source chat JSON.\"}";
    }

    std::string new_id = generate_chat_id();
    std::string now_str = current_timestamp_iso();
    std::string base_title = existing.get_string("title", "Conversation");
    std::string new_title = custom_title.empty() ? (base_title + " (Branch)") : std::string(custom_title);

    std::vector<std::string> raw_msgs;
    if (const json::Value* arr = existing.get("messages"); arr && arr->is_array()) {
        const auto& vec = std::get<json::Array>(arr->v);
        int limit = (message_index < 0 || message_index >= static_cast<int>(vec.size()))
            ? (static_cast<int>(vec.size()) - 1)
            : message_index;
        for (int i = 0; i <= limit && i < static_cast<int>(vec.size()); ++i) {
            if (!vec[i].is_object()) continue;
            raw_msgs.push_back(serialize_msg_object(vec[i]));
        }
    }

    std::ostringstream out;
    out << "{\n"
        << "  \"ok\": true,\n"
        << "  \"id\": \"" << json::escape(new_id) << "\",\n"
        << "  \"parent_chat_id\": \"" << json::escape(src_id) << "\",\n"
        << "  \"title\": \"" << json::escape(new_title) << "\",\n"
        << "  \"created_at\": \"" << json::escape(now_str) << "\",\n"
        << "  \"updated_at\": \"" << json::escape(now_str) << "\",\n"
        << "  \"messages\": [\n";
    for (std::size_t i = 0; i < raw_msgs.size(); ++i) {
        out << "    " << raw_msgs[i] << (i + 1 < raw_msgs.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";

    fs::path dst_path = fs::path(cfg.chats_dir) / (new_id + ".json");
    write_file_atomic(dst_path, out.str(), 0644);
    return out.str();
}

std::string ChatStore::append_exchange(
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
    std::string_view kind) {

    // Serialize the final history commit against Stop. Once saved, a late Stop
    // reports completion and the UI keeps the successful response.
    const auto generation = current_generation();
    std::unique_lock<std::mutex> commit_lock;
    if (generation) commit_lock = std::unique_lock<std::mutex>(generation->commit_mu);
    generation_checkpoint();
    std::error_code ec;
    fs::create_directories(cfg.chats_dir, ec);
    std::string id = sanitize_id(raw_chat_id);
    if (id.empty()) id = generate_chat_id();

    fs::path p = fs::path(cfg.chats_dir) / (id + ".json");
    std::string now_str = current_timestamp_iso();
    std::string title = derive_title(user_prompt);
    std::string parent_id;
    std::string created_at = now_str;
    std::vector<std::string> raw_msgs;

    if (fs::exists(p, ec)) {
        json::Value existing = json::parse(read_file_bytes(p));
        if (existing.is_object()) {
            std::string old_title = existing.get_string("title", "");
            if (!old_title.empty() && old_title != "New Conversation") title = old_title;
            parent_id  = existing.get_string("parent_chat_id", "");
            created_at = existing.get_string("created_at", now_str);
            if (const json::Value* arr = existing.get("messages"); arr && arr->is_array()) {
                for (const auto& m : std::get<json::Array>(arr->v)) {
                    if (!m.is_object()) continue;
                    raw_msgs.push_back(serialize_msg_object(m));
                }
            }
        }
    }

    {
        std::ostringstream u;
        u << "{"
          << "\"role\":\"user\","
          << "\"content\":\"" << json::escape(user_prompt) << "\","
          << "\"thinking\":\"\","
          << "\"attached_name\":\"" << json::escape(attached_name) << "\","
          << "\"selected_tier\":\"\","
          << "\"selected_model\":\"\","
          << "\"routing_reason\":\"\","
          << "\"media_url\":\"\","
          << "\"download_url\":\"\","
          << "\"kind\":\"\","
          << "\"timestamp\":\"" << json::escape(now_str) << "\""
          << "}";
        raw_msgs.push_back(u.str());
    }
    {
        std::ostringstream a;
        a << "{"
          << "\"role\":\"assistant\","
          << "\"content\":\"" << json::escape(assistant_reply) << "\","
          << "\"thinking\":\"" << json::escape(assistant_thinking) << "\","
          << "\"attached_name\":\"\","
          << "\"selected_tier\":\"" << json::escape(selected_tier) << "\","
          << "\"selected_model\":\"" << json::escape(selected_model) << "\","
          << "\"routing_reason\":\"" << json::escape(routing_reason) << "\","
          << "\"media_url\":\"" << json::escape(media_url) << "\","
          << "\"download_url\":\"" << json::escape(download_url) << "\","
          << "\"kind\":\"" << json::escape(kind) << "\","
          << "\"timestamp\":\"" << json::escape(now_str) << "\""
          << "}";
        raw_msgs.push_back(a.str());
    }

    std::ostringstream out;
    out << "{\n"
        << "  \"ok\": true,\n"
        << "  \"id\": \"" << json::escape(id) << "\",\n"
        << "  \"parent_chat_id\": \"" << json::escape(parent_id) << "\",\n"
        << "  \"title\": \"" << json::escape(title) << "\",\n"
        << "  \"created_at\": \"" << json::escape(created_at) << "\",\n"
        << "  \"updated_at\": \"" << json::escape(now_str) << "\",\n"
        << "  \"messages\": [\n";
    for (std::size_t i = 0; i < raw_msgs.size(); ++i) {
        out << "    " << raw_msgs[i] << (i + 1 < raw_msgs.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
    write_file_atomic(p, out.str(), 0644);
    if (generation) generation->finished.store(true);
    return id;
}

} // namespace lais
