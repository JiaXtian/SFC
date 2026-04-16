#include "services/AuthService.h"

#include <nlohmann/json.hpp>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <array>
#include <chrono>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace sfc {

namespace {

std::string to_hex(const unsigned char* data, size_t len) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = kHex[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[data[i] & 0x0F];
    }
    return out;
}

std::vector<unsigned char> from_hex(const std::string& hex) {
    auto from_digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
        if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
        return -1;
    };

    if (hex.size() % 2 != 0) return {};
    std::vector<unsigned char> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = from_digit(hex[i]);
        const int lo = from_digit(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    return out;
}

std::string base64url_encode(const std::string& input) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i + 3 <= input.size()) {
        const uint32_t v = (static_cast<unsigned char>(input[i]) << 16) |
                           (static_cast<unsigned char>(input[i + 1]) << 8) |
                           (static_cast<unsigned char>(input[i + 2]));
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back(kTable[v & 0x3F]);
        i += 3;
    }

    const size_t rem = input.size() - i;
    if (rem == 1) {
        const uint32_t v = static_cast<unsigned char>(input[i]) << 16;
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const uint32_t v = (static_cast<unsigned char>(input[i]) << 16) |
                           (static_cast<unsigned char>(input[i + 1]) << 8);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back('=');
    }

    for (char& c : out) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

std::optional<std::string> base64url_decode(std::string input) {
    for (char& c : input) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (input.size() % 4 != 0) input.push_back('=');

    auto decode_char = [](char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+') return 62;
        if (ch == '/') return 63;
        if (ch == '=') return 0;
        return -1;
    };

    std::string out;
    out.reserve((input.size() / 4) * 3);
    for (size_t i = 0; i < input.size(); i += 4) {
        const int c0 = decode_char(input[i]);
        const int c1 = decode_char(input[i + 1]);
        const int c2 = decode_char(input[i + 2]);
        const int c3 = decode_char(input[i + 3]);
        if (c0 < 0 || c1 < 0 || c2 < 0 || c3 < 0) return std::nullopt;

        const uint32_t v = (static_cast<uint32_t>(c0) << 18) |
                           (static_cast<uint32_t>(c1) << 12) |
                           (static_cast<uint32_t>(c2) << 6) |
                           static_cast<uint32_t>(c3);
        out.push_back(static_cast<char>((v >> 16) & 0xFF));
        if (input[i + 2] != '=') out.push_back(static_cast<char>((v >> 8) & 0xFF));
        if (input[i + 3] != '=') out.push_back(static_cast<char>(v & 0xFF));
    }
    return out;
}

bool secure_equals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

std::string hmac_sha256(const std::string& secret, const std::string& data) {
    unsigned int out_len = 0;
    std::array<unsigned char, EVP_MAX_MD_SIZE> out{};
    HMAC(EVP_sha256(),
         secret.data(),
         static_cast<int>(secret.size()),
         reinterpret_cast<const unsigned char*>(data.data()),
         data.size(),
         out.data(),
         &out_len);
    return std::string(reinterpret_cast<const char*>(out.data()), out_len);
}

std::string random_salt_hex(size_t bytes_len = 16) {
    std::vector<unsigned char> bytes(bytes_len, 0);
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        std::random_device rd;
        for (auto& b : bytes) {
            b = static_cast<unsigned char>(rd() & 0xFF);
        }
    }
    return to_hex(bytes.data(), bytes.size());
}

int64_t now_unix_sec() {
    const auto now = std::chrono::system_clock::now();
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count());
}

}  // namespace

AuthService::AuthService(std::string jwt_secret, int token_expire_hours)
    : jwt_secret_(std::move(jwt_secret)),
      token_expire_hours_(token_expire_hours > 0 ? token_expire_hours : 24) {
    if (jwt_secret_.empty()) {
        throw std::runtime_error("JWT secret must not be empty");
    }
}

std::string AuthService::issue_token(int64_t user_id, const std::string& username, const std::string& role) const {
    const nlohmann::json header = {
        {"alg", "HS256"},
        {"typ", "JWT"},
    };

    const int64_t iat = now_unix_sec();
    const int64_t exp = iat + static_cast<int64_t>(token_expire_hours_) * 3600;
    const nlohmann::json payload = {
        {"uid", user_id},
        {"username", username},
        {"role", role},
        {"iat", iat},
        {"exp", exp},
    };

    const std::string header_b64 = base64url_encode(header.dump());
    const std::string payload_b64 = base64url_encode(payload.dump());
    const std::string signing_input = header_b64 + "." + payload_b64;
    const std::string signature = hmac_sha256(jwt_secret_, signing_input);
    const std::string signature_b64 = base64url_encode(signature);
    return signing_input + "." + signature_b64;
}

std::optional<JwtClaims> AuthService::verify_token(const std::string& token) const {
    const size_t first_dot = token.find('.');
    if (first_dot == std::string::npos) return std::nullopt;
    const size_t second_dot = token.find('.', first_dot + 1);
    if (second_dot == std::string::npos) return std::nullopt;

    const std::string header_b64 = token.substr(0, first_dot);
    const std::string payload_b64 = token.substr(first_dot + 1, second_dot - first_dot - 1);
    const std::string signature_b64 = token.substr(second_dot + 1);
    if (header_b64.empty() || payload_b64.empty() || signature_b64.empty()) return std::nullopt;

    const std::string signing_input = header_b64 + "." + payload_b64;
    const std::string expected_signature_b64 = base64url_encode(hmac_sha256(jwt_secret_, signing_input));
    if (!secure_equals(signature_b64, expected_signature_b64)) return std::nullopt;

    const auto header_decoded = base64url_decode(header_b64);
    const auto payload_decoded = base64url_decode(payload_b64);
    if (!header_decoded || !payload_decoded) return std::nullopt;

    nlohmann::json header_json;
    nlohmann::json payload_json;
    try {
        header_json = nlohmann::json::parse(*header_decoded);
        payload_json = nlohmann::json::parse(*payload_decoded);
    } catch (...) {
        return std::nullopt;
    }

    if (!header_json.is_object() || header_json.value("alg", "") != "HS256") return std::nullopt;
    if (!payload_json.is_object()) return std::nullopt;

    JwtClaims claims;
    claims.user_id = payload_json.value("uid", static_cast<int64_t>(0));
    claims.username = payload_json.value("username", "");
    claims.role = payload_json.value("role", "");
    claims.issued_at = payload_json.value("iat", static_cast<int64_t>(0));
    claims.expires_at = payload_json.value("exp", static_cast<int64_t>(0));

    if (claims.user_id <= 0 || claims.username.empty() || claims.role.empty()) return std::nullopt;
    if (claims.expires_at <= now_unix_sec()) return std::nullopt;
    return claims;
}

std::string AuthService::hash_password(const std::string& plain_password) {
    const std::string salt = random_salt_hex(16);
    const std::string salted = salt + ":" + plain_password;
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(salted.data()), salted.size(), digest.data());
    return salt + "$" + to_hex(digest.data(), digest.size());
}

bool AuthService::verify_password(const std::string& plain_password, const std::string& stored_hash) {
    const size_t pos = stored_hash.find('$');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= stored_hash.size()) return false;
    const std::string salt = stored_hash.substr(0, pos);
    const std::string expected_hex = stored_hash.substr(pos + 1);
    if (from_hex(expected_hex).empty()) return false;

    const std::string salted = salt + ":" + plain_password;
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(salted.data()), salted.size(), digest.data());
    const std::string computed_hex = to_hex(digest.data(), digest.size());
    return secure_equals(expected_hex, computed_hex);
}

}  // namespace sfc
