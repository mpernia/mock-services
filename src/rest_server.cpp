#include "mock-services/rest_server.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include <httplib.h>

namespace mock_services {

static request from_httplib(const httplib::Request& src) {
    request req;
    req.method = src.method;
    req.path = src.path;
    req.body = src.body;
    return req;
}

static void into_httplib(const response& src, httplib::Response& dst) {
    dst.status = src.status_code;
    dst.set_content(src.body, src.content_type);
}

class rest_server::implementation {
public:
    implementation() = default;

    ~implementation() { stop(); }

    void start() {
        if (running_.load()) {
            throw std::runtime_error("rest_server is already running");
        }

        port_ = svr_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) {
            throw std::runtime_error("failed to bind to 127.0.0.1:0");
        }

        running_.store(true);
        thread_ = std::thread([this] {
            svr_.listen_after_bind();
        });

        while (!svr_.is_running()) {
            std::this_thread::yield();
        }
    }

    void stop() {
        if (!running_.load()) {
            return;
        }
        running_.store(false);
        svr_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::string base_url() const {
        if (!running_.load()) {
            throw std::runtime_error("rest_server is not running");
        }
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    void add_route(const std::string& method, const std::string& path,
                    rest_server::handler handler_fn) {
        auto cb = [this, handler_fn](const httplib::Request& req,
                             httplib::Response& res) {

            {
                std::lock_guard<std::mutex> lock(history_mtx_);
                requests_.push_back(from_httplib(req));
            }

            try {
                response our_resp = handler_fn(from_httplib(req));
                into_httplib(our_resp, res);
            } catch (const std::exception& ex) {
                res.status = 500;
                res.set_content(R"({"error":")" + std::string(ex.what()) + R"("})",
                                "application/json");
            }
        };

        if (method == "GET") {
            svr_.Get(path.c_str(), cb);
        } else if (method == "POST") {
            svr_.Post(path.c_str(), cb);
        } else if (method == "PUT") {
            svr_.Put(path.c_str(), cb);
        } else if (method == "DELETE") {
            svr_.Delete(path.c_str(), cb);
        } else if (method == "PATCH") {
            svr_.Patch(path.c_str(), cb);
        } else {
            svr_.Get(path.c_str(), cb);
        }
    }

    std::vector<request> requests() const {
        std::lock_guard<std::mutex> lock(history_mtx_);
        return requests_;
    }

    void clear_requests() {
        std::lock_guard<std::mutex> lock(history_mtx_);
        requests_.clear();
    }

private:
    httplib::Server svr_;
    std::thread thread_;
    int port_ = 0;
    std::atomic<bool> running_{false};

    mutable std::mutex history_mtx_;
    std::vector<request> requests_;
};

rest_server::rest_server()
    : implementation_(new implementation()) {}

rest_server::~rest_server() = default;

void rest_server::start() { implementation_->start(); }
void rest_server::stop() { implementation_->stop(); }
std::string rest_server::base_url() const { return implementation_->base_url(); }

void rest_server::route(const std::string& method,
                         const std::string& path,
                         handler handler_fn) {
    implementation_->add_route(method, path, std::move(handler_fn));
}

rest_server::response_setter rest_server::when(
        const std::string& method, const std::string& path) {
    return response_setter(*this, method, path);
}

rest_server::response_setter::response_setter(
        rest_server& server, std::string method, std::string path)
    : server_(server), method_(std::move(method)), path_(std::move(path)) {}

void rest_server::response_setter::then_return(const response_builder& builder) {
    response resp = builder.build();
    server_.route(method_, path_,
                  [resp](const request&) { return resp; });
}

std::vector<request> rest_server::requests() const { return implementation_->requests(); }
void rest_server::clear_requests() { implementation_->clear_requests(); }

}
