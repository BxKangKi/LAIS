// ==============================================================================
// LAIS (Local AI Studio) v3.6.0 - Hardware Telemetry, Memory Reclamation & RAG
// SPDX-License-Identifier: MIT
// ==============================================================================
#include "common.hpp"

namespace lais {

MemoryStats query_memory_stats_sync() {
    MemoryStats st;
#ifdef _WIN32
    MEMORYSTATUSEX mem_stat{};
    mem_stat.dwLength = sizeof(mem_stat);
    if (::GlobalMemoryStatusEx(&mem_stat)) {
        st.dram_total_mb  = static_cast<long>(mem_stat.ullTotalPhys / (1024ULL * 1024ULL));
        st.dram_avail_mb  = static_cast<long>(mem_stat.ullAvailPhys / (1024ULL * 1024ULL));
        st.dram_used_mb   = std::max(0L, st.dram_total_mb - st.dram_avail_mb);
        st.dram_cached_mb = 0;
        st.dram_used_pct  = static_cast<int>(mem_stat.dwMemoryLoad);
    }
    std::string smi = capture_cmd(
        "nvidia-smi --query-gpu=name,memory.total,memory.used,memory.free "
        "--format=csv,noheader,nounits 2>NUL");
#else
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo) {
        long mem_total_kb = 0, mem_avail_kb = 0, mem_free_kb = 0, buffers_kb = 0, cached_kb = 0;
        std::string key, unit;
        long val = 0;
        while (meminfo >> key >> val) {
            std::getline(meminfo, unit);
            if (key == "MemTotal:")          mem_total_kb = val;
            else if (key == "MemAvailable:") mem_avail_kb = val;
            else if (key == "MemFree:")      mem_free_kb  = val;
            else if (key == "Buffers:")      buffers_kb   = val;
            else if (key == "Cached:")       cached_kb    = val;
        }
        if (mem_total_kb > 0) {
            st.dram_total_mb  = mem_total_kb / 1024;
            st.dram_cached_mb = (buffers_kb + cached_kb) / 1024;
            st.dram_avail_mb  = (mem_avail_kb > 0 ? mem_avail_kb : (mem_free_kb + buffers_kb + cached_kb)) / 1024;
            st.dram_used_mb   = std::max(0L, st.dram_total_mb - st.dram_avail_mb);
            st.dram_used_pct  = static_cast<int>((st.dram_used_mb * 100L) / std::max(1L, st.dram_total_mb));
        }
    }
    std::string smi = capture_cmd(
        "nvidia-smi --query-gpu=name,memory.total,memory.used,memory.free "
        "--format=csv,noheader,nounits 2>/dev/null | head -n 1");
#endif
    if (!smi.empty()) {
        std::size_t nl = smi.find('\n');
        if (nl != std::string::npos) smi.resize(nl);
        std::vector<std::string> parts;
        std::stringstream ss(smi);
        std::string item;
        while (std::getline(ss, item, ',')) {
            parts.push_back(trim_copy(item));
        }
        if (parts.size() >= 4) {
            if (!parts[0].empty()) st.gpu_name = parts[0];
            long tot  = std::atol(parts[1].c_str());
            long used = std::atol(parts[2].c_str());
            long free = std::atol(parts[3].c_str());
            if (tot > 0) {
                st.vram_total_mb = tot;
                st.vram_used_mb  = used;
                st.vram_free_mb  = free;
                st.vram_used_pct = static_cast<int>((used * 100L) / std::max(1L, tot));
            }
        }
    }
    return st;
}

void trim_process_memory() {
#ifdef _WIN32
    ::_heapmin();
    HANDLE hProc = ::GetCurrentProcess();
    (void)::SetProcessWorkingSetSizeEx(
        hProc,
        static_cast<SIZE_T>(-1),
        static_cast<SIZE_T>(-1),
        QUOTA_LIMITS_HARDWS_MIN_ENABLE);
    (void)::EmptyWorkingSet(hProc);
#else
    ::malloc_trim(0);
#endif
}

std::size_t purge_dram_and_pagecache(const std::string& models_root) {
    trim_process_memory();
    std::size_t evicted_files = 0;
    std::error_code ec;
    if (fs::exists(models_root, ec)) {
        for (auto it = fs::recursive_directory_iterator(models_root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec || !it->is_regular_file(ec)) continue;
            std::string ext = it->path().extension().string();
            if (ext == ".gguf" || ext == ".safetensors" || ext == ".bin" || ext == ".ckpt" || ext == ".pt") {
#ifdef _WIN32
                HANDLE hFile = ::CreateFileW(
                    it->path().c_str(),
                    GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                    nullptr);
                if (hFile != INVALID_HANDLE_VALUE) {
                    ::CloseHandle(hFile);
                    ++evicted_files;
                }
#else
                int fd = ::open(it->path().c_str(), O_RDONLY | O_CLOEXEC);
                if (fd >= 0) {
                    (void)::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
                    ::close(fd);
                    ++evicted_files;
                }
#endif
            }
        }
    }
#ifndef _WIN32
    if (std::system("sudo -n true >/dev/null 2>&1") == 0) {
        (void)std::system("sync && echo 3 | sudo -n tee /proc/sys/vm/drop_caches >/dev/null 2>&1 || true");
    }
#endif
    trim_process_memory();
    return evicted_files;
}

static std::string base64_encode_bytes(const std::vector<unsigned char>& data) {
    static constexpr char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        unsigned int n = static_cast<unsigned int>(data[i]) << 16;
        if (i + 1 < data.size()) n |= static_cast<unsigned int>(data[i + 1]) << 8;
        if (i + 2 < data.size()) n |= static_cast<unsigned int>(data[i + 2]);
        out.push_back(tbl[(n >> 18) & 63]);
        out.push_back(tbl[(n >> 12) & 63]);
        out.push_back((i + 1 < data.size()) ? tbl[(n >> 6) & 63] : '=');
        out.push_back((i + 2 < data.size()) ? tbl[n & 63] : '=');
    }
    return out;
}

std::string powershell_encoded_command(std::string_view ascii_script) {
    std::vector<unsigned char> utf16le;
    utf16le.reserve(ascii_script.size() * 2);
    for (unsigned char c : ascii_script) {
        utf16le.push_back(c);
        utf16le.push_back(0x00);
    }
    return base64_encode_bytes(utf16le);
}

std::string url_encode(std::string_view value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << static_cast<char>(c);
        } else if (c == ' ') {
            escaped << '+';
        } else {
            escaped << '%' << std::uppercase << std::setw(2) << static_cast<int>(c) << std::nouppercase;
        }
    }
    return escaped.str();
}

static std::string strip_html_tags(std::string_view html) {
    std::string out;
    out.reserve(html.size());
    bool in_tag = false;
    for (char c : html) {
        if (c == '<') in_tag = true;
        else if (c == '>') in_tag = false;
        else if (!in_tag) out.push_back(c);
    }
    auto replace_all = [](std::string& s, std::string_view from, std::string_view to) {
        std::string res;
        res.reserve(s.size());
        std::size_t last = 0, pos = 0;
        while ((pos = s.find(from, last)) != std::string::npos) {
            res.append(s, last, pos - last);
            res.append(to);
            last = pos + from.size();
        }
        res.append(s, last, std::string::npos);
        s.swap(res);
    };
    replace_all(out, "&amp;", "&");
    replace_all(out, "&quot;", "\"");
    replace_all(out, "&#x27;", "'");
    replace_all(out, "&#39;", "'");
    replace_all(out, "&lt;", "<");
    replace_all(out, "&gt;", ">");
    replace_all(out, "&nbsp;", " ");
    return trim_copy(out);
}

std::string fetch_duckduckgo_rag(const Config& cfg, std::string_view query, int max_results) {
    if (cfg.rag_search_url.empty()) return {};
    std::string url = cfg.rag_search_url + url_encode(query);
    std::string ua = cfg.http_user_agent.empty() ? "LAIS/3.6" : cfg.http_user_agent;
    std::string cmd = "curl --fail --silent --location --max-time 8 -A " +
                      shell_quote(ua) + " " + shell_quote(url) + null_stderr_redirect();
    std::string html = capture_cmd(cmd);
    if (html.empty()) return {};

    std::ostringstream rag;
    int count = 0;
    std::size_t pos = 0;
    while (count < max_results) {
        std::size_t snip = html.find("result__snippet", pos);
        if (snip == std::string::npos) break;
        std::size_t gt = html.find('>', snip);
        std::size_t end_a = html.find("</a>", gt);
        if (gt == std::string::npos || end_a == std::string::npos) break;
        std::string text = strip_html_tags(std::string_view(html).substr(gt + 1, end_a - gt - 1));
        if (!text.empty()) {
            ++count;
            rag << "[" << count << "] " << text << "\n";
        }
        pos = end_a + 4;
    }
    return rag.str();
}

} // namespace lais
