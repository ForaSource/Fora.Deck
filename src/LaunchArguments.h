#ifndef FORADECK_LAUNCH_ARGUMENTS_H
#define FORADECK_LAUNCH_ARGUMENTS_H

#include <string>
#include <string_view>
#include <vector>

namespace foradeck {

// Разбирает пользовательские аргументы Linux без запуска оболочки и подстановок.
// Кавычки сохраняют пустые аргументы; обратная косая черта экранирует символы
// по правилам shell. NUL, незакрытые кавычки и конечный escape дают invalid_argument.
// Остальные байты (в том числе UTF-8) сохраняются без перекодирования.
std::vector<std::string> split_launch_arguments(std::string_view arguments);

} // namespace foradeck

#endif // FORADECK_LAUNCH_ARGUMENTS_H
