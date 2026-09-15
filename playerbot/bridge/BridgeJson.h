#pragma once

// The bridge speaks JSON. The core ships nlohmann/json under dep/json; the
// module's CMakeLists puts that directory on the include path.
#include "json.hpp"

using Json = nlohmann::json;

// One protocol line: compact JSON, invalid UTF-8 replaced rather than thrown
// on (chat text and database names are not guaranteed to be clean), newline
// terminated.
inline std::string JsonLine(const Json& message)
{
    return message.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
}
