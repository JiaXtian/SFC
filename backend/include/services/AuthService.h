#pragma once

#include <optional>
#include <string>

namespace sfc {

struct JwtClaims {
    int64_t user_id = 0;
    std::string username;
    std::string role;
    int64_t issued_at = 0;
    int64_t expires_at = 0;

    bool is_admin() const { return role == "admin"; }
};

class AuthService {
  public:
    AuthService(std::string jwt_secret, int token_expire_hours);

    std::string issue_token(int64_t user_id, const std::string& username, const std::string& role) const;
    std::optional<JwtClaims> verify_token(const std::string& token) const;
    int token_expire_hours() const { return token_expire_hours_; }

    static std::string hash_password(const std::string& plain_password);
    static bool verify_password(const std::string& plain_password, const std::string& stored_hash);

  private:
    std::string jwt_secret_;
    int token_expire_hours_ = 24;
};

}  // namespace sfc
