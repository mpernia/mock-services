#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mock_services {

enum class sftp_permission : int {
    none  = 0,
    read  = 1,
    write = 2,
    all   = read | write
};

inline sftp_permission operator|(sftp_permission a, sftp_permission b) {
    return static_cast<sftp_permission>(static_cast<int>(a) | static_cast<int>(b));
}

inline sftp_permission operator&(sftp_permission a, sftp_permission b) {
    return static_cast<sftp_permission>(static_cast<int>(a) & static_cast<int>(b));
}

inline sftp_permission operator~(sftp_permission a) {
    return static_cast<sftp_permission>(~static_cast<int>(a));
}

struct sftp_activity {
    std::string user;
    std::string operation;
    std::string path;
    std::string result;
};

class sftp_server {
public:
    sftp_server();
    ~sftp_server();

    sftp_server(const sftp_server&) = delete;
    sftp_server& operator=(const sftp_server&) = delete;

    void start();

    void stop();

    std::string base_url() const;

    uint16_t port() const;

    void add_user(const std::string& username, const std::string& password,
                  const std::string& root_path, sftp_permission perm);

    void clear_users();

    std::string temp_root() const;

    std::vector<std::string> connected_users() const;

    std::vector<sftp_activity> activity() const;

    void clear_activity();

private:
    class implementation;
    std::unique_ptr<implementation> implementation_;
};

}
