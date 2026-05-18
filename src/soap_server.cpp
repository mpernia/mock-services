#include "mock-services/soap_server.h"

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <httplib.h>
#include <pugixml.hpp>

namespace mock_services {

soap_response soap_response::fault(int status_code,
                                    const std::string& faultcode,
                                    const std::string& faultstring,
                                    soap_version version) {
    pugi::xml_document doc;

    auto decl = doc.prepend_child(pugi::node_declaration);
    decl.append_attribute("version") = "1.0";
    decl.append_attribute("encoding") = "utf-8";

    if (version == soap_version::v1_1) {

        auto envelope = doc.append_child("soap:Envelope");
        envelope.append_attribute("xmlns:soap") =
            "http://schemas.xmlsoap.org/soap/envelope/";

        auto body = envelope.append_child("soap:Body");
        auto fault = body.append_child("soap:Fault");
        fault.append_child("faultcode").text().set(faultcode.c_str());
        fault.append_child("faultstring").text().set(faultstring.c_str());
    } else {

        auto envelope = doc.append_child("env:Envelope");
        envelope.append_attribute("xmlns:env") =
            "http://www.w3.org/2003/05/soap-envelope";

        auto body = envelope.append_child("env:Body");
        auto fault = body.append_child("env:Fault");
        auto code = fault.append_child("env:Code");
        code.append_child("env:Value").text().set(faultcode.c_str());
        auto reason = fault.append_child("env:Reason");
        reason.append_child("env:Text").text().set(faultstring.c_str());
    }

    std::ostringstream oss;
    doc.save(oss, "  ", pugi::format_default);
    return {status_code, oss.str()};
}

class soap_server::implementation {
public:
    implementation() = default;

    ~implementation() { stop(); }

    void start() {
        if (running_.load()) {
            throw std::runtime_error("soap_server is already running");
        }

        port_ = svr_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) {
            throw std::runtime_error("failed to bind to 127.0.0.1:0");
        }

        running_.store(true);
        thread_ = std::thread([this] { svr_.listen_after_bind(); });

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
            throw std::runtime_error("soap_server is not running");
        }
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    void add_route(const std::string& path, const std::string& soap_action,
                    soap_server::handler handler_fn) {
        std::lock_guard<std::mutex> lock(handlers_mtx_);

        bool is_new_path = (paths_.count(path) == 0);
        paths_.insert(path);
        handlers_[{path, soap_action}] = std::move(handler_fn);

        if (is_new_path) {
            svr_.Post(path.c_str(),
                       [this](const httplib::Request& req,
                              httplib::Response& res) {
                           handle_request(req, res);
                       });
        }
    }

    std::vector<soap_request> requests() const {
        std::lock_guard<std::mutex> lock(history_mtx_);
        return requests_;
    }

    void clear_requests() {
        std::lock_guard<std::mutex> lock(history_mtx_);
        requests_.clear();
    }

private:
    void handle_request(const httplib::Request& req, httplib::Response& res) {
        soap_version version = soap_version::v1_1;
        auto content_type_header = req.headers.find("Content-Type");
        if (content_type_header != req.headers.end() &&
            content_type_header->second.find("application/soap+xml") != std::string::npos) {
            version = soap_version::v1_2;
        }

        std::string soap_action;
        auto soap_action_header = req.headers.find("SOAPAction");
        if (soap_action_header != req.headers.end()) {
            soap_action = soap_action_header->second;
            if (soap_action.size() >= 2 && soap_action.front() == '"' &&
                soap_action.back() == '"') {
                soap_action = soap_action.substr(1, soap_action.size() - 2);
            }
        }

        soap_request soap_req;
        soap_req.path = req.path;
        soap_req.soap_action = soap_action;
        soap_req.body = req.body;
        soap_req.version = version;

        {
            std::lock_guard<std::mutex> lock(history_mtx_);
            requests_.push_back(soap_req);
        }

        soap_server::handler handler_fn;
        auto key = std::make_pair(req.path, soap_action);
        {
            std::lock_guard<std::mutex> lock(handlers_mtx_);
            auto it = handlers_.find(key);
            if (it != handlers_.end()) {
                handler_fn = it->second;
            }
        }

        std::string content_type =
            (version == soap_version::v1_1)
                ? "text/xml; charset=utf-8"
                : "application/soap+xml; charset=utf-8";

        if (handler_fn) {
            try {
                soap_response soap_resp = handler_fn(soap_req);
                res.status = soap_resp.status_code;
                res.set_content(soap_resp.body, content_type);
            } catch (const std::exception& ex) {
                auto fault = soap_response::fault(500, "soap:Server",
                                                   ex.what(), version);
                res.status = 500;
                res.set_content(fault.body, content_type);
            }
        } else {
            auto fault = soap_response::fault(
                500, "soap:Client",
                "Action '" + soap_action + "' not supported", version);
            res.status = 500;
            res.set_content(fault.body, content_type);
        }
    }

    httplib::Server svr_;
    std::thread thread_;
    int port_ = 0;
    std::atomic<bool> running_{false};

    mutable std::mutex handlers_mtx_;
    std::map<std::pair<std::string, std::string>, soap_server::handler>
        handlers_;
    std::set<std::string> paths_;

    mutable std::mutex history_mtx_;
    std::vector<soap_request> requests_;
};

soap_server::soap_server()
    : implementation_(new implementation()) {}

soap_server::~soap_server() = default;

void soap_server::start() { implementation_->start(); }
void soap_server::stop() { implementation_->stop(); }
std::string soap_server::base_url() const { return implementation_->base_url(); }

void soap_server::route(const std::string& path,
                         const std::string& soap_action, handler handler_fn) {
    implementation_->add_route(path, soap_action, std::move(handler_fn));
}

soap_server::response_setter::response_setter(
        soap_server& server, std::string path, std::string soap_action)
    : server_(server), path_(std::move(path)), soap_action_(std::move(soap_action)) {}

void soap_server::response_setter::then_return(const soap_response& resp) {
    server_.route(path_, soap_action_,
                  [resp](const soap_request&) { return resp; });
}

soap_server::response_setter soap_server::when(
        const std::string& path, const std::string& soap_action) {
    return response_setter(*this, path, soap_action);
}

std::vector<soap_request> soap_server::requests() const {
    return implementation_->requests();
}
void soap_server::clear_requests() { implementation_->clear_requests(); }

}
