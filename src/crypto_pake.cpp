// ==============================================================================
// LAIS (Local AI Studio) v3.6.0 - SHA-256 / HMAC / PBKDF2 / PAKE Implementation
// SPDX-License-Identifier: MIT
// ==============================================================================
#include "crypto_pake.hpp"

namespace lais {

namespace {

inline constexpr uint32_t kSha256K[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

inline uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32U - n));
}

struct Sha256Ctx {
    uint32_t state[8];
    uint64_t bitlen = 0;
    uint8_t  buffer[64];
    std::size_t buflen = 0;

    Sha256Ctx() {
        state[0] = 0x6a09e667UL;
        state[1] = 0xbb67ae85UL;
        state[2] = 0x3c6ef372UL;
        state[3] = 0xa54ff53aUL;
        state[4] = 0x510e527fUL;
        state[5] = 0x9b05688cUL;
        state[6] = 0x1f83d9abUL;
        state[7] = 0x5be0cd19UL;
    }

    void transform(const uint8_t data[64]) {
        uint32_t m[64];
        for (int i = 0; i < 16; ++i) {
            m[i] = (static_cast<uint32_t>(data[i * 4]) << 24) |
                   (static_cast<uint32_t>(data[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(data[i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(data[i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr32(m[i - 15], 7) ^ rotr32(m[i - 15], 18) ^ (m[i - 15] >> 3);
            uint32_t s1 = rotr32(m[i - 2], 17) ^ rotr32(m[i - 2], 19) ^ (m[i - 2] >> 10);
            m[i] = m[i - 16] + s0 + m[i - 7] + s1;
        }

        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = h + S1 + ch + kSha256K[i] + m[i];
            uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    void update(const uint8_t* data, std::size_t len) {
        for (std::size_t i = 0; i < len; ++i) {
            buffer[buflen++] = data[i];
            if (buflen == 64) {
                transform(buffer);
                bitlen += 512;
                buflen = 0;
            }
        }
    }

    Sha256Digest final() {
        std::size_t i = buflen;
        if (buflen < 56) {
            buffer[i++] = 0x80;
            while (i < 56) buffer[i++] = 0x00;
        } else {
            buffer[i++] = 0x80;
            while (i < 64) buffer[i++] = 0x00;
            transform(buffer);
            std::memset(buffer, 0, 56);
        }

        bitlen += static_cast<uint64_t>(buflen) * 8ULL;
        for (int j = 0; j < 8; ++j) {
            buffer[63 - j] = static_cast<uint8_t>((bitlen >> (j * 8)) & 0xFFU);
        }
        transform(buffer);

        Sha256Digest out{};
        for (int j = 0; j < 8; ++j) {
            out[j * 4]     = static_cast<uint8_t>((state[j] >> 24) & 0xFFU);
            out[j * 4 + 1] = static_cast<uint8_t>((state[j] >> 16) & 0xFFU);
            out[j * 4 + 2] = static_cast<uint8_t>((state[j] >> 8) & 0xFFU);
            out[j * 4 + 3] = static_cast<uint8_t>(state[j] & 0xFFU);
        }
        return out;
    }
};

} // namespace

Sha256Digest sha256_bytes(std::string_view data) {
    Sha256Ctx ctx;
    ctx.update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    return ctx.final();
}

std::string bytes_to_hex(const uint8_t* data, std::size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0x0FU]);
        out.push_back(kHex[data[i] & 0x0FU]);
    }
    return out;
}

std::vector<uint8_t> hex_to_bytes(std::string_view hex) {
    std::vector<uint8_t> out;
    if (hex.size() % 2 != 0) return out;
    out.reserve(hex.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        int hi = nib(hex[i]);
        int lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::string sha256_hex(std::string_view data) {
    Sha256Digest d = sha256_bytes(data);
    return bytes_to_hex(d.data(), d.size());
}

Sha256Digest hmac_sha256_bytes(std::string_view key, std::string_view msg) {
    uint8_t k_block[64];
    std::memset(k_block, 0, sizeof(k_block));
    if (key.size() > 64) {
        Sha256Digest kh = sha256_bytes(key);
        std::memcpy(k_block, kh.data(), kh.size());
    } else if (!key.empty()) {
        std::memcpy(k_block, key.data(), key.size());
    }

    uint8_t o_key_pad[64];
    uint8_t i_key_pad[64];
    for (int i = 0; i < 64; ++i) {
        o_key_pad[i] = static_cast<uint8_t>(k_block[i] ^ 0x5cU);
        i_key_pad[i] = static_cast<uint8_t>(k_block[i] ^ 0x36U);
    }

    Sha256Ctx inner;
    inner.update(i_key_pad, 64);
    inner.update(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    Sha256Digest inner_hash = inner.final();

    Sha256Ctx outer;
    outer.update(o_key_pad, 64);
    outer.update(inner_hash.data(), inner_hash.size());
    return outer.final();
}

std::string hmac_sha256_hex(std::string_view key, std::string_view msg) {
    Sha256Digest d = hmac_sha256_bytes(key, msg);
    return bytes_to_hex(d.data(), d.size());
}

Sha256Digest pbkdf2_hmac_sha256(std::string_view password, std::string_view salt, int iterations) {
    int iters = std::max(1, iterations);
    std::string salt_block(salt);
    salt_block.push_back('\x00');
    salt_block.push_back('\x00');
    salt_block.push_back('\x00');
    salt_block.push_back('\x01');

    Sha256Digest u = hmac_sha256_bytes(password, salt_block);
    Sha256Digest t = u;
    for (int i = 1; i < iters; ++i) {
        u = hmac_sha256_bytes(
            password,
            std::string_view(reinterpret_cast<const char*>(u.data()), u.size()));
        for (std::size_t b = 0; b < 32; ++b) {
            t[b] ^= u[b];
        }
    }
    return t;
}

std::string random_hex(std::size_t num_bytes) {
    std::vector<uint8_t> buf(num_bytes, 0);
    std::random_device rd;
    for (std::size_t i = 0; i < num_bytes; ++i) {
        buf[i] = static_cast<uint8_t>(rd() & 0xFFU);
    }
    return bytes_to_hex(buf.data(), buf.size());
}

bool constant_time_equals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    uint8_t diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

// ------------------------------------------------------------------------------
// PakeAuthManager Implementation
// ------------------------------------------------------------------------------
PakeAuthManager::PakeAuthManager(const Config& cfg) : cfg_(cfg) {
    ensure_user_file();
    std::lock_guard<std::mutex> lk(mu_);
    load_sessions_locked();
}

void PakeAuthManager::compute_pake_verifier(
    std::string_view password,
    std::string_view salt_hex,
    int iterations,
    std::string& out_stored_key_hex,
    std::string& out_server_key_hex) {

    Sha256Digest salted = pbkdf2_hmac_sha256(password, salt_hex, iterations);
    std::string_view salted_sv(reinterpret_cast<const char*>(salted.data()), salted.size());

    Sha256Digest client_key = hmac_sha256_bytes(salted_sv, "LAIS-PAKE-Client-Key");
    Sha256Digest stored_key = sha256_bytes(
        std::string_view(reinterpret_cast<const char*>(client_key.data()), client_key.size()));
    Sha256Digest server_key = hmac_sha256_bytes(salted_sv, "LAIS-PAKE-Server-Key");

    out_stored_key_hex = bytes_to_hex(stored_key.data(), stored_key.size());
    out_server_key_hex = bytes_to_hex(server_key.data(), server_key.size());
}

void PakeAuthManager::ensure_user_file() {
    std::error_code ec;
    fs::create_directories(cfg_.data_dir, ec);
    if (fs::exists(cfg_.user_file, ec) && fs::file_size(cfg_.user_file, ec) > 20) {
        UserVerifierRecord rec = load_user();
        if (!rec.username.empty() && rec.stored_key_hex.size() == 64 && rec.server_key_hex.size() == 64) {
            return;
        }
    }
    reset_to_default_admin();
}

bool PakeAuthManager::reset_to_default_admin() {
    UserVerifierRecord rec;
    rec.username             = "admin";
    rec.salt_hex             = "e4a9710c5b823f91d0468ca2197fe354";
    rec.iterations           = 10000;
    rec.must_change_password = true;
    rec.updated_at           = current_timestamp_iso();
    compute_pake_verifier("admin", rec.salt_hex, rec.iterations, rec.stored_key_hex, rec.server_key_hex);
    {
        std::lock_guard<std::mutex> lk(mu_);
        sessions_.clear();
        save_sessions_locked();
    }
    return save_user(rec);
}

UserVerifierRecord PakeAuthManager::load_user() const {
    UserVerifierRecord rec;
    std::string content = read_file_bytes(cfg_.user_file);
    if (content.empty()) return rec;
    json::Value j = json::parse(content);
    if (!j.is_object()) return rec;
    rec.username             = j.get_string("username", "admin");
    rec.salt_hex             = j.get_string("salt_hex", rec.salt_hex);
    rec.iterations           = static_cast<int>(j.get_int("iterations", 10000));
    rec.stored_key_hex       = to_lower_ascii(j.get_string("stored_key_hex", ""));
    rec.server_key_hex       = to_lower_ascii(j.get_string("server_key_hex", ""));
    rec.must_change_password = j.get_bool("must_change_password", true);
    rec.updated_at           = j.get_string("updated_at", "");
    return rec;
}

bool PakeAuthManager::save_user(const UserVerifierRecord& rec) {
    std::ostringstream o;
    o << "{\n"
      << "  \"protocol\": \"LAIS-SCRAM-PAKE-SHA256-v1\",\n"
      << "  \"username\": \"" << json::escape(rec.username) << "\",\n"
      << "  \"salt_hex\": \"" << json::escape(rec.salt_hex) << "\",\n"
      << "  \"iterations\": " << rec.iterations << ",\n"
      << "  \"stored_key_hex\": \"" << json::escape(rec.stored_key_hex) << "\",\n"
      << "  \"server_key_hex\": \"" << json::escape(rec.server_key_hex) << "\",\n"
      << "  \"must_change_password\": " << (rec.must_change_password ? "true" : "false") << ",\n"
      << "  \"updated_at\": \"" << json::escape(rec.updated_at.empty() ? current_timestamp_iso() : rec.updated_at) << "\"\n"
      << "}\n";
    return write_file_atomic(cfg_.user_file, o.str(), 0600);
}

void PakeAuthManager::load_sessions_locked() {
    sessions_.clear();
    std::string content = read_file_bytes(cfg_.sessions_file);
    if (content.empty()) return;
    json::Value j = json::parse(content);
    if (!j.is_object()) return;
    std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    if (const json::Value* arr = j.get("sessions"); arr && arr->is_array()) {
        for (const auto& item : std::get<json::Array>(arr->v)) {
            if (!item.is_object()) continue;
            std::string tok = item.get_string("token", "");
            std::int64_t exp = item.get_int("expires", 0);
            if (!tok.empty() && exp > now) {
                SessionEntry se;
                se.username          = item.get_string("username", "admin");
                se.expires_epoch_sec = exp;
                se.auto_login        = item.get_bool("auto_login", false);
                sessions_[tok]       = se;
            }
        }
    }
}

void PakeAuthManager::save_sessions_locked() const {
    std::ostringstream o;
    o << "{\n  \"sessions\": [\n";
    bool first = true;
    for (const auto& [tok, se] : sessions_) {
        if (!first) o << ",\n";
        first = false;
        o << "    {\"token\":\"" << json::escape(tok) << "\","
          << "\"username\":\"" << json::escape(se.username) << "\","
          << "\"expires\":" << se.expires_epoch_sec << ","
          << "\"auto_login\":" << (se.auto_login ? "true" : "false") << "}";
    }
    o << "\n  ]\n}\n";
    write_file_atomic(cfg_.sessions_file, o.str(), 0600);
}

void PakeAuthManager::prune_expired_locked() {
    std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    for (auto it = challenges_.begin(); it != challenges_.end(); ) {
        if (it->second.expires_epoch_sec <= now) it = challenges_.erase(it);
        else ++it;
    }
    bool changed = false;
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if (it->second.expires_epoch_sec <= now) {
            it = sessions_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) save_sessions_locked();
}

bool PakeAuthManager::is_session_valid(std::string_view token) {
    if (token.empty()) return false;
    std::lock_guard<std::mutex> lk(mu_);
    prune_expired_locked();
    return sessions_.find(std::string(token)) != sessions_.end();
}

void PakeAuthManager::invalidate_session(std::string_view token) {
    if (token.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (sessions_.erase(std::string(token)) > 0) {
        save_sessions_locked();
    }
}

std::string PakeAuthManager::get_auth_status_json(std::string_view token) {
    ensure_user_file();
    UserVerifierRecord user = load_user();
    bool authed = is_session_valid(token);
    std::ostringstream o;
    o << "{"
      << "\"ok\":true,"
      << "\"authenticated\":" << (authed ? "true" : "false") << ","
      << "\"username\":\"" << json::escape(user.username) << "\","
      << "\"must_change_password\":" << (user.must_change_password ? "true" : "false")
      << "}";
    return o.str();
}

std::string PakeAuthManager::create_challenge_json(std::string_view username, std::string_view client_nonce) {
    ensure_user_file();
    UserVerifierRecord user = load_user();
    std::string req_user = trim_copy(username);
    if (req_user.empty() || client_nonce.size() < 16) {
        return "{\"ok\":false,\"error\":\"Invalid PAKE initialization parameters.\"}";
    }
    if (req_user != user.username) {
        return "{\"ok\":false,\"error\":\"Invalid administrator ID or password.\"}";
    }

    std::string cid = random_hex(16);
    std::string s_nonce = random_hex(24);
    ChallengeState st;
    st.username          = user.username;
    st.client_nonce      = std::string(client_nonce);
    st.server_nonce      = s_nonce;
    st.salt_hex          = user.salt_hex;
    st.iterations        = user.iterations;
    st.expires_epoch_sec = static_cast<std::int64_t>(std::time(nullptr)) + 120;

    {
        std::lock_guard<std::mutex> lk(mu_);
        prune_expired_locked();
        challenges_[cid] = st;
    }

    std::ostringstream o;
    o << "{"
      << "\"ok\":true,"
      << "\"challenge_id\":\"" << json::escape(cid) << "\","
      << "\"salt_hex\":\"" << json::escape(user.salt_hex) << "\","
      << "\"iterations\":" << user.iterations << ","
      << "\"server_nonce\":\"" << json::escape(s_nonce) << "\""
      << "}";
    return o.str();
}

std::string PakeAuthManager::verify_challenge_json(
    std::string_view challenge_id,
    std::string_view username,
    std::string_view client_proof_hex,
    bool auto_login,
    std::string& out_session_token) {

    out_session_token.clear();
    ensure_user_file();
    UserVerifierRecord user = load_user();

    ChallengeState ch;
    {
        std::lock_guard<std::mutex> lk(mu_);
        prune_expired_locked();
        auto it = challenges_.find(std::string(challenge_id));
        if (it == challenges_.end()) {
            return "{\"ok\":false,\"error\":\"PAKE challenge expired or invalid. Please try again.\"}";
        }
        ch = it->second;
        challenges_.erase(it);
    }

    if (username != user.username || username != ch.username) {
        return "{\"ok\":false,\"error\":\"Invalid administrator ID or password.\"}";
    }

    std::vector<uint8_t> stored_key = hex_to_bytes(user.stored_key_hex);
    std::vector<uint8_t> server_key = hex_to_bytes(user.server_key_hex);
    std::vector<uint8_t> client_proof = hex_to_bytes(client_proof_hex);
    if (stored_key.size() != 32 || server_key.size() != 32 || client_proof.size() != 32) {
        return "{\"ok\":false,\"error\":\"Malformed PAKE cryptographic proof.\"}";
    }

    std::string auth_msg = std::string(username) + ":" + ch.salt_hex + ":" +
                           ch.client_nonce + ":" + ch.server_nonce;
    Sha256Digest client_sig = hmac_sha256_bytes(
        std::string_view(reinterpret_cast<const char*>(stored_key.data()), stored_key.size()),
        auth_msg);

    uint8_t recovered_client_key[32];
    for (int i = 0; i < 32; ++i) {
        recovered_client_key[i] = static_cast<uint8_t>(client_proof[i] ^ client_sig[i]);
    }
    std::string candidate_stored_hex = sha256_hex(
        std::string_view(reinterpret_cast<const char*>(recovered_client_key), 32));

    if (!constant_time_equals(candidate_stored_hex, user.stored_key_hex)) {
        return "{\"ok\":false,\"error\":\"Invalid administrator ID or password.\"}";
    }

    std::string server_sig_hex = hmac_sha256_hex(
        std::string_view(reinterpret_cast<const char*>(server_key.data()), server_key.size()),
        auth_msg);

    std::string token = random_hex(32);
    std::int64_t ttl = auto_login ? (30LL * 24 * 3600) : (24LL * 3600);
    {
        std::lock_guard<std::mutex> lk(mu_);
        SessionEntry se;
        se.username          = user.username;
        se.expires_epoch_sec = static_cast<std::int64_t>(std::time(nullptr)) + ttl;
        se.auto_login        = auto_login;
        sessions_[token]     = se;
        save_sessions_locked();
    }
    out_session_token = token;

    std::ostringstream o;
    o << "{"
      << "\"ok\":true,"
      << "\"username\":\"" << json::escape(user.username) << "\","
      << "\"must_change_password\":" << (user.must_change_password ? "true" : "false") << ","
      << "\"auto_login\":" << (auto_login ? "true" : "false") << ","
      << "\"server_sig\":\"" << json::escape(server_sig_hex) << "\""
      << "}";
    return o.str();
}

std::string PakeAuthManager::change_credentials_json(
    std::string_view session_token,
    std::string_view challenge_id,
    std::string_view current_client_proof_hex,
    std::string_view new_username,
    std::string_view new_salt_hex,
    int new_iterations,
    std::string_view new_stored_key_hex,
    std::string_view new_server_key_hex) {

    if (!is_session_valid(session_token)) {
        return "{\"ok\":false,\"error\":\"Unauthorized session. Please log in again.\"}";
    }

    ensure_user_file();
    UserVerifierRecord user = load_user();

    // Verify current password via PAKE proof
    ChallengeState ch;
    {
        std::lock_guard<std::mutex> lk(mu_);
        prune_expired_locked();
        auto it = challenges_.find(std::string(challenge_id));
        if (it == challenges_.end()) {
            return "{\"ok\":false,\"error\":\"PAKE verification challenge expired. Please try again.\"}";
        }
        ch = it->second;
        challenges_.erase(it);
    }

    std::vector<uint8_t> stored_key = hex_to_bytes(user.stored_key_hex);
    std::vector<uint8_t> client_proof = hex_to_bytes(current_client_proof_hex);
    if (stored_key.size() != 32 || client_proof.size() != 32) {
        return "{\"ok\":false,\"error\":\"Invalid PAKE proof for current password.\"}";
    }

    std::string auth_msg = user.username + ":" + ch.salt_hex + ":" +
                           ch.client_nonce + ":" + ch.server_nonce;
    Sha256Digest client_sig = hmac_sha256_bytes(
        std::string_view(reinterpret_cast<const char*>(stored_key.data()), stored_key.size()),
        auth_msg);

    uint8_t recovered_client_key[32];
    for (int i = 0; i < 32; ++i) {
        recovered_client_key[i] = static_cast<uint8_t>(client_proof[i] ^ client_sig[i]);
    }
    std::string candidate_stored_hex = sha256_hex(
        std::string_view(reinterpret_cast<const char*>(recovered_client_key), 32));
    if (!constant_time_equals(candidate_stored_hex, user.stored_key_hex)) {
        return "{\"ok\":false,\"error\":\"Current password is incorrect.\"}";
    }

    std::string clean_user = trim_copy(new_username);
    std::string clean_salt = to_lower_ascii(trim_copy(new_salt_hex));
    std::string clean_stored = to_lower_ascii(trim_copy(new_stored_key_hex));
    std::string clean_server = to_lower_ascii(trim_copy(new_server_key_hex));

    if (clean_user.empty() || clean_salt.size() < 16 ||
        clean_stored.size() != 64 || clean_server.size() != 64) {
        return "{\"ok\":false,\"error\":\"Invalid new administrator credentials or PAKE verifier.\"}";
    }

    UserVerifierRecord updated;
    updated.username             = clean_user;
    updated.salt_hex             = clean_salt;
    updated.iterations           = std::max(1000, new_iterations);
    updated.stored_key_hex       = clean_stored;
    updated.server_key_hex       = clean_server;
    updated.must_change_password = false;
    updated.updated_at           = current_timestamp_iso();

    if (!save_user(updated)) {
        return "{\"ok\":false,\"error\":\"Failed to write ~/lais/data/user.json.\"}";
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [tok, se] : sessions_) {
            se.username = updated.username;
        }
        save_sessions_locked();
    }

    std::ostringstream o;
    o << "{"
      << "\"ok\":true,"
      << "\"username\":\"" << json::escape(updated.username) << "\","
      << "\"must_change_password\":false"
      << "}";
    return o.str();
}

} // namespace lais
