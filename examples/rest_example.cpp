#include <cstdlib>
#include <iostream>
#include <thread>

#include <httplib.h>

#include "mock-services/rest_server.h"

int main() {
    mock_services::rest_server server;

    server.when(mock_services::method::get, "/hello")
        .then_return(mock_services::response_builder{}
            .status(mock_services::status::ok)
            .content_type(mock_services::content_type::json)
            .body(R"({"message":"Hello, world!"})"));

    server.route(mock_services::method::post, "/echo",
        [](const mock_services::request& request) {
            return mock_services::response{
                mock_services::status::ok,
                request.body,
                mock_services::content_type::text};
        });

    server.route(mock_services::method::get, "/greet",
        [](const mock_services::request&) {
            return mock_services::response{
                mock_services::status::ok,
                R"({"greeting":"Hello from mock-services"})",
                mock_services::content_type::json};
        });

    server.start();
    std::cout << "REST server running at " << server.base_url() << '\n';

    httplib::Client client(server.base_url());

    auto hello = client.Get("/hello");
    if (hello) {
        std::cout << "GET /hello -> " << hello->status << ": "
                  << hello->body << '\n';
    }

    auto echo = client.Post("/echo", "Hi from REST!", "text/plain");
    if (echo) {
        std::cout << "POST /echo -> " << echo->status << ": "
                  << echo->body << '\n';
    }

    auto greet = client.Get("/greet");
    if (greet) {
        std::cout << "GET /greet/User -> " << greet->status << ": "
                  << greet->body << '\n';
    }

    std::cout << "Request history: " << server.requests().size()
              << " request(s)\n";

    std::cout << "\nPress Enter to stop the server...";
    std::cin.get();

    server.stop();
    std::cout << "Server stopped.\n";
    return EXIT_SUCCESS;
}
