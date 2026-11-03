#include <stdexcept>
#include <utility>

#include "LaunchArguments.h"

std::vector<std::string> xlibrary::split_launch_arguments(const std::string_view arguments) {
    if (arguments.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("Application arguments contain a null character");
    }
    std::vector<std::string> result;
    std::string argument;
    char quote = 0;
    bool started = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const char character = arguments[index];
        if (character == '\\' && quote != '\'') {
            if (++index == arguments.size()) {
                throw std::invalid_argument("Application arguments end with an escape");
            }
            const char escaped = arguments[index];
            if (quote == '"' && escaped != '"' && escaped != '\\' && escaped != '$' && escaped != '`' &&
                escaped != '\n') {
                argument.push_back('\\');
            }
            if (escaped != '\n') {
                argument.push_back(escaped);
                started = true;
            }
        } else if (quote != 0) {
            if (character == quote) {
                quote = 0;
            } else {
                argument.push_back(character);
            }
        } else if (character == '\'' || character == '"') {
            quote = character;
            started = true;
        } else if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
            if (started) {
                result.push_back(std::move(argument));
                argument.clear();
                started = false;
            }
        } else {
            argument.push_back(character);
            started = true;
        }
    }
    if (quote != 0) {
        throw std::invalid_argument("Application arguments contain an unterminated quote");
    }
    if (started) {
        result.push_back(std::move(argument));
    }
    return result;
}
