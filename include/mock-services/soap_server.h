#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mock_services {

enum class soap_version { v1_1, v1_2 };

struct soap_request {
    std::string path;
    std::string soap_action;
    std::string body;
    soap_version version = soap_version::v1_1;
};

struct soap_response {
    int status_code = 200;
    std::string body;

    soap_response() = default;
    soap_response(int code, std::string body)
        : status_code(code), body(std::move(body)) {}

    static soap_response fault(int status_code,
                               const std::string& faultcode,
                               const std::string& faultstring,
                               soap_version version = soap_version::v1_1);
};

class soap_server {
public:
    using handler = std::function<soap_response(const soap_request&)>;

    soap_server();
    ~soap_server();

    soap_server(const soap_server&) = delete;
    soap_server& operator=(const soap_server&) = delete;

    void start();

    void stop();

    std::string base_url() const;

    void route(const std::string& path, const std::string& soap_action,
                handler handler_fn);

    class response_setter {
    public:
        void then_return(const soap_response& resp);
    private:
        friend class soap_server;
        response_setter(soap_server& server, std::string path, std::string soap_action);
        soap_server& server_;
        std::string path_;
        std::string soap_action_;
    };

    response_setter when(const std::string& path, const std::string& soap_action);

    std::vector<soap_request> requests() const;

    void clear_requests();

private:
    class implementation;
    std::unique_ptr<implementation> implementation_;
};

}
