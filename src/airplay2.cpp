/*!
 * @file airplay2.cpp
 * @brief Compilation unit for public docs / library anchor symbols.
 *
 * Public API implementation lives in core/airplay_server_impl.cpp.
 * This file exists to provide a clearly named translation unit and
 * a central place for library-level globals (e.g. version string).
 */
#include "../include/airplay2/airplay2.h"
#include "platform/platform_socket.h"
#include "platform/platform_log.h"

namespace airplay2 {

// Explicit anchor for the version string (avoids unused-variable warnings).
const char* library_version() { return AIRPLAY2_VERSION_STRING; }
uint32_t    library_version_int() { return AIRPLAY2_VERSION; }

} // namespace airplay2
