// ==============================================================================
// LAIS (Local AI Studio) v3.7.0 - Engine Builder, Bundle Extractor & Service CLI
// SPDX-License-Identifier: MIT
// ==============================================================================
#include "common.hpp"
#include "crypto_pake.hpp"

namespace lais {

static bool configure_wsl_memory_discipline() {
    if (!can_run_sudo()) return true;
    log_info("Configuring WSL2 systemd, NVIDIA WSL driver linker path, and kernel memory discipline...");

    std::string wsl_conf = read_file_bytes("/etc/wsl.conf");
    if (wsl_conf.find("systemd") == std::string::npos) {
        std::string updated_wsl = wsl_conf;
        if (updated_wsl.find("[boot]") == std::string::npos) {
            if (!updated_wsl.empty() && updated_wsl.back() != '\n') updated_wsl += "\n";
            updated_wsl += "[boot]\nsystemd=true\n";
            write_file_atomic("/tmp/lais-wsl.conf", updated_wsl);
            run_cmd("sudo mv -f /tmp/lais-wsl.conf /etc/wsl.conf && sudo chmod 0644 /etc/wsl.conf", true);
        }
    }

    if (fs::exists("/usr/lib/wsl/lib")) {
        write_file_atomic("/tmp/wsl-nvidia.conf", "/usr/lib/wsl/lib\n/usr/local/cuda/lib64\n");
        run_cmd("sudo mv -f /tmp/wsl-nvidia.conf /etc/ld.so.conf.d/wsl-nvidia.conf && "
                "sudo chmod 0644 /etc/ld.so.conf.d/wsl-nvidia.conf && "
                "sudo ldconfig 2>/dev/null || true", true);
    }

    run_cmd("sudo rm -f /etc/sysctl.d/99-wsl-ai-overcommit.conf 2>/dev/null || true", true);
    std::string sysctl_conf =
        "vm.overcommit_memory=0\n"
        "vm.swappiness=10\n"
        "vm.vfs_cache_pressure=200\n"
        "vm.dirty_background_ratio=3\n"
        "vm.dirty_ratio=8\n";
    fs::path tmp_sysctl = "/tmp/99-lais-memory.conf";
    write_file_atomic(tmp_sysctl, sysctl_conf);
    run_cmd("sudo mv -f /tmp/99-lais-memory.conf /etc/sysctl.d/99-lais-memory.conf 2>/dev/null || true", true);
    run_cmd("sudo chmod 0644 /etc/sysctl.d/99-lais-memory.conf 2>/dev/null || true", true);
    run_cmd("sudo sysctl --system >/dev/null 2>&1 || sudo sysctl -w vm.overcommit_memory=0 vm.swappiness=10 vm.vfs_cache_pressure=200 >/dev/null 2>&1 || true", true);
    return true;
}

static bool install_miniforge3(const Config& cfg) {
    fs::path mf_dir = fs::path(cfg.home_dir) / "miniforge3";
    if (fs::exists(mf_dir / "bin/conda")) {
        log_ok("Miniforge3 already installed at " + mf_dir.string());
        return true;
    }
    if (cfg.miniforge_linux_x86_64_url.empty()) {
        log_err("miniforge_linux_x86_64_url is not configured in " + cfg.sources_file);
        return false;
    }
    log_info("Installing Miniforge3 (conda/mamba) into " + mf_dir.string() + " ...");
    std::string installer = "/tmp/Miniforge3-Linux-x86_64.sh";
    if (run_cmd("curl -fsSL " + shell_quote(cfg.miniforge_linux_x86_64_url) + " -o " + shell_quote(installer) +
                " && bash " + shell_quote(installer) + " -b -u -p " + shell_quote(mf_dir.string()) +
                " && rm -f " + shell_quote(installer)) != 0) {
        log_err("Failed to install Miniforge3.");
        return false;
    }
    run_cmd(shell_quote((mf_dir / "bin/conda").string()) + " config --set auto_activate_base false 2>/dev/null || true", true);
    run_cmd(shell_quote((mf_dir / "bin/conda").string()) + " init bash >/dev/null 2>&1 || true", true);
    log_ok("Miniforge3 installed at " + mf_dir.string());
    return true;
}

bool download_hf_asset(
    const std::string& url,
    const fs::path& dest,
    const std::string& label,
    double approx_gb) {

    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    static constexpr uint64_t kMinBinaryBytes = 1024ULL * 1024ULL;
    uint64_t min_valid_bytes = (approx_gb > 0.05)
        ? std::max<uint64_t>(kMinBinaryBytes, static_cast<uint64_t>(approx_gb * 0.15 * 1024.0 * 1024.0 * 1024.0))
        : 65536ULL;

    if (fs::exists(dest, ec) && fs::file_size(dest, ec) >= min_valid_bytes) {
        log_ok(label + " already installed at " + dest.string());
        return true;
    }

    int max_attempts = 30;
    if (const char* env_att = std::getenv("HF_DOWNLOAD_ATTEMPTS")) {
        int v = std::atoi(env_att);
        if (v > 0) max_attempts = v;
    }

    auto make_main_branch_url = [](const std::string& u) -> std::string {
        const std::string marker = "/resolve/";
        std::size_t pos = u.find(marker);
        if (pos == std::string::npos) return u;
        std::size_t rev_start = pos + marker.size();
        std::size_t rev_end = u.find('/', rev_start);
        if (rev_end == std::string::npos) return u;
        std::string rev = u.substr(rev_start, rev_end - rev_start);
        if (rev == "main") return u;
        return u.substr(0, rev_start) + "main" + u.substr(rev_end);
    };

    std::string active_url = url;
    const std::string main_url = make_main_branch_url(url);
    log_info("Downloading " + label + " -> " + dest.string() + " ...");
    fs::path part = dest.string() + ".part";
    std::string hf_token;
    if (const char* token = std::getenv("HF_TOKEN")) {
        if (*token) hf_token = token;
    }
    bool use_auth_header = false;

    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        uint64_t prev_sz = fs::exists(part, ec) ? fs::file_size(part, ec) : 0ULL;
        if (prev_sz == 0 && fs::exists(part, ec)) {
            fs::remove(part, ec);
        }
        std::ostringstream cmd;
        cmd << "curl --http1.1 --fail --location --show-error "
            << "--connect-timeout 30 --keepalive-time 30 "
            << "--speed-time 60 --speed-limit 1024 "
            << "--retry 2 --retry-delay 2 --retry-connrefused ";
        if (prev_sz > 0) {
            cmd << "-C - ";
        }
        if (use_auth_header && !hf_token.empty()) {
            cmd << "-H " << shell_quote(std::string("Authorization: Bearer ") + hf_token) << " ";
        }
        cmd << shell_quote(active_url)
            << " --output " << shell_quote(part.string());
        int exit_code = run_cmd(cmd.str(), true);
        uint64_t part_sz = fs::exists(part, ec) ? fs::file_size(part, ec) : 0ULL;

        if ((exit_code == 0 || exit_code == 33) &&
            (part_sz >= min_valid_bytes || (part_sz >= kMinBinaryBytes && (exit_code == 0 || part_sz == prev_sz)))) {
            fs::remove(dest, ec);
            fs::rename(part, dest, ec);
#ifndef _WIN32
            int fd = ::open(dest.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                (void)::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
                ::close(fd);
            }
#endif
            trim_process_memory();
            log_ok("Downloaded " + label + " (" + std::to_string(part_sz / (1024ULL * 1024ULL)) + " MB, cache evicted).");
            return true;
        }
        if ((exit_code == 22 || exit_code == 33 || exit_code == 36) && part_sz < min_valid_bytes) {
            // Reset broken or rejected partial state before retrying.
            if (prev_sz > 0 || part_sz < kMinBinaryBytes) {
                fs::remove(part, ec);
            }
            if (exit_code == 22 && active_url != main_url) {
                log_warn("Pinned revision returned HTTP error (curl exit 22); falling back to main branch: " + main_url);
                active_url = main_url;
                continue;
            }
            if (exit_code == 22 && !hf_token.empty() && !use_auth_header) {
                use_auth_header = true;
                continue;
            } else if (exit_code == 22 && use_auth_header) {
                use_auth_header = false;
            }
        }
        log_warn("Download interrupted (curl exit " + std::to_string(exit_code) + ") on attempt " +
                 std::to_string(attempt) + "/" + std::to_string(max_attempts) +
                 " [" + active_url + "]; resuming in 5s...");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    log_err("Failed to download " + label + " after " + std::to_string(max_attempts) + " attempts.");
    return false;
}

// Quantize both Standard (Safe) and Uncensored Wan 2.1 FP16 models into Q4_K GGUF
// for Eco / RAM-Saving Video Generation whenever the FP16 model is present.
void maybe_quantize_wan_eco(const Config& cfg) {
    std::string sfx = exe_suffix();
    fs::path sd_cli = fs::path(cfg.bin_dir) / ("sd-cli" + sfx);
    if (!fs::exists(sd_cli)) sd_cli = fs::path(cfg.bin_dir) / ("sd" + sfx);
    if (!fs::exists(sd_cli)) sd_cli = fs::path(cfg.engines_dir) / ("stable-diffusion.cpp/build/bin/sd-cli" + sfx);
    if (!fs::exists(sd_cli)) sd_cli = fs::path(cfg.engines_dir) / ("stable-diffusion.cpp/build/bin/sd" + sfx);
    if (!fs::exists(sd_cli)) return;

    struct QuantPair {
        std::string hi_role;
        std::string eco_role;
        std::string label;
    };
    const std::vector<QuantPair> pairs = {
        {"video_std_dit_high", "video_std_dit_eco", "Standard Wan 2.1 1.3B (Safe Mode)"},
        {"video_dit_high",     "video_dit_eco",     "Uncensored Wan 2.1 1.3B"}
    };

    for (const auto& pr : pairs) {
        std::string hi_name  = cfg.role_filename(pr.hi_role);
        std::string eco_name = cfg.role_filename(pr.eco_role);
        if (hi_name.empty() || eco_name.empty()) continue;
        // Legacy conversion is only valid for the original Wan 2.1 FP16 pair.
        // Modern catalog tiers are independent published GGUF assets.
        if (!hi_name.ends_with(".safetensors") ||
            (hi_name.find("1.3B") == std::string::npos && hi_name.find("1.3b") == std::string::npos)) continue;

        fs::path wan_fp16 = fs::path(cfg.models_dir) / "diffusion" / hi_name;
        fs::path wan_q4   = fs::path(cfg.models_dir) / "diffusion" / eco_name;
        if (fs::exists(wan_fp16) && !fs::exists(wan_q4)) {
            log_info("Quantizing " + pr.label + " into Eco Q4_K GGUF (" + eco_name + ") via sd-cli -M convert...");
            std::string qcmd = shell_quote(sd_cli.string()) + " -M convert -m " +
                               shell_quote(wan_fp16.string()) + " -o " +
                               shell_quote(wan_q4.string()) + " --type q4_k";
            run_cmd(qcmd, true);
            purge_dram_and_pagecache(cfg.models_dir);
        }
    }
}

static std::string patch_math_functions_content(std::string content) {
    auto replace_all = [](std::string& s, std::string_view from, std::string_view to) {
        std::size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace_all(content, " noexcept (true)", "");
    replace_all(content, " noexcept(true)", "");

    auto replace_token = [](std::string& s, std::string_view from, std::string_view to) {
        std::size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            bool boundary_ok = (pos == 0) ||
                               std::isspace(static_cast<unsigned char>(s[pos - 1])) ||
                               s[pos - 1] == '(';
            if (boundary_ok) {
                s.replace(pos, from.size(), to);
                pos += to.size();
            } else {
                pos += from.size();
            }
        }
    };
    replace_token(content, "rsqrt(double x);", "rsqrt(double x) noexcept (true);");
    replace_token(content, "rsqrtf(float x);", "rsqrtf(float x) noexcept (true);");
    replace_token(content, "rsqrt(double a));", "rsqrt(double a) noexcept (true));");
    replace_token(content, "rsqrtf(float a));", "rsqrtf(float a) noexcept (true));");
    replace_token(content, "sinpi(double x);", "sinpi(double x) noexcept (true);");
    replace_token(content, "sinpif(float x);", "sinpif(float x) noexcept (true);");
    replace_token(content, "cospi(double x);", "cospi(double x) noexcept (true);");
    replace_token(content, "cospif(float x);", "cospif(float x) noexcept (true);");
    replace_token(content, "sinpi(double a));", "sinpi(double a) noexcept (true));");
    replace_token(content, "sinpif(float a));", "sinpif(float a) noexcept (true));");
    replace_token(content, "cospi(double a));", "cospi(double a) noexcept (true));");
    replace_token(content, "cospif(float a));", "cospif(float a) noexcept (true));");
    return content;
}

static fs::path prepare_cuda_glibc242_compat(const Config& cfg, const fs::path& cuda_root) {
    std::vector<fs::path> math_hdrs = {
        cuda_root / "targets/x86_64-linux/include/crt/math_functions.h",
        cuda_root / "include/crt/math_functions.h",
        "/usr/local/cuda/targets/x86_64-linux/include/crt/math_functions.h",
        "/usr/include/crt/math_functions.h"
    };

    if (can_run_sudo()) {
        run_cmd(
            "for hc in /usr/local/cuda*/targets/x86_64-linux/include/crt/host_config.h /usr/include/crt/host_config.h; do "
            "[ -f \"$hc\" ] || continue; "
            "sudo sed -i -E 's/#if __GNUC__ > 1[0-5]/#if __GNUC__ > 16/g' \"$hc\" 2>/dev/null || true; "
            "done", true);
    }

    fs::path compat_dir = fs::path(cfg.cache_dir) / "cuda_compat";
    std::error_code ec;
    fs::create_directories(compat_dir / "crt", ec);

    fs::path sys_inc = cuda_root / "targets/x86_64-linux/include";
    if (!fs::exists(sys_inc, ec)) sys_inc = cuda_root / "include";
    if (fs::exists(sys_inc / "cuda_runtime.h", ec)) {
        fs::copy_file(sys_inc / "cuda_runtime.h", compat_dir / "cuda_runtime.h",
                      fs::copy_options::overwrite_existing, ec);
    }
    if (fs::exists(sys_inc / "crt", ec)) {
        for (auto it = fs::directory_iterator(sys_inc / "crt", ec); it != fs::directory_iterator(); it.increment(ec)) {
            if (ec || !it->is_regular_file(ec)) continue;
            fs::copy_file(it->path(), compat_dir / "crt" / it->path().filename(),
                          fs::copy_options::overwrite_existing, ec);
        }
    }

    for (const auto& hdr : math_hdrs) {
        if (fs::exists(hdr, ec)) {
            std::string raw = read_file_bytes(hdr);
            if (!raw.empty()) {
                std::string patched = patch_math_functions_content(raw);
                fs::path out_hdr = compat_dir / "crt/math_functions.h";
                write_file_atomic(out_hdr, patched, 0644);
                if (can_run_sudo()) {
                    run_cmd("sudo cp -f " + shell_quote(out_hdr.string()) + " " + shell_quote(hdr.string()) +
                            " && sudo chmod 0644 " + shell_quote(hdr.string()), true);
                }
                log_ok("Synced glibc 2.42 CUDA math_functions.h patch (lines 629/653 noexcept; lines 5706-5714 float overloads clean).");
                return compat_dir;
            }
        }
    }
    return {};
}

static std::string detect_nvcc_binary(const Config& cfg) {
    std::vector<std::string> candidates = {
        cfg.cuda_home + "/bin/nvcc",
        "/usr/local/cuda-13.3/bin/nvcc",
        "/usr/local/cuda-13.2/bin/nvcc",
        "/usr/local/cuda-13.1/bin/nvcc",
        "/usr/local/cuda-13.0/bin/nvcc",
        "/usr/local/cuda/bin/nvcc",
        "/usr/bin/nvcc"
    };
    std::error_code ec;
    for (const auto& c : candidates) {
        if (fs::exists(c, ec)) return c;
    }
    if (fs::exists("/usr/local", ec)) {
        std::vector<std::string> found;
        for (auto it = fs::directory_iterator("/usr/local", ec); it != fs::directory_iterator(); it.increment(ec)) {
            if (ec || !it->is_directory(ec)) continue;
            std::string name = it->path().filename().string();
            if (name.rfind("cuda-", 0) == 0) {
                fs::path p = it->path() / "bin/nvcc";
                if (fs::exists(p, ec)) found.push_back(p.string());
            }
        }
        if (!found.empty()) {
            std::sort(found.rbegin(), found.rend());
            return found.front();
        }
    }
    if (command_exists("nvcc")) {
        return capture_cmd("command -v nvcc");
    }
    return {};
}

static std::string detect_gpu_cuda_arch() {
    std::string cap = capture_cmd(
        "PATH=\"/usr/lib/wsl/lib:$PATH\" nvidia-smi --query-gpu=compute_cap "
        "--format=csv,noheader 2>/dev/null | head -n 1");
    std::string digits;
    for (char c : cap) {
        if (std::isdigit(static_cast<unsigned char>(c))) digits.push_back(c);
    }
    if (digits.size() >= 2) return digits;
    return "native";
}

static void sanitize_thirdparty_cmake_warnings(const fs::path& repo_dir) {
    std::error_code ec;
    if (fs::exists(repo_dir / ".git", ec)) {
        run_cmd("git -C " + shell_quote(repo_dir.string()) + " checkout -- . 2>/dev/null || true", true);
        run_cmd("git -C " + shell_quote(repo_dir.string()) + " submodule foreach --recursive 'git checkout -- .' 2>/dev/null || true", true);
    }
    std::vector<fs::path> cmake_files = {
        repo_dir / "CMakeLists.txt",
        repo_dir / "ggml/CMakeLists.txt",
        repo_dir / "ggml/src/CMakeLists.txt",
        repo_dir / "ggml/src/ggml-cuda/CMakeLists.txt"
    };
    auto replace_all = [](std::string& s, std::string_view from, std::string_view to) {
        std::size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    for (const auto& f : cmake_files) {
        if (!fs::exists(f, ec)) continue;
        std::string txt = read_file_bytes(f);
        if (txt.empty()) continue;
        std::string orig = txt;
        replace_all(txt, " =implicit-int", " -Werror=implicit-int");
        replace_all(txt, "\"=implicit-int", "\"-Werror=implicit-int");
        replace_all(txt, " =implicit-function-declaration", " -Werror=implicit-function-declaration");
        replace_all(txt, "\"=implicit-function-declaration", "\"-Werror=implicit-function-declaration");
        replace_all(txt, " =vla", " -Werror=vla");
        replace_all(txt, "\"=vla", "\"-Werror=vla");
        if (txt != orig) {
            write_file_atomic(f, txt, 0644);
        }
    }

    fs::path mul_mat_i8 = repo_dir / "ggml/src/ggml-cuda/mul-mat-i8.cu";
    if (fs::exists(mul_mat_i8, ec)) {
        std::string cu = read_file_bytes(mul_mat_i8);
        const std::string pragma_guard =
            "// LAIS CUTLASS diagnostic guard\n"
            "#pragma GCC diagnostic ignored \"-Wunused-parameter\"\n"
            "#pragma GCC diagnostic ignored \"-Wunused-variable\"\n"
            "#pragma GCC diagnostic ignored \"-Wsign-compare\"\n"
            "#pragma GCC diagnostic ignored \"-Wdeprecated-declarations\"\n"
            "#ifdef __CUDACC__\n"
            "#pragma nv_diag_suppress 177,550,20012\n"
            "#endif\n";
        if (!cu.empty() && cu.find("LAIS CUTLASS diagnostic guard") == std::string::npos) {
            write_file_atomic(mul_mat_i8, pragma_guard + cu, 0644);
        }
    }
}

static bool ensure_cuda_and_build_deps(const Config& cfg, std::string& out_nvcc) {
    if (can_run_sudo()) {
        log_info("Checking system build & multimedia packages for LAIS C++ engines...");
        run_cmd("sudo DEBIAN_FRONTEND=noninteractive apt-get update && "
                "sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "
                "ca-certificates gnupg curl wget git build-essential cmake ninja-build pkg-config ccache "
                "zstd ffmpeg libgomp1 libgl1 libglib2.0-0t64 libsndfile1", true);

        log_info("Ensuring GCC 14 host compiler (gcc-14 / g++-14) is installed for CUDA nvcc compatibility...");
        run_cmd("sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends gcc-14 g++-14 2>/dev/null || "
                "sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends gcc-13 g++-13 2>/dev/null || true", true);
    }

    out_nvcc = detect_nvcc_binary(cfg);
    if (out_nvcc.empty() && can_run_sudo() && !cfg.cuda_keyring_deb_url.empty() && !cfg.cuda_apt_package.empty()) {
        log_warn("NVIDIA CUDA Toolkit (nvcc) not found yet. Installing " + cfg.cuda_apt_package + " via " + cfg.sources_file + "...");
        run_cmd("wget -q " + shell_quote(cfg.cuda_keyring_deb_url) +
                " -O /tmp/cuda-keyring.deb && "
                "sudo dpkg -i /tmp/cuda-keyring.deb && rm -f /tmp/cuda-keyring.deb && "
                "sudo DEBIAN_FRONTEND=noninteractive apt-get update && "
                "sudo DEBIAN_FRONTEND=noninteractive apt-get install -y " + shell_quote(cfg.cuda_apt_package), true);
        out_nvcc = detect_nvcc_binary(cfg);
    }
    return !out_nvcc.empty() && fs::exists(out_nvcc);
}

bool build_cpp_ai_engines(const Config& cfg, bool force_update) {
    cfg.ensure_directories();
    std::string nvcc;
    bool has_cuda = ensure_cuda_and_build_deps(cfg, nvcc);
    std::string cuda_flags;
    std::string compiler_flags = " -DCMAKE_C_FLAGS=-w -DCMAKE_CXX_FLAGS=-w";

    if (has_cuda) {
        fs::path nvcc_path(nvcc);
        fs::path cuda_root = nvcc_path.parent_path().parent_path();
        fs::path compat_inc = prepare_cuda_glibc242_compat(cfg, cuda_root);

        const char* old_path = std::getenv("PATH");
        std::string new_path = nvcc_path.parent_path().string() + ":/usr/lib/wsl/lib:" + (old_path ? old_path : "/usr/bin:/bin");
        set_env_var("PATH", new_path.c_str());

        const char* old_ld = std::getenv("LD_LIBRARY_PATH");
        std::string new_ld = "/usr/lib/wsl/lib:" + (cuda_root / "lib64").string() +
                             (old_ld && *old_ld ? (std::string(":") + old_ld) : "");
        set_env_var("LD_LIBRARY_PATH", new_ld.c_str());
        set_env_var("CUDA_HOME", cuda_root.string().c_str());

        std::string host_cc, host_cxx;
        if (fs::exists("/usr/bin/gcc-14") && fs::exists("/usr/bin/g++-14")) {
            host_cc  = "/usr/bin/gcc-14";
            host_cxx = "/usr/bin/g++-14";
        } else if (fs::exists("/usr/bin/gcc-13") && fs::exists("/usr/bin/g++-13")) {
            host_cc  = "/usr/bin/gcc-13";
            host_cxx = "/usr/bin/g++-13";
        }

        if (!host_cxx.empty()) {
            set_env_var("CC", host_cc.c_str());
            set_env_var("CXX", host_cxx.c_str());
            set_env_var("CUDAHOSTCXX", host_cxx.c_str());
            set_env_var("NVCC_CCBIN", host_cxx.c_str());
            compiler_flags += " -DCMAKE_C_COMPILER=" + shell_quote(host_cc) +
                              " -DCMAKE_CXX_COMPILER=" + shell_quote(host_cxx) +
                              " -DCMAKE_CUDA_HOST_COMPILER=" + shell_quote(host_cxx);
        }

        std::string nvcc_prepend = "-allow-unsupported-compiler -w";
        if (!compat_inc.empty()) {
            nvcc_prepend += " -I" + compat_inc.string();
        }
        set_env_var("NVCC_PREPEND_FLAGS", nvcc_prepend.c_str());
        set_env_var("NVCC_APPEND_FLAGS", "-w -Xcompiler -w,-Wno-unused-parameter,-Wno-unused-variable,-Wno-sign-compare,-Wno-deprecated-declarations");
        std::string gpu_arch = detect_gpu_cuda_arch();
        cuda_flags = " -DCMAKE_CUDA_COMPILER=" + shell_quote(nvcc) +
                     " -DCMAKE_CUDA_ARCHITECTURES=" + shell_quote(gpu_arch) +
                     " -DCMAKE_CUDA_FLAGS=-allow-unsupported-compiler" +
                     compiler_flags;
        log_info("Configured CUDA compiler: " + nvcc +
                 (host_cxx.empty() ? "" : (" (Host C++: " + host_cxx + ")")) +
                 " [GPU Arch: " + gpu_arch + "]");
    } else {
        cuda_flags = compiler_flags;
        log_warn("nvcc not detected; building C++ engines with CPU fallback.");
    }

    unsigned int hw_threads = std::max(1u, std::thread::hardware_concurrency());
    unsigned int build_jobs = std::min(hw_threads, 8u);
    std::string jobs_str = std::to_string(build_jobs);
    set_env_var("NVCC_THREADS", "2");

    auto clean_stale_cmake_cache = [](const fs::path& build_dir) {
        std::error_code ec;
        fs::remove(build_dir / "CMakeCache.txt", ec);
        fs::remove_all(build_dir / "CMakeFiles", ec);
    };

    // 1. llama.cpp
    fs::path llama_dir = fs::path(cfg.engines_dir) / "llama.cpp";
    fs::path llama_bin = llama_dir / "build/bin/llama-server";
    if (!fs::exists(llama_bin) || force_update) {
        if (cfg.llama_cpp_repo.empty()) {
            log_err("llama_cpp_repo is not configured in " + cfg.sources_file);
            return false;
        }
        log_info(std::string(force_update ? "Updating & rebuilding " : "Building ") +
                 "llama.cpp in " + llama_dir.string() + " ...");
        if (!fs::exists(llama_dir / ".git")) {
            std::error_code ec;
            fs::remove_all(llama_dir, ec);
            if (run_cmd("git clone " + shell_quote(cfg.llama_cpp_repo) + " " +
                        shell_quote(llama_dir.string())) != 0) return false;
        } else {
            run_cmd("git -C " + shell_quote(llama_dir.string()) + " fetch --unshallow origin master 2>/dev/null || "
                    "git -C " + shell_quote(llama_dir.string()) + " fetch origin master", true);
            run_cmd("git -C " + shell_quote(llama_dir.string()) + " reset --hard origin/master", true);
        }
        sanitize_thirdparty_cmake_warnings(llama_dir);
        clean_stale_cmake_cache(llama_dir / "build");
        std::string ggml_cuda_opt = has_cuda ? "-DGGML_CUDA=ON" : "-DGGML_CUDA=OFF";
        std::string bcmd = "cmake -S " + shell_quote(llama_dir.string()) +
                           " -B " + shell_quote((llama_dir / "build").string()) +
                           " -G Ninja -DCMAKE_BUILD_TYPE=Release " + ggml_cuda_opt +
                           " -DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=ON" +
                           cuda_flags +
                           " && cmake --build " + shell_quote((llama_dir / "build").string()) +
                           " --config Release --target llama-server llama-cli -j" + jobs_str;
        if (run_cmd(bcmd) != 0) return false;
    }
    log_ok("llama.cpp ready: " + llama_bin.string());

    // 2. stable-diffusion.cpp
    std::string sd_tag = cfg.stable_diffusion_cpp_tag.empty() ? "master-1028-37bc88b" : cfg.stable_diffusion_cpp_tag;
    fs::path sd_dir = fs::path(cfg.engines_dir) / "stable-diffusion.cpp";
    fs::path sd_bin = sd_dir / "build/bin/sd-cli";
    if ((!fs::exists(sd_bin) && !fs::exists(sd_dir / "build/bin/sd")) || force_update) {
        if (cfg.stable_diffusion_cpp_repo.empty()) {
            log_err("stable_diffusion_cpp_repo is not configured in " + cfg.sources_file);
            return false;
        }
        log_info(std::string(force_update ? "Updating & rebuilding " : "Building ") +
                 "stable-diffusion.cpp in " + sd_dir.string() + " ...");
        if (!fs::exists(sd_dir / ".git")) {
            std::error_code ec;
            fs::remove_all(sd_dir, ec);
            std::string clone_cmd =
                "git clone --depth 1 --branch " + shell_quote(sd_tag) +
                " --recurse-submodules " + shell_quote(cfg.stable_diffusion_cpp_repo) + " " +
                shell_quote(sd_dir.string()) + " || "
                "git clone --depth 1 --recurse-submodules " + shell_quote(cfg.stable_diffusion_cpp_repo) + " " +
                shell_quote(sd_dir.string());
            if (run_cmd(clone_cmd) != 0) return false;
        } else {
            if ((!force_update && fs::exists(sd_dir / "ggml/src/ggml-cuda/mul-mat-i8.cu")) || force_update) {
                run_cmd("git -C " + shell_quote(sd_dir.string()) + " fetch --depth 1 origin tag " + shell_quote(sd_tag) +
                        " && git -C " + shell_quote(sd_dir.string()) + " checkout -f " + shell_quote(sd_tag) +
                        " && git -C " + shell_quote(sd_dir.string()) + " submodule update --init --recursive --force" +
                        " && git -C " + shell_quote((sd_dir / "ggml").string()) + " clean -fdx", true);
            } else {
                run_cmd("git -C " + shell_quote(sd_dir.string()) + " submodule update --init --recursive", true);
            }
        }

        sanitize_thirdparty_cmake_warnings(sd_dir);
        clean_stale_cmake_cache(sd_dir / "build");
        std::string sd_cuda_opt = has_cuda ? "-DSD_CUDA=ON" : "-DSD_CUDA=OFF";
        std::string bcmd = "cmake -S " + shell_quote(sd_dir.string()) +
                           " -B " + shell_quote((sd_dir / "build").string()) +
                           " -G Ninja -DCMAKE_BUILD_TYPE=Release " + sd_cuda_opt +
                           cuda_flags +
                           " && cmake --build " + shell_quote((sd_dir / "build").string()) +
                           " --config Release -j" + jobs_str;
        if (run_cmd(bcmd) != 0) {
            log_warn("Retrying stable-diffusion.cpp build with upstream GGML compatibility mode (-DSD_USE_UPSTREAM_GGML=ON)...");
            clean_stale_cmake_cache(sd_dir / "build");
            std::string upstream_ggml_opt = sd_cuda_opt + " -DSD_USE_UPSTREAM_GGML=ON";
            if (fs::exists(llama_dir / "ggml/CMakeLists.txt")) {
                upstream_ggml_opt += " -DSD_GGML_SOURCE_DIR=" + shell_quote((llama_dir / "ggml").string());
            }
            std::string fallback_cmd = "cmake -S " + shell_quote(sd_dir.string()) +
                                       " -B " + shell_quote((sd_dir / "build").string()) +
                                       " -G Ninja -DCMAKE_BUILD_TYPE=Release " + upstream_ggml_opt +
                                       cuda_flags +
                                       " && cmake --build " + shell_quote((sd_dir / "build").string()) +
                                       " --config Release -j" + jobs_str;
            if (run_cmd(fallback_cmd) != 0) return false;
        }
    }
    log_ok("stable-diffusion.cpp ready.");

    // 3. acestep.cpp
    fs::path ace_dir = fs::path(cfg.engines_dir) / "acestep.cpp";
    fs::path ace_bin = ace_dir / "build/ace-synth";
    if (!fs::exists(ace_bin)) ace_bin = ace_dir / "build/bin/ace-synth";
    if (!fs::exists(ace_bin) || force_update) {
        if (cfg.acestep_cpp_repo.empty()) {
            log_err("acestep_cpp_repo is not configured in " + cfg.sources_file);
            return false;
        }
        log_info(std::string(force_update ? "Updating & rebuilding " : "Building ") +
                 "acestep.cpp in " + ace_dir.string() + " ...");
        if (!fs::exists(ace_dir / ".git")) {
            std::error_code ec;
            fs::remove_all(ace_dir, ec);
            if (run_cmd("git clone --depth 1 --recurse-submodules " + shell_quote(cfg.acestep_cpp_repo) + " " +
                        shell_quote(ace_dir.string())) != 0) return false;
        } else {
            if (force_update) run_cmd("git -C " + shell_quote(ace_dir.string()) + " pull --ff-only", true);
            run_cmd("git -C " + shell_quote(ace_dir.string()) + " submodule update --init --recursive", true);
        }
        sanitize_thirdparty_cmake_warnings(ace_dir);
        clean_stale_cmake_cache(ace_dir / "build");
        std::string ace_cuda_opt = has_cuda ? "-DGGML_CUDA=ON" : "-DGGML_CUDA=OFF";
        std::string bcmd = "cmake -S " + shell_quote(ace_dir.string()) +
                           " -B " + shell_quote((ace_dir / "build").string()) +
                           " -G Ninja -DCMAKE_BUILD_TYPE=Release " + ace_cuda_opt +
                           cuda_flags +
                           " && cmake --build " + shell_quote((ace_dir / "build").string()) +
                           " --config Release -j" + jobs_str;
        if (run_cmd(bcmd) != 0) return false;
    }
    log_ok("acestep.cpp ready.");

    std::error_code ec;
    for (const fs::path& src_b : {
             llama_dir / "build/bin/llama-server",
             llama_dir / "build/bin/llama-cli",
             sd_dir / "build/bin/sd-cli",
             sd_dir / "build/bin/sd",
             ace_dir / "build/ace-lm",
             ace_dir / "build/bin/ace-lm",
             ace_dir / "build/ace-synth",
             ace_dir / "build/bin/ace-synth" }) {
        if (fs::exists(src_b, ec)) {
            fs::path dst_b = fs::path(cfg.bin_dir) / src_b.filename();
            fs::copy_file(src_b, dst_b, fs::copy_options::overwrite_existing, ec);
#ifndef _WIN32
            ::chmod(dst_b.c_str(), 0755);
#endif
        }
    }
    return true;
}

bool install_models_by_selector(Config& cfg, std::string_view selector) {
    cfg.ensure_directories();
    cfg.sync_seed_configs();
    cfg.load_config_file();
    if (const char* skip = std::getenv("SKIP_AI_MODELS")) {
        if (std::string(skip) == "1") {
            log_warn("SKIP_AI_MODELS=1 set: skipping Hugging Face GGUF downloads.");
            return true;
        }
    }

    std::string q = to_lower_ascii(trim_copy(selector));
    if (q == "engines" || q == "engine") {
        return build_cpp_ai_engines(cfg, false);
    }
    if (q == "conda" || q == "miniforge" || q == "miniforge3") {
        return install_miniforge3(cfg);
    }
    if (q == "cuda") {
        std::string nvcc;
        bool ok = ensure_cuda_and_build_deps(cfg, nvcc);
        if (ok) {
            fs::path cuda_root = fs::path(nvcc).parent_path().parent_path();
            prepare_cuda_glibc242_compat(cfg, cuda_root);
            log_ok("CUDA Toolkit ready at " + cuda_root.string());
        }
        return ok;
    }

    auto targets = match_models_by_selector(cfg, selector);
    if (targets.empty()) {
        log_err("No model matched selector '" + std::string(selector) +
                "' in " + cfg.models_file + ". Run 'models' to view available keys, categories, or tiers.");
        return false;
    }

    log_info("Installing " + std::to_string(targets.size()) + " model asset(s) into " + cfg.models_dir +
             " (Catalog: " + cfg.models_file + ") ...");
    std::vector<ModelAsset> ordered;
    std::set<std::string> visited;
    bool dependency_ok = true;
    std::function<void(const ModelAsset&)> include = [&](const ModelAsset& asset) {
        if (!visited.insert(asset.key).second) return;
        for (const auto& key : asset.dependencies) {
            auto it = std::find_if(cfg.models.begin(), cfg.models.end(), [&](const auto& m) { return m.key == key; });
            if (it == cfg.models.end() || it->safety != asset.safety) { dependency_ok = false; continue; }
            include(*it);
        }
        ordered.push_back(asset);
    };
    for (const auto& target : targets) include(target);
    if (!dependency_ok) { log_err("Missing or cross-mode model dependency in catalog."); return false; }
    bool all_ok = true;
    for (const auto& m : ordered) {
        fs::path dest = fs::path(cfg.models_dir) / m.subdir / m.filename;
        if (!download_hf_asset(m.url, dest, m.key + " (" + m.filename + ")", m.approx_gb)) {
            all_ok = false;
        }
    }
    maybe_quantize_wan_eco(cfg);
    purge_dram_and_pagecache(cfg.models_dir);
    return all_ok;
}

bool uninstall_models_by_selector(Config& cfg, std::string_view selector) {
    cfg.load_config_file();
    std::string q = to_lower_ascii(trim_copy(selector));
    if (q.empty()) {
        log_warn("Specify a model key, category (llm/coder/image/video/music), safety (standard/uncensored), tier (eco/high), or 'all'.");
        log_info("Example: uninstall llm_high_9b_q8   or   uninstall uncensored   or   uninstall all");
        return false;
    }

    auto targets = match_models_by_selector(cfg, selector);
    if (targets.empty()) {
        log_err("No model matched selector '" + std::string(selector) + "'.");
        return false;
    }

    std::error_code ec;
    std::size_t removed_count = 0;
    for (const auto& m : targets) {
        fs::path p = fs::path(cfg.models_dir) / m.subdir / m.filename;
        fs::path part = p.string() + ".part";
        if (fs::exists(p, ec)) {
            fs::remove(p, ec);
            log_ok("Removed " + m.key + " -> " + p.string());
            ++removed_count;
        }
        if (fs::exists(part, ec)) {
            fs::remove(part, ec);
        }
        if (m.key == "wan_high_fp16" || m.key == "wan_std_high_fp16" || q == "all" || q == "video" || q == "eco") {
            for (std::string_view eco_role : {"video_dit_eco", "video_std_dit_eco"}) {
                std::string eco_vid = cfg.role_filename(eco_role);
                if (!eco_vid.empty()) {
                    fs::path wan_q4 = fs::path(cfg.models_dir) / "diffusion" / eco_vid;
                    if (fs::exists(wan_q4, ec)) {
                        fs::remove(wan_q4, ec);
                        log_ok("Removed derived Eco video model -> " + wan_q4.string());
                        ++removed_count;
                    }
                }
            }
        }
    }
    if (removed_count == 0) {
        log_info("No matching model files were currently installed on disk.");
    } else {
        log_ok("Uninstalled " + std::to_string(removed_count) + " model file(s).");
    }
    return true;
}

static void ensure_bashrc_lais_path(const Config& cfg) {
    fs::path bashrc = fs::path(cfg.home_dir) / ".bashrc";
    std::string content = read_file_bytes(bashrc);
    if (content.find("# >>> LAIS CLI PATH >>>") == std::string::npos) {
        std::ofstream ofs(bashrc, std::ios::app);
        if (ofs) {
            ofs << "\n# >>> LAIS CLI PATH >>>\n"
                << "export PATH=\"$HOME/lais/bin:$HOME/.local/bin:/usr/local/bin:$PATH\"\n"
                << "# <<< LAIS CLI PATH <<<\n";
        }
    }
}

static void remove_bashrc_lais_path(const Config& cfg) {
    fs::path bashrc = fs::path(cfg.home_dir) / ".bashrc";
    std::error_code ec;
    if (!fs::exists(bashrc, ec)) return;
    run_cmd("sed -i '/# >>> LAIS CLI PATH >>>/,/# <<< LAIS CLI PATH <<</d' " +
            shell_quote(bashrc.string()) + " 2>/dev/null || true", true);
}

fs::path install_self_binary(const Config& cfg, const char* argv0, bool install_usr_local) {
    cfg.ensure_directories();
    cfg.sync_seed_configs(argv0);
    std::error_code ec;
    fs::path dest_bin = fs::path(cfg.bin_dir) / (std::string("lais") + exe_suffix());
    fs::path self_exe = get_self_exe_path(argv0);
    if (!self_exe.empty() && fs::exists(self_exe, ec)) {
        if (self_exe != dest_bin) {
            fs::copy_file(self_exe, dest_bin, fs::copy_options::overwrite_existing, ec);
#ifndef _WIN32
            ::chmod(dest_bin.c_str(), 0755);
#endif
        }
#ifdef _WIN32
        (void)install_usr_local;
        HKEY hKey = nullptr;
        if (::RegOpenKeyExA(HKEY_CURRENT_USER, "Environment", 0, KEY_READ | KEY_WRITE, &hKey) == ERROR_SUCCESS) {
            char cur_path[8192] = {0};
            DWORD sz = sizeof(cur_path) - 1;
            DWORD type = REG_EXPAND_SZ;
            (void)::RegQueryValueExA(hKey, "Path", nullptr, &type, reinterpret_cast<LPBYTE>(cur_path), &sz);
            std::string path_str(cur_path);
            if (to_lower_ascii(path_str).find(to_lower_ascii(cfg.bin_dir)) == std::string::npos) {
                if (!path_str.empty() && path_str.back() != ';') path_str.push_back(';');
                path_str += cfg.bin_dir;
                (void)::RegSetValueExA(
                    hKey, "Path", 0, REG_EXPAND_SZ,
                    reinterpret_cast<const BYTE*>(path_str.c_str()),
                    static_cast<DWORD>(path_str.size() + 1));
            }
            ::RegCloseKey(hKey);
        }
#else
        fs::path local_bin = fs::path(cfg.home_dir) / ".local/bin";
        fs::create_directories(local_bin, ec);
        fs::path local_link = local_bin / "lais";
        if (self_exe != local_link) {
            fs::copy_file(self_exe, local_link, fs::copy_options::overwrite_existing, ec);
            ::chmod(local_link.c_str(), 0755);
        }
        if (install_usr_local && can_run_sudo()) {
            run_cmd("sudo install -m 0755 " + shell_quote(self_exe.string()) + " /usr/local/bin/lais 2>/dev/null || true", true);
        }
        ensure_bashrc_lais_path(cfg);
#endif
        return fs::exists(dest_bin, ec) ? dest_bin : self_exe;
    }
    return self_exe;
}

bool extract_embedded_engines(Config& cfg, const char* argv0, bool force_extract) {
    cfg.ensure_directories();
    std::string sfx = exe_suffix();
    bool already_have_bins = fs::exists(fs::path(cfg.bin_dir) / ("llama-server" + sfx)) &&
                             (fs::exists(fs::path(cfg.bin_dir) / ("sd-cli" + sfx)) || fs::exists(fs::path(cfg.bin_dir) / ("sd" + sfx))) &&
                             fs::exists(fs::path(cfg.bin_dir) / ("ace-lm" + sfx)) &&
                             fs::exists(fs::path(cfg.bin_dir) / ("ace-synth" + sfx));
    bool already_have_cfgs = fs::exists(cfg.config_file) && fs::exists(cfg.sources_file) && fs::exists(cfg.models_file);
    if (already_have_bins && already_have_cfgs && !force_extract) {
        return true;
    }

    std::error_code ec;
    std::vector<fs::path> candidates;
    fs::path self_exe = get_self_exe_path(argv0);
    if (!self_exe.empty()) candidates.push_back(self_exe);
    candidates.push_back(fs::path(cfg.bin_dir) / ("lais" + sfx));
    candidates.push_back(fs::path(cfg.home_dir) / ".local/bin" / ("lais" + sfx));
    candidates.push_back(fs::path("/usr/local/bin/lais"));
    candidates.push_back(fs::current_path(ec) / "build" / ("lais" + sfx));
    candidates.push_back(fs::current_path(ec) / ("lais" + sfx));

    const std::string prefix = "LAIS_BUNDLE_V1|";
    const std::string suffix = "|END_LAIS_BUNDLE";

    fs::path chosen_exe;
    uint64_t offset = 0;
    uint64_t length = 0;

    for (const auto& cand : candidates) {
        if (cand.empty() || !fs::exists(cand, ec)) continue;
        uint64_t total_sz = fs::file_size(cand, ec);
        if (ec || total_sz < 256) continue;
        std::ifstream in(cand, std::ios::binary);
        if (!in) continue;
        uint64_t tail_len = std::min<uint64_t>(total_sz, 256);
        in.seekg(static_cast<std::streamoff>(total_sz - tail_len), std::ios::beg);
        std::string tail(tail_len, '\0');
        in.read(tail.data(), static_cast<std::streamsize>(tail_len));

        std::size_t p1 = tail.rfind(prefix);
        if (p1 == std::string::npos) continue;
        std::size_t p2 = tail.find(suffix, p1);
        if (p2 == std::string::npos) continue;
        std::string meta = tail.substr(p1 + prefix.size(), p2 - (p1 + prefix.size()));
        std::size_t bar = meta.find('|');
        if (bar == std::string::npos) continue;

        uint64_t off = std::strtoull(meta.substr(0, bar).c_str(), nullptr, 10);
        uint64_t len = std::strtoull(meta.substr(bar + 1).c_str(), nullptr, 10);
        if (off > 0 && len > 0 && off + len <= total_sz) {
            chosen_exe = cand;
            offset = off;
            length = len;
            break;
        }
    }

    if (chosen_exe.empty()) return false;

    fs::path marker_file = fs::path(cfg.bin_dir) / ".lais_bundle_stamp";
    std::string expected_stamp = std::to_string(offset) + ":" + std::to_string(length);
    if (!force_extract && already_have_cfgs && fs::exists(marker_file, ec)) {
        if (trim_copy(read_file_bytes(marker_file)) == expected_stamp) {
            return true;
        }
    }

    std::ifstream in(chosen_exe, std::ios::binary);
    if (!in) return false;

    log_info("Extracting bundled C++ inference engines & configs from " + chosen_exe.string() + " into " + cfg.bin_dir + " ...");
    fs::path tmp_tar = fs::path(cfg.cache_dir) / "tmp" / ("lais_bundle_" + std::to_string(get_current_pid()) + ".tar.gz");
    fs::create_directories(tmp_tar.parent_path(), ec);

    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    {
        std::ofstream out(tmp_tar, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        std::vector<char> buf(65536);
        uint64_t rem = length;
        while (rem > 0 && in) {
            std::size_t chunk = static_cast<std::size_t>(std::min<uint64_t>(rem, buf.size()));
            in.read(buf.data(), static_cast<std::streamsize>(chunk));
            std::streamsize got = in.gcount();
            if (got <= 0) break;
            out.write(buf.data(), got);
            rem -= static_cast<uint64_t>(got);
        }
        if (rem != 0) {
            fs::remove(tmp_tar, ec);
            return false;
        }
    }

#ifdef _WIN32
    int rc = run_cmd("tar -xzf " + shell_quote(tmp_tar.string()) + " -C " + shell_quote(cfg.bin_dir) + " >NUL 2>&1", true);
#else
    int rc = run_cmd("tar -xzf " + shell_quote(tmp_tar.string()) + " -C " + shell_quote(cfg.bin_dir) +
                     " && chmod 0755 " + shell_quote(cfg.bin_dir) + "/* 2>/dev/null || true", true);
#endif
    fs::remove(tmp_tar, ec);

    cfg.sync_seed_configs(argv0);
    cfg.load_config_file();

    if (rc == 0) {
        write_file_atomic(marker_file, expected_stamp + "\n", 0644);
        log_ok("Extracted bundled C++ engines & configs into " + cfg.bin_dir);
        return true;
    }
    return false;
}

void sync_existing_global_binaries(const Config& cfg, const char* argv0) {
    std::error_code ec;
    fs::path self_exe = get_self_exe_path(argv0);
    if (self_exe.empty() || !fs::exists(self_exe, ec)) return;

    std::vector<fs::path> user_targets = {
        fs::path(cfg.bin_dir) / (std::string("lais") + exe_suffix()),
        fs::path(cfg.home_dir) / ".local/bin/lais"
    };
    for (const auto& t : user_targets) {
        if (fs::exists(t, ec) && t != self_exe) {
            fs::copy_file(self_exe, t, fs::copy_options::overwrite_existing, ec);
#ifndef _WIN32
            ::chmod(t.c_str(), 0755);
#endif
        }
    }
}

static bool is_systemd_active() {
#ifdef _WIN32
    return false;
#else
    return capture_cmd("cat /proc/1/comm 2>/dev/null") == "systemd" && command_exists("systemctl");
#endif
}

static std::string get_systemd_unit_name() {
#ifdef _WIN32
    return "lais.service";
#else
    return "lais-" + std::to_string(::getuid()) + ".service";
#endif
}

bool is_service_running(const Config& cfg) {
#ifndef _WIN32
    if (is_systemd_active()) {
        std::string unit = get_systemd_unit_name();
        if (std::system(("systemctl is-active --quiet " + shell_quote(unit) + " 2>/dev/null").c_str()) == 0) {
            return true;
        }
    }
#endif
    std::error_code ec;
    if (fs::exists(cfg.pid_file, ec)) {
        std::string s = trim_copy(read_file_bytes(cfg.pid_file));
        long pid = std::atol(s.c_str());
        if (pid > 0) {
#ifdef _WIN32
            HANDLE hProc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
            if (hProc) {
                DWORD wait_rc = ::WaitForSingleObject(hProc, 0);
                ::CloseHandle(hProc);
                if (wait_rc == WAIT_TIMEOUT) return true;
            }
#else
            if (::kill(static_cast<pid_t>(pid), 0) == 0) {
                return true;
            }
#endif
        }
    }
    std::string health = "http://127.0.0.1:" + std::to_string(cfg.bind_port) + "/health";
    std::string body = capture_cmd("curl --fail --silent --max-time 1 " + shell_quote(health) + null_stderr_redirect());
    return body.find("\"service\":\"lais\"") != std::string::npos;
}

bool start_lais_service(Config& cfg, const char* argv0) {
    cfg.ensure_directories();
    cfg.sync_seed_configs(argv0);
    configure_wsl_memory_discipline();
    fs::path bin_path = install_self_binary(cfg, argv0);
    extract_embedded_engines(cfg, argv0);
    PakeAuthManager auth(cfg);
    auth.ensure_user_file();

#ifdef _WIN32
    if (!is_service_running(cfg)) {
        log_info("Starting detached Windows background service on http://" +
                 cfg.bind_host + ":" + std::to_string(cfg.bind_port) + " ...");
        std::string log_path = cfg.logs_dir + "\\lais-daemon.log";
        std::ostringstream cmd_line;
        cmd_line << shell_quote(bin_path.string())
                 << " --serve --host " << cfg.bind_host
                 << " --port " << cfg.bind_port;

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE hLog = ::CreateFileA(
            log_path.c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &sa,
            OPEN_ALWAYS,
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
            DETACHED_PROCESS | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
            nullptr,
            cfg.lais_dir.c_str(),
            &si,
            &pi);
        if (hLog != INVALID_HANDLE_VALUE) ::CloseHandle(hLog);
        if (ok) {
            write_file_atomic(cfg.pid_file, std::to_string(pi.dwProcessId) + "\n", 0644);
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
        } else {
            log_err("Failed to spawn detached Windows background process.");
            return false;
        }
        for (int i = 0; i < 20; ++i) {
            if (is_service_running(cfg)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }

    HKEY hKey = nullptr;
    if (::RegOpenKeyExA(HKEY_CURRENT_USER,
                        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        std::string autorun_cmd = "\"" + bin_path.string() + "\" ensure-daemon";
        (void)::RegSetValueExA(
            hKey, "LAIS", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(autorun_cmd.c_str()),
            static_cast<DWORD>(autorun_cmd.size() + 1));
        ::RegCloseKey(hKey);
    }

    log_ok("LAIS Windows background service is ACTIVE on http://" + cfg.bind_host + ":" +
           std::to_string(cfg.bind_port) + " (Runs silently even if terminal is closed & auto-starts on PC boot).");
    return true;
#else
    run_cmd("pkill -u $(id -u) -f '[w]sl_ai_studio' 2>/dev/null || true", true);

    std::string uid_str    = std::to_string(::getuid());
    std::string user_name  = capture_cmd("id -un");
    std::string group_name = capture_cmd("id -gn");
    std::string unit_name  = get_systemd_unit_name();

    if (is_systemd_active() && can_run_sudo()) {
        log_info("Registering and starting systemd service '" + unit_name + "' on " +
                 cfg.bind_host + ":" + std::to_string(cfg.bind_port) + "...");
        run_cmd("sudo systemctl disable --now open-webui-" + uid_str + ".service comfyui-" + uid_str +
                ".service wsl-ai-studio-" + uid_str + ".service ollama.service 2>/dev/null || true", true);

        std::ostringstream unit;
        unit << "[Unit]\n"
             << "Description=LAIS - Local AI Studio C++20 Multimodal Router (" << cfg.bind_host << ":" << cfg.bind_port << ")\n"
             << "After=network-online.target\n"
             << "Wants=network-online.target\n"
             << "[Service]\n"
             << "Type=simple\n"
             << "User=" << user_name << "\n"
             << "Group=" << group_name << "\n"
             << "WorkingDirectory=" << cfg.lais_dir << "\n"
             << "Environment=\"HOME=" << cfg.home_dir << "\"\n"
             << "Environment=\"LAIS_HOME=" << cfg.lais_dir << "\"\n"
             << "Environment=\"XDG_CACHE_HOME=" << cfg.cache_dir << "\"\n"
             << "Environment=\"HF_HOME=" << cfg.cache_dir << "/hf\"\n"
             << "Environment=\"MALLOC_ARENA_MAX=2\"\n"
             << "Environment=\"MALLOC_TRIM_THRESHOLD_=65536\"\n"
             << "Environment=\"MALLOC_MMAP_THRESHOLD_=65536\"\n"
             << "ExecStart=\"" << bin_path.string() << "\" --serve --host " << cfg.bind_host << " --port " << cfg.bind_port << "\n"
             << "MemoryHigh=75%\n"
             << "MemoryMax=85%\n"
             << "Restart=always\n"
             << "RestartSec=3\n"
             << "[Install]\n"
             << "WantedBy=multi-user.target\n";

        fs::path tmp_unit = fs::path(cfg.cache_dir) / "tmp" / unit_name;
        write_file_atomic(tmp_unit, unit.str());
        run_cmd("sudo mv -f " + shell_quote(tmp_unit.string()) + " /etc/systemd/system/" + unit_name);
        run_cmd("sudo systemctl daemon-reload");
        run_cmd("sudo systemctl enable --now " + unit_name);
        run_cmd("sudo systemctl restart " + unit_name);
    } else {
        if (!is_service_running(cfg)) {
            log_info("Starting LAIS background daemon on " +
                     cfg.bind_host + ":" + std::to_string(cfg.bind_port) + "...");
            std::string log_file = cfg.logs_dir + "/lais-daemon.log";
            std::string cmd = "nohup " + shell_quote(bin_path.string()) +
                              " --serve --host " + shell_quote(cfg.bind_host) +
                              " --port " + std::to_string(cfg.bind_port) +
                              " >>" + shell_quote(log_file) + " 2>&1 &";
            run_cmd(cmd, true);
            for (int i = 0; i < 15; ++i) {
                if (is_service_running(cfg)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    const char* distro_env = std::getenv("WSL_DISTRO_NAME");
    std::string distro = (distro_env && *distro_env) ? distro_env : "";
    std::string ps_bin = command_exists("powershell.exe") ? "powershell.exe"
        : (fs::exists("/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe")
           ? "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe" : "");

    if (!distro.empty() && !ps_bin.empty()) {
        log_info("Registering Windows boot/sign-in autostart keepalive supervisor for WSL2 & LAIS...");
        std::string supervisor_ps1 =
            "$ErrorActionPreference = 'Stop'\n"
            "$mutex = New-Object System.Threading.Mutex($false, 'Local\\LAIS-WSL-KeepAlive')\n"
            "$owned = $false\n"
            "try {\n"
            "    try { $owned = $mutex.WaitOne(0) } catch [System.Threading.AbandonedMutexException] { $owned = $true }\n"
            "    if (-not $owned) { exit 0 }\n"
            "    $logDir = Join-Path $env:LOCALAPPDATA 'LAIS'\n"
            "    New-Item -ItemType Directory -Force -Path $logDir | Out-Null\n"
            "    $log = Join-Path $logDir 'lais-wsl.log'\n"
            "    $wsl = Join-Path $env:SystemRoot 'System32\\wsl.exe'\n"
            "    $bootCmd = '\"" + bin_path.string() + "\" --ensure-daemon && exec /usr/bin/sleep infinity'\n"
            "    $wslArgs = @('--distribution', '" + distro + "', '--user', '" + user_name + "', '--exec', '/bin/bash', '-lc', $bootCmd)\n"
            "    while ($true) {\n"
            "        try { & $wsl @wslArgs >> $log 2>&1 } catch { Add-Content -LiteralPath $log -Value $_.Exception.Message }\n"
            "        Start-Sleep -Seconds 10\n"
            "    }\n"
            "} finally {\n"
            "    if ($owned) { $mutex.ReleaseMutex() }\n"
            "    $mutex.Dispose()\n"
            "}\n";

        write_file_atomic(fs::path(cfg.cache_dir) / "windows-keepalive.ps1", supervisor_ps1);
        std::string sup_b64 = powershell_encoded_command(supervisor_ps1);

        std::string reg_ps1 =
            "$ErrorActionPreference = 'Stop'\n"
            "$startup = [Environment]::GetFolderPath('Startup')\n"
            "New-Item -ItemType Directory -Force -Path $startup | Out-Null\n"
            "$ps = Join-Path $env:SystemRoot 'System32\\WindowsPowerShell\\v1.0\\powershell.exe'\n"
            "$configDir = Join-Path $env:LOCALAPPDATA 'LAIS'\n"
            "New-Item -ItemType Directory -Force -Path $configDir | Out-Null\n"
            "$scriptPath = Join-Path $configDir 'lais-keepalive.ps1'\n"
            "$sup = [Text.Encoding]::Unicode.GetString([Convert]::FromBase64String('" + sup_b64 + "'))\n"
            "[IO.File]::WriteAllText($scriptPath, $sup, [Text.Encoding]::UTF8)\n"
            "$arguments = '-NoLogo -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File \"' + $scriptPath + '\"'\n"
            "$shell = New-Object -ComObject WScript.Shell\n"
            "$shortcut = $shell.CreateShortcut((Join-Path $startup 'LAIS-KeepAlive.lnk'))\n"
            "$shortcut.TargetPath = $ps\n"
            "$shortcut.Arguments = $arguments\n"
            "$shortcut.WorkingDirectory = $env:USERPROFILE\n"
            "$shortcut.WindowStyle = 7\n"
            "$shortcut.Save()\n"
            "Start-Process -FilePath $ps -ArgumentList $arguments -WindowStyle Hidden\n";

        std::string reg_b64 = powershell_encoded_command(reg_ps1);
        run_cmd(shell_quote(ps_bin) + " -NoLogo -NoProfile -NonInteractive -EncodedCommand " + shell_quote(reg_b64), true);
    }

    log_ok("LAIS service is ACTIVE on http://" + cfg.bind_host + ":" + std::to_string(cfg.bind_port) +
           " (Persistent across WSL sessions & automatic on PC boot).");
    return true;
#endif
}

bool stop_lais_service(const Config& cfg) {
#ifdef _WIN32
    std::error_code ec;
    if (fs::exists(cfg.pid_file, ec)) {
        std::string s = trim_copy(read_file_bytes(cfg.pid_file));
        long pid = std::atol(s.c_str());
        if (pid > 0 && pid != get_current_pid()) {
            HANDLE hProc = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
            if (hProc) {
                ::TerminateProcess(hProc, 0);
                ::WaitForSingleObject(hProc, 3000);
                ::CloseHandle(hProc);
            }
        }
        fs::remove(cfg.pid_file, ec);
    }

    run_cmd("taskkill /F /IM llama-server.exe /T >NUL 2>&1", true);
    run_cmd("taskkill /F /IM sd-cli.exe /T >NUL 2>&1", true);
    run_cmd("taskkill /F /IM ace-lm.exe /T >NUL 2>&1", true);
    run_cmd("taskkill /F /IM ace-synth.exe /T >NUL 2>&1", true);

    HKEY hKey = nullptr;
    if (::RegOpenKeyExA(HKEY_CURRENT_USER,
                        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        (void)::RegDeleteValueA(hKey, "LAIS");
        ::RegCloseKey(hKey);
    }

    purge_dram_and_pagecache(cfg.models_dir);
    log_ok("LAIS Windows background service stopped, boot autostart disabled, and VRAM/DRAM purged.");
    return true;
#else
    std::string unit_name = get_systemd_unit_name();
    if (is_systemd_active() && can_run_sudo()) {
        log_info("Stopping and disabling systemd service '" + unit_name + "'...");
        run_cmd("sudo systemctl disable --now " + shell_quote(unit_name) + " 2>/dev/null || true", true);
    }

    std::error_code ec;
    if (fs::exists(cfg.pid_file, ec)) {
        std::string s = trim_copy(read_file_bytes(cfg.pid_file));
        long pid = std::atol(s.c_str());
        if (pid > 0 && pid != ::getpid()) {
            ::kill(static_cast<pid_t>(pid), SIGTERM);
            for (int i = 0; i < 20; ++i) {
                if (::kill(static_cast<pid_t>(pid), 0) != 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (::kill(static_cast<pid_t>(pid), 0) == 0) {
                ::kill(static_cast<pid_t>(pid), SIGKILL);
            }
        }
        fs::remove(cfg.pid_file, ec);
    }

    run_cmd("pkill -u $(id -u) -f '[l]ais --serve' 2>/dev/null || true", true);
    run_cmd("pkill -u $(id -u) -f '[w]sl_ai_studio' 2>/dev/null || true", true);
    run_cmd("pkill -u $(id -u) -f '[l]lama-server' 2>/dev/null || true", true);
    run_cmd("pkill -u $(id -u) -f '[s]d-cli' 2>/dev/null || true", true);
    run_cmd("pkill -u $(id -u) -f '[a]ce-lm' 2>/dev/null || true", true);
    run_cmd("pkill -u $(id -u) -f '[a]ce-synth' 2>/dev/null || true", true);

    std::string ps_bin = command_exists("powershell.exe") ? "powershell.exe"
        : (fs::exists("/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe")
           ? "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe" : "");
    if (!ps_bin.empty()) {
        std::string stop_ps1 =
            "$ErrorActionPreference = 'SilentlyContinue'\n"
            "$startup = [Environment]::GetFolderPath('Startup')\n"
            "$lnk = Join-Path $startup 'LAIS-KeepAlive.lnk'\n"
            "if (Test-Path -LiteralPath $lnk) { Remove-Item -LiteralPath $lnk -Force }\n"
            "Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*lais-keepalive.ps1*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }\n";
        std::string stop_b64 = powershell_encoded_command(stop_ps1);
        run_cmd(shell_quote(ps_bin) + " -NoLogo -NoProfile -NonInteractive -EncodedCommand " + shell_quote(stop_b64), true);
    }

    purge_dram_and_pagecache(cfg.models_dir);
    log_ok("LAIS service stopped, boot autostart disabled, and VRAM/DRAM cache purged.");
    return true;
#endif
}

void print_models_table(Config& cfg) {
    cfg.sync_seed_configs();
    cfg.load_config_file();
    std::cout << "\n========================================================================================\n"
              << " LAIS Dual-Safety & Dual-Tier Model Catalog (" << cfg.models_file << ")\n"
              << "========================================================================================\n"
              << "  " << std::left << std::setw(22) << "MODEL KEY"
              << std::setw(9)  << "CATEGORY"
              << std::setw(7)  << "TIER"
              << std::setw(11) << "SAFETY"
              << std::setw(8)  << "SIZE"
              << std::setw(12) << "STATE"
              << "FILENAME\n"
              << "----------------------------------------------------------------------------------------\n";
    for (const auto& m : cfg.models) {
        fs::path p = fs::path(cfg.models_dir) / m.subdir / m.filename;
        bool ok = fs::exists(p);
        std::ostringstream sz;
        sz << std::fixed << std::setprecision(2) << m.approx_gb << "GB";
        std::cout << "  " << std::left << std::setw(22) << m.key
                  << std::setw(9)  << m.category
                  << std::setw(10) << (m.tiers.size() == 2 ? "eco/high" : m.tiers.front())
                  << std::setw(11) << m.safety
                  << std::setw(8)  << sz.str()
                  << std::setw(12) << (ok ? "[INSTALLED]" : "[ MISSING ]")
                  << m.filename << "\n";
    }
    std::cout << "========================================================================================\n"
              << " Tip: Edit " << cfg.models_file << " anytime to update download URLs or filenames.\n"
              << "      Use 'install <all|standard|uncensored|eco|high|llm|coder|image|video|music|<key>>'\n\n";
}

void print_status(Config& cfg) {
    cfg.sync_seed_configs();
    cfg.load_config_file();
    MemoryStats st = query_memory_stats_sync();
    bool running = is_service_running(cfg);
    uint64_t total_bytes = directory_size_bytes(cfg.lais_dir);
    double total_gb = static_cast<double>(total_bytes) / (1024.0 * 1024.0 * 1024.0);

    std::size_t installed_cnt = 0;
    for (const auto& m : cfg.models) {
        if (fs::exists(fs::path(cfg.models_dir) / m.subdir / m.filename)) ++installed_cnt;
    }

    std::size_t saved_chats = 0;
    std::error_code ec;
    if (fs::exists(cfg.chats_dir, ec)) {
        for (auto it = fs::directory_iterator(cfg.chats_dir, ec); it != fs::directory_iterator(); it.increment(ec)) {
            if (!ec && it->is_regular_file(ec) && it->path().extension() == ".json") ++saved_chats;
        }
    }

    PakeAuthManager auth(cfg);
    UserVerifierRecord u = auth.load_user();

    std::string sfx = exe_suffix();
    bool llama_ok = fs::exists(fs::path(cfg.bin_dir) / ("llama-server" + sfx)) ||
                    fs::exists(fs::path(cfg.engines_dir) / ("llama.cpp/build/bin/Release/llama-server" + sfx)) ||
                    fs::exists(fs::path(cfg.engines_dir) / ("llama.cpp/build/bin/llama-server" + sfx));
    bool sd_ok    = fs::exists(fs::path(cfg.bin_dir) / ("sd-cli" + sfx)) ||
                    fs::exists(fs::path(cfg.bin_dir) / ("sd" + sfx)) ||
                    fs::exists(fs::path(cfg.engines_dir) / ("stable-diffusion.cpp/build/bin/Release/sd-cli" + sfx)) ||
                    fs::exists(fs::path(cfg.engines_dir) / ("stable-diffusion.cpp/build/bin/sd-cli" + sfx)) ||
                    fs::exists(fs::path(cfg.engines_dir) / ("stable-diffusion.cpp/build/bin/sd" + sfx));
    bool ace_ok   = (fs::exists(fs::path(cfg.bin_dir) / ("ace-synth" + sfx)) &&
                     fs::exists(fs::path(cfg.bin_dir) / ("ace-lm" + sfx))) ||
                    fs::exists(fs::path(cfg.engines_dir) / ("acestep.cpp/build/Release/ace-synth" + sfx)) ||
                    fs::exists(fs::path(cfg.engines_dir) / ("acestep.cpp/build/ace-synth" + sfx)) ||
                    fs::exists(fs::path(cfg.engines_dir) / ("acestep.cpp/build/bin/ace-synth" + sfx));

    std::cout << "\n==============================================================================\n"
              << " LAIS (Local AI Studio) v" << LAIS_VERSION << " - System & Service Status\n"
              << "==============================================================================\n"
              << "  Service State : " << (running ? "\033[1;32mRUNNING (Active in Background & Boot Autostart ON)\033[0m"
                                                : "\033[1;33mSTOPPED (Run 'start' to enable background service)\033[0m") << "\n"
              << "  LAN Endpoint  : http://" << cfg.bind_host << ":" << std::to_string(cfg.bind_port) << " (Port-Forwarding Ready)\n"
              << "  Safety Mode   : " << (cfg.default_uncensored ? "UNCENSORED (Toggleable in WebUI)" : "SAFE / STANDARD (Toggleable in WebUI)") << "\n"
              << "  Unified Home  : " << cfg.lais_dir << " (" << std::fixed << std::setprecision(2) << total_gb << " GB used)\n"
              << "  JSON Configs  : " << cfg.configs_dir << " (settings.json, sources.json, models.json)\n"
              << "  PAKE Admin ID : " << u.username << " (" << cfg.user_file << ")"
              << (u.must_change_password ? " [Default 'admin' - change on login]" : " [Custom PAKE Verifier]") << "\n"
              << "  Saved Chats   : " << saved_chats << " session(s) in " << cfg.chats_dir << "\n"
              << "  GPU Device    : " << st.gpu_name << "\n"
              << "  GPU VRAM      : " << st.vram_used_mb << " MB used / " << st.vram_total_mb
              << " MB total (" << st.vram_free_mb << " MB free, " << cfg.vram_reserve_mb << " MB WDDM guard)\n"
              << "  Host DRAM     : " << st.dram_used_mb << " MB used / " << st.dram_total_mb
              << " MB total (" << st.dram_avail_mb << " MB available, " << st.dram_cached_mb << " MB VFS cache)\n"
              << "  C++ Engines   : llama.cpp [" << (llama_ok ? "OK" : "MISSING") << "] | "
              << "stable-diffusion.cpp [" << (sd_ok ? "OK" : "MISSING") << "] | "
              << "acestep.cpp [" << (ace_ok ? "OK" : "MISSING") << "]\n"
              << "  AI Models     : " << installed_cnt << " / " << cfg.models.size() << " installed in " << cfg.models_dir << "\n"
              << "==============================================================================\n\n";
}

void print_help() {
    std::cout << "\n==============================================================================\n"
              << " LAIS (Local AI Studio) v" << LAIS_VERSION << " - Interactive & CLI Command Reference\n"
              << "==============================================================================\n"
              << "  setup                    Configure OS memory discipline, register global 'lais',\n"
              << "                           unpack bundled engines/configs, and download all models\n"
              << "                           (both Standard/Safe and Uncensored) in models.json.\n"
              << "  start                    Register & start LAIS background service (0.0.0.0:3000)\n"
              << "                           and enable automatic launch on Windows PC boot.\n"
              << "  stop                     Stop LAIS background service, unload models from VRAM,\n"
              << "                           and disable automatic PC boot launch.\n"
              << "  restart                  Restart the LAIS background service cleanly.\n"
              << "  status                   Display live VRAM/DRAM telemetry, service state,\n"
              << "                           PAKE admin state, engine readiness, and ~/lais usage.\n"
              << "  models | list            List all Standard & Uncensored models in models.json.\n"
              << "  install [target]         Download & install models ('all', 'standard', 'uncensored',\n"
              << "                           'eco', 'high', 'llm', 'coder', 'image', 'video', 'music',\n"
              << "                           'engines', or a specific model key/filename).\n"
              << "  uninstall <target>       Remove installed model(s) by key, category, safety, or 'all'.\n"
              << "  update                   Pull latest commits and recompile C++ engines in ~/lais.\n"
              << "  purge                    Evict OS file cache (POSIX_FADV_DONTNEED / EmptyWorkingSet)\n"
              << "                           and trim heap arenas immediately.\n"
              << "  reset-auth               Reset WebUI PAKE admin credentials in ~/lais/data/user.json\n"
              << "                           back to initial default (ID: admin, Password: admin).\n"
              << "  config [key] [val]       View or modify settings in ~/lais/configs/settings.json\n"
              << "                           (keys: port, host, tier, uncensored, idle_timeout, vram_reserve).\n"
              << "  history [clear]          Display or clear command history in ~/lais/history.log.\n"
              << "  logs [lines]             Show recent service & engine logs from ~/lais/logs.\n"
              << "  reset [--keep-models]    Remove service, cache, logs, outputs, data, and configs\n"
              << "                           (pass '--keep-models' to preserve ~/lais/models).\n"
              << "  clear                    Clear the terminal screen.\n"
              << "  help                     Show this command manual.\n"
              << "  exit | quit              Exit the interactive command loop.\n"
              << "==============================================================================\n\n";
}

bool execute_command(Config& cfg, const std::vector<std::string>& tokens, const char* argv0, bool& should_exit) {
    if (tokens.empty()) return true;
    std::string cmd = to_lower_ascii(tokens[0]);

    if (cmd.rfind("--", 0) == 0) cmd = cmd.substr(2);
    if (cmd == "-h") cmd = "help";

    if (cmd == "exit" || cmd == "quit" || cmd == "q") {
        should_exit = true;
        return true;
    }
    if (cmd == "clear" || cmd == "cls") {
        std::cout << "\033[2J\033[H" << std::flush;
        return true;
    }
    if (cmd == "help" || cmd == "?") {
        print_help();
        return true;
    }
    if (cmd == "status") {
        print_status(cfg);
        return true;
    }
    if (cmd == "models" || cmd == "list") {
        print_models_table(cfg);
        return true;
    }
    if (cmd == "reset-auth" || cmd == "passwd") {
        PakeAuthManager auth(cfg);
        if (auth.reset_to_default_admin()) {
            log_ok("Reset PAKE administrator credentials in " + cfg.user_file +
                   " back to initial default (ID: admin, Password: admin).");
            return true;
        }
        log_err("Failed to reset PAKE credentials in " + cfg.user_file);
        return false;
    }
    if (cmd == "purge") {
        if (is_service_running(cfg)) {
            std::string url = "http://127.0.0.1:" + std::to_string(cfg.bind_port) + "/api/purge-memory";
            run_cmd("curl --fail --silent -X POST " + shell_quote(url) + " >/dev/null 2>&1", true);
        }
        std::size_t n = purge_dram_and_pagecache(cfg.models_dir);
        log_ok("Evicted OS page cache for " + std::to_string(n) + " model files and trimmed heap arenas.");
        return true;
    }
    if (cmd == "setup" || cmd == "ai-only") {
        log_info("Starting LAIS environment & model setup in " + cfg.lais_dir + " ...");
        cfg.ensure_directories();
        cfg.sync_seed_configs(argv0);
        cfg.load_config_file();
        configure_wsl_memory_discipline();
        install_self_binary(cfg, argv0);
        extract_embedded_engines(cfg, argv0, true);
        PakeAuthManager auth(cfg);
        auth.ensure_user_file();
        if (!install_models_by_selector(cfg, "all")) return false;
        log_ok("LAIS setup complete! All Standard & Uncensored models + bundled engines are ready in ~/lais. Type 'start' to start the background service.");
        return true;
    }
    if (cmd == "start" || cmd == "enable-autostart") {
        return start_lais_service(cfg, argv0);
    }
    if (cmd == "stop") {
        return stop_lais_service(cfg);
    }
    if (cmd == "restart") {
        stop_lais_service(cfg);
        return start_lais_service(cfg, argv0);
    }
    if (cmd == "ensure-daemon") {
        if (!is_service_running(cfg)) {
            return start_lais_service(cfg, argv0);
        }
        return true;
    }
    if (cmd == "install") {
        std::string target = tokens.size() >= 2 ? tokens[1] : "all";
        for (std::size_t i = 2; i < tokens.size(); ++i) target += " " + tokens[i];
        extract_embedded_engines(cfg, argv0);
        return install_models_by_selector(cfg, target);
    }
    if (cmd == "uninstall" || cmd == "remove") {
        std::string target = tokens.size() >= 2 ? tokens[1] : "";
        for (std::size_t i = 2; i < tokens.size(); ++i) target += " " + tokens[i];
        return uninstall_models_by_selector(cfg, target);
    }
    if (cmd == "update") {
        log_info("Updating C++ inference engines in " + cfg.engines_dir + " ...");
        cfg.load_config_file();
        bool ok = build_cpp_ai_engines(cfg, true);
        if (ok && is_service_running(cfg)) {
            log_info("Restarting active LAIS service to apply updated engines...");
            stop_lais_service(cfg);
            start_lais_service(cfg, argv0);
        }
        return ok;
    }
    if (cmd == "reset") {
        bool keep_models = (tokens.size() >= 2 && (tokens[1] == "--keep-models" || tokens[1] == "keep-models"));
        stop_lais_service(cfg);
        std::error_code ec;

        fs::remove(fs::path(cfg.home_dir) / ".local/bin/lais", ec);
        fs::remove_all(cfg.bin_dir, ec);
        remove_bashrc_lais_path(cfg);
        if (can_run_sudo()) {
            std::string unit_name = get_systemd_unit_name();
            run_cmd("sudo rm -f /usr/local/bin/lais /usr/bin/lais /etc/sysctl.d/99-lais-memory.conf "
                    "/etc/systemd/system/" + shell_quote(unit_name) + " 2>/dev/null || true", true);
            run_cmd("sudo systemctl daemon-reload 2>/dev/null || true", true);
        }

        fs::remove_all(cfg.cache_dir, ec);
        fs::remove_all(cfg.logs_dir, ec);
        fs::remove_all(cfg.media_out_dir, ec);
        fs::remove_all(cfg.chats_dir, ec);
        fs::remove_all(cfg.data_dir, ec);
        fs::remove_all(cfg.engines_dir, ec);
        fs::remove_all(cfg.configs_dir, ec);
        fs::remove(cfg.history_file, ec);

        if (!keep_models) {
            fs::remove_all(cfg.models_dir, ec);
            fs::remove_all(cfg.lais_dir, ec);
            log_ok("LAIS reset complete: removed global 'lais' command, service, boot autostart, and ~/lais directory.");
        } else {
            log_ok("LAIS reset complete: removed global 'lais' command, service, engines, cache, chats, data, and configs "
                   "(preserved ~/lais/models).");
        }
        return true;
    }
    if (cmd == "config") {
        if (tokens.size() == 1) {
            std::cout << "\nCurrent " << cfg.config_file << " settings:\n"
                      << read_file_bytes(cfg.config_file)
                      << "\nExternal sources file : " << cfg.sources_file
                      << "\nModels catalog file   : " << cfg.models_file
                      << "\nPAKE user verifier    : " << cfg.user_file << "\n\n";
            return true;
        }
        if (tokens.size() >= 3) {
            std::string k = to_lower_ascii(tokens[1]);
            std::string v = tokens[2];
            if (k == "port" || k == "bind_port") {
                int p = std::atoi(v.c_str());
                if (p > 0 && p < 65536) cfg.bind_port = p;
                else { log_err("Invalid port number: " + v); return false; }
            } else if (k == "host" || k == "bind_host") {
                cfg.bind_host = v;
            } else if (k == "tier" || k == "default_tier") {
                std::string tl = to_lower_ascii(v);
                if (tl == "auto" || tl == "high" || tl == "eco") cfg.default_tier = tl;
                else { log_err("Tier must be 'auto', 'high', or 'eco'."); return false; }
            } else if (k == "uncensored" || k == "default_uncensored") {
                std::string bl = to_lower_ascii(v);
                cfg.default_uncensored = (bl == "1" || bl == "true" || bl == "on" || bl == "yes");
            } else if (k == "idle_timeout" || k == "idle_timeout_sec") {
                cfg.idle_timeout_sec = std::max(10L, std::atol(v.c_str()));
            } else if (k == "vram_reserve" || k == "vram_reserve_mb") {
                cfg.vram_reserve_mb = std::max(512L, std::atol(v.c_str()));
            } else if (k == "cuda_home") {
                cfg.cuda_home = v;
            } else {
                log_err("Unknown config key '" + k + "'. Valid keys: port, host, tier, uncensored, idle_timeout, vram_reserve, cuda_home");
                return false;
            }
            cfg.save_config_file();
            log_ok("Updated " + k + " = " + v + " in " + cfg.config_file);
            return true;
        }
        log_info("Usage: config [port|host|tier|uncensored|idle_timeout|vram_reserve|cuda_home] <value>");
        return true;
    }
    if (cmd == "history") {
        if (tokens.size() >= 2 && to_lower_ascii(tokens[1]) == "clear") {
            write_file_atomic(cfg.history_file, "", 0600);
            log_ok("Cleared command history in " + cfg.history_file);
            return true;
        }
        std::string h = read_file_bytes(cfg.history_file);
        if (h.empty()) {
            log_info("No command history recorded yet in " + cfg.history_file);
        } else {
            std::cout << "\n--- Command History (" << cfg.history_file << ") ---\n" << h << "\n";
        }
        return true;
    }
    if (cmd == "logs") {
        std::size_t nlines = (tokens.size() >= 2) ? static_cast<std::size_t>(std::max(5, std::atoi(tokens[1].c_str()))) : 25;
        std::error_code ec;
        bool found_any = false;
        if (fs::exists(cfg.logs_dir, ec)) {
            for (auto it = fs::directory_iterator(cfg.logs_dir, ec); it != fs::directory_iterator(); it.increment(ec)) {
                if (ec || !it->is_regular_file(ec) || it->path().extension() != ".log") continue;
                found_any = true;
                std::cout << "\n==> " << it->path().string() << " <==\n";
                std::ifstream ifs(it->path());
                std::vector<std::string> lines;
                std::string l;
                while (std::getline(ifs, l)) lines.push_back(std::move(l));
                std::size_t start = (lines.size() > nlines) ? (lines.size() - nlines) : 0;
                for (std::size_t i = start; i < lines.size(); ++i) {
                    std::cout << lines[i] << "\n";
                }
            }
        }
        if (!found_any) log_info("No log files present in " + cfg.logs_dir);
        else std::cout << "\n";
        return true;
    }

    log_err("Unknown command: '" + tokens[0] + "'. Type 'help' to see available commands.");
    return false;
}

} // namespace lais
