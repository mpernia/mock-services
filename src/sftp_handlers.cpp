#include "mock-services/detail/sftp_handlers.h"
#include "mock-services/detail/path_utils.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

namespace mock_services {
namespace {

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

struct sftp_attr_ptr {
    sftp_attributes attr_;

    sftp_attr_ptr() : attr_(nullptr) {}
    explicit sftp_attr_ptr(sftp_attributes a) : attr_(a) {}
    ~sftp_attr_ptr() {
        if (attr_) {
            sftp_attributes_free(attr_);
            attr_ = nullptr;
        }
    }

    sftp_attr_ptr(const sftp_attr_ptr&) = delete;
    sftp_attr_ptr& operator=(const sftp_attr_ptr&) = delete;

    sftp_attr_ptr(sftp_attr_ptr&& other) : attr_(other.attr_) {
        other.attr_ = nullptr;
    }

    sftp_attr_ptr& operator=(sftp_attr_ptr&& other) {
        if (this != &other) {
            if (attr_) sftp_attributes_free(attr_);
            attr_ = other.attr_;
            other.attr_ = nullptr;
        }
        return *this;
    }

    sftp_attributes get() const { return attr_; }
    bool valid() const { return attr_ != nullptr; }
    sftp_attributes release() {
        sftp_attributes a = attr_;
        attr_ = nullptr;
        return a;
    }
};

struct file_ptr {
    FILE* fp_;

    file_ptr() : fp_(nullptr) {}
    explicit file_ptr(FILE* f) : fp_(f) {}
    ~file_ptr() { if (fp_) { std::fclose(fp_); fp_ = nullptr; } }

    file_ptr(const file_ptr&) = delete;
    file_ptr& operator=(const file_ptr&) = delete;

    file_ptr(file_ptr&& other) : fp_(other.fp_) { other.fp_ = nullptr; }
    file_ptr& operator=(file_ptr&& other) {
        if (this != &other) {
            if (fp_) std::fclose(fp_);
            fp_ = other.fp_;
            other.fp_ = nullptr;
        }
        return *this;
    }

    FILE* get() const { return fp_; }
    bool valid() const { return fp_ != nullptr; }
    FILE* release() { FILE* f = fp_; fp_ = nullptr; return f; }
};

#ifdef _WIN32
struct find_handle {
    HANDLE h_;

    find_handle() : h_(INVALID_HANDLE_VALUE) {}
    explicit find_handle(HANDLE h) : h_(h) {}
    ~find_handle() {
        if (h_ != INVALID_HANDLE_VALUE) {
            FindClose(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }

    find_handle(const find_handle&) = delete;
    find_handle& operator=(const find_handle&) = delete;

    find_handle(find_handle&& other) : h_(other.h_) {
        other.h_ = INVALID_HANDLE_VALUE;
    }
    find_handle& operator=(find_handle&& other) {
        if (this != &other) {
            if (h_ != INVALID_HANDLE_VALUE) FindClose(h_);
            h_ = other.h_;
            other.h_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    HANDLE get() const { return h_; }
    bool valid() const { return h_ != INVALID_HANDLE_VALUE; }
    HANDLE release() { HANDLE h = h_; h_ = INVALID_HANDLE_VALUE; return h; }
};
#else
struct dir_ptr {
    DIR* d_;

    dir_ptr() : d_(nullptr) {}
    explicit dir_ptr(DIR* d) : d_(d) {}
    ~dir_ptr() { if (d_) { closedir(d_); d_ = nullptr; } }

    dir_ptr(const dir_ptr&) = delete;
    dir_ptr& operator=(const dir_ptr&) = delete;

    dir_ptr(dir_ptr&& other) : d_(other.d_) { other.d_ = nullptr; }
    dir_ptr& operator=(dir_ptr&& other) {
        if (this != &other) {
            if (d_) closedir(d_);
            d_ = other.d_;
            other.d_ = nullptr;
        }
        return *this;
    }

    DIR* get() const { return d_; }
    bool valid() const { return d_ != nullptr; }
    DIR* release() { DIR* d = d_; d_ = nullptr; return d; }
};
#endif

static sftp_attr_ptr make_attr(const std::string& name,
                               unsigned long long size,
                               int perm,
                               time_t mtime,
                               time_t atime) {
    sftp_attributes a = static_cast<sftp_attributes>(
        std::calloc(1, sizeof(struct sftp_attributes_struct)));
    if (!a) return sftp_attr_ptr();

    a->name = strdup(name.c_str());
    a->size = size;
    a->permissions = static_cast<uint32_t>(perm);
    a->atime = static_cast<uint32_t>(atime);
    a->mtime = static_cast<uint32_t>(mtime);
    a->flags = SSH_FILEXFER_ATTR_SIZE |
               SSH_FILEXFER_ATTR_PERMISSIONS |
               SSH_FILEXFER_ATTR_ACMODTIME;

    if (perm & S_IFDIR)
        a->type = SSH_FILEXFER_TYPE_DIRECTORY;
    else if (perm & S_IFREG)
        a->type = SSH_FILEXFER_TYPE_REGULAR;
    else
        a->type = SSH_FILEXFER_TYPE_UNKNOWN;

    return sftp_attr_ptr(a);
}

static sftp_attr_ptr stat_to_attr(const std::string& full_path,
                                  const std::string& display_name) {
    struct stat st;
    if (stat(full_path.c_str(), &st) != 0)
        return sftp_attr_ptr();
    return make_attr(display_name,
                     static_cast<unsigned long long>(st.st_size),
                     static_cast<int>(st.st_mode),
                     st.st_mtime,
                     st.st_atime);
}

static inline bool can_read(sftp_permission permission) {
    return (static_cast<int>(permission) & static_cast<int>(sftp_permission::read)) != 0;
}
static inline bool can_write(sftp_permission permission) {
    return (static_cast<int>(permission) & static_cast<int>(sftp_permission::write)) != 0;
}

static const char* sftp_flags_to_mode(int sftp_flags, bool* trunc) {
    *trunc = false;
    int rw = sftp_flags & (SSH_FXF_READ | SSH_FXF_WRITE);
    if (rw == SSH_FXF_READ)
        return "rb";
    if (rw == SSH_FXF_WRITE) {
        if (sftp_flags & SSH_FXF_APPEND) return "ab";
        if (sftp_flags & SSH_FXF_TRUNC) { *trunc = true; return "wb"; }
        if (sftp_flags & SSH_FXF_CREAT) return "wb";
        return "rb+";
    }
    if (rw == (SSH_FXF_READ | SSH_FXF_WRITE)) {
        if (sftp_flags & SSH_FXF_APPEND) return "ab+";
        if (sftp_flags & SSH_FXF_TRUNC) { *trunc = true; return "wb+"; }
        return "rb+";
    }
    return "rb";
}

#ifdef _WIN32
static sftp_attr_ptr win32_find_to_attr(const WIN32_FIND_DATAA& ffd,
                                        const std::string& /*parent*/,
                                        const std::string& name) {
    unsigned long long size =
        (static_cast<unsigned long long>(ffd.nFileSizeHigh) << 32) |
        static_cast<unsigned long long>(ffd.nFileSizeLow);
    int perm = S_IFREG | 0644;
    if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        perm = S_IFDIR | 0755;

    time_t mtime = detail::filetime_to_time_t(ffd.ftLastWriteTime);
    time_t atime = detail::filetime_to_time_t(ffd.ftLastAccessTime);

    return make_attr(name, size, perm, mtime, atime);
}
#endif

enum class HandleKind { file, directory };

struct HandleData {
    explicit HandleData(HandleKind k) : kind(k) {}
    HandleKind kind;
};

struct FileData : HandleData {
    FileData() : HandleData(HandleKind::file) {}
    FILE* fp = nullptr;
    std::string path;
};

struct DirData : HandleData {
    DirData() : HandleData(HandleKind::directory) {}
#ifdef _WIN32
    HANDLE hFind = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAA ffd;
    bool first = true;
#endif
    std::string path;
#ifndef _WIN32
    DIR* dir = nullptr;
#endif
};

}

void sftp_do_open(SftpContext& ctx, sftp_client_message msg) {
    const char* client_path = sftp_client_message_get_filename(msg);
    int flags = static_cast<int>(sftp_client_message_get_flags(msg));

    if (flags & (SSH_FXF_WRITE | SSH_FXF_CREAT | SSH_FXF_TRUNC)) {
        if (!can_write(ctx.perm)) {
            ctx.log_activity("OPEN", client_path, "DENIED");
            sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, nullptr);
            return;
        }
    }
    if ((flags & SSH_FXF_READ) && !can_read(ctx.perm)) {
        ctx.log_activity("OPEN", client_path, "DENIED");
        sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, nullptr);
        return;
    }

    std::string full = ctx.resolve_path(client_path,
                                        !(flags & SSH_FXF_CREAT));
    if (full.empty()) {
        ctx.log_activity("OPEN", client_path, "NO_SUCH_FILE");
        sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, nullptr);
        return;
    }

    bool trunc = false;
    const char* mode = sftp_flags_to_mode(flags, &trunc);

    file_ptr fp(std::fopen(full.c_str(), mode));
    if (!fp.valid()) {
        ctx.log_activity("OPEN", client_path, "FAILURE");
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
        return;
    }

    FileData* fd = new FileData();
    fd->fp = fp.release();
    fd->path = full;

    ssh_string handle = sftp_handle_alloc(ctx.sftp, fd);
    if (!handle) {
        if (fd->fp) std::fclose(fd->fp);
        delete fd;
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
        return;
    }

    int reply_status = sftp_reply_handle(msg, handle);
    ssh_string_free(handle);

    if (reply_status != SSH_OK) {
        if (fd->fp) std::fclose(fd->fp);
        sftp_handle_remove(ctx.sftp, handle);
        delete fd;
    }

    ctx.log_activity("OPEN", client_path, "OK");
}

void sftp_do_close(SftpContext& ctx, sftp_client_message msg) {
    ssh_string request_handle = msg->handle;
    void* info = sftp_handle(ctx.sftp, request_handle);
    if (!info) {
        sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, nullptr);
        return;
    }

    auto* handle = static_cast<HandleData*>(info);
    if (handle->kind == HandleKind::file) {
        auto* fd = static_cast<FileData*>(info);
        if (fd->fp) {
            std::fclose(fd->fp);
        }
        delete fd;
    } else {
        auto* dd = static_cast<DirData*>(info);
#ifdef _WIN32
        if (dd->hFind != INVALID_HANDLE_VALUE) {
            FindClose(dd->hFind);
            dd->hFind = INVALID_HANDLE_VALUE;
        }
#else
        if (dd->dir) {
            closedir(dd->dir);
            dd->dir = nullptr;
        }
#endif
        delete dd;
    }
    sftp_handle_remove(ctx.sftp, request_handle);

    sftp_reply_status(msg, SSH_FX_OK, nullptr);
    ctx.log_activity("CLOSE", "", "OK");
}

void sftp_do_read(SftpContext& ctx, sftp_client_message msg) {
    ssh_string request_handle = msg->handle;
    void* info = sftp_handle(ctx.sftp, request_handle);
    if (!info) {
        sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, nullptr);
        return;
    }

    FileData* fd = static_cast<FileData*>(info);
    if (!fd->fp) {
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
        return;
    }

    if (fseek(fd->fp, static_cast<long>(msg->offset), SEEK_SET) != 0) {
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
        return;
    }

    std::vector<char> buffer(static_cast<size_t>(msg->len > 65536 ? 65536 : msg->len));
    size_t read_count = std::fread(buffer.data(), 1, buffer.size(), fd->fp);

    if (read_count == 0) {
        sftp_reply_status(msg, SSH_FX_EOF, nullptr);
        return;
    }

    int reply_status = sftp_reply_data(msg, buffer.data(), static_cast<int>(read_count));
    if (reply_status != SSH_OK) {
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
    }
}

void sftp_do_write(SftpContext& ctx, sftp_client_message msg) {
    if (!can_write(ctx.perm)) {
        sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, nullptr);
        return;
    }

    ssh_string request_handle = msg->handle;
    void* info = sftp_handle(ctx.sftp, request_handle);
    if (!info) {
        sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, nullptr);
        return;
    }

    FileData* fd = static_cast<FileData*>(info);
    if (!fd->fp) {
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
        return;
    }

    if (fseek(fd->fp, static_cast<long>(msg->offset), SEEK_SET) != 0) {
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
        return;
    }

    const char* data = sftp_client_message_get_data(msg);
    uint32_t data_length = static_cast<uint32_t>(ssh_string_len(msg->data));

    size_t written_count = std::fwrite(data, 1, data_length, fd->fp);
    if (written_count != data_length) {
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
        return;
    }

    std::fflush(fd->fp);
    sftp_reply_status(msg, SSH_FX_OK, nullptr);
    ctx.log_activity("WRITE", fd->path, "OK");
}

void sftp_do_opendir(SftpContext& ctx, sftp_client_message msg) {
    const char* client_path = sftp_client_message_get_filename(msg);
    if (!can_read(ctx.perm)) {
        ctx.log_activity("OPENDIR", client_path, "DENIED");
        sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, nullptr);
        return;
    }

    std::string full = ctx.resolve_path(client_path, true);
    if (full.empty()) {
        ctx.log_activity("OPENDIR", client_path, "NO_SUCH_FILE");
        sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, nullptr);
        return;
    }

    DirData* dd = new DirData();
    dd->path = full;

#ifdef _WIN32
    dd->hFind = INVALID_HANDLE_VALUE;
    dd->first = true;
#else
    dir_ptr d(opendir(full.c_str()));
    if (!d.valid()) {
        delete dd;
        ctx.log_activity("OPENDIR", client_path, "FAILURE");
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
        return;
    }
    dd->dir = d.release();
#endif

    ssh_string handle = sftp_handle_alloc(ctx.sftp, dd);
    if (!handle) {
#ifdef _WIN32
        if (dd->hFind != INVALID_HANDLE_VALUE) FindClose(dd->hFind);
#else
        if (dd->dir) closedir(dd->dir);
#endif
        delete dd;
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
        return;
    }

    int reply_status = sftp_reply_handle(msg, handle);
    ssh_string_free(handle);

    if (reply_status != SSH_OK) {
#ifdef _WIN32
        if (dd->hFind != INVALID_HANDLE_VALUE) FindClose(dd->hFind);
#else
        if (dd->dir) closedir(dd->dir);
#endif
        sftp_handle_remove(ctx.sftp, handle);
        delete dd;
    }

    ctx.log_activity("OPENDIR", client_path, "OK");
}

void sftp_do_readdir(SftpContext& ctx, sftp_client_message msg) {
    ssh_string request_handle = msg->handle;
    void* info = sftp_handle(ctx.sftp, request_handle);
    if (!info) {
        sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, nullptr);
        return;
    }

    DirData* dd = static_cast<DirData*>(info);

#ifdef _WIN32
    if (dd->first) {
        std::string pattern = dd->path + "\\*";
        dd->hFind = FindFirstFileA(pattern.c_str(), &dd->ffd);
        dd->first = false;
        if (dd->hFind == INVALID_HANDLE_VALUE) {
            sftp_reply_status(msg, SSH_FX_EOF, nullptr);
            return;
        }
    } else {
        if (!FindNextFileA(dd->hFind, &dd->ffd)) {
            sftp_reply_status(msg, SSH_FX_EOF, nullptr);
            return;
        }
    }

    while (std::strcmp(dd->ffd.cFileName, ".") == 0 ||
           std::strcmp(dd->ffd.cFileName, "..") == 0) {
        if (!FindNextFileA(dd->hFind, &dd->ffd)) {
            sftp_reply_status(msg, SSH_FX_EOF, nullptr);
            return;
        }
    }

    sftp_attr_ptr attr = win32_find_to_attr(dd->ffd, dd->path,
                                              dd->ffd.cFileName);
    if (!attr.valid()) {
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
        return;
    }

    int rc = sftp_reply_name(msg, dd->ffd.cFileName, attr.get());
    if (rc != SSH_OK) {
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
    }
#else
    struct dirent* entry = readdir(dd->dir);
    if (!entry) {
        sftp_reply_status(msg, SSH_FX_EOF, nullptr);
        return;
    }

    while (entry && (std::strcmp(entry->d_name, ".") == 0 ||
                     std::strcmp(entry->d_name, "..") == 0)) {
        entry = readdir(dd->dir);
    }
    if (!entry) {
        sftp_reply_status(msg, SSH_FX_EOF, nullptr);
        return;
    }

    std::string entry_full = dd->path + "/" + entry->d_name;
    sftp_attr_ptr attr = stat_to_attr(entry_full, entry->d_name);
    if (!attr.valid()) {
        attr = make_attr(entry->d_name, 0, S_IFREG | 0644, 0, 0);
    }

    int rc = sftp_reply_name(msg, entry->d_name, attr.get());
    if (rc != SSH_OK) {
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
    }
#endif
}

void sftp_do_stat(SftpContext& ctx, sftp_client_message msg) {
    const char* client_path = sftp_client_message_get_filename(msg);
    std::string full = ctx.resolve_path(client_path, true);
    if (full.empty()) {
        ctx.log_activity("STAT", client_path, "NO_SUCH_FILE");
        sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, nullptr);
        return;
    }

    std::string display(client_path);
    size_t slash = display.rfind('/');
    if (slash != std::string::npos)
        display = display.substr(slash + 1);

    sftp_attr_ptr attr = stat_to_attr(full, display);
    if (!attr.valid()) {
        sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, nullptr);
        return;
    }

    int rc = sftp_reply_attr(msg, attr.get());
    if (rc != SSH_OK) {
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
    }
}

void sftp_do_fstat(SftpContext& ctx, sftp_client_message msg) {
    ssh_string request_handle = msg->handle;
    void* info = sftp_handle(ctx.sftp, request_handle);
    if (!info) {
        sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, nullptr);
        return;
    }

    FileData* fd = static_cast<FileData*>(info);

    std::string display = fd->path;
    size_t slash = display.rfind('/');
#ifdef _WIN32
    if (slash == std::string::npos)
        slash = display.rfind('\\');
#endif
    if (slash != std::string::npos)
        display = display.substr(slash + 1);

    sftp_attr_ptr attr = stat_to_attr(fd->path, display);
    if (!attr.valid()) {
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
        return;
    }

    int rc = sftp_reply_attr(msg, attr.get());
    if (rc != SSH_OK) {
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
    }
}

void sftp_do_setstat(SftpContext& ctx, sftp_client_message msg) {
    const char* client_path = sftp_client_message_get_filename(msg);
    if (!can_write(ctx.perm)) {
        ctx.log_activity("SETSTAT", client_path, "DENIED");
        sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, nullptr);
        return;
    }

    std::string full = ctx.resolve_path(client_path, true);
    if (full.empty()) {
        ctx.log_activity("SETSTAT", client_path, "NO_SUCH_FILE");
        sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, nullptr);
        return;
    }

    sftp_reply_status(msg, SSH_FX_OK, nullptr);
    ctx.log_activity("SETSTAT", client_path, "OK");
}

void sftp_do_fsetstat(SftpContext& ctx, sftp_client_message msg) {
    ssh_string request_handle = msg->handle;
    void* info = sftp_handle(ctx.sftp, request_handle);
    if (!info) {
        sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, nullptr);
        return;
    }

    if (!can_write(ctx.perm)) {
        sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, nullptr);
        return;
    }

    sftp_reply_status(msg, SSH_FX_OK, nullptr);
}

void sftp_do_mkdir(SftpContext& ctx, sftp_client_message msg) {
    const char* client_path = sftp_client_message_get_filename(msg);
    if (!can_write(ctx.perm)) {
        ctx.log_activity("MKDIR", client_path, "DENIED");
        sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, nullptr);
        return;
    }

    std::string full = ctx.resolve_path(client_path, false);
    if (full.empty()) {
        ctx.log_activity("MKDIR", client_path, "NO_SUCH_PATH");
        sftp_reply_status(msg, SSH_FX_NO_SUCH_PATH, nullptr);
        return;
    }

#ifdef _WIN32
    int rc = _mkdir(full.c_str());
#else
    int rc = mkdir(full.c_str(), 0755);
#endif
    if (rc != 0) {
        ctx.log_activity("MKDIR", client_path, "FAILURE");
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
        return;
    }

    sftp_reply_status(msg, SSH_FX_OK, nullptr);
    ctx.log_activity("MKDIR", client_path, "OK");
}

void sftp_do_rmdir(SftpContext& ctx, sftp_client_message msg) {
    const char* client_path = sftp_client_message_get_filename(msg);
    if (!can_write(ctx.perm)) {
        ctx.log_activity("RMDIR", client_path, "DENIED");
        sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, nullptr);
        return;
    }

    std::string full = ctx.resolve_path(client_path, true);
    if (full.empty()) {
        ctx.log_activity("RMDIR", client_path, "NO_SUCH_FILE");
        sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, nullptr);
        return;
    }

#ifdef _WIN32
    int rc = _rmdir(full.c_str());
#else
    int rc = rmdir(full.c_str());
#endif
    if (rc != 0) {
        ctx.log_activity("RMDIR", client_path, "FAILURE");
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
        return;
    }

    sftp_reply_status(msg, SSH_FX_OK, nullptr);
    ctx.log_activity("RMDIR", client_path, "OK");
}

void sftp_do_remove(SftpContext& ctx, sftp_client_message msg) {
    const char* client_path = sftp_client_message_get_filename(msg);
    if (!can_write(ctx.perm)) {
        ctx.log_activity("REMOVE", client_path, "DENIED");
        sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, nullptr);
        return;
    }

    std::string full = ctx.resolve_path(client_path, true);
    if (full.empty()) {
        ctx.log_activity("REMOVE", client_path, "NO_SUCH_FILE");
        sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, nullptr);
        return;
    }

#ifdef _WIN32
    int rc = _unlink(full.c_str());
#else
    int rc = unlink(full.c_str());
#endif
    if (rc != 0) {
        ctx.log_activity("REMOVE", client_path, "FAILURE");
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
        return;
    }

    sftp_reply_status(msg, SSH_FX_OK, nullptr);
    ctx.log_activity("REMOVE", client_path, "OK");
}

void sftp_do_rename(SftpContext& ctx, sftp_client_message msg) {
    const char* src_path = sftp_client_message_get_filename(msg);
    const char* dst_path = sftp_client_message_get_submessage(msg);
    if (!dst_path) {
        dst_path = msg->str_data;
    }

    if (!can_write(ctx.perm)) {
        ctx.log_activity("RENAME", src_path, "DENIED");
        sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, nullptr);
        return;
    }

    std::string full_src = ctx.resolve_path(src_path, true);
    if (full_src.empty()) {
        ctx.log_activity("RENAME", src_path, "NO_SUCH_FILE");
        sftp_reply_status(msg, SSH_FX_NO_SUCH_FILE, nullptr);
        return;
    }

    if (!dst_path) {
        sftp_reply_status(msg, SSH_FX_FAILURE, "No destination path");
        return;
    }

    std::string full_dst = ctx.resolve_path(dst_path, false);
    if (full_dst.empty()) {
        ctx.log_activity("RENAME", dst_path, "NO_SUCH_PATH");
        sftp_reply_status(msg, SSH_FX_NO_SUCH_PATH, nullptr);
        return;
    }

    int rc = std::rename(full_src.c_str(), full_dst.c_str());
    if (rc != 0) {
        ctx.log_activity("RENAME", src_path, "FAILURE");
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
        return;
    }

    sftp_reply_status(msg, SSH_FX_OK, nullptr);
    ctx.log_activity("RENAME",
                     src_path + std::string(" -> ") + dst_path, "OK");
}

void sftp_do_realpath(SftpContext& ctx, sftp_client_message msg) {
    const char* client_path = sftp_client_message_get_filename(msg);
    if (!can_read(ctx.perm)) {
        ctx.log_activity("REALPATH", client_path, "DENIED");
        sftp_reply_status(msg, SSH_FX_PERMISSION_DENIED, nullptr);
        return;
    }

    if (client_path == nullptr || client_path[0] == '\0') {
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
        return;
    }

    std::string full;
    if (std::strcmp(client_path, "/") == 0 ||
        std::strcmp(client_path, ".") == 0) {
        full = ctx.root;
    } else {
        full = ctx.resolve_path(client_path, false);
        if (full.empty()) {
            full = detail::normalize_separators(ctx.root) + "/" + client_path;

            std::string cleaned;
            for (size_t path_index = 0; path_index < full.size(); ++path_index) {
                if (path_index > 0 && full[path_index] == '/' && full[path_index - 1] == '/')
                    continue;
                cleaned += full[path_index];
            }
            full = cleaned;
        }
    }

    std::string display = full;
    {
        std::string nroot = detail::normalize_separators(ctx.root);
        std::string nfull = detail::normalize_separators(full);
        if (detail::has_case_insensitive_prefix(nfull, nroot)) {
            std::string suffix = nfull.substr(nroot.size());

            while (!suffix.empty() &&
                   (suffix[0] == '/' || suffix[0] == '\\'))
                suffix.erase(0, 1);
            if (suffix.empty())
                display = "/";
            else
                display = "/" + suffix;
        }
    }

    sftp_attr_ptr attr = make_attr(display, 0, S_IFDIR | 0755, 0, 0);
    if (!attr.valid()) {
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
        return;
    }

    int rc = sftp_reply_name(msg, display.c_str(), attr.get());
    if (rc != SSH_OK) {
        sftp_reply_status(msg, SSH_FX_FAILURE, nullptr);
    }
}

}
