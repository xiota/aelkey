#pragma once

#include <string_view>

bool looks_like_regex(std::string_view s);
bool match_string(std::string_view pattern, std::string_view value);
