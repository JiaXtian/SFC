#pragma once

#include <string>

namespace sfc {

struct CommandResult {
    int exit_code = -1;
    std::string output;
};

CommandResult run_command(const std::string& command);
std::string shell_escape_single_quotes(const std::string& value);

} // namespace sfc

