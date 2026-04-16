#include "services/UserService.h"

#include "services/AuthService.h"

#include <spdlog/spdlog.h>

#include <array>
#include <sstream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sfc {

namespace {

std::string read_all_from_fd(int fd) {
    std::string out;
    std::array<char, 4096> buf{};
    while (true) {
        const ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n <= 0) break;
        out.append(buf.data(), static_cast<size_t>(n));
    }
    return out;
}

std::optional<UserRecord> parse_user_row(const std::vector<std::string>& fields, bool include_password_hash) {
    try {
        UserRecord u;
        u.id = std::stoll(fields.at(0));
        u.username = fields.at(1);
        u.role = fields.at(2);
        if (include_password_hash) {
            u.password_hash = fields.at(3);
            u.created_at = fields.at(4);
            u.updated_at = fields.at(5);
        } else {
            u.created_at = fields.at(3);
            u.updated_at = fields.at(4);
        }
        return u;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

UserService::UserService(UserDBConfig config)
    : config_(std::move(config)) {}

bool UserService::init_schema() {
    const std::string sql =
        "CREATE TABLE IF NOT EXISTS users ("
        "id BIGINT PRIMARY KEY AUTO_INCREMENT,"
        "username VARCHAR(64) NOT NULL UNIQUE,"
        "password_hash VARCHAR(255) NOT NULL,"
        "role VARCHAR(16) NOT NULL DEFAULT 'user',"
        "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci";
    std::string stderr_out;
    if (!exec_sql(sql, nullptr, &stderr_out)) {
        spdlog::error("init users table failed: {}", stderr_out);
        return false;
    }
    return true;
}

bool UserService::seed_default_accounts() {
    std::string stderr_out;
    const std::string admin_sql =
        "INSERT INTO users (username, password_hash, role) VALUES (" +
        sql_quote("admin") + ", " + sql_quote(AuthService::hash_password("123456")) + ", 'admin') "
        "ON DUPLICATE KEY UPDATE password_hash = VALUES(password_hash), role = VALUES(role)";
    if (!exec_sql(admin_sql, nullptr, &stderr_out)) {
        spdlog::error("seed admin failed: {}", stderr_out);
        return false;
    }

    const std::string user_sql =
        "INSERT INTO users (username, password_hash, role) VALUES (" +
        sql_quote("user") + ", " + sql_quote(AuthService::hash_password("123456")) + ", 'user') "
        "ON DUPLICATE KEY UPDATE password_hash = VALUES(password_hash), role = VALUES(role)";
    if (!exec_sql(user_sql, nullptr, &stderr_out)) {
        spdlog::error("seed user failed: {}", stderr_out);
        return false;
    }
    return true;
}

std::optional<UserRecord> UserService::find_by_username(const std::string& username) {
    std::string stdout_out;
    std::string stderr_out;
    const std::string sql =
        "SELECT id,username,role,password_hash,"
        "DATE_FORMAT(created_at,'%Y-%m-%d %H:%i:%s'),"
        "DATE_FORMAT(updated_at,'%Y-%m-%d %H:%i:%s') "
        "FROM users WHERE username=" + sql_quote(username) + " LIMIT 1";
    if (!exec_sql(sql, &stdout_out, &stderr_out)) {
        spdlog::warn("find_by_username failed: {}", stderr_out);
        return std::nullopt;
    }
    const auto lines = split_lines(stdout_out);
    if (lines.empty()) return std::nullopt;
    const auto fields = split_tab_line(lines.front());
    if (fields.size() < 6) return std::nullopt;
    return parse_user_row(fields, true);
}

std::optional<UserRecord> UserService::find_by_id(int64_t id) {
    if (id <= 0) return std::nullopt;
    std::string stdout_out;
    std::string stderr_out;
    const std::string sql =
        "SELECT id,username,role,password_hash,"
        "DATE_FORMAT(created_at,'%Y-%m-%d %H:%i:%s'),"
        "DATE_FORMAT(updated_at,'%Y-%m-%d %H:%i:%s') "
        "FROM users WHERE id=" + std::to_string(id) + " LIMIT 1";
    if (!exec_sql(sql, &stdout_out, &stderr_out)) {
        spdlog::warn("find_by_id failed: {}", stderr_out);
        return std::nullopt;
    }
    const auto lines = split_lines(stdout_out);
    if (lines.empty()) return std::nullopt;
    const auto fields = split_tab_line(lines.front());
    if (fields.size() < 6) return std::nullopt;
    return parse_user_row(fields, true);
}

std::vector<UserRecord> UserService::list_users() {
    std::vector<UserRecord> out;
    std::string stdout_out;
    std::string stderr_out;
    const std::string sql =
        "SELECT id,username,role,"
        "DATE_FORMAT(created_at,'%Y-%m-%d %H:%i:%s'),"
        "DATE_FORMAT(updated_at,'%Y-%m-%d %H:%i:%s') "
        "FROM users ORDER BY id ASC";
    if (!exec_sql(sql, &stdout_out, &stderr_out)) {
        spdlog::error("list_users failed: {}", stderr_out);
        return out;
    }
    for (const auto& line : split_lines(stdout_out)) {
        const auto fields = split_tab_line(line);
        if (fields.size() < 5) continue;
        const auto u = parse_user_row(fields, false);
        if (u) out.push_back(*u);
    }
    return out;
}

bool UserService::create_user(const std::string& username,
                              const std::string& plain_password,
                              const std::string& role,
                              std::string* error_message,
                              int64_t* inserted_id) {
    if (!is_valid_role(role)) {
        if (error_message) *error_message = "invalid_role";
        return false;
    }
    std::string stderr_out;
    const std::string sql =
        "INSERT INTO users (username,password_hash,role) VALUES (" +
        sql_quote(username) + ", " + sql_quote(AuthService::hash_password(plain_password)) + ", " + sql_quote(role) + ")";
    if (!exec_sql(sql, nullptr, &stderr_out)) {
        if (error_message) {
            *error_message = stderr_out.find("Duplicate entry") != std::string::npos
                ? "username_exists"
                : stderr_out;
        }
        return false;
    }
    const auto created = find_by_username(username);
    if (!created) {
        if (error_message) *error_message = "create_success_but_user_not_found";
        return false;
    }
    if (inserted_id) *inserted_id = created->id;
    return true;
}

bool UserService::update_user(int64_t id,
                              const std::optional<std::string>& username,
                              const std::optional<std::string>& plain_password,
                              const std::optional<std::string>& role,
                              std::string* error_message) {
    if (id <= 0) {
        if (error_message) *error_message = "invalid_user_id";
        return false;
    }
    if (role && !is_valid_role(*role)) {
        if (error_message) *error_message = "invalid_role";
        return false;
    }
    if (!find_by_id(id)) {
        if (error_message) *error_message = "user_not_found";
        return false;
    }
    if (!username && !plain_password && !role) {
        if (error_message) *error_message = "nothing_to_update";
        return false;
    }
    if (role && *role != "admin") {
        if (!ensure_can_demote_or_delete_admin(id, error_message)) return false;
    }

    std::string stderr_out;
    if (username && !username->empty()) {
        const std::string sql =
            "UPDATE users SET username=" + sql_quote(*username) + " WHERE id=" + std::to_string(id);
        if (!exec_sql(sql, nullptr, &stderr_out)) {
            if (error_message) {
                *error_message = stderr_out.find("Duplicate entry") != std::string::npos
                    ? "username_exists"
                    : stderr_out;
            }
            return false;
        }
    }
    if (plain_password && !plain_password->empty()) {
        const std::string sql =
            "UPDATE users SET password_hash=" + sql_quote(AuthService::hash_password(*plain_password)) +
            " WHERE id=" + std::to_string(id);
        if (!exec_sql(sql, nullptr, &stderr_out)) {
            if (error_message) *error_message = stderr_out;
            return false;
        }
    }
    if (role && !role->empty()) {
        const std::string sql =
            "UPDATE users SET role=" + sql_quote(*role) + " WHERE id=" + std::to_string(id);
        if (!exec_sql(sql, nullptr, &stderr_out)) {
            if (error_message) *error_message = stderr_out;
            return false;
        }
    }
    return true;
}

bool UserService::delete_user(int64_t id, std::string* error_message) {
    if (id <= 0) {
        if (error_message) *error_message = "invalid_user_id";
        return false;
    }
    if (!find_by_id(id)) {
        if (error_message) *error_message = "user_not_found";
        return false;
    }
    if (!ensure_can_demote_or_delete_admin(id, error_message)) return false;

    std::string stderr_out;
    const std::string sql = "DELETE FROM users WHERE id=" + std::to_string(id);
    if (!exec_sql(sql, nullptr, &stderr_out)) {
        if (error_message) *error_message = stderr_out;
        return false;
    }
    return true;
}

bool UserService::exec_sql(const std::string& sql, std::string* stdout_out, std::string* stderr_out) {
    std::lock_guard<std::mutex> lock(exec_mutex_);
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        if (stderr_out) *stderr_out = "pipe_failed";
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        if (stderr_out) *stderr_out = "fork_failed";
        return false;
    }

    if (pid == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);

        std::string user_arg = "-u" + config_.user;
        std::string pass_arg = "-p" + config_.password;
        std::vector<std::string> args = {
            "docker", "exec", "-i", config_.container_name, "mysql",
            "-N", "-B", user_arg, pass_arg, "-D", config_.database, "-e", sql,
        };
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execvp("docker", argv.data());
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);
    std::string out = read_all_from_fd(out_pipe[0]);
    std::string err = read_all_from_fd(err_pipe[0]);
    close(out_pipe[0]);
    close(err_pipe[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (stdout_out) *stdout_out = out;
    if (stderr_out) *stderr_out = err;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string UserService::sql_quote(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('\'');
    for (char c : value) {
        if (c == '\'') out.append("''");
        else if (c == '\\') out.append("\\\\");
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::vector<std::string> UserService::split_tab_line(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string item;
    while (std::getline(ss, item, '\t')) {
        fields.push_back(item);
    }
    return fields;
}

std::vector<std::string> UserService::split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

bool UserService::is_valid_role(const std::string& role) {
    return role == "admin" || role == "user";
}

bool UserService::ensure_can_demote_or_delete_admin(int64_t target_user_id, std::string* error_message) {
    std::string stdout_out;
    std::string stderr_out;
    const std::string target_role_sql =
        "SELECT role FROM users WHERE id=" + std::to_string(target_user_id) + " LIMIT 1";
    if (!exec_sql(target_role_sql, &stdout_out, &stderr_out)) {
        if (error_message) *error_message = stderr_out;
        return false;
    }
    const auto role_lines = split_lines(stdout_out);
    if (role_lines.empty()) {
        if (error_message) *error_message = "user_not_found";
        return false;
    }
    if (role_lines[0] != "admin") return true;

    stdout_out.clear();
    const std::string count_sql = "SELECT COUNT(*) FROM users WHERE role='admin'";
    if (!exec_sql(count_sql, &stdout_out, &stderr_out)) {
        if (error_message) *error_message = stderr_out;
        return false;
    }
    const auto count_lines = split_lines(stdout_out);
    if (count_lines.empty()) {
        if (error_message) *error_message = "count_admin_failed";
        return false;
    }
    int64_t admin_count = 0;
    try {
        admin_count = std::stoll(count_lines[0]);
    } catch (...) {
        if (error_message) *error_message = "count_admin_parse_failed";
        return false;
    }
    if (admin_count <= 1) {
        if (error_message) *error_message = "last_admin_forbidden";
        return false;
    }
    return true;
}

}  // namespace sfc
