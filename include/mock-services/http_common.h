#pragma once

#include <string>
#include <utility>

namespace mock_services {

namespace status {
    const int ok = 200;
    const int created = 201;
    const int no_content = 204;
    const int bad_request = 400;
    const int not_found = 404;
    const int internal_server_error = 500;
}

namespace method {
    extern const std::string get;
    extern const std::string post;
    extern const std::string put;
    extern const std::string del;
    extern const std::string patch;
    extern const std::string head;
    extern const std::string options;
}

namespace content_type {
    extern const std::string json;
    extern const std::string html;
    extern const std::string text;
    extern const std::string xml;
    extern const std::string form_urlencoded;
}

struct response {
    int status_code = 200;
    std::string body;
    std::string content_type = "application/json";

    response() = default;
    response(int code, std::string body, std::string content_type)
        : status_code(code), body(std::move(body)), content_type(std::move(content_type)) {}
};

class response_builder {
public:
    response_builder() = default;
    response_builder& status(int code);
    response_builder& content_type(const std::string& ct);
    response_builder& body(const std::string& body);
    response build() const;

private:
    int status_code_ = 200;
    std::string content_type_ = "application/json";
    std::string body_;
};

}
