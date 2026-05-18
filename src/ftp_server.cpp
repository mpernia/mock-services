#include "mock-services/ftp_server.h"
#include "mock-services/detail/path_utils.h"
#include "mock-services/detail/platform_socket.h"
#include "mock-services/detail/temp_dir.h"
#include "mock-services/detail/thread_utils.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32) && !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <fileapi.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#endif

namespace mock_services {

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

static std::string resolve(const std::string& base, const std::string& cwd,
                            const std::string& target) {
    std::string raw = base;
    if (!cwd.empty()) raw += "/" + cwd;
    if (!target.empty()) {
        if (target[0] == '/') {
            raw = base + "/" + target.substr(1);
        } else {
            raw += "/" + target;
        }
    }

    for (auto& character : raw) if (character == '\\') character = '/';

    std::vector<std::string> parts;
    std::string current_part;
    for (size_t path_index = 0; path_index <= raw.size(); ++path_index) {
        char character = (path_index < raw.size()) ? raw[path_index] : '/';
        if (character == '/') {
            if (current_part == "..") {
                if (!parts.empty()) parts.pop_back();
                else return {};
            } else if (!current_part.empty() && current_part != ".") {
                parts.push_back(current_part);
            }
            current_part.clear();
        } else {
            current_part += character;
        }
    }

    std::string result;
    for (size_t part_index = 0; part_index < parts.size(); ++part_index) {
        if (part_index > 0) result += "/";
        result += parts[part_index];
    }

    if (!raw.empty() && raw[0] == '/') result.insert(0, "/");
    if (result.empty()) return base;

    std::string norm_base = base;
    for (auto& character : norm_base) if (character == '\\') character = '/';
    if (result.compare(0, norm_base.size(), norm_base) != 0) return {};

    return result;
}

static std::string fmt_month(int month) {
    static const char* month_names[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return (month >= 1 && month <= 12) ? month_names[month - 1] : "???";
}

#ifdef _WIN32
static std::string fmt_filetime(const FILETIME& ft) {
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s %02d %02d:%02d",
                  fmt_month(st.wMonth).c_str(), st.wDay, st.wHour, st.wMinute);
    return buffer;
}
#endif

static std::string fmt_unixtime(time_t timestamp) {
    struct tm result;
#ifdef _WIN32
    localtime_s(&result, &timestamp);
#else
    localtime_r(&timestamp, &result);
#endif
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s %02d %02d:%02d",
                  fmt_month(result.tm_mon + 1).c_str(), result.tm_mday,
                  result.tm_hour, result.tm_min);
    return buffer;
}

static std::string dir_listing(const std::string& dir_path) {
    std::string listing;
#ifdef _WIN32
    std::string search_pattern = dir_path + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE find_handle = FindFirstFileA(search_pattern.c_str(), &fd);
    if (find_handle == INVALID_HANDLE_VALUE) return listing;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        std::string permissions = is_dir ? "drwx------" : "-rwx------";
        std::string file_size = std::to_string(fd.nFileSizeLow);
        std::string modified_at = fmt_filetime(fd.ftLastWriteTime);
        listing += permissions + " 1 owner group " + file_size + " " + modified_at + " " +
               std::string(fd.cFileName) + "\r\n";
    } while (FindNextFileA(find_handle, &fd));
    FindClose(find_handle);
#else
    DIR* directory = opendir(dir_path.c_str());
    if (!directory) return listing;
    struct dirent* entry;
    while ((entry = readdir(directory)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        std::string child_path = dir_path + "/" + entry->d_name;
        struct stat file_stat;
        bool is_dir = false;
        long long file_size = 0;
        time_t mtime = 0;
        if (stat(child_path.c_str(), &file_stat) == 0) {
            is_dir = S_ISDIR(file_stat.st_mode);
            file_size = static_cast<long long>(file_stat.st_size);
            mtime = file_stat.st_mtime;
        }
        std::string permissions = is_dir ? "drwx------" : "-rwx------";
        std::string modified_at = fmt_unixtime(mtime);
        listing += permissions + " 1 owner group " + std::to_string(file_size) + " " + modified_at +
               " " + entry->d_name + "\r\n";
    }
    closedir(directory);
#endif
    return listing;
}

struct ftp_server::implementation {

    implementation() : ctrl_(kInvalidSock), port_(0), running_(false) {
        tmp_.reset(new detail::TempDir());
        wsa_init();
    }

    ~implementation() {
        stop();
        wsa_fini();
    }

    void start() {
        if (running_.load()) throw std::runtime_error("ftp_server is already running");

        ctrl_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (ctrl_ == kInvalidSock)
            throw std::runtime_error("socket creation failed");
        set_reuseaddr(ctrl_);

        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = 0;

        if (::bind(ctrl_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == kSockErr) {
            sock_close(ctrl_);
            ctrl_ = kInvalidSock;
            throw std::runtime_error("bind failed");
        }

        sockaddr_in bound;
        socklen_t alen = sizeof(bound);
        if (::getsockname(ctrl_, reinterpret_cast<sockaddr*>(&bound), &alen) == kSockErr) {
            sock_close(ctrl_);
            ctrl_ = kInvalidSock;
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(bound.sin_port);

        if (::listen(ctrl_, 10) == kSockErr) {
            sock_close(ctrl_);
            ctrl_ = kInvalidSock;
            throw std::runtime_error("listen failed");
        }

        running_.store(true);
        acceptor_ = std::thread([this] { accept_loop(); });
    }

    void stop() {
        if (!running_.load()) return;
        running_.store(false);

        {
            std::lock_guard<std::mutex> lock(clients_mtx_);
            for (auto& client : clients_) close_client(client.get());
            clients_.clear();
        }

        if (acceptor_.joinable()) acceptor_.join();

        client_thread_group_.join_all();
    }

    std::string base_url() const {
        if (!running_.load())
            throw std::runtime_error("ftp_server is not running");
        return "ftp://127.0.0.1:" + std::to_string(port_);
    }

    uint16_t port() const { return port_; }

    void add_user(const std::string& name, const std::string& pass,
                   const std::string& root, ftp_permission perm) {
        std::lock_guard<std::mutex> lock(users_mtx_);
        for (auto& u : users_) {
            if (u.name == name && !u.anon) {
                u.pass = pass;
                u.root = root;
                u.perm = perm;
                return;
            }
        }
        users_.push_back({name, pass, root, perm, false});
    }

    void add_anonymous_user(const std::string& root, ftp_permission perm) {
        std::lock_guard<std::mutex> lock(users_mtx_);
        users_.push_back({"anonymous", "", root, perm, true});
    }

    void clear_users() {
        std::lock_guard<std::mutex> lock(users_mtx_);
        users_.clear();
    }

    std::string temp_root() const { return tmp_ ? tmp_->path() : std::string(); }

    std::vector<std::string> connected_users() const {
        std::lock_guard<std::mutex> lock(conn_mtx_);
        std::vector<std::string> users;
        users.reserve(conn_users_.size());
        for (const auto& user : conn_users_) users.push_back(user);
        return users;
    }

private:

    struct User {
        std::string name;
        std::string pass;
        std::string root;
        ftp_permission perm;
        bool anon;
    };

    struct Client {
        raw_socket sock{kInvalidSock};
        std::string user;
        std::string cwd;
        std::string phy_root;
        ftp_permission perm{ftp_permission::none};
        bool auth{false};

        raw_socket data_sock{kInvalidSock};
        bool pasv{false};
    };

    raw_socket ctrl_;
    uint16_t port_;
    std::atomic<bool> running_;
    std::thread acceptor_;

    std::unique_ptr<detail::TempDir> tmp_;

    mutable std::mutex users_mtx_;
    std::vector<User> users_;

    mutable std::mutex clients_mtx_;
    std::vector<std::shared_ptr<Client>> clients_;

    mutable std::mutex conn_mtx_;
    std::set<std::string> conn_users_;

    detail::thread_group client_thread_group_;

    void accept_loop() {
        while (running_.load()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(ctrl_, &rfds);
            timeval tv{1, 0};
            int ready_count = ::select(static_cast<int>(ctrl_ + 1), &rfds, nullptr, nullptr, &tv);
            if (ready_count == kSockErr) break;
            if (ready_count == 0) continue;

            sockaddr_in client_addr;
            socklen_t client_addr_len = sizeof(client_addr);
            raw_socket client_sock = ::accept(ctrl_, reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
            if (client_sock == kInvalidSock) continue;

            auto client = std::make_shared<Client>();
            client->sock = client_sock;
            {
                std::lock_guard<std::mutex> lock(clients_mtx_);
                clients_.push_back(client);
            }
            ftp_send(client_sock, "220 FTP Mock Server ready\r\n");
            auto done = detail::thread_group::make_stop_flag();
            client_thread_group_.add(
                std::thread([this, client, done]() {
                    client_loop(client, done);
                    done->store(true);
                }), done);
            client_thread_group_.join_finished();
        }

        if (ctrl_ != kInvalidSock) {
            sock_close(ctrl_);
            ctrl_ = kInvalidSock;
        }

        {
            std::lock_guard<std::mutex> lock(clients_mtx_);
            for (auto& client : clients_) close_client(client.get());
            clients_.clear();
        }
    }

    void client_loop(std::shared_ptr<Client> client,
                     std::shared_ptr<std::atomic<bool>> done) {
        char buffer[4096];
        std::string partial;
        while (!done->load() && client->sock != kInvalidSock) {
            std::memset(buffer, 0, sizeof(buffer));
            int received_count = ::recv(client->sock, buffer, static_cast<int>(sizeof(buffer) - 1), 0);
            if (received_count <= 0) break;
            buffer[received_count] = 0;
            partial += buffer;

            size_t pos;
            while ((pos = partial.find('\n')) != std::string::npos) {
                std::string line = partial.substr(0, pos);
                partial.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                dispatch(client, line);
            }
        }

        if (client->auth) {
            std::lock_guard<std::mutex> lock(conn_mtx_);
            conn_users_.erase(client->user);
        }
        remove_client(client);
    }

    void dispatch(std::shared_ptr<Client> client, const std::string& line) {
        std::string cmd, arg;
        size_t sp = line.find(' ');
        if (sp == std::string::npos) {
            cmd = line;
        } else {
            cmd = line.substr(0, sp);
            arg = line.substr(sp + 1);
        }
        for (auto& character : cmd) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));

        if (cmd == "USER")      user_cmd(client, arg);
        else if (cmd == "PASS") pass_cmd(client, arg);
        else if (cmd == "SYST") ok_if(client, "215 UNIX Type: L8\r\n");
        else if (cmd == "PWD")  ok_if(client, "257 \"/" + client->cwd + "\" is current directory\r\n");
        else if (cmd == "TYPE") ok_if(client, "200 TYPE set OK\r\n");
        else if (cmd == "PASV") pasv_cmd(client);
        else if (cmd == "LIST") list_cmd(client, arg);
        else if (cmd == "RETR") retr_cmd(client, arg);
        else if (cmd == "STOR") stor_cmd(client, arg);
        else if (cmd == "DELE") dele_cmd(client, arg);
        else if (cmd == "RMD")  rmd_cmd(client, arg);
        else if (cmd == "MKD")  mkd_cmd(client, arg);
        else if (cmd == "CWD")  cwd_cmd(client, arg);
        else if (cmd == "CDUP") cwd_cmd(client, "..");
        else if (cmd == "QUIT") { ftp_send(client->sock, "221 Goodbye\r\n"); close_client(client.get()); }
        else if (cmd == "NOOP") ftp_send(client->sock, "200 OK\r\n");
        else if (cmd == "FEAT") feat_cmd(client);
        else ftp_send(client->sock, "502 Command not implemented\r\n");
    }

    void user_cmd(std::shared_ptr<Client> client, const std::string& arg) {
        std::lock_guard<std::mutex> lock(users_mtx_);
        for (const auto& user : users_) {
            if (user.name == arg || (user.anon && arg == "anonymous")) {
                client->user = user.name;
                if (user.anon) {
                    client->auth = true;
                    client->perm = user.perm;
                    client->phy_root = tmp_->path();
                    {
                        std::lock_guard<std::mutex> lock(conn_mtx_);
                        conn_users_.insert(user.name);
                    }
                    ftp_send(client->sock, "230 Login successful\r\n");
                    return;
                }
                ftp_send(client->sock, "331 Password required\r\n");
                return;
            }
        }
        ftp_send(client->sock, "530 Login incorrect\r\n");
    }

    void pass_cmd(std::shared_ptr<Client> client, const std::string& arg) {
        if (client->auth) { ftp_send(client->sock, "230 Already logged in\r\n"); return; }
        if (client->user.empty()) { ftp_send(client->sock, "503 Login with USER first\r\n"); return; }

        std::lock_guard<std::mutex> lock(users_mtx_);
        for (const auto& user : users_) {
            if (user.name == client->user && !user.anon) {
                if (user.pass == arg) {
                    client->auth = true;
                    client->perm = user.perm;
                    client->phy_root = tmp_->path();
                    std::string relative_root = user.root;
                    if (!relative_root.empty() && relative_root != "/") {
                        if (relative_root[0] == '/') relative_root = relative_root.substr(1);
                        if (!relative_root.empty()) {
                            client->phy_root = tmp_->path() + "/" + relative_root;

#ifdef _WIN32
                            _mkdir(client->phy_root.c_str());
#else
                            mkdir(client->phy_root.c_str(), 0700);
#endif
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lock(conn_mtx_);
                        conn_users_.insert(user.name);
                    }
                    ftp_send(client->sock, "230 Login successful\r\n");
                    return;
                }
                ftp_send(client->sock, "530 Login incorrect\r\n");
                return;
            }
        }
        ftp_send(client->sock, "530 Login incorrect\r\n");
    }

    void pasv_cmd(std::shared_ptr<Client> client) {
        if (!require_auth(client)) return;
        close_pasv(client.get());

        raw_socket data_sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (data_sock == kInvalidSock) { ftp_send(client->sock, "425 Can't open data connection\r\n"); return; }
        set_reuseaddr(data_sock);

        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = 0;
        if (::bind(data_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == kSockErr) {
            sock_close(data_sock);
            ftp_send(client->sock, "425 Can't open data connection\r\n");
            return;
        }
        if (::listen(data_sock, 1) == kSockErr) {
            sock_close(data_sock);
            ftp_send(client->sock, "425 Can't open data connection\r\n");
            return;
        }

        sockaddr_in bound_addr;
        socklen_t bound_addr_len = sizeof(bound_addr);
        if (::getsockname(data_sock, reinterpret_cast<sockaddr*>(&bound_addr), &bound_addr_len) == kSockErr) {
            sock_close(data_sock);
            ftp_send(client->sock, "425 Can't open data connection\r\n");
            return;
        }

        uint16_t data_port = ntohs(bound_addr.sin_port);
        client->data_sock = data_sock;
        client->pasv = true;

        ftp_send(client->sock,
                 "227 Entering Passive Mode (127,0,0,1," +
                     std::to_string(data_port / 256) + "," +
                     std::to_string(data_port % 256) + ")\r\n");
    }

    void list_cmd(std::shared_ptr<Client> client, const std::string& arg) {
        if (!require_auth(client)) return;
        if (!require_perm(client, ftp_permission::read)) return;

        std::string resolved_path = resolve(client->phy_root, client->cwd, arg);
        if (resolved_path.empty()) { ftp_send(client->sock, "550 Invalid path\r\n"); return; }

        ftp_send(client->sock, "150 Opening data connection\r\n");

        raw_socket data_sock = accept_data(client);
        if (data_sock == kInvalidSock) { ftp_send(client->sock, "425 Can't open data connection\r\n"); return; }

        std::string listing = dir_listing(resolved_path);
        ::send(data_sock, listing.data(), static_cast<int>(listing.size()), 0);
        sock_close(data_sock);
        ftp_send(client->sock, "226 Transfer complete\r\n");
    }

    void retr_cmd(std::shared_ptr<Client> client, const std::string& arg) {
        if (!require_auth(client)) return;
        if (!require_perm(client, ftp_permission::read)) return;
        if (arg.empty()) { ftp_send(client->sock, "501 Syntax error\r\n"); return; }

        std::string resolved_path = resolve(client->phy_root, client->cwd, arg);
        if (resolved_path.empty()) { ftp_send(client->sock, "550 File not found\r\n"); return; }

        ftp_send(client->sock, "150 Opening data connection\r\n");

        raw_socket data_sock = accept_data(client);
        if (data_sock == kInvalidSock) { ftp_send(client->sock, "425 Can't open data connection\r\n"); return; }

        std::ifstream input(resolved_path.c_str(), std::ios::binary);
        if (!input) { sock_close(data_sock); ftp_send(client->sock, "550 File not found\r\n"); return; }

        char buffer[8192];
        while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
            ::send(data_sock, buffer, static_cast<int>(input.gcount()), 0);
        }
        sock_close(data_sock);
        ftp_send(client->sock, "226 Transfer complete\r\n");
    }

    void stor_cmd(std::shared_ptr<Client> client, const std::string& arg) {
        if (!require_auth(client)) return;
        if (!require_perm(client, ftp_permission::write)) return;
        if (arg.empty()) { ftp_send(client->sock, "501 Syntax error\r\n"); return; }

        std::string resolved_path = resolve(client->phy_root, client->cwd, arg);
        if (resolved_path.empty()) { ftp_send(client->sock, "550 Invalid path\r\n"); return; }

        ftp_send(client->sock, "150 Opening data connection\r\n");

        raw_socket data_sock = accept_data(client);
        if (data_sock == kInvalidSock) { ftp_send(client->sock, "425 Can't open data connection\r\n"); return; }

        std::ofstream output(resolved_path.c_str(), std::ios::binary);
        if (!output) { sock_close(data_sock); ftp_send(client->sock, "550 Cannot create file\r\n"); return; }

        char buffer[8192];
        int received_count;
        while ((received_count = ::recv(data_sock, buffer, sizeof(buffer), 0)) > 0) {
            output.write(buffer, received_count);
        }
        output.close();
        sock_close(data_sock);
        ftp_send(client->sock, "226 Transfer complete\r\n");
    }

    void dele_cmd(std::shared_ptr<Client> client, const std::string& arg) {
        if (!require_auth(client)) return;
        if (!require_perm(client, ftp_permission::write)) return;
        if (arg.empty()) { ftp_send(client->sock, "501 Syntax error\r\n"); return; }

        std::string resolved_path = resolve(client->phy_root, client->cwd, arg);
        if (resolved_path.empty()) { ftp_send(client->sock, "550 File not found\r\n"); return; }

#ifdef _WIN32
        if (DeleteFileA(resolved_path.c_str())) {
            ftp_send(client->sock, "250 File deleted\r\n");
        } else {
            ftp_send(client->sock, "550 File not found\r\n");
        }
#else
        if (unlink(resolved_path.c_str()) == 0) {
            ftp_send(client->sock, "250 File deleted\r\n");
        } else {
            ftp_send(client->sock, "550 File not found\r\n");
        }
#endif
    }

    void mkd_cmd(std::shared_ptr<Client> client, const std::string& arg) {
        if (!require_auth(client)) return;
        if (!require_perm(client, ftp_permission::write)) return;
        if (arg.empty()) { ftp_send(client->sock, "501 Syntax error\r\n"); return; }

        std::string resolved_path = resolve(client->phy_root, client->cwd, arg);
        if (resolved_path.empty()) { ftp_send(client->sock, "550 Invalid path\r\n"); return; }

#ifdef _WIN32
        if (_mkdir(resolved_path.c_str()) == 0) {
            ftp_send(client->sock, "257 \"" + arg + "\" created\r\n");
        } else {
            ftp_send(client->sock, "550 Cannot create directory\r\n");
        }
#else
        if (mkdir(resolved_path.c_str(), 0700) == 0) {
            ftp_send(client->sock, "257 \"" + arg + "\" created\r\n");
        } else {
            ftp_send(client->sock, "550 Cannot create directory\r\n");
        }
#endif
    }

    void rmd_cmd(std::shared_ptr<Client> client, const std::string& arg) {
        if (!require_auth(client)) return;
        if (!require_perm(client, ftp_permission::write)) return;
        if (arg.empty()) { ftp_send(client->sock, "501 Syntax error\r\n"); return; }

        std::string resolved_path = resolve(client->phy_root, client->cwd, arg);
        if (resolved_path.empty()) { ftp_send(client->sock, "550 Invalid path\r\n"); return; }

#ifdef _WIN32
        if (RemoveDirectoryA(resolved_path.c_str())) {
            ftp_send(client->sock, "250 Directory removed\r\n");
        } else {
            ftp_send(client->sock, "550 Cannot remove directory\r\n");
        }
#else
        if (rmdir(resolved_path.c_str()) == 0) {
            ftp_send(client->sock, "250 Directory removed\r\n");
        } else {
            ftp_send(client->sock, "550 Cannot remove directory\r\n");
        }
#endif
    }

    void cwd_cmd(std::shared_ptr<Client> client, const std::string& arg) {
        if (!require_auth(client)) return;
        if (!require_perm(client, ftp_permission::read)) return;

        if (arg.empty() || arg == "/") {
            client->cwd.clear();
            ftp_send(client->sock, "250 Directory changed\r\n");
            return;
        }

        std::string new_cwd;
        if (arg[0] == '/') {
            new_cwd = arg.substr(1);
        } else {
            new_cwd = client->cwd;
            if (!new_cwd.empty()) new_cwd += "/";
            new_cwd += arg;
        }

        std::vector<std::string> parts;
        std::string current_part;
        for (size_t path_index = 0; path_index <= new_cwd.size(); ++path_index) {
            char character = (path_index < new_cwd.size()) ? new_cwd[path_index] : '/';
            if (character == '/') {
                if (current_part == "..") {
                    if (!parts.empty()) parts.pop_back();
                } else if (!current_part.empty() && current_part != ".") {
                    parts.push_back(current_part);
                }
                current_part.clear();
            } else {
                current_part += character;
            }
        }
        new_cwd.clear();
        for (size_t part_index = 0; part_index < parts.size(); ++part_index) {
            if (part_index > 0) new_cwd += "/";
            new_cwd += parts[part_index];
        }
        client->cwd = new_cwd;
        ftp_send(client->sock, "250 Directory changed\r\n");
    }

    void feat_cmd(std::shared_ptr<Client> client) {
        ftp_send(client->sock, "211-Extensions supported:\r\n");
        ftp_send(client->sock, " PASV\r\n");
        ftp_send(client->sock, "211 End\r\n");
    }

    void ok_if(std::shared_ptr<Client> client, const std::string& msg) {
        if (!require_auth(client)) return;
        ftp_send(client->sock, msg);
    }

    bool require_auth(std::shared_ptr<Client> client) {
        if (!client->auth) {
            ftp_send(client->sock, "530 Please login with USER and PASS\r\n");
            return false;
        }
        return true;
    }

    bool require_perm(std::shared_ptr<Client> client, ftp_permission need) {
        int have_permission = static_cast<int>(client->perm);
        int need_permission = static_cast<int>(need);
        if ((have_permission & need_permission) != need_permission) {
            ftp_send(client->sock, "550 Permission denied\r\n");
            return false;
        }
        return true;
    }

    void ftp_send(raw_socket socket, const std::string& msg) {
        if (socket == kInvalidSock) return;
        ::send(socket, msg.data(), static_cast<int>(msg.size()), 0);
    }

    raw_socket accept_data(std::shared_ptr<Client> client) {
        if (!client->pasv || client->data_sock == kInvalidSock) return kInvalidSock;
        sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        raw_socket data_sock = ::accept(client->data_sock, reinterpret_cast<sockaddr*>(&addr), &addr_len);
        sock_close(client->data_sock);
        client->data_sock = kInvalidSock;
        client->pasv = false;
        return data_sock;
    }

    void close_pasv(Client* client) {
        if (client->data_sock != kInvalidSock) {
            sock_close(client->data_sock);
            client->data_sock = kInvalidSock;
        }
        client->pasv = false;
    }

    void close_client(Client* client) {
        if (client->sock != kInvalidSock) {
            sock_shutdown(client->sock);
            sock_close(client->sock);
            client->sock = kInvalidSock;
        }
        close_pasv(client);
    }

    void remove_client(std::shared_ptr<Client> client) {
        std::lock_guard<std::mutex> lock(clients_mtx_);
        for (auto it = clients_.begin(); it != clients_.end(); ++it) {
            if (*it == client) {
                close_client(it->get());
                clients_.erase(it);
                return;
            }
        }
    }
};

ftp_server::ftp_server() : implementation_(new implementation()) {}

ftp_server::~ftp_server() = default;

void ftp_server::start() { implementation_->start(); }

void ftp_server::stop() { implementation_->stop(); }

std::string ftp_server::base_url() const { return implementation_->base_url(); }

uint16_t ftp_server::port() const { return implementation_->port(); }

void ftp_server::add_user(const std::string& username,
                            const std::string& password,
                            const std::string& root_path,
                            ftp_permission perm) {
    implementation_->add_user(username, password, root_path, perm);
}

void ftp_server::add_anonymous_user(const std::string& root_path,
                                     ftp_permission perm) {
    implementation_->add_anonymous_user(root_path, perm);
}

void ftp_server::clear_users() { implementation_->clear_users(); }

std::string ftp_server::temp_root() const { return implementation_->temp_root(); }

std::vector<std::string> ftp_server::connected_users() const {
    return implementation_->connected_users();
}

}
