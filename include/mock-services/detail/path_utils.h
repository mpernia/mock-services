#ifndef MOCK_SERVICES_DETAIL_PATH_UTILS_H
#define MOCK_SERVICES_DETAIL_PATH_UTILS_H

#include <cstddef>
#include <ctime>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstring>
#endif

namespace mock_services {
namespace detail {

inline std::string normalize_separators(const std::string& path) {
    std::string result = path;
#ifdef _WIN32
    for (auto& ch : result) { if (ch == '/') ch = '\\'; }
#else
    for (auto& ch : result) { if (ch == '\\') ch = '/'; }
#endif
    return result;
}

inline bool has_case_insensitive_prefix(const std::string& s,
                                         const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
#ifdef _WIN32
    return _strnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0;
#else
    return s.compare(0, prefix.size(), prefix) == 0;
#endif
}

#ifdef _WIN32
inline time_t filetime_to_time_t(const FILETIME& ft) {
    FILETIME ftLocal;
    if (!FileTimeToLocalFileTime(&ft, &ftLocal))
        return 0;
    ULARGE_INTEGER ui;
    ui.LowPart = ftLocal.dwLowDateTime;
    ui.HighPart = ftLocal.dwHighDateTime;
    return static_cast<time_t>((ui.QuadPart - 116444736000000000ULL) / 10000000ULL);
}
#endif

}
}
#endif
