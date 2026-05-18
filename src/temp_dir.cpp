#include "mock-services/detail/temp_dir.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>

#if defined(_WIN32) && !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <fileapi.h>
#include <io.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

namespace mock_services {
namespace detail {

std::string unique_id() {
    static std::atomic<unsigned> counter{0};
    unsigned current_count = ++counter;
#ifdef _WIN32
    unsigned pid = static_cast<unsigned>(GetCurrentProcessId());
#else
    unsigned pid = static_cast<unsigned>(getpid());
#endif
    unsigned timestamp = static_cast<unsigned>(std::time(nullptr));
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%x%x%x", pid, timestamp, current_count);
    return buffer;
}

TempDir::TempDir(const std::string& name_prefix) {
#ifdef _WIN32
    char buffer[MAX_PATH + 1] = {};
    if (GetTempPathA(MAX_PATH + 1, buffer) == 0)
        throw std::runtime_error("GetTempPathA failed");
    path_ = buffer;
#else
    const char* temp_dir_env = std::getenv("TMPDIR");
    if (temp_dir_env != nullptr && temp_dir_env[0] != '\0')
        path_ = temp_dir_env;
    else
        path_ = "/tmp";
#endif
    if (!path_.empty() && path_.back() != '/' && path_.back() != '\\')
        path_ += '/';
    path_ += name_prefix + unique_id();

#ifdef _WIN32
    if (_mkdir(path_.c_str()) != 0)
        throw std::runtime_error("mkdir temp failed: " + path_);
#else
    if (mkdir(path_.c_str(), 0700) != 0)
        throw std::runtime_error("mkdir temp failed: " + path_);
#endif
}

TempDir::~TempDir() { erase_all(path_); }

const std::string& TempDir::path() const { return path_; }

void TempDir::erase_all(const std::string& dir) {
#ifdef _WIN32
    std::string search_pattern = dir + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE find_handle = FindFirstFileA(search_pattern.c_str(), &fd);
    if (find_handle == INVALID_HANDLE_VALUE) {
        RemoveDirectoryA(dir.c_str());
        return;
    }
    do {
        if (std::strcmp(fd.cFileName, ".") == 0 ||
            std::strcmp(fd.cFileName, "..") == 0)
            continue;
        std::string child_path = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            erase_all(child_path);
            RemoveDirectoryA(child_path.c_str());
        } else {
            DeleteFileA(child_path.c_str());
        }
    } while (FindNextFileA(find_handle, &fd));
    FindClose(find_handle);
    RemoveDirectoryA(dir.c_str());
#else
    DIR* directory = opendir(dir.c_str());
    if (!directory) {
        rmdir(dir.c_str());
        return;
    }
    struct dirent* entry;
    while ((entry = readdir(directory)) != nullptr) {
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0)
            continue;
        std::string child_path = dir + "/" + entry->d_name;
        struct stat file_stat;
        if (stat(child_path.c_str(), &file_stat) == 0 && S_ISDIR(file_stat.st_mode)) {
            erase_all(child_path);
            rmdir(child_path.c_str());
        } else {
            unlink(child_path.c_str());
        }
    }
    closedir(directory);
    rmdir(dir.c_str());
#endif
}

}
}
