#include <gtest/gtest.h>

#include <thread>

#include <httplib.h>

#include "mock-services/rest_server.h"







TEST(rest_server, create_destroy) {
    mock_services::rest_server srv;

}

TEST(rest_server, copy_is_deleted) {
    EXPECT_FALSE(std::is_copy_constructible<mock_services::rest_server>::value);
    EXPECT_FALSE(std::is_copy_assignable<mock_services::rest_server>::value);
}



TEST(rest_server, start_stop) {
    mock_services::rest_server srv;
    srv.start();
    EXPECT_TRUE(srv.base_url().find("http://127.0.0.1:") == 0);
    srv.stop();
}

TEST(rest_server, stop_is_idempotent) {
    mock_services::rest_server srv;
    srv.start();
    srv.stop();

    srv.stop();
}

TEST(rest_server, destructor_stops) {
    {
        mock_services::rest_server srv;
        srv.start();

    }
}



TEST(rest_server, get_route) {
    mock_services::rest_server srv;
    srv.when(mock_services::method::get, "/hello")
        .then_return(mock_services::response_builder()
                         .status(mock_services::status::ok)
                         .body(R"({"msg":"ok"})")
                         .content_type(mock_services::content_type::json));
    srv.start();

    httplib::Client cli(srv.base_url());
    auto res = cli.Get("/hello");
    ASSERT_TRUE(res);
    EXPECT_EQ(mock_services::status::ok, res->status);
    EXPECT_EQ(R"({"msg":"ok"})", res->body);

    srv.stop();
}

TEST(rest_server, post_route) {
    mock_services::rest_server srv;
    srv.route(mock_services::method::post, "/echo",
              [](const mock_services::request& req) {
                  return mock_services::response{mock_services::status::ok,
                                                  req.body,
                                                  mock_services::content_type::text};
              });
    srv.start();

    httplib::Client cli(srv.base_url());
    auto res = cli.Post("/echo", "hello", mock_services::content_type::text);
    ASSERT_TRUE(res);
    EXPECT_EQ(mock_services::status::ok, res->status);
    EXPECT_EQ("hello", res->body);

    srv.stop();
}

TEST(rest_server, multiple_routes) {
    mock_services::rest_server srv;
    srv.when(mock_services::method::get, "/a")
        .then_return(mock_services::response_builder()
                         .status(mock_services::status::ok)
                         .body("A")
                         .content_type(mock_services::content_type::text));
    srv.when(mock_services::method::get, "/b")
        .then_return(mock_services::response_builder()
                         .status(mock_services::status::ok)
                         .body("B")
                         .content_type(mock_services::content_type::text));
    srv.start();

    httplib::Client cli(srv.base_url());
    {
        auto res = cli.Get("/a");
        ASSERT_TRUE(res);
        EXPECT_EQ("A", res->body);
    }
    {
        auto res = cli.Get("/b");
        ASSERT_TRUE(res);
        EXPECT_EQ("B", res->body);
    }

    srv.stop();
}



TEST(rest_server, records_request_history) {
    mock_services::rest_server srv;
    srv.when(mock_services::method::get, "/hist")
        .then_return(mock_services::response_builder()
                         .status(mock_services::status::ok)
                         .body("")
                         .content_type(mock_services::content_type::text));
    srv.start();

    httplib::Client cli(srv.base_url());
    cli.Get("/hist");

    auto history = srv.requests();
    ASSERT_EQ(1, history.size());
    EXPECT_EQ(mock_services::method::get, history[0].method);
    EXPECT_EQ("/hist", history[0].path);

    srv.stop();
}

TEST(rest_server, clear_requests) {
    mock_services::rest_server srv;
    srv.when(mock_services::method::get, "/clr")
        .then_return(mock_services::response_builder()
                         .status(mock_services::status::ok)
                         .body("")
                         .content_type(mock_services::content_type::text));
    srv.start();

    httplib::Client cli(srv.base_url());
    cli.Get("/clr");
    ASSERT_EQ(1, srv.requests().size());

    srv.clear_requests();
    EXPECT_EQ(0, srv.requests().size());

    srv.stop();
}



TEST(rest_server, not_found_returns_404) {
    mock_services::rest_server srv;
    srv.start();

    httplib::Client cli(srv.base_url());
    auto res = cli.Get("/nonexistent");
    ASSERT_TRUE(res);
    EXPECT_EQ(404, res->status);

    srv.stop();
}

TEST(rest_server, base_url_throws_when_not_started) {
    mock_services::rest_server srv;
    EXPECT_THROW(srv.base_url(), std::runtime_error);
}

TEST(rest_server, start_throws_when_already_started) {
    mock_services::rest_server srv;
    srv.start();
    EXPECT_THROW(srv.start(), std::runtime_error);
    srv.stop();
}





TEST(http_common, method_get_equals_GET) {
    EXPECT_EQ(mock_services::method::get, "GET");
    EXPECT_EQ(mock_services::method::post, "POST");
    EXPECT_EQ(mock_services::method::put, "PUT");
    EXPECT_EQ(mock_services::method::del, "DELETE");
    EXPECT_EQ(mock_services::method::patch, "PATCH");
    EXPECT_EQ(mock_services::method::head, "HEAD");
    EXPECT_EQ(mock_services::method::options, "OPTIONS");
}

TEST(http_common, content_type_json_equals_application_json) {
    EXPECT_EQ(mock_services::content_type::json, "application/json");
    EXPECT_EQ(mock_services::content_type::html, "text/html");
    EXPECT_EQ(mock_services::content_type::text, "text/plain");
    EXPECT_EQ(mock_services::content_type::xml, "application/xml");
    EXPECT_EQ(mock_services::content_type::form_urlencoded,
              "application/x-www-form-urlencoded");
}

TEST(http_common, status_ok_equals_200) {
    EXPECT_EQ(mock_services::status::ok, 200);
    EXPECT_EQ(mock_services::status::created, 201);
    EXPECT_EQ(mock_services::status::no_content, 204);
    EXPECT_EQ(mock_services::status::bad_request, 400);
    EXPECT_EQ(mock_services::status::not_found, 404);
    EXPECT_EQ(mock_services::status::internal_server_error, 500);
}





TEST(response_builder, defaults_match_response_defaults) {
    mock_services::response r = mock_services::response_builder().build();
    EXPECT_EQ(r.status_code, mock_services::status::ok);
    EXPECT_EQ(r.content_type, mock_services::content_type::json);
    EXPECT_TRUE(r.body.empty());
}

TEST(response_builder, full_chain) {
    mock_services::response r =
        mock_services::response_builder()
            .status(mock_services::status::created)
            .content_type(mock_services::content_type::text)
            .body("OK")
            .build();
    EXPECT_EQ(r.status_code, mock_services::status::created);
    EXPECT_EQ(r.content_type, mock_services::content_type::text);
    EXPECT_EQ(r.body, "OK");
}

TEST(response_builder, partial_chain_defaults_unset_fields) {
    mock_services::response r =
        mock_services::response_builder()
            .status(mock_services::status::not_found)
            .build();
    EXPECT_EQ(r.status_code, mock_services::status::not_found);
    EXPECT_TRUE(r.body.empty());
    EXPECT_EQ(r.content_type, mock_services::content_type::json);
}





TEST(rest_server, when_then_returns_declarative_route) {
    mock_services::rest_server srv;
    srv.when(mock_services::method::get, "/users")
        .then_return(mock_services::response_builder()
                         .status(mock_services::status::ok)
                         .body(R"([])"));
    srv.start();

    httplib::Client cli(srv.base_url());
    auto res = cli.Get("/users");
    ASSERT_TRUE(res);
    EXPECT_EQ(mock_services::status::ok, res->status);
    EXPECT_EQ("[]", res->body);

    srv.stop();
}

TEST(rest_server, when_then_coexists_with_route) {
    mock_services::rest_server srv;
    srv.route(mock_services::method::get, "/callback",
              [](const mock_services::request&) {
                  return mock_services::response{mock_services::status::ok,
                                                  "from_callback",
                                                  mock_services::content_type::text};
              });
    srv.when(mock_services::method::get, "/declarative")
        .then_return(mock_services::response_builder()
                         .status(mock_services::status::created)
                         .body("from_declarative"));
    srv.start();

    httplib::Client cli(srv.base_url());
    {
        auto res = cli.Get("/callback");
        ASSERT_TRUE(res);
        EXPECT_EQ("from_callback", res->body);
    }
    {
        auto res = cli.Get("/declarative");
        ASSERT_TRUE(res);
        EXPECT_EQ(mock_services::status::created, res->status);
        EXPECT_EQ("from_declarative", res->body);
    }

    srv.stop();
}

TEST(rest_server, when_then_records_request_history) {
    mock_services::rest_server srv;
    srv.when(mock_services::method::post, "/items")
        .then_return(mock_services::response_builder()
                         .status(mock_services::status::created)
                         .body(R"({"id":1})"));
    srv.start();

    httplib::Client cli(srv.base_url());
    cli.Post("/items", "", mock_services::content_type::json);

    auto history = srv.requests();
    ASSERT_EQ(1, history.size());
    EXPECT_EQ(mock_services::method::post, history[0].method);
    EXPECT_EQ("/items", history[0].path);

    srv.clear_requests();
    EXPECT_EQ(0, srv.requests().size());

    srv.stop();
}

TEST(rest_server, when_then_defaults_via_builder_defaults) {
    mock_services::rest_server srv;
    srv.when(mock_services::method::get, "/x")
        .then_return(mock_services::response_builder());
    srv.start();

    httplib::Client cli(srv.base_url());
    auto res = cli.Get("/x");
    ASSERT_TRUE(res);
    EXPECT_EQ(mock_services::status::ok, res->status);
    EXPECT_EQ(mock_services::content_type::json,
              res->get_header_value("Content-Type"));

    srv.stop();
}

TEST(rest_server, when_then_accepts_typed_method_constants) {
    mock_services::rest_server srv;
    srv.when(mock_services::method::get, "/users")
        .then_return(mock_services::response_builder()
                         .status(mock_services::status::ok)
                         .body("ok"));
    srv.start();

    httplib::Client cli(srv.base_url());
    auto res = cli.Get("/users");
    ASSERT_TRUE(res);
    EXPECT_EQ(mock_services::status::ok, res->status);
    EXPECT_EQ("ok", res->body);

    srv.stop();
}
