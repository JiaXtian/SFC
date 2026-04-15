#include "utils/CommandRunner.h"
#include <array>
#include <cstdio>
#include <memory>
#include <sys/wait.h>

namespace sfc {

CommandResult run_command(const std::string& command) {
    CommandResult result;
    std::array<char, 4096> buffer{};
    std::string output;

    std::unique_ptr<FILE, decltype(&pclose)> pipe(
        popen((command + " 2>&1").c_str(), "r"),
        pclose
    );
    if (!pipe) {
        result.exit_code = -1;
        result.output = "popen failed";
        return result;
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        output.append(buffer.data());
    }

    const int rc = pclose(pipe.release());
    if (WIFEXITED(rc)) {
        result.exit_code = WEXITSTATUS(rc);
    } else {
        result.exit_code = -1;
    }
    result.output = std::move(output);
    return result;
}

std::string shell_escape_single_quotes(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char ch : value) {
        if (ch == '\'') {
            escaped.append("'\"'\"'");
        } else {
            escaped.push_back(ch);
        }
    }
    return escaped;
}

} // namespace sfc

