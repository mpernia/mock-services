#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <mutex>

#include "mock-services/http_common.h"

namespace mock_services {

struct request {
    std::string method;
    std::string path;
    std::string body;
};

class rest_server {
public:
    using handler = std::function<response(const request&)>;

    rest_server();
    ~rest_server();

    rest_server(const rest_server&) = delete;
    rest_server& operator=(const rest_server&) = delete;

    void start();

    void stop();

    std::string base_url() const;

    void route(const std::string& method, const std::string& path, handler handler_fn);

    class response_setter {
    public:
        void then_return(const response_builder& builder);
    private:
        friend class rest_server;
        response_setter(rest_server& server, std::string method, std::string path);
        rest_server& server_;
        std::string method_;
        std::string path_;
    };

    response_setter when(const std::string& method, const std::string& path);

    std::vector<request> requests() const;

    void clear_requests();

private:
    class implementation;
    std::unique_ptr<implementation> implementation_;
};

}
