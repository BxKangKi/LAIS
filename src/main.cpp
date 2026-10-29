// ==============================================================================
// LAIS (Local AI Studio) v3.6.0 - Interactive Terminal Shell & Main Entrypoint
// SPDX-License-Identifier: MIT
// ==============================================================================
#include "common.hpp"
#include "crypto_pake.hpp"
#include "line_editor.hpp"

namespace lais {

static int run_interactive_shell(Config& cfg, const char* argv0) {
    g_in_interactive_repl.store(true, std::memory_order_relaxed);
    std::cout << "\033[1;36m==============================================================================\033[0m\n"
              << "\033[1;36m LAIS (Local AI Studio) v" << LAIS_VERSION << " - Cross-Platform C++20 Multimodal AI Studio\033[0m\n"
              << "\033[1;36m==============================================================================\033[0m\n"
              << "  Unified Directory : " << cfg.lais_dir << " (models, engines, configs, data, chats)\n"
              << "  Quick Commands    : setup | start | stop | status | models | install <model>\n"
              << "                      uninstall <model> | reset-auth | purge | clear | help | exit\n"
              << "------------------------------------------------------------------------------\n";

    std::vector<std::string> mem_history = cfg.load_command_history_lines();
    std::string line;
    while (read_interactive_line(line, mem_history)) {
        std::string trimmed = trim_copy(line);
        if (trimmed.empty()) continue;

        cfg.append_command_history(trimmed);
        mem_history.push_back(trimmed);

        auto tokens = split_command_tokens(trimmed);
        bool should_exit = false;
        execute_command(cfg, tokens, argv0, should_exit);
        if (should_exit) break;
    }
    g_in_interactive_repl.store(false, std::memory_order_relaxed);
    return 0;
}

} // namespace lais

int main(int argc, char** argv) {
    using namespace lais;

#ifdef _WIN32
    WSADATA wsa{};
    (void)::WSAStartup(MAKEWORD(2, 2), &wsa);
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);
    HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (::GetConsoleMode(hOut, &mode)) {
            ::SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
#else
    std::signal(SIGPIPE, SIG_IGN);
    if (::geteuid() == 0) {
        log_err("Please run LAIS as your normal user (without sudo); sudo will be requested only when needed.");
        return 1;
    }
#endif
    std::signal(SIGINT,  handle_termination_signal);
    std::signal(SIGTERM, handle_termination_signal);

    Config cfg = Config::from_env(argv[0]);
    PakeAuthManager auth_init(cfg);
    auth_init.ensure_user_file();

#ifdef _WIN32
    {
        const char* old_path = std::getenv("PATH");
        std::string new_path = cfg.bin_dir + ";" + (old_path ? old_path : "");
        set_env_var("PATH", new_path.c_str());
    }
#else
    {
        const char* old_path = std::getenv("PATH");
        std::string new_path = cfg.bin_dir + ":/usr/local/cuda/bin:/usr/lib/wsl/lib:" + (old_path ? old_path : "/usr/bin:/bin");
        set_env_var("PATH", new_path.c_str());

        const char* old_ld = std::getenv("LD_LIBRARY_PATH");
        std::string new_ld = cfg.bin_dir + ":/usr/lib/wsl/lib:/usr/local/cuda/lib64" +
                             (old_ld && *old_ld ? (std::string(":") + old_ld) : "");
        set_env_var("LD_LIBRARY_PATH", new_ld.c_str());
    }
#endif
    if (argc < 2 || to_lower_ascii(argv[1]) != "reset") {
        sync_existing_global_binaries(cfg, argv[0]);
        extract_embedded_engines(cfg, argv[0]);
    }

    if (argc == 1) {
        return run_interactive_shell(cfg, argv[0]);
    }

    bool do_serve = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--serve" || arg == "serve") {
            do_serve = true;
        } else if (arg == "--host" && i + 1 < argc) {
            cfg.bind_host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            int p = std::atoi(argv[++i]);
            if (p > 0 && p < 65536) cfg.bind_port = p;
        }
    }

    if (do_serve) {
        cfg.append_command_history("serve --host " + cfg.bind_host + " --port " + std::to_string(cfg.bind_port));
        InferenceCoordinator coordinator(cfg);
        return run_http_server_forever(cfg, coordinator) ? 0 : 1;
    }

    std::vector<std::string> tokens;
    std::ostringstream joined;
    for (int i = 1; i < argc; ++i) {
        tokens.emplace_back(argv[i]);
        if (i > 1) joined << " ";
        joined << argv[i];
    }
    cfg.append_command_history(joined.str());
    bool should_exit = false;
    return execute_command(cfg, tokens, argv[0], should_exit) ? 0 : 1;
}
