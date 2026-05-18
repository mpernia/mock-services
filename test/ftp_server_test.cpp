#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <fileapi.h>
#include <io.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <dirent.h>
#endif

#include "mock-services/ftp_common.h"
#include "mock-services/ftp_server.h"

namespace ftp = mock_services::ftp;





#ifdef _WIN32
using test_socket_t = SOCKET;
constexpr test_socket_t test_invalid_socket = INVALID_SOCKET;
constexpr int test_socket_error = SOCKET_ERROR;

static void test_close_socket(test_socket_t socket) { closesocket(socket); }

class TestWsaGuard {
public:
    TestWsaGuard() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~TestWsaGuard() { WSACleanup(); }
};
#else
using test_socket_t = int;
constexpr test_socket_t test_invalid_socket = -1;
constexpr int test_socket_error = -1;
static void test_close_socket(test_socket_t socket) { close(socket); }
class TestWsaGuard {
public:
    TestWsaGuard() {}
};
#endif





class test_ftp_client {
public:
    test_ftp_client() : sock_(test_invalid_socket) {}

    ~test_ftp_client() { disconnect(); }

    bool connect(const std::string& host, uint16_t port) {
        sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ == test_invalid_socket) return false;

        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
#ifdef _WIN32
        addr.sin_addr.s_addr = inet_addr(host.c_str());
#else
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
#endif

        if (::connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
            test_socket_error) {
            test_close_socket(sock_);
            sock_ = test_invalid_socket;
            return false;
        }


        read_response();
        return true;
    }

    void disconnect() {
        if (sock_ != test_invalid_socket) {
            send_raw("QUIT\r\n");
            test_close_socket(sock_);
            sock_ = test_invalid_socket;
        }
    }

    int command(const std::string& cmd) {
        send_raw(cmd + "\r\n");
        return read_response();
    }

    int last_code() const { return last_code_; }
    const std::string& last_text() const { return last_text_; }

    bool login(const std::string& user, const std::string& pass) {
        int code = command("USER " + user);
        if (code == ftp::status::logged_in) return true;
        if (code != ftp::status::user_ok) return false;
        return command("PASS " + pass) == ftp::status::logged_in;
    }

    bool stor(const std::string& path, const std::string& data) {
        if (!enter_pasv()) return false;
        int status_code = command("STOR " + path);
        if (status_code != ftp::status::file_ok && status_code != 125) return false;

        test_socket_t data_sock = connect_pasv();
        if (data_sock == test_invalid_socket) return false;
        ::send(data_sock, data.data(), static_cast<int>(data.size()), 0);
        test_close_socket(data_sock);

        return read_response() == ftp::status::transfer_complete;
    }

    std::string retr(const std::string& path) {
        if (!enter_pasv()) return {};
        int status_code = command("RETR " + path);
        if (status_code != ftp::status::file_ok && status_code != 125) return {};

        std::string result;
        test_socket_t data_sock = connect_pasv();
        if (data_sock == test_invalid_socket) return {};
        char buffer[4096];
        int received_count;
        while ((received_count = ::recv(data_sock, buffer, sizeof(buffer), 0)) > 0) {
            result.append(buffer, received_count);
        }
        test_close_socket(data_sock);
        read_response();
        return result;
    }

    std::string list(const std::string& path = "") {
        if (!enter_pasv()) return {};
        int status_code = path.empty() ? command("LIST") : command("LIST " + path);
        if (status_code != ftp::status::file_ok && status_code != 125) return {};

        std::string result;
        test_socket_t data_sock = connect_pasv();
        if (data_sock == test_invalid_socket) return {};
        char buffer[4096];
        int received_count;
        while ((received_count = ::recv(data_sock, buffer, sizeof(buffer), 0)) > 0) {
            result.append(buffer, received_count);
        }
        test_close_socket(data_sock);
        read_response();
        return result;
    }

    bool dele(const std::string& path) {
        return command("DELE " + path) == ftp::status::file_deleted;
    }

    bool mkd(const std::string& path) {
        return command("MKD " + path) == ftp::status::path_created;
    }

    bool rmd(const std::string& path) {
        return command("RMD " + path) == ftp::status::file_deleted;
    }

private:
    test_socket_t sock_;
    int last_code_ = 0;
    std::string last_text_;
    uint16_t pasv_port_ = 0;
    std::string pasv_host_;

    void send_raw(const std::string& data) {
        ::send(sock_, data.data(), static_cast<int>(data.size()), 0);
    }

    int read_response() {
        char buffer[4096];
        int received_count = ::recv(sock_, buffer, sizeof(buffer) - 1, 0);
        if (received_count <= 0) {
            last_code_ = 0;
            last_text_.clear();
            return 0;
        }
        buffer[received_count] = 0;
        last_text_ = std::string(buffer, received_count);

        if (received_count >= 3 && std::isdigit(static_cast<unsigned char>(buffer[0])) &&
            std::isdigit(static_cast<unsigned char>(buffer[1])) &&
            std::isdigit(static_cast<unsigned char>(buffer[2]))) {
            last_code_ = std::stoi(std::string(buffer, 3));
        }

        if (received_count > 3 && buffer[3] == '-') {
            std::string expected = std::string(buffer, 3) + " ";
            while (true) {
                received_count = ::recv(sock_, buffer, sizeof(buffer) - 1, 0);
                if (received_count <= 0) break;
                buffer[received_count] = 0;
                last_text_ += buffer;
                if (static_cast<size_t>(received_count) >= 4 &&
                    buffer[0] == expected[0] && buffer[1] == expected[1] &&
                    buffer[2] == expected[2] && buffer[3] == ' ') {
                    break;
                }
            }
        }
        return last_code_;
    }

    bool enter_pasv() {
        int status_code = command("PASV");
        if (status_code != ftp::status::passive_mode) return false;

        auto paren = last_text_.find('(');
        auto end = last_text_.find(')');
        if (paren == std::string::npos || end == std::string::npos) return false;

        std::string passive_parts = last_text_.substr(paren + 1, end - paren - 1);
        std::replace(passive_parts.begin(), passive_parts.end(), ',', ' ');
        std::istringstream passive_stream(passive_parts);
        int host_octet_1 = 0;
        int host_octet_2 = 0;
        int host_octet_3 = 0;
        int host_octet_4 = 0;
        int port_high = 0;
        int port_low = 0;
        if (!(passive_stream >> host_octet_1 >> host_octet_2 >> host_octet_3 >> host_octet_4 >> port_high >> port_low)) return false;

        pasv_host_ = std::to_string(host_octet_1) + "." + std::to_string(host_octet_2) + "." +
                     std::to_string(host_octet_3) + "." + std::to_string(host_octet_4);
        pasv_port_ = static_cast<uint16_t>(port_high * 256 + port_low);
        return true;
    }

    test_socket_t connect_pasv() {
        test_socket_t data_sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (data_sock == test_invalid_socket) return test_invalid_socket;

        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(pasv_port_);
#ifdef _WIN32
        addr.sin_addr.s_addr = inet_addr(pasv_host_.c_str());
#else
        inet_pton(AF_INET, pasv_host_.c_str(), &addr.sin_addr);
#endif

        if (::connect(data_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
            test_socket_error) {
            test_close_socket(data_sock);
            return test_invalid_socket;
        }
        return data_sock;
    }
};






static bool parse_ftp_url(const std::string& url, std::string& host,
                          uint16_t& port) {

    if (url.find("ftp://") != 0) return false;
    std::string rest = url.substr(6);
    auto colon = rest.rfind(':');
    if (colon == std::string::npos) return false;
    host = rest.substr(0, colon);
    port = static_cast<uint16_t>(std::stoul(rest.substr(colon + 1)));
    return true;
}


static bool write_test_file(const std::string& path, const std::string& content) {
    std::ofstream ofs(path.c_str(), std::ios::binary);
    if (!ofs) return false;
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    return ofs.good();
}


static std::string read_test_file(const std::string& path) {
    std::ifstream ifs(path.c_str(), std::ios::binary);
    if (!ifs) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}


static bool path_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}







TEST(ftp_server, create_destroy) {
    mock_services::ftp_server srv;

}

TEST(ftp_server, copy_is_deleted) {
    EXPECT_FALSE(
        std::is_copy_constructible<mock_services::ftp_server>::value);
    EXPECT_FALSE(std::is_copy_assignable<mock_services::ftp_server>::value);
}

TEST(ftp_server, start_stop) {
    TestWsaGuard wsa;
    mock_services::ftp_server srv;
    srv.start();
    EXPECT_TRUE(srv.base_url().find("ftp://127.0.0.1:") == 0);
    EXPECT_GT(srv.port(), 0);
    srv.stop();
}

TEST(ftp_server, stop_is_idempotent) {
    mock_services::ftp_server srv;
    srv.start();
    srv.stop();

    srv.stop();
}

TEST(ftp_server, destructor_stops) {
    TestWsaGuard wsa;
    {
        mock_services::ftp_server srv;
        srv.start();

    }
}

TEST(ftp_server, start_throws_already_started) {
    mock_services::ftp_server srv;
    srv.start();
    EXPECT_THROW(srv.start(), std::runtime_error);
    srv.stop();
}

TEST(ftp_server, stop_before_start_ok) {
    mock_services::ftp_server srv;

    srv.stop();
}



TEST(ftp_server, temp_root_exists) {
    mock_services::ftp_server srv;
    std::string root = srv.temp_root();
    EXPECT_FALSE(root.empty());
    EXPECT_TRUE(path_exists(root));
}

TEST(ftp_server, temp_root_cleaned_on_destroy) {
    std::string root;
    {
        mock_services::ftp_server srv;
        root = srv.temp_root();
        EXPECT_TRUE(path_exists(root));
    }

    EXPECT_FALSE(path_exists(root));
}



TEST(ftp_server, add_user) {
    TestWsaGuard wsa;
    mock_services::ftp_server srv;
    srv.start();

    srv.add_user("alice", "secret", "/alice",
                 mock_services::ftp_permission::all);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_ftp_url(srv.base_url(), host, port));


    test_ftp_client cli;
    EXPECT_TRUE(cli.connect(host, port));
    EXPECT_TRUE(cli.login("alice", "secret"));

    srv.stop();
}

TEST(ftp_server, add_anonymous_user) {
    TestWsaGuard wsa;
    mock_services::ftp_server srv;
    srv.start();

    srv.add_anonymous_user("/pub", mock_services::ftp_permission::read);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_ftp_url(srv.base_url(), host, port));

    test_ftp_client cli;
    EXPECT_TRUE(cli.connect(host, port));

    EXPECT_TRUE(cli.login("anonymous", "guest@example.com"));

    srv.stop();
}

TEST(ftp_server, invalid_login_returns_530) {
    TestWsaGuard wsa;
    mock_services::ftp_server srv;
    srv.start();

    srv.add_user("alice", "secret", "/alice",
                 mock_services::ftp_permission::all);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_ftp_url(srv.base_url(), host, port));

    test_ftp_client cli;
    EXPECT_TRUE(cli.connect(host, port));

    EXPECT_EQ(cli.command("USER alice"), ftp::status::user_ok);
    EXPECT_EQ(cli.command("PASS wrongpass"), ftp::status::login_incorrect);

    srv.stop();
}

TEST(ftp_server, duplicate_user_overwrites) {
    TestWsaGuard wsa;
    mock_services::ftp_server srv;
    srv.start();


    srv.add_user("alice", "secret", "/alice",
                 mock_services::ftp_permission::all);
    srv.add_user("alice", "newsecret", "/alice",
                 mock_services::ftp_permission::read);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_ftp_url(srv.base_url(), host, port));


    test_ftp_client cli;
    EXPECT_TRUE(cli.connect(host, port));
    EXPECT_TRUE(cli.login("alice", "newsecret"));

    srv.stop();
}

TEST(ftp_server, clear_users) {
    TestWsaGuard wsa;
    mock_services::ftp_server srv;
    srv.start();

    srv.add_user("alice", "secret", "/alice",
                 mock_services::ftp_permission::all);
    srv.clear_users();

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_ftp_url(srv.base_url(), host, port));

    test_ftp_client cli;
    EXPECT_TRUE(cli.connect(host, port));
    EXPECT_EQ(cli.command("USER alice"), ftp::status::login_incorrect);

    srv.stop();
}



TEST(ftp_server, stor_creates_file) {
    TestWsaGuard wsa;
    mock_services::ftp_server srv;
    srv.start();

    srv.add_user("alice", "secret", "/",
                 mock_services::ftp_permission::all);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_ftp_url(srv.base_url(), host, port));

    test_ftp_client cli;
    EXPECT_TRUE(cli.connect(host, port));
    ASSERT_TRUE(cli.login("alice", "secret"));

    EXPECT_TRUE(cli.stor("test_stor.txt", "Hello FTP World!"));


    std::string file_path = srv.temp_root() + "/test_stor.txt";
    EXPECT_TRUE(path_exists(file_path));
    EXPECT_EQ(read_test_file(file_path), "Hello FTP World!");

    srv.stop();
}

TEST(ftp_server, retr_downloads_file) {
    TestWsaGuard wsa;
    mock_services::ftp_server srv;
    srv.start();

    srv.add_user("alice", "secret", "/",
                 mock_services::ftp_permission::all);


    std::string file_path = srv.temp_root() + "/test_retr.txt";
    ASSERT_TRUE(write_test_file(file_path, "Content for RETR test"));

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_ftp_url(srv.base_url(), host, port));

    test_ftp_client cli;
    EXPECT_TRUE(cli.connect(host, port));
    ASSERT_TRUE(cli.login("alice", "secret"));

    std::string data = cli.retr("test_retr.txt");
    EXPECT_EQ(data, "Content for RETR test");

    srv.stop();
}

TEST(ftp_server, list_shows_files) {
    TestWsaGuard wsa;
    mock_services::ftp_server srv;
    srv.start();

    srv.add_user("alice", "secret", "/",
                 mock_services::ftp_permission::all);


    ASSERT_TRUE(write_test_file(srv.temp_root() + "/file_a.txt", "aaa"));
    ASSERT_TRUE(write_test_file(srv.temp_root() + "/file_b.txt", "bbb"));

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_ftp_url(srv.base_url(), host, port));

    test_ftp_client cli;
    EXPECT_TRUE(cli.connect(host, port));
    ASSERT_TRUE(cli.login("alice", "secret"));

    std::string listing = cli.list();
    EXPECT_FALSE(listing.empty());
    EXPECT_NE(listing.find("file_a.txt"), std::string::npos);
    EXPECT_NE(listing.find("file_b.txt"), std::string::npos);

    srv.stop();
}

TEST(ftp_server, dele_removes_file) {
    TestWsaGuard wsa;
    mock_services::ftp_server srv;
    srv.start();

    srv.add_user("alice", "secret", "/",
                 mock_services::ftp_permission::all);


    std::string file_path = srv.temp_root() + "/old.txt";
    ASSERT_TRUE(write_test_file(file_path, "delete me"));
    ASSERT_TRUE(path_exists(file_path));

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_ftp_url(srv.base_url(), host, port));

    test_ftp_client cli;
    EXPECT_TRUE(cli.connect(host, port));
    ASSERT_TRUE(cli.login("alice", "secret"));

    EXPECT_TRUE(cli.dele("old.txt"));
    EXPECT_FALSE(path_exists(file_path));

    srv.stop();
}

TEST(ftp_server, mkd_creates_directory) {
    TestWsaGuard wsa;
    mock_services::ftp_server srv;
    srv.start();

    srv.add_user("alice", "secret", "/",
                 mock_services::ftp_permission::all);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_ftp_url(srv.base_url(), host, port));

    test_ftp_client cli;
    EXPECT_TRUE(cli.connect(host, port));
    ASSERT_TRUE(cli.login("alice", "secret"));

    EXPECT_TRUE(cli.mkd("newdir"));

    std::string dir_path = srv.temp_root() + "/newdir";
    EXPECT_TRUE(path_exists(dir_path));

    srv.stop();
}

TEST(ftp_server, rmd_removes_directory) {
    TestWsaGuard wsa;
    mock_services::ftp_server srv;
    srv.start();

    srv.add_user("alice", "secret", "/",
                 mock_services::ftp_permission::all);


    std::string dir_path = srv.temp_root() + "/emptydir";
#ifdef _WIN32
    _mkdir(dir_path.c_str());
#else
    mkdir(dir_path.c_str(), 0777);
#endif
    ASSERT_TRUE(path_exists(dir_path));

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_ftp_url(srv.base_url(), host, port));

    test_ftp_client cli;
    EXPECT_TRUE(cli.connect(host, port));
    ASSERT_TRUE(cli.login("alice", "secret"));

    EXPECT_TRUE(cli.rmd("emptydir"));
    EXPECT_FALSE(path_exists(dir_path));

    srv.stop();
}



TEST(ftp_server, permission_denied_write) {
    TestWsaGuard wsa;
    mock_services::ftp_server srv;
    srv.start();


    srv.add_user("reader", "pass", "/",
                 mock_services::ftp_permission::read);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_ftp_url(srv.base_url(), host, port));

    test_ftp_client cli;
    EXPECT_TRUE(cli.connect(host, port));
    ASSERT_TRUE(cli.login("reader", "pass"));


    EXPECT_FALSE(cli.stor("test.txt", "data"));


    int code = cli.last_code();
    EXPECT_TRUE(code == ftp::status::permission_denied || code == ftp::status::file_unavailable_name)
        << "Expected 550 or 553 but got " << code;

    srv.stop();
}

TEST(ftp_server, permission_denied_dele) {
    TestWsaGuard wsa;
    mock_services::ftp_server srv;
    srv.start();

    srv.add_user("reader", "pass", "/",
                 mock_services::ftp_permission::read);

    std::string file_path = srv.temp_root() + "/readonly.txt";
    ASSERT_TRUE(write_test_file(file_path, "cannot delete"));
    ASSERT_TRUE(path_exists(file_path));

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_ftp_url(srv.base_url(), host, port));

    test_ftp_client cli;
    EXPECT_TRUE(cli.connect(host, port));
    ASSERT_TRUE(cli.login("reader", "pass"));

    EXPECT_FALSE(cli.dele("readonly.txt"));
    EXPECT_EQ(ftp::status::permission_denied, cli.last_code());
    EXPECT_TRUE(path_exists(file_path));

    srv.stop();
}

TEST(ftp_server, file_not_found_returns_550) {
    TestWsaGuard wsa;
    mock_services::ftp_server srv;
    srv.start();

    srv.add_user("alice", "secret", "/",
                 mock_services::ftp_permission::all);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_ftp_url(srv.base_url(), host, port));

    test_ftp_client cli;
    EXPECT_TRUE(cli.connect(host, port));
    ASSERT_TRUE(cli.login("alice", "secret"));


    std::string data = cli.retr("nonexistent.txt");
    EXPECT_TRUE(data.empty());

    int code = cli.last_code();
    EXPECT_TRUE(code == ftp::status::file_unavailable || code == 0)
        << "Expected 550 but got " << code;

    srv.stop();
}



TEST(ftp_server, two_servers_different_ports) {
    TestWsaGuard wsa;
    mock_services::ftp_server srv1;
    mock_services::ftp_server srv2;

    srv1.start();
    srv2.start();

    EXPECT_GT(srv1.port(), 0);
    EXPECT_GT(srv2.port(), 0);
    EXPECT_NE(srv1.port(), srv2.port());
    EXPECT_NE(srv1.base_url(), srv2.base_url());


    std::string host1, host2;
    uint16_t port1 = 0, port2 = 0;
    ASSERT_TRUE(parse_ftp_url(srv1.base_url(), host1, port1));
    ASSERT_TRUE(parse_ftp_url(srv2.base_url(), host2, port2));

    {
        test_ftp_client cli1;
        EXPECT_TRUE(cli1.connect(host1, port1));
    }
    {
        test_ftp_client cli2;
        EXPECT_TRUE(cli2.connect(host2, port2));
    }

    srv1.stop();
    srv2.stop();
}



TEST(ftp_server, connected_users_tracks_logins) {
    TestWsaGuard wsa;
    mock_services::ftp_server srv;
    srv.start();

    srv.add_user("alice", "secret", "/",
                 mock_services::ftp_permission::all);
    srv.add_user("bob", "pass", "/",
                 mock_services::ftp_permission::read);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_ftp_url(srv.base_url(), host, port));


    EXPECT_TRUE(srv.connected_users().empty());


    {
        test_ftp_client cli;
        ASSERT_TRUE(cli.connect(host, port));
        ASSERT_TRUE(cli.login("alice", "secret"));

        auto users = srv.connected_users();
        EXPECT_FALSE(users.empty());
        EXPECT_NE(std::find(users.begin(), users.end(), "alice"),
                  users.end());
    }

    srv.stop();
}
