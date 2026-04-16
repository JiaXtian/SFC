#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sfc {

struct UserRecord {
    int64_t id = 0;
    std::string username;
    std::string role;
    std::string password_hash;
    std::string created_at;
    std::string updated_at;
};

struct UserDBConfig {
    std::string container_name = "sfc-mysql";
    std::string database = "sfc_runtime";
    std::string user = "sfc";
    std::string password = "sfc123456";
};

class UserService {
  public:
    explicit UserService(UserDBConfig config);

    bool init_schema();
    bool seed_default_accounts();

    std::optional<UserRecord> find_by_username(const std::string& username);
    std::optional<UserRecord> find_by_id(int64_t id);
    std::vector<UserRecord> list_users();

    bool create_user(const std::string& username,
                     const std::string& plain_password,
                     const std::string& role,
                     std::string* error_message = nullptr,
                     int64_t* inserted_id = nullptr);

    bool update_user(int64_t id,
                     const std::optional<std::string>& username,
                     const std::optional<std::string>& plain_password,
                     const std::optional<std::string>& role,
                     std::string* error_message = nullptr);

    bool delete_user(int64_t id, std::string* error_message = nullptr);

  private:
    bool exec_sql(const std::string& sql, std::string* stdout_out = nullptr, std::string* stderr_out = nullptr);
    static std::string sql_quote(const std::string& value);
    static std::vector<std::string> split_tab_line(const std::string& line);
    static std::vector<std::string> split_lines(const std::string& text);
    static bool is_valid_role(const std::string& role);
    bool ensure_can_demote_or_delete_admin(int64_t target_user_id, std::string* error_message);

    UserDBConfig config_;
    std::mutex exec_mutex_;
};

}  // namespace sfc
