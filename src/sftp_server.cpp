#include "mock-services/sftp_server.h"

#if defined(_WIN32) && !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
#include <io.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <dirent.h>
#endif

#include <sys/stat.h>

#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh/callbacks.h>
extern "C" int ssh_handle_packets(ssh_session session, int timeout);
#ifndef SSH_TIMEOUT_USER
#define SSH_TIMEOUT_USER (-2)
#endif
#ifndef SSH_TIMEOUT_NONBLOCKING
#define SSH_TIMEOUT_NONBLOCKING 0
#endif

#ifndef WITH_SERVER
#define WITH_SERVER
#endif
#include <libssh/sftp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "mock-services/detail/path_utils.h"
#include "mock-services/detail/temp_dir.h"
#include "mock-services/detail/sftp_handlers.h"

namespace mock_services {

static ssh_key generate_rsa_key() {
    ssh_key raw = nullptr;
    ssh_pki_ctx pk = ssh_pki_ctx_new();
    if (pk == nullptr)
        throw std::runtime_error("Failed to create PKI context");
    int key_size = 2048;
    ssh_pki_ctx_options_set(pk, SSH_PKI_OPTION_RSA_KEY_SIZE, &key_size);
    int rc = ssh_pki_generate_key(SSH_KEYTYPE_RSA, pk, &raw);
    ssh_pki_ctx_free(pk);
    if (rc != SSH_OK)
        throw std::runtime_error("Failed to generate RSA host key");
    return raw;
}

struct sftp_server::implementation {


    struct ssh_bind_ptr {
        ssh_bind bind_;

        ssh_bind_ptr() : bind_(ssh_bind_new()) {
            if (!bind_)
                throw std::runtime_error("ssh_bind_new failed");
        }

        ~ssh_bind_ptr() {
            if (bind_) {
                ssh_bind_free(bind_);
                bind_ = nullptr;
            }
        }

        ssh_bind_ptr(const ssh_bind_ptr&) = delete;
        ssh_bind_ptr& operator=(const ssh_bind_ptr&) = delete;

        ssh_bind_ptr(ssh_bind_ptr&& other) : bind_(other.bind_) {
            other.bind_ = nullptr;
        }

        ssh_bind_ptr& operator=(ssh_bind_ptr&& other) {
            if (this != &other) {
                if (bind_)
                    ssh_bind_free(bind_);
                bind_ = other.bind_;
                other.bind_ = nullptr;
            }
            return *this;
        }

        ssh_bind get() const { return bind_; }
        bool valid() const { return bind_ != nullptr; }

        void reset() {
            if (bind_) {
                ssh_bind_free(bind_);
                bind_ = nullptr;
            }
        }
    };



    struct ssh_key_ptr {
        ssh_key key_;

        ssh_key_ptr() : key_(nullptr) {}

        explicit ssh_key_ptr(ssh_key k) : key_(k) {}

        ~ssh_key_ptr() {
            if (key_) {
                ssh_key_free(key_);
                key_ = nullptr;
            }
        }

        ssh_key_ptr(const ssh_key_ptr&) = delete;
        ssh_key_ptr& operator=(const ssh_key_ptr&) = delete;

        ssh_key_ptr(ssh_key_ptr&& other) : key_(other.key_) {
            other.key_ = nullptr;
        }

        ssh_key_ptr& operator=(ssh_key_ptr&& other) {
            if (this != &other) {
                if (key_)
                    ssh_key_free(key_);
                key_ = other.key_;
                other.key_ = nullptr;
            }
            return *this;
        }

        ssh_key get() const { return key_; }
        ssh_key* ptr() { return &key_; }
        bool valid() const { return key_ != nullptr; }
        ssh_key release() {
            ssh_key k = key_;
            key_ = nullptr;
            return k;
        }
    };



    struct User {
        std::string name;
        std::string pass;
        std::string root;
        sftp_permission perm;
    };

    struct Activity {
        std::string user;
        std::string operation;
        std::string path;
        std::string result;
    };



    ssh_bind_ptr bind_;
    ssh_key_ptr hostkey_;
    uint16_t port_ = 0;
    std::unique_ptr<detail::TempDir> tmp_;
    std::atomic<bool> running_{false};
    mutable std::mutex mtx_;

    std::vector<User> users_;
    std::vector<Activity> activity_;
    std::vector<std::string> conn_users_;


    std::thread accept_thread_;
    mutable std::mutex thread_mtx_;
    std::vector<std::thread> session_threads_;





    std::map<ssh_session, std::string> session_users_;



    struct HandshakeState {
        std::atomic<ssh_channel> pending_channel{nullptr};
        std::atomic<bool> subsystem_accepted{false};
    };
    mutable std::mutex hs_mtx_;
    std::map<ssh_session, std::unique_ptr<HandshakeState>> handshakes_;

    HandshakeState& get_hs(ssh_session session) {
        std::lock_guard<std::mutex> lock(hs_mtx_);
        auto it = handshakes_.find(session);

        return *it->second;
    }
    HandshakeState& make_hs(ssh_session session) {
        std::lock_guard<std::mutex> lock(hs_mtx_);
        auto insert_result = handshakes_.emplace(
            session, std::unique_ptr<HandshakeState>(new HandshakeState()));
        return *insert_result.first->second;
    }
    void remove_hs(ssh_session session) {
        std::lock_guard<std::mutex> lock(hs_mtx_);
        handshakes_.erase(session);
    }
    ssh_channel_callbacks_struct channel_callbacks_{};



    implementation() {
        hostkey_ = ssh_key_ptr(generate_rsa_key());

        tmp_.reset(new detail::TempDir("mock-services-sftp-"));
    }

    ~implementation() { stop(); }



    void ensure_hostkey() {
        if (!hostkey_.valid()) {
            hostkey_ = ssh_key_ptr(generate_rsa_key());
        }
    }



    void start() {
        if (running_.load())
            throw std::runtime_error("sftp_server is already running");


        if (!bind_.valid())
            bind_ = ssh_bind_ptr();


        ensure_hostkey();


        const char* addr = "127.0.0.1";
        unsigned int port_val = 0;

        int rc;
        rc = ssh_bind_options_set(bind_.get(),
                                  SSH_BIND_OPTIONS_BINDADDR, addr);
        if (rc != SSH_OK) {
            bind_.reset();
            throw std::runtime_error(
                "ssh_bind_options_set BINDADDR failed: " +
                std::string(ssh_get_error(bind_.get())));
        }

        rc = ssh_bind_options_set(bind_.get(),
                                  SSH_BIND_OPTIONS_BINDPORT, &port_val);
        if (rc != SSH_OK) {
            bind_.reset();
            throw std::runtime_error(
                "ssh_bind_options_set BINDPORT failed: " +
                std::string(ssh_get_error(bind_.get())));
        }

        if (hostkey_.valid()) {
            rc = ssh_bind_options_set(bind_.get(),
                                      SSH_BIND_OPTIONS_IMPORT_KEY,
                                      static_cast<const void*>(hostkey_.get()));
            if (rc != SSH_OK) {
                bind_.reset();
                throw std::runtime_error(
                    "ssh_bind_options_set IMPORT_KEY failed: " +
                    std::string(ssh_get_error(bind_.get())));
            }

            (void)hostkey_.release();
        }


        rc = ssh_bind_listen(bind_.get());
        if (rc != SSH_OK) {
            const char* err = ssh_get_error(bind_.get());
            bind_.reset();
            throw std::runtime_error(
                std::string("ssh_bind_listen failed: ") +
                (err ? std::string(err) : "unknown error"));
        }



        auto fd = ssh_bind_get_fd(bind_.get());
        struct sockaddr_in sin;
#ifdef _WIN32
        int sin_len = static_cast<int>(sizeof(sin));
#else
        socklen_t sin_len = static_cast<socklen_t>(sizeof(sin));
#endif
        std::memset(&sin, 0, sizeof(sin));

        int grc = ::getsockname(fd,
                                reinterpret_cast<struct sockaddr*>(&sin),
                                &sin_len);
        if (grc != 0) {
            bind_.reset();
            throw std::runtime_error(
                "getsockname after ssh_bind_listen failed");
        }

        port_ = ntohs(sin.sin_port);
        running_.store(true);


        accept_thread_ = std::thread(&implementation::acceptor_loop, this);
    }

    void stop() {
        if (!running_.load()) return;
        running_.store(false);


        unblock_accept();


        if (accept_thread_.joinable())
            accept_thread_.join();



        std::vector<std::thread> session_threads;
        {
            std::lock_guard<std::mutex> lock(thread_mtx_);
            session_threads.swap(session_threads_);
        }

        for (auto& session_thread : session_threads) {
            if (session_thread.joinable()) session_thread.join();
        }


        bind_.reset();

        port_ = 0;

        {
            std::lock_guard<std::mutex> lock(mtx_);
            conn_users_.clear();
            session_users_.clear();
        }
    }



    void unblock_accept() {
        if (port_ == 0) return;

#ifdef _WIN32
        SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_handle != INVALID_SOCKET) {
            struct sockaddr_in addr;
            std::memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port_);
            addr.sin_addr.s_addr = inet_addr("127.0.0.1");
            ::connect(socket_handle, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
            ::closesocket(socket_handle);
        }
#else
        int socket_handle = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        if (socket_handle >= 0) {
            struct sockaddr_in addr;
            std::memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port_);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            ::connect(socket_handle, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
            ::close(socket_handle);
        }
#endif
    }



    void acceptor_loop() {
        while (running_.load()) {
            ssh_session session = ssh_new();
            if (!session) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }


            int rc = ssh_bind_accept(bind_.get(), session);
            if (rc != SSH_OK) {
                ssh_free(session);
                if (!running_.load()) break;
                continue;
            }


            if (!running_.load()) {
                ssh_free(session);
                break;
            }





            struct ssh_server_callbacks_struct cb;
            std::memset(&cb, 0, sizeof(cb));
            cb.size = sizeof(cb);
            cb.userdata = this;
            cb.auth_password_function = on_auth_password;
            cb.channel_open_request_session_function = on_channel_open_session;
            ssh_callbacks_init(&cb);
            ssh_set_server_callbacks(session, &cb);
            ssh_set_auth_methods(session, SSH_AUTH_METHOD_PASSWORD);

            rc = ssh_handle_key_exchange(session);
            if (rc != SSH_OK) {
                remove_session(session);
                ssh_disconnect(session);
                ssh_free(session);
                if (!running_.load()) break;
                continue;
            }




            {
                std::lock_guard<std::mutex> lock(thread_mtx_);
                session_threads_.emplace_back(
                    &implementation::handle_session_after_auth, this, session);
            }
        }
    }



    static int on_auth_password(ssh_session session,
                                const char* user,
                                const char* password,
                                void* userdata) {
        auto* self = static_cast<implementation*>(userdata);
        return self->check_auth(session, user, password);
    }

    int check_auth(ssh_session session,
                   const char* user,
                   const char* password) {
        std::lock_guard<std::mutex> lock(mtx_);
        for (const auto& configured_user : users_) {
            if (configured_user.name == user && configured_user.pass == password) {
                session_users_[session] = user;
                if (std::find(conn_users_.begin(), conn_users_.end(), user)
                        == conn_users_.end()) {
                    conn_users_.push_back(user);
                }
                return SSH_AUTH_SUCCESS;
            }
        }
        return SSH_AUTH_DENIED;
    }



    void remove_session(ssh_session session) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = session_users_.find(session);
        if (it != session_users_.end()) {
            auto ci = std::find(conn_users_.begin(),
                                conn_users_.end(),
                                it->second);
            if (ci != conn_users_.end())
                conn_users_.erase(ci);
            session_users_.erase(it);
        }
    }



    bool lookup_user(ssh_session session,
                     std::string& username,
                     std::string& root,
                     sftp_permission& perm) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = session_users_.find(session);
        if (it == session_users_.end()) return false;
        username = it->second;
        for (const auto& configured_user : users_) {
            if (configured_user.name == username) {
                root = configured_user.root;
                perm = configured_user.perm;
                return true;
            }
        }
        return false;
    }



    void log_activity(const std::string& user,
                      const std::string& operation,
                      const std::string& path,
                      const std::string& result) {
        std::lock_guard<std::mutex> lock(mtx_);
        activity_.push_back({user, operation, path, result});
    }



    std::string resolve_path(const std::string& root,
                             const std::string& client_path,
                             bool must_exist) {
        if (root.empty()) return "";


        std::string combined = detail::normalize_separators(root);
        if (!combined.empty() && combined.back() != '/'
#ifdef _WIN32
            && combined.back() != '\\'
#endif
        ) {
            combined += '/';
        }

        std::string relative_client_path = client_path;
        while (!relative_client_path.empty() &&
               (relative_client_path[0] == '/' || relative_client_path[0] == '\\')) {
            relative_client_path.erase(0, 1);
        }
#ifdef _WIN32

        if (relative_client_path.size() >= 2 && relative_client_path[1] == ':')
            relative_client_path = relative_client_path.substr(2);
        while (!relative_client_path.empty() &&
               (relative_client_path[0] == '/' || relative_client_path[0] == '\\')) {
            relative_client_path.erase(0, 1);
        }
#endif
        combined += relative_client_path;


        std::string resolved;
#ifdef _WIN32
        char canon[4096] = {};
        if (_fullpath(canon, combined.c_str(), 4096) == nullptr)
            return "";
        resolved = canon;
#else
        char* resolved_realpath = ::realpath(combined.c_str(), nullptr);
        if (resolved_realpath != nullptr) {
            resolved = resolved_realpath;
            std::free(resolved_realpath);
        } else if (!must_exist) {

            std::string dir = combined;
            std::string base;
            size_t slash = combined.rfind('/');
            if (slash != std::string::npos) {
                dir = combined.substr(0, slash);
                base = combined.substr(slash + 1);
            }

            resolved_realpath = ::realpath(dir.c_str(), nullptr);
            if (resolved_realpath != nullptr) {
                resolved = resolved_realpath;
                std::free(resolved_realpath);
                resolved += '/';
                resolved += base;
            } else {
                return "";
            }
        } else {
            return "";
        }
#endif


        std::string nroot = detail::normalize_separators(root);
        std::string nresolved = detail::normalize_separators(resolved);
        if (!detail::has_case_insensitive_prefix(nresolved, nroot))
            return "";

        return resolved;
    }




    static ssh_channel on_channel_open_session(ssh_session session,
                                                void* userdata) {
        auto* self = static_cast<implementation*>(userdata);
        ssh_channel chan = ssh_channel_new(session);
        if (chan) {
            ssh_set_channel_callbacks(chan, &self->channel_callbacks_);
            self->get_hs(session).pending_channel.store(chan);
        }
        return chan;
    }


    static int on_channel_subsystem(ssh_session session,
                                    ssh_channel ,
                                    const char* subsystem,
                                    void* userdata) {
        auto* self = static_cast<implementation*>(userdata);
        if (subsystem && std::strcmp(subsystem, "sftp") == 0) {
            self->get_hs(session).subsystem_accepted.store(true);
            return 0;
        }
        return 1;
    }



    bool do_callback_handshake(ssh_session session,
                               ssh_channel& out_chan) {


        HandshakeState& hs = make_hs(session);


        std::memset(&channel_callbacks_, 0, sizeof(channel_callbacks_));
        channel_callbacks_.size = sizeof(channel_callbacks_);
        channel_callbacks_.userdata = this;
        channel_callbacks_.channel_subsystem_request_function =
            on_channel_subsystem;
        ssh_callbacks_init(&channel_callbacks_);


        while (running_.load()) {
            int rc = ssh_handle_packets(session, SSH_TIMEOUT_USER);
            if (rc == SSH_ERROR) break;


            ssh_channel chan = hs.pending_channel.load();
            if (chan && !out_chan) {
                out_chan = chan;
            }


            if (out_chan && hs.subsystem_accepted.load()) {
                return true;
            }
        }
        return false;
    }



    void handle_session_after_auth(ssh_session session) {
        int rc = SSH_OK;


        ssh_channel chan = nullptr;
        if (!do_callback_handshake(session, chan)) {
            if (chan) {
                ssh_channel_send_eof(chan);
                ssh_channel_close(chan);
                ssh_channel_free(chan);
            }
            remove_hs(session);
            remove_session(session);
            ssh_disconnect(session);
            ssh_free(session);
            return;
        }


        sftp_session sftp_srv = sftp_server_new(session, chan);
        if (!sftp_srv) {
            ssh_channel_send_eof(chan);
            ssh_channel_close(chan);
            ssh_channel_free(chan);
            remove_hs(session);
            remove_session(session);
            ssh_disconnect(session);
            ssh_free(session);
            return;
        }


#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        rc = sftp_server_init(sftp_srv);
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
        if (rc != SSH_OK) {
            sftp_server_free(sftp_srv);
            ssh_channel_send_eof(chan);
            ssh_channel_close(chan);
            ssh_channel_free(chan);
            remove_hs(session);
            remove_session(session);
            ssh_disconnect(session);
            ssh_free(session);
            return;
        }


        std::string username, user_root;
        sftp_permission user_perm = sftp_permission::none;
        lookup_user(session, username, user_root, user_perm);


        SftpHandlerServices svc;
        svc.userdata = this;
        svc.resolve_path = &resolve_path_binder;
        svc.log_activity = &log_activity_binder;

        SftpContext ctx;
        ctx.sftp = sftp_srv;
        ctx.channel = chan;
        ctx.username = username;
        ctx.root = user_root;
        ctx.perm = user_perm;
        ctx.svc = &svc;
        process_sftp(ctx);


        sftp_server_free(sftp_srv);
        ssh_channel_send_eof(chan);
        ssh_channel_close(chan);
        ssh_channel_free(chan);

        remove_hs(session);
        remove_session(session);
        ssh_disconnect(session);
        ssh_free(session);
    }







    void process_sftp(SftpContext& ctx) {
        while (running_.load()) {
            int available = ssh_channel_poll_timeout(ctx.channel, 100, 0);
            if (available == SSH_ERROR)
                break;
            if (available == 0) {
                if (!ssh_channel_is_open(ctx.channel) ||
                    ssh_channel_is_eof(ctx.channel)) {
                    break;
                }
                continue;
            }

            sftp_client_message msg = sftp_get_client_message(ctx.sftp);
            if (!msg) break;

            uint8_t type = sftp_client_message_get_type(msg);
            switch (type) {
                case SSH_FXP_OPEN:      sftp_do_open(ctx, msg); break;
                case SSH_FXP_CLOSE:     sftp_do_close(ctx, msg); break;
                case SSH_FXP_READ:      sftp_do_read(ctx, msg); break;
                case SSH_FXP_WRITE:     sftp_do_write(ctx, msg); break;
                case SSH_FXP_OPENDIR:   sftp_do_opendir(ctx, msg); break;
                case SSH_FXP_READDIR:   sftp_do_readdir(ctx, msg); break;
                case SSH_FXP_STAT:
                case SSH_FXP_LSTAT:     sftp_do_stat(ctx, msg); break;
                case SSH_FXP_FSTAT:     sftp_do_fstat(ctx, msg); break;
                case SSH_FXP_SETSTAT:   sftp_do_setstat(ctx, msg); break;
                case SSH_FXP_FSETSTAT:  sftp_do_fsetstat(ctx, msg); break;
                case SSH_FXP_MKDIR:     sftp_do_mkdir(ctx, msg); break;
                case SSH_FXP_RMDIR:     sftp_do_rmdir(ctx, msg); break;
                case SSH_FXP_REMOVE:    sftp_do_remove(ctx, msg); break;
                case SSH_FXP_RENAME:    sftp_do_rename(ctx, msg); break;
                case SSH_FXP_REALPATH:  sftp_do_realpath(ctx, msg); break;
                default:
                    sftp_reply_status(msg, SSH_FX_OP_UNSUPPORTED, nullptr);
                    break;
            }

            sftp_client_message_free(msg);
        }
    }



    std::string base_url() const {
        if (!running_.load())
            throw std::runtime_error("sftp_server is not running");
        return "sftp://127.0.0.1:" + std::to_string(port_);
    }

    uint16_t port() const { return port_; }



    void add_user(const std::string& name, const std::string& pass,
                  const std::string& root, sftp_permission perm) {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& u : users_) {
            if (u.name == name) {
                u.pass = pass;
                u.root = root;
                u.perm = perm;
                return;
            }
        }
        users_.push_back({name, pass, root, perm});
    }

    void clear_users() {
        std::lock_guard<std::mutex> lock(mtx_);
        users_.clear();
    }

    std::string temp_root() const {
        return tmp_ ? tmp_->path() : std::string();
    }

    std::vector<std::string> connected_users() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return conn_users_;
    }

    std::vector<sftp_activity> activity() const {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<sftp_activity> result;
        result.reserve(activity_.size());
        for (const auto& a : activity_)
            result.push_back({a.user, a.operation, a.path, a.result});
        return result;
    }

    void clear_activity() {
        std::lock_guard<std::mutex> lock(mtx_);
        activity_.clear();
    }

    static std::string resolve_path_binder(void* userdata, const std::string& root,
                                            const std::string& path, bool must_exist) {
        return static_cast<implementation*>(userdata)->resolve_path(root, path, must_exist);
    }
    static void log_activity_binder(void* userdata, const std::string& user,
                                     const std::string& op, const std::string& path,
                                     const std::string& result) {
        static_cast<implementation*>(userdata)->log_activity(user, op, path, result);
    }
};

sftp_server::sftp_server() : implementation_(new implementation()) {}

sftp_server::~sftp_server() = default;

void sftp_server::start() { implementation_->start(); }

void sftp_server::stop() { implementation_->stop(); }

std::string sftp_server::base_url() const { return implementation_->base_url(); }

uint16_t sftp_server::port() const { return implementation_->port(); }

void sftp_server::add_user(const std::string& username,
                            const std::string& password,
                            const std::string& root_path,
                            sftp_permission perm) {
    implementation_->add_user(username, password, root_path, perm);
}

void sftp_server::clear_users() { implementation_->clear_users(); }

std::string sftp_server::temp_root() const { return implementation_->temp_root(); }

std::vector<std::string> sftp_server::connected_users() const {
    return implementation_->connected_users();
}

std::vector<sftp_activity> sftp_server::activity() const {
    return implementation_->activity();
}

void sftp_server::clear_activity() { implementation_->clear_activity(); }

}
