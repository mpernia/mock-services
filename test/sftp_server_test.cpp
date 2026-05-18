#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
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
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <dirent.h>
#endif


#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include "mock-services/sftp_server.h"
#include "mock-services/sftp_common.h"

namespace sftp = mock_services::sftp;





#ifdef _WIN32
class TestWsaGuard {
public:
    TestWsaGuard() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~TestWsaGuard() { WSACleanup(); }
};
#else
class TestWsaGuard {
public:
    TestWsaGuard() {}
};
#endif






static bool parse_sftp_url(const std::string& url,
                            std::string& host,
                            uint16_t& port) {
    if (url.find("sftp://") != 0) return false;
    std::string rest = url.substr(7);
    auto colon = rest.rfind(':');
    if (colon == std::string::npos) return false;
    host = rest.substr(0, colon);
    port = static_cast<uint16_t>(std::stoul(rest.substr(colon + 1)));
    return true;
}


static bool write_test_file(const std::string& path,
                             const std::string& content) {
    std::ofstream ofs(path.c_str(), std::ios::binary);
    if (!ofs) return false;
    ofs.write(content.data(),
              static_cast<std::streamsize>(content.size()));
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





class test_sftp_client {
public:
    test_sftp_client() : session_(nullptr), sftp_(nullptr) {}

    ~test_sftp_client() { disconnect(); }


    bool connect(const std::string& host, uint16_t port,
                 const std::string& user, const std::string& pass,
                 bool report_errors = true) {
        session_ = ssh_new();
        if (!session_) return false;

        ssh_options_set(session_, SSH_OPTIONS_HOST, host.c_str());
        int port_int = static_cast<int>(port);
        ssh_options_set(session_, SSH_OPTIONS_PORT, &port_int);
        ssh_options_set(session_, SSH_OPTIONS_USER, user.c_str());
        ssh_options_set(session_, SSH_OPTIONS_STRICTHOSTKEYCHECK, "no");
        int log_verbosity = SSH_LOG_PROTOCOL;
        ssh_options_set(session_, SSH_OPTIONS_LOG_VERBOSITY, &log_verbosity);

        if (ssh_connect(session_) != SSH_OK) {
            if (report_errors) {
                ADD_FAILURE() << "ssh_connect failed: "
                              << ssh_get_error(session_);
            }
            ssh_free(session_);
            session_ = nullptr;
            return false;
        }

        if (ssh_userauth_password(session_, nullptr, pass.c_str())
                != SSH_AUTH_SUCCESS) {
            if (report_errors) {
                ADD_FAILURE() << "ssh_userauth_password failed: "
                              << ssh_get_error(session_);
            }
            ssh_disconnect(session_);
            ssh_free(session_);
            session_ = nullptr;
            return false;
        }

        sftp_ = sftp_new(session_);
        if (!sftp_ || sftp_init(sftp_) != SSH_OK) {
            std::string err_msg;
            if (sftp_) {
                err_msg = "sftp_init failed: ";
                err_msg += ssh_get_error(session_);
                sftp_free(sftp_);
                sftp_ = nullptr;
            } else {
                err_msg = "sftp_new failed: ";
                err_msg += ssh_get_error(session_);
            }
            if (report_errors) {
                ADD_FAILURE() << err_msg;
            }
            ssh_disconnect(session_);
            ssh_free(session_);
            session_ = nullptr;
            return false;
        }

        return true;
    }


    void disconnect() {
        if (sftp_) {
            sftp_free(sftp_);
            sftp_ = nullptr;
        }
        if (session_) {
            ssh_disconnect(session_);
            ssh_free(session_);
            session_ = nullptr;
        }
    }

    bool is_connected() const { return sftp_ != nullptr; }


    bool write_file(const std::string& path, const std::string& data) {
        if (!sftp_) return false;

        int access_type = O_WRONLY | O_CREAT | O_TRUNC;
        mode_t mode = 0644;

        sftp_file file = sftp_open(sftp_, path.c_str(), access_type, mode);
        if (!file) return false;

        ssize_t written = sftp_write(file, data.data(), data.size());
        bool ok = (written == static_cast<ssize_t>(data.size()));
        int rc = sftp_close(file);

        return ok && (rc == SSH_OK);
    }


    std::string read_file(const std::string& path) {
        if (!sftp_) return {};

        sftp_file file = sftp_open(sftp_, path.c_str(), O_RDONLY, 0);
        if (!file) return {};

        std::string result;
        char buffer[4096];
        ssize_t read_count;
        while ((read_count = sftp_read(file, buffer, sizeof(buffer))) > 0) {
            result.append(buffer, static_cast<size_t>(read_count));
        }

        sftp_close(file);
        return result;
    }


    bool delete_file(const std::string& path) {
        if (!sftp_) return false;
        return sftp_unlink(sftp_, path.c_str()) == SSH_OK;
    }


    bool make_dir(const std::string& path) {
        if (!sftp_) return false;
        return sftp_mkdir(sftp_, path.c_str(), 0755) == SSH_OK;
    }


    bool remove_dir(const std::string& path) {
        if (!sftp_) return false;
        return sftp_rmdir(sftp_, path.c_str()) == SSH_OK;
    }


    bool stat(const std::string& path, sftp_attributes& attr) {
        if (!sftp_) return false;
        attr = sftp_stat(sftp_, path.c_str());
        return attr != nullptr;
    }


    std::string realpath(const std::string& path) {
        if (!sftp_) return {};
        char* resolved_path = sftp_canonicalize_path(sftp_, path.c_str());
        if (!resolved_path) return {};
        std::string result(resolved_path);
        ssh_string_free_char(resolved_path);
        return result;
    }


    std::vector<std::string> list_dir(const std::string& path) {
        std::vector<std::string> entries;
        if (!sftp_) return entries;

        sftp_dir dir = sftp_opendir(sftp_, path.c_str());
        if (!dir) return entries;

        sftp_attributes attr;
        while ((attr = sftp_readdir(sftp_, dir)) != nullptr) {
            const char* name = attr->name;
            if (name && std::strcmp(name, ".") != 0
                     && std::strcmp(name, "..") != 0) {
                entries.push_back(name);
            }
            sftp_attributes_free(attr);
        }

        sftp_closedir(dir);
        return entries;
    }


    std::string last_error() const {
        if (!sftp_) return "no session";
        return ssh_get_error(sftp_->session);
    }

private:
    ssh_session session_;
    sftp_session sftp_;
};







TEST(sftp_server, create_destroy) {
    mock_services::sftp_server srv;

}

TEST(sftp_server, copy_is_deleted) {
    EXPECT_FALSE(
        std::is_copy_constructible<mock_services::sftp_server>::value);
    EXPECT_FALSE(std::is_copy_assignable<mock_services::sftp_server>::value);
}

TEST(sftp_server, start_stop) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();
    EXPECT_GT(srv.port(), 0);
    EXPECT_TRUE(srv.base_url().find("sftp://127.0.0.1:") == 0);
    srv.stop();
}

TEST(sftp_server, stop_is_idempotent) {
    mock_services::sftp_server srv;
    srv.start();
    srv.stop();

    srv.stop();
}

TEST(sftp_server, destructor_stops) {
    TestWsaGuard wsa;
    {
        mock_services::sftp_server srv;
        srv.start();

    }
}

TEST(sftp_server, start_throws_already_started) {
    mock_services::sftp_server srv;
    srv.start();
    EXPECT_THROW(srv.start(), std::runtime_error);
    srv.stop();
}

TEST(sftp_server, stop_before_start_ok) {
    mock_services::sftp_server srv;

    srv.stop();
}



TEST(sftp_server, add_and_connect) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("alice", "secret", root,
                 mock_services::sftp_permission::all);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_sftp_url(srv.base_url(), host, port));

    test_sftp_client cli;

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_TRUE(cli.connect(host, port, "alice", "secret"));
    EXPECT_TRUE(cli.is_connected());

    srv.stop();
}

TEST(sftp_server, invalid_login_rejected) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("alice", "secret", root,
                 mock_services::sftp_permission::all);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_sftp_url(srv.base_url(), host, port));


    test_sftp_client cli;
    EXPECT_FALSE(cli.connect(host, port, "alice", "wrongpass", false));


    test_sftp_client cli2;
    EXPECT_FALSE(cli2.connect(host, port, "bogus", "anything", false));

    srv.stop();
}

TEST(sftp_server, clear_users_rejects_previous) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("alice", "secret", root,
                 mock_services::sftp_permission::all);
    srv.clear_users();

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_sftp_url(srv.base_url(), host, port));

    test_sftp_client cli;
    EXPECT_FALSE(cli.connect(host, port, "alice", "secret", false));

    srv.stop();
}

TEST(sftp_server, duplicate_user_overwrites) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("alice", "secret", root,
                 mock_services::sftp_permission::all);

    srv.add_user("alice", "newsecret", root,
                 mock_services::sftp_permission::read);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_sftp_url(srv.base_url(), host, port));


    {
        test_sftp_client cli;
        EXPECT_FALSE(cli.connect(host, port, "alice", "secret", false));
    }


    {
        test_sftp_client cli;
        EXPECT_TRUE(cli.connect(host, port, "alice", "newsecret"));
    }

    srv.stop();
}



TEST(sftp_server, temp_root_exists) {
    mock_services::sftp_server srv;
    std::string root = srv.temp_root();
    EXPECT_FALSE(root.empty());
    EXPECT_TRUE(path_exists(root));
}

TEST(sftp_server, temp_root_cleaned_on_destroy) {
    std::string root;
    {
        mock_services::sftp_server srv;
        root = srv.temp_root();
        EXPECT_TRUE(path_exists(root));
    }

    EXPECT_FALSE(path_exists(root));
}






TEST(sftp_server, upload_file) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("alice", "secret", root,
                 mock_services::sftp_permission::all);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_sftp_url(srv.base_url(), host, port));

    test_sftp_client cli;
    ASSERT_TRUE(cli.connect(host, port, "alice", "secret"));


    EXPECT_TRUE(cli.write_file("/uploaded.txt", "Hello SFTP World!"));


    std::string file_path = root + "/uploaded.txt";
    EXPECT_TRUE(path_exists(file_path));
    EXPECT_EQ(read_test_file(file_path), "Hello SFTP World!");

    srv.stop();
}

TEST(sftp_server, download_file) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("alice", "secret", root,
                 mock_services::sftp_permission::all);


    ASSERT_TRUE(write_test_file(root + "/download_me.txt", "Content"));

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_sftp_url(srv.base_url(), host, port));

    test_sftp_client cli;
    ASSERT_TRUE(cli.connect(host, port, "alice", "secret"));

    std::string data = cli.read_file("/download_me.txt");
    EXPECT_EQ(data, "Content");

    srv.stop();
}

TEST(sftp_server, list_directory) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("alice", "secret", root,
                 mock_services::sftp_permission::all);


    ASSERT_TRUE(write_test_file(root + "/list_a.txt", "aaa"));
    ASSERT_TRUE(write_test_file(root + "/list_b.txt", "bbb"));

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_sftp_url(srv.base_url(), host, port));

    test_sftp_client cli;
    ASSERT_TRUE(cli.connect(host, port, "alice", "secret"));


    std::vector<std::string> entries = cli.list_dir("/");
    EXPECT_FALSE(entries.empty());
    EXPECT_NE(std::find(entries.begin(), entries.end(), "list_a.txt"),
              entries.end());
    EXPECT_NE(std::find(entries.begin(), entries.end(), "list_b.txt"),
              entries.end());

    srv.stop();
}

TEST(sftp_server, stat_file) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("alice", "secret", root,
                 mock_services::sftp_permission::all);


    std::string content = "stat test content";
    ASSERT_TRUE(write_test_file(root + "/stat_me.txt", content));

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_sftp_url(srv.base_url(), host, port));

    test_sftp_client cli;
    ASSERT_TRUE(cli.connect(host, port, "alice", "secret"));

    sftp_attributes attr = nullptr;
    EXPECT_TRUE(cli.stat("/stat_me.txt", attr));
    ASSERT_NE(attr, nullptr);
    EXPECT_EQ(static_cast<uint64_t>(attr->size),
              static_cast<uint64_t>(content.size()));
    EXPECT_TRUE(attr->permissions & S_IFREG);
    sftp_attributes_free(attr);

    srv.stop();
}

TEST(sftp_server, stat_nonexistent_returns_null) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("alice", "secret", root,
                 mock_services::sftp_permission::all);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_sftp_url(srv.base_url(), host, port));

    test_sftp_client cli;
    ASSERT_TRUE(cli.connect(host, port, "alice", "secret"));

    sftp_attributes attr = nullptr;
    EXPECT_FALSE(cli.stat("/does_not_exist.txt", attr));
    EXPECT_EQ(attr, nullptr);

    srv.stop();
}

TEST(sftp_server, delete_file) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("alice", "secret", root,
                 mock_services::sftp_permission::all);


    std::string file_path = root + "/to_delete.txt";
    ASSERT_TRUE(write_test_file(file_path, "delete me"));
    ASSERT_TRUE(path_exists(file_path));

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_sftp_url(srv.base_url(), host, port));

    test_sftp_client cli;
    ASSERT_TRUE(cli.connect(host, port, "alice", "secret"));

    EXPECT_TRUE(cli.delete_file("/to_delete.txt"));
    EXPECT_FALSE(path_exists(file_path));

    srv.stop();
}

TEST(sftp_server, make_and_remove_directory) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("alice", "secret", root,
                 mock_services::sftp_permission::all);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_sftp_url(srv.base_url(), host, port));

    test_sftp_client cli;
    ASSERT_TRUE(cli.connect(host, port, "alice", "secret"));


    EXPECT_TRUE(cli.make_dir("/sftp_newdir"));

    std::string dir_path = root + "/sftp_newdir";
    EXPECT_TRUE(path_exists(dir_path));


    EXPECT_TRUE(cli.remove_dir("/sftp_newdir"));
    EXPECT_FALSE(path_exists(dir_path));

    srv.stop();
}

TEST(sftp_server, realpath_resolution) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("alice", "secret", root,
                 mock_services::sftp_permission::all);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_sftp_url(srv.base_url(), host, port));

    test_sftp_client cli;
    ASSERT_TRUE(cli.connect(host, port, "alice", "secret"));

    std::string resolved = cli.realpath("/");
    EXPECT_FALSE(resolved.empty());

    EXPECT_EQ(resolved, "/");

    srv.stop();
}

TEST(sftp_server, missing_file_returns_error) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("alice", "secret", root,
                 mock_services::sftp_permission::all);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_sftp_url(srv.base_url(), host, port));

    test_sftp_client cli;
    ASSERT_TRUE(cli.connect(host, port, "alice", "secret"));


    std::string data = cli.read_file("/nonexistent.txt");
    EXPECT_TRUE(data.empty());

    srv.stop();
}



TEST(sftp_server, read_only_can_list_and_read) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("reader", "pass", root,
                 mock_services::sftp_permission::read);


    ASSERT_TRUE(write_test_file(root + "/readable.txt", "can read"));

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_sftp_url(srv.base_url(), host, port));

    test_sftp_client cli;
    ASSERT_TRUE(cli.connect(host, port, "reader", "pass"));


    std::string data = cli.read_file("/readable.txt");
    EXPECT_EQ(data, "can read");


    auto entries = cli.list_dir("/");
    EXPECT_FALSE(entries.empty());

    srv.stop();
}

TEST(sftp_server, read_only_cannot_write) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("reader", "pass", root,
                 mock_services::sftp_permission::read);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_sftp_url(srv.base_url(), host, port));

    test_sftp_client cli;
    ASSERT_TRUE(cli.connect(host, port, "reader", "pass"));


    EXPECT_FALSE(cli.write_file("/should_fail.txt", "data"));


    ASSERT_TRUE(write_test_file(root + "/cant_delete.txt", "readonly"));
    EXPECT_FALSE(cli.delete_file("/cant_delete.txt"));


    EXPECT_FALSE(cli.make_dir("/cant_mkdir"));


    std::string dir_path = root + "/cant_rmdir";
#ifdef _WIN32
    _mkdir(dir_path.c_str());
#else
    mkdir(dir_path.c_str(), 0755);
#endif
    ASSERT_TRUE(path_exists(dir_path));
    EXPECT_FALSE(cli.remove_dir("/cant_rmdir"));

    srv.stop();
}



TEST(sftp_server, activity_records_operations) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("alice", "secret", root,
                 mock_services::sftp_permission::all);


    ASSERT_TRUE(write_test_file(root + "/activity_check.txt",
                                "activity data"));

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_sftp_url(srv.base_url(), host, port));

    test_sftp_client cli;
    ASSERT_TRUE(cli.connect(host, port, "alice", "secret"));


    cli.list_dir("/");
    cli.read_file("/activity_check.txt");


    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto activity = srv.activity();
    EXPECT_FALSE(activity.empty());


    bool found_open = false;
    bool found_readdir = false;
    for (const auto& a : activity) {
        if (a.operation == sftp::operation::opendir) found_readdir = true;
        if (a.operation == sftp::operation::open) found_open = true;
    }
    EXPECT_TRUE(found_readdir);
    EXPECT_TRUE(found_open);

    srv.stop();
}

TEST(sftp_server, clear_activity_resets) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("alice", "secret", root,
                 mock_services::sftp_permission::all);

    srv.clear_activity();
    EXPECT_EQ(srv.activity().size(), 0);

    srv.stop();
}

TEST(sftp_server, activity_order) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("alice", "secret", root,
                 mock_services::sftp_permission::all);

    srv.clear_activity();
    ASSERT_TRUE(srv.activity().empty());


    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_sftp_url(srv.base_url(), host, port));

    {
        test_sftp_client cli;
        ASSERT_TRUE(cli.connect(host, port, "alice", "secret"));
        cli.write_file("/activity_order.txt", "ordered data");
    }


    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto activity = srv.activity();
    EXPECT_FALSE(activity.empty());

    auto it = std::find_if(activity.begin(), activity.end(),
        [](const mock_services::sftp_activity& a) {
            return a.operation == sftp::operation::write;
        });
    EXPECT_NE(it, activity.end());

    srv.stop();
}



TEST(sftp_server, connected_users_tracks_logins) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("alice", "secret", root,
                 mock_services::sftp_permission::all);
    srv.add_user("bob", "pass", root,
                 mock_services::sftp_permission::read);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_sftp_url(srv.base_url(), host, port));


    EXPECT_TRUE(srv.connected_users().empty());


    {
        test_sftp_client cli;
        ASSERT_TRUE(cli.connect(host, port, "alice", "secret"));

        auto users = srv.connected_users();
        EXPECT_FALSE(users.empty());
        EXPECT_NE(std::find(users.begin(), users.end(), "alice"),
                  users.end());
    }

    srv.stop();
}



TEST(sftp_server, two_servers_different_ports) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv1;
    mock_services::sftp_server srv2;

    std::string root1 = srv1.temp_root();
    std::string root2 = srv2.temp_root();

    srv1.add_user("alice", "secret", root1,
                  mock_services::sftp_permission::all);
    srv2.add_user("bob", "pass", root2,
                  mock_services::sftp_permission::all);

    srv1.start();
    srv2.start();

    EXPECT_GT(srv1.port(), 0);
    EXPECT_GT(srv2.port(), 0);
    EXPECT_NE(srv1.port(), srv2.port());
    EXPECT_NE(srv1.base_url(), srv2.base_url());


    std::string host1, host2;
    uint16_t port1 = 0, port2 = 0;
    ASSERT_TRUE(parse_sftp_url(srv1.base_url(), host1, port1));
    ASSERT_TRUE(parse_sftp_url(srv2.base_url(), host2, port2));

    {
        test_sftp_client cli;
        EXPECT_TRUE(cli.connect(host1, port1, "alice", "secret"));
        EXPECT_TRUE(cli.is_connected());
    }
    {
        test_sftp_client cli;
        EXPECT_TRUE(cli.connect(host2, port2, "bob", "pass"));
        EXPECT_TRUE(cli.is_connected());
    }

    srv1.stop();
    srv2.stop();
}



TEST(sftp_server, upload_binary_content) {
    TestWsaGuard wsa;
    mock_services::sftp_server srv;
    srv.start();

    std::string root = srv.temp_root();
    srv.add_user("alice", "secret", root,
                 mock_services::sftp_permission::all);

    std::string host;
    uint16_t port;
    ASSERT_TRUE(parse_sftp_url(srv.base_url(), host, port));

    test_sftp_client cli;
    ASSERT_TRUE(cli.connect(host, port, "alice", "secret"));


    std::string bin_data;
    bin_data.push_back('\0');
    bin_data.push_back('\x01');
    bin_data.push_back('\x02');
    bin_data.push_back('\xff');
    bin_data.push_back('A');

    EXPECT_TRUE(cli.write_file("/binary.bin", bin_data));


    std::string readback = cli.read_file("/binary.bin");
    EXPECT_EQ(readback.size(), bin_data.size());
    EXPECT_EQ(readback, bin_data);

    srv.stop();
}
