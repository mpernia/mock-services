#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mock_services {

enum class ftp_permission : int {
    none  = 0,
    read  = 1,
    write = 2,
    all   = read | write
};

inline ftp_permission operator|(ftp_permission a, ftp_permission b) {
    return static_cast<ftp_permission>(static_cast<int>(a) | static_cast<int>(b));
}

inline ftp_permission operator&(ftp_permission a, ftp_permission b) {
    return static_cast<ftp_permission>(static_cast<int>(a) & static_cast<int>(b));
}

inline ftp_permission operator~(ftp_permission a) {
    return static_cast<ftp_permission>(~static_cast<int>(a));
}

class ftp_server {
public:
    ftp_server();
    ~ftp_server();

    ftp_server(const ftp_server&) = delete;
    ftp_server& operator=(const ftp_server&) = delete;

    void start();

    void stop();

    std::string base_url() const;

    uint16_t port() const;

    void add_user(const std::string& username, const std::string& password,
                  const std::string& root_path, ftp_permission perm);

    void add_anonymous_user(const std::string& root_path, ftp_permission perm);

    void clear_users();

    std::string temp_root() const;

    std::vector<std::string> connected_users() const;

private:
    class implementation;
    std::unique_ptr<implementation> implementation_;
};

}
