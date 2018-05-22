#pragma once

#include <string>
#include <vector>

/**
 * Utility function to split a string by the delimiter
 */
inline std::vector<std::string>
split(const std::string& record, char delimiter = ',')
{
    std::vector<std::string> fields;

    size_t begin = 0, end;
    do {
        end = record.find_first_of(delimiter, begin);
        fields.push_back(record.substr(begin, end - begin));
        begin = end + 1;
    } while (end != std::string::npos);

    return fields;
}
