#include <gtest/gtest.h>

#include <thread>

#include <httplib.h>

#include "mock-services/soap_server.h"







TEST(soap_server, create_destroy) {
    mock_services::soap_server srv;

}

TEST(soap_server, copy_is_deleted) {
    EXPECT_FALSE(std::is_copy_constructible<mock_services::soap_server>::value);
    EXPECT_FALSE(std::is_copy_assignable<mock_services::soap_server>::value);
}



TEST(soap_server, start_stop) {
    mock_services::soap_server srv;
    srv.start();
    EXPECT_TRUE(srv.base_url().find("http://127.0.0.1:") == 0);
    srv.stop();
}

TEST(soap_server, stop_is_idempotent) {
    mock_services::soap_server srv;
    srv.start();
    srv.stop();

    srv.stop();
}

TEST(soap_server, destructor_stops) {
    {
        mock_services::soap_server srv;
        srv.start();

    }
}



TEST(soap_server, when_then_returns_declarative_route) {
    mock_services::soap_server srv;
    srv.when("/Submit", "DoWork")
        .then_return(mock_services::soap_response{200, "<result/>"});
    srv.start();

    httplib::Client cli(srv.base_url());
    httplib::Headers headers;
    headers.emplace("SOAPAction", "\"DoWork\"");
    auto res = cli.Post("/Submit", headers, "", "text/xml");
    ASSERT_TRUE(res);
    EXPECT_EQ(200, res->status);
    EXPECT_EQ("<result/>", res->body);

    srv.stop();
}

TEST(soap_server, when_then_coexists_with_route) {
    mock_services::soap_server srv;
    srv.when("/Decl", "Action")
        .then_return(mock_services::soap_response{200, "from_decl"});
    srv.route("/Callback", "Action",
              [](const mock_services::soap_request&) {
                  return mock_services::soap_response{201, "from_callback"};
              });
    srv.start();

    httplib::Client cli(srv.base_url());
    {
        httplib::Headers headers;
        headers.emplace("SOAPAction", "\"Action\"");
        auto res = cli.Post("/Decl", headers, "", "text/xml");
        ASSERT_TRUE(res);
        EXPECT_EQ("from_decl", res->body);
    }
    {
        httplib::Headers headers;
        headers.emplace("SOAPAction", "\"Action\"");
        auto res = cli.Post("/Callback", headers, "", "text/xml");
        ASSERT_TRUE(res);
        EXPECT_EQ(201, res->status);
        EXPECT_EQ("from_callback", res->body);
    }

    srv.stop();
}

TEST(soap_server, when_then_records_request_history) {
    mock_services::soap_server srv;
    srv.when("/Hist", "DoWork")
        .then_return(mock_services::soap_response{200, "<ok/>"});
    srv.start();

    httplib::Client cli(srv.base_url());
    httplib::Headers headers;
    headers.emplace("SOAPAction", "\"DoWork\"");
    auto res = cli.Post("/Hist", headers, "<body/>", "text/xml");
    ASSERT_TRUE(res);
    ASSERT_EQ(200, res->status);

    auto history = srv.requests();
    ASSERT_EQ(1, history.size());
    EXPECT_EQ("/Hist", history[0].path);
    EXPECT_EQ("DoWork", history[0].soap_action);
    EXPECT_EQ("<body/>", history[0].body);

    srv.stop();
}



TEST(soap_server, soap11_route) {
    mock_services::soap_server srv;
    srv.route("/Submit", "DoWork",
              [](const mock_services::soap_request& req) {
                  EXPECT_EQ("DoWork", req.soap_action);
                  EXPECT_EQ("/Submit", req.path);
                  EXPECT_EQ(mock_services::soap_version::v1_1, req.version);
                  return mock_services::soap_response{
                      200, R"(<?xml version="1.0"?>)"
                           R"(<soap:Envelope xmlns:soap="http://schemas.xmlsoap.org/soap/envelope/">)"
                           R"(<soap:Body><DoWorkResponse/></soap:Body>)"
                           R"(</soap:Envelope>)"};
              });
    srv.start();

    httplib::Client cli(srv.base_url());
    httplib::Headers headers;
    headers.emplace("SOAPAction", "\"DoWork\"");
    auto res = cli.Post("/Submit", headers, R"(<soap:Envelope/>)", "text/xml");
    ASSERT_TRUE(res);
    EXPECT_EQ(200, res->status);
    EXPECT_NE(res->body.find("<DoWorkResponse/>"), std::string::npos);

    auto content_type = res->get_header_value("Content-Type");
    EXPECT_NE(content_type.find("text/xml"), std::string::npos);

    srv.stop();
}

TEST(soap_server, soap12_route) {
    mock_services::soap_server srv;

    srv.route("/Process", "",
              [](const mock_services::soap_request& req) {
                  EXPECT_EQ("", req.soap_action);
                  EXPECT_EQ("/Process", req.path);
                  EXPECT_EQ(mock_services::soap_version::v1_2, req.version);
                  return mock_services::soap_response{
                      200, R"(<?xml version="1.0"?>)"
                           R"(<soap:Envelope xmlns:soap="http://schemas.xmlsoap.org/soap/envelope/">)"
                           R"(<soap:Body><ProcessResponse/></soap:Body>)"
                           R"(</soap:Envelope>)"};
              });
    srv.start();

    httplib::Client cli(srv.base_url());
    httplib::Headers headers;

    auto res = cli.Post("/Process", headers, R"(<soap:Envelope/>)",
                        "application/soap+xml");
    ASSERT_TRUE(res);
    EXPECT_EQ(200, res->status);
    EXPECT_NE(res->body.find("<ProcessResponse/>"), std::string::npos);

    auto content_type = res->get_header_value("Content-Type");
    EXPECT_NE(content_type.find("application/soap+xml"), std::string::npos);

    srv.stop();
}



TEST(soap_server, action_matching) {
    mock_services::soap_server srv;
    srv.when("/Service", "ActionA")
        .then_return(mock_services::soap_response{200, "A"});
    srv.when("/Service", "ActionB")
        .then_return(mock_services::soap_response{200, "B"});
    srv.start();

    httplib::Client cli(srv.base_url());

    {
        httplib::Headers headers;
        headers.emplace("SOAPAction", "\"ActionA\"");
        auto res = cli.Post("/Service", headers, "", "text/xml");
        ASSERT_TRUE(res);
        EXPECT_EQ("A", res->body);
    }
    {
        httplib::Headers headers;
        headers.emplace("SOAPAction", "\"ActionB\"");
        auto res = cli.Post("/Service", headers, "", "text/xml");
        ASSERT_TRUE(res);
        EXPECT_EQ("B", res->body);
    }

    srv.stop();
}



TEST(soap_server, multiple_paths) {
    mock_services::soap_server srv;
    srv.when("/A", "Act")
        .then_return(mock_services::soap_response{200, "ResponseA"});
    srv.when("/B", "Act")
        .then_return(mock_services::soap_response{200, "ResponseB"});
    srv.start();

    httplib::Client cli(srv.base_url());

    {
        httplib::Headers headers;
        headers.emplace("SOAPAction", "\"Act\"");
        auto res = cli.Post("/A", headers, "", "text/xml");
        ASSERT_TRUE(res);
        EXPECT_EQ("ResponseA", res->body);
    }
    {
        httplib::Headers headers;
        headers.emplace("SOAPAction", "\"Act\"");
        auto res = cli.Post("/B", headers, "", "text/xml");
        ASSERT_TRUE(res);
        EXPECT_EQ("ResponseB", res->body);
    }

    srv.stop();
}



TEST(soap_server, unmatched_action_returns_fault) {
    mock_services::soap_server srv;
    srv.route("/Service", "KnownAction",
              [](const mock_services::soap_request&) {
                  return mock_services::soap_response{200, "ok"};
              });
    srv.start();

    httplib::Client cli(srv.base_url());
    httplib::Headers headers;
    headers.emplace("SOAPAction", "\"UnknownAction\"");
    auto res = cli.Post("/Service", headers, "", "text/xml");
    ASSERT_TRUE(res);
    EXPECT_EQ(500, res->status);

    EXPECT_NE(res->body.find("soap:Client"), std::string::npos);
    EXPECT_NE(res->body.find("UnknownAction"), std::string::npos);

    srv.stop();
}

TEST(soap_server, unmatched_path_returns_404) {
    mock_services::soap_server srv;
    srv.route("/Known", "Act",
              [](const mock_services::soap_request&) {
                  return mock_services::soap_response{200, "ok"};
              });
    srv.start();

    httplib::Client cli(srv.base_url());
    httplib::Headers headers;
    headers.emplace("SOAPAction", "\"Act\"");
    auto res = cli.Post("/UnknownPath", headers, "", "text/xml");
    ASSERT_TRUE(res);
    EXPECT_EQ(404, res->status);

    srv.stop();
}

TEST(soap_server, non_post_returns_404) {
    mock_services::soap_server srv;
    srv.route("/OnlyPost", "Act",
              [](const mock_services::soap_request&) {
                  return mock_services::soap_response{200, "ok"};
              });
    srv.start();

    httplib::Client cli(srv.base_url());
    auto res = cli.Get("/OnlyPost");
    ASSERT_TRUE(res);
    EXPECT_EQ(404, res->status);

    srv.stop();
}



TEST(soap_server, records_request_history) {
    mock_services::soap_server srv;
    srv.when("/Hist", "A")
        .then_return(mock_services::soap_response{200, ""});
    srv.start();

    httplib::Client cli(srv.base_url());
    httplib::Headers headers;
    headers.emplace("SOAPAction", "\"A\"");
    auto res = cli.Post("/Hist", headers, "<body/>", "text/xml");
    ASSERT_TRUE(res);
    ASSERT_EQ(200, res->status);

    auto history = srv.requests();
    ASSERT_EQ(1, history.size());
    EXPECT_EQ("/Hist", history[0].path);
    EXPECT_EQ("A", history[0].soap_action);
    EXPECT_EQ("<body/>", history[0].body);
    EXPECT_EQ(mock_services::soap_version::v1_1, history[0].version);

    srv.stop();
}

TEST(soap_server, multiple_requests_history) {
    mock_services::soap_server srv;
    srv.when("/Echo", "Act")
        .then_return(mock_services::soap_response{200, ""});
    srv.start();

    httplib::Client cli(srv.base_url());
    httplib::Headers headers;
    headers.emplace("SOAPAction", "\"Act\"");

    auto first_response = cli.Post("/Echo", headers, "<one/>", "text/xml");
    ASSERT_TRUE(first_response);
    auto second_response = cli.Post("/Echo", headers, "<two/>", "text/xml");
    ASSERT_TRUE(second_response);

    auto history = srv.requests();
    ASSERT_EQ(2, history.size());
    EXPECT_EQ("<one/>", history[0].body);
    EXPECT_EQ("<two/>", history[1].body);

    srv.stop();
}

TEST(soap_server, clear_requests) {
    mock_services::soap_server srv;
    srv.when("/Clr", "X")
        .then_return(mock_services::soap_response{200, ""});
    srv.start();

    httplib::Client cli(srv.base_url());
    httplib::Headers headers;
    headers.emplace("SOAPAction", "\"X\"");
    auto res = cli.Post("/Clr", headers, "", "text/xml");
    ASSERT_TRUE(res);
    ASSERT_EQ(1, srv.requests().size());

    srv.clear_requests();
    EXPECT_EQ(0, srv.requests().size());

    srv.stop();
}



TEST(soap_server, handler_exception_returns_fault) {
    mock_services::soap_server srv;
    srv.route("/Fail", "Crash",
              [](const mock_services::soap_request&) -> mock_services::soap_response {
                  throw std::runtime_error("internal error");
              });
    srv.start();

    httplib::Client cli(srv.base_url());
    httplib::Headers headers;
    headers.emplace("SOAPAction", "\"Crash\"");
    auto res = cli.Post("/Fail", headers, "", "text/xml");
    ASSERT_TRUE(res);
    EXPECT_EQ(500, res->status);
    EXPECT_NE(res->body.find("soap:Server"), std::string::npos);
    EXPECT_NE(res->body.find("internal error"), std::string::npos);

    srv.stop();
}



TEST(soap_server, soap_fault_factory) {

    {
        auto resp = mock_services::soap_response::fault(
            500, "soap:VersionMismatch", "bad version",
            mock_services::soap_version::v1_1);
        EXPECT_EQ(500, resp.status_code);
        EXPECT_NE(resp.body.find("soap:Envelope"), std::string::npos);
        EXPECT_NE(resp.body.find("soap:Fault"), std::string::npos);
        EXPECT_NE(resp.body.find("soap:VersionMismatch"), std::string::npos);
        EXPECT_NE(resp.body.find("bad version"), std::string::npos);
        EXPECT_NE(resp.body.find("http://schemas.xmlsoap.org/soap/envelope/"),
                  std::string::npos);
    }


    {
        auto resp = mock_services::soap_response::fault(
            500, "env:Sender", "invalid message",
            mock_services::soap_version::v1_2);
        EXPECT_EQ(500, resp.status_code);
        EXPECT_NE(resp.body.find("env:Envelope"), std::string::npos);
        EXPECT_NE(resp.body.find("env:Fault"), std::string::npos);
        EXPECT_NE(resp.body.find("env:Code"), std::string::npos);
        EXPECT_NE(resp.body.find("env:Reason"), std::string::npos);
        EXPECT_NE(resp.body.find("env:Sender"), std::string::npos);
        EXPECT_NE(resp.body.find("invalid message"), std::string::npos);
        EXPECT_NE(resp.body.find("http://www.w3.org/2003/05/soap-envelope"),
                  std::string::npos);
    }
}



TEST(soap_server, custom_status_code) {
    mock_services::soap_server srv;
    srv.when("/Teapot", "Brew")
        .then_return(mock_services::soap_response{418, "I'm a teapot"});
    srv.start();

    httplib::Client cli(srv.base_url());
    httplib::Headers headers;
    headers.emplace("SOAPAction", "\"Brew\"");
    auto res = cli.Post("/Teapot", headers, "", "text/xml");
    ASSERT_TRUE(res);
    EXPECT_EQ(418, res->status);
    EXPECT_EQ("I'm a teapot", res->body);

    srv.stop();
}



TEST(soap_server, start_throws_when_already_started) {
    mock_services::soap_server srv;
    srv.start();
    EXPECT_THROW(srv.start(), std::runtime_error);
    srv.stop();
}
