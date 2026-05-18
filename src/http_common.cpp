#include "mock-services/http_common.h"

namespace mock_services {

const std::string method::get     = "GET";
const std::string method::post    = "POST";
const std::string method::put     = "PUT";
const std::string method::del     = "DELETE";
const std::string method::patch   = "PATCH";
const std::string method::head    = "HEAD";
const std::string method::options = "OPTIONS";

const std::string content_type::json            = "application/json";
const std::string content_type::html            = "text/html";
const std::string content_type::text            = "text/plain";
const std::string content_type::xml             = "application/xml";
const std::string content_type::form_urlencoded = "application/x-www-form-urlencoded";

response_builder& response_builder::status(int code) {
    status_code_ = code;
    return *this;
}

response_builder& response_builder::content_type(const std::string& content_type) {
    content_type_ = content_type;
    return *this;
}

response_builder& response_builder::body(const std::string& body) {
    body_ = body;
    return *this;
}

response response_builder::build() const {
    return response{status_code_, body_, content_type_};
}

}
