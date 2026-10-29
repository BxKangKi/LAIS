// ==============================================================================
// LAIS (Local AI Studio) v3.6.0 - PAKE Authentication & Session Manager
// ------------------------------------------------------------------------------
// Implements RFC 5802 / SCRAM-SHA-256 Password-Authenticated Key Exchange (PAKE):
//   - Only cryptographic verifiers (salt, iterations, stored_key, server_key)
//     are persisted in ~/lais/data/user.json (NEVER plaintext passwords).
//   - Browser cookies store only an opaque 256-bit random session token and
//     UI theme preferences (NEVER credentials).
//   - Default initial credentials: ID = "admin", Password = "admin",
//     with mandatory password change enforced on first login.
// SPDX-License-Identifier: MIT
// ==============================================================================
#pragma once

#include "common.hpp"
#include <array>

namespace lais {

using Sha256Digest = std::array<uint8_t, 32>;

Sha256Digest sha256_bytes(std::string_view data);
std::string sha256_hex(std::string_view data);
Sha256Digest hmac_sha256_bytes(std::string_view key, std::string_view msg);
std::string hmac_sha256_hex(std::string_view key, std::string_view msg);
Sha256Digest pbkdf2_hmac_sha256(std::string_view password, std::string_view salt, int iterations);

std::string bytes_to_hex(const uint8_t* data, std::size_t len);
std::vector<uint8_t> hex_to_bytes(std::string_view hex);
std::string random_hex(std::size_t num_bytes);
bool constant_time_equals(std::string_view a, std::string_view b);

struct UserVerifierRecord {
    std::string username              = "admin";
    std::string salt_hex              = "e4a9710c5b823f91d0468ca2197fe354";
    int         iterations            = 10000;
    std::string stored_key_hex;
    std::string server_key_hex;
    bool        must_change_password  = true;
    std::string updated_at;
};

class PakeAuthManager {
public:
    explicit PakeAuthManager(const Config& cfg);

    void ensure_user_file();
    bool reset_to_default_admin();

    UserVerifierRecord load_user() const;
    bool save_user(const UserVerifierRecord& rec);

    bool is_session_valid(std::string_view token);
    void invalidate_session(std::string_view token);

    std::string get_auth_status_json(std::string_view token);
    std::string create_challenge_json(std::string_view username, std::string_view client_nonce);
    std::string verify_challenge_json(
        std::string_view challenge_id,
        std::string_view username,
        std::string_view client_proof_hex,
        bool auto_login,
        std::string& out_session_token);
    std::string change_credentials_json(
        std::string_view session_token,
        std::string_view challenge_id,
        std::string_view current_client_proof_hex,
        std::string_view new_username,
        std::string_view new_salt_hex,
        int new_iterations,
        std::string_view new_stored_key_hex,
        std::string_view new_server_key_hex);

    static void compute_pake_verifier(
        std::string_view password,
        std::string_view salt_hex,
        int iterations,
        std::string& out_stored_key_hex,
        std::string& out_server_key_hex);

private:
    struct ChallengeState {
        std::string username;
        std::string client_nonce;
        std::string server_nonce;
        std::string salt_hex;
        int         iterations = 10000;
        std::int64_t expires_epoch_sec = 0;
    };

    struct SessionEntry {
        std::string username;
        std::int64_t expires_epoch_sec = 0;
        bool auto_login = false;
    };

    Config cfg_;
    mutable std::mutex mu_;
    std::map<std::string, ChallengeState> challenges_;
    std::map<std::string, SessionEntry>   sessions_;

    void load_sessions_locked();
    void save_sessions_locked() const;
    void prune_expired_locked();
};

} // namespace lais
