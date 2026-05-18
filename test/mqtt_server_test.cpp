#include "mock-services/mqtt_common.h"
#include "mock-services/mqtt_server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32) && !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

namespace mqtt = mock_services::mqtt;






#ifdef _WIN32
using test_sock = SOCKET;
constexpr test_sock kTestInvalid = INVALID_SOCKET;
constexpr int kTestSockErr = SOCKET_ERROR;
static void test_sock_close(test_sock socket) { closesocket(socket); }
#else
using test_sock = int;
constexpr test_sock kTestInvalid = -1;
constexpr int kTestSockErr = -1;
static void test_sock_close(test_sock socket) { close(socket); }
#endif





class TestClient {
public:
    TestClient() : sock_(kTestInvalid) {}
    ~TestClient() { disconnect(); }

    bool connect(uint16_t port, unsigned timeout_ms = 3000) {
        sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock_ == kTestInvalid) return false;


#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(sock_, FIONBIO, &mode);
#else
        int flags = fcntl(sock_, F_GETFL, 0);
        fcntl(sock_, F_SETFL, flags | O_NONBLOCK);
#endif

        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(port);

        ::connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));


        fd_set fdset;
        FD_ZERO(&fdset);
        FD_SET(sock_, &fdset);
        timeval tv;
        tv.tv_sec = static_cast<long>(timeout_ms / 1000);
        tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);

        int rc = ::select(static_cast<int>(sock_ + 1),
                          nullptr, &fdset, nullptr, &tv);
        if (rc <= 0) {
            test_sock_close(sock_);
            sock_ = kTestInvalid;
            return false;
        }

        int so_error = 0;
        socklen_t len = sizeof(so_error);
        getsockopt(sock_, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&so_error), &len);
        if (so_error != 0) {
            test_sock_close(sock_);
            sock_ = kTestInvalid;
            return false;
        }


#ifdef _WIN32
        mode = 0;
        ioctlsocket(sock_, FIONBIO, &mode);
#else
        fcntl(sock_, F_SETFL, flags);
#endif

        return true;
    }

    void disconnect() {
        if (sock_ != kTestInvalid) {
#ifdef _WIN32
            ::shutdown(sock_, SD_BOTH);
#else
            ::shutdown(sock_, SHUT_RDWR);
#endif
            test_sock_close(sock_);
            sock_ = kTestInvalid;
        }
    }

    bool send_raw(const std::vector<uint8_t>& data) {
        size_t remaining = data.size();
        const uint8_t* ptr = data.data();
        while (remaining > 0) {
            int sent_count = ::send(sock_,
                           reinterpret_cast<const char*>(ptr),
                           static_cast<int>(remaining), 0);
            if (sent_count <= 0) return false;
            ptr += sent_count;
            remaining -= static_cast<size_t>(sent_count);
        }
        return true;
    }

    bool recv_raw(std::vector<uint8_t>& out, size_t count,
                  unsigned timeout_ms = 1000) {
        out.resize(count);
        size_t received = 0;
        auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);

        while (received < count) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;

            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock_, &rfds);

            auto remaining_us =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    deadline - now);
            timeval tv;
            tv.tv_sec = static_cast<long>(remaining_us.count() / 1000000);
            tv.tv_usec =
                static_cast<long>(remaining_us.count() % 1000000);

            int ready_count = ::select(static_cast<int>(sock_ + 1),
                               &rfds, nullptr, nullptr, &tv);
            if (ready_count <= 0) return false;

            int received_count = ::recv(sock_,
                           reinterpret_cast<char*>(out.data() + received),
                           static_cast<int>(count - received), 0);
            if (received_count <= 0) return false;
            received += static_cast<size_t>(received_count);
        }
        return true;
    }

    bool recv_some(std::vector<uint8_t>& out, unsigned timeout_ms = 1000) {
        auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);

        while (std::chrono::steady_clock::now() < deadline) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sock_, &rfds);

            auto remaining_us =
                std::chrono::duration_cast<std::chrono::microseconds>(
                    deadline - std::chrono::steady_clock::now());
            if (remaining_us.count() <= 0) break;
            timeval tv;
            tv.tv_sec = static_cast<long>(remaining_us.count() / 1000000);
            tv.tv_usec =
                static_cast<long>(remaining_us.count() % 1000000);

            int ready_count = ::select(static_cast<int>(sock_ + 1),
                               &rfds, nullptr, nullptr, &tv);
            if (ready_count <= 0) break;

            uint8_t buffer[4096];
            int received_count = ::recv(sock_, reinterpret_cast<char*>(buffer),
                           sizeof(buffer), 0);
            if (received_count > 0) {
                out.assign(buffer, buffer + received_count);
                return true;
            }
            break;
        }
        return false;
    }

    bool is_connected() const { return sock_ != kTestInvalid; }

private:
    test_sock sock_;
};





static std::vector<uint8_t> mqtt_encode_varint(size_t length) {
    std::vector<uint8_t> out;
    do {
        uint8_t byte = static_cast<uint8_t>(length % 128);
        length /= 128;
        if (length > 0) byte |= 0x80;
        out.push_back(byte);
    } while (length > 0);
    return out;
}

static std::vector<uint8_t> build_connect(
    const std::string& client_id,
    uint16_t keepalive = 60) {
    std::vector<uint8_t> payload;

    payload.push_back(0x00);
    payload.push_back(0x04);
    payload.push_back('M');
    payload.push_back('Q');
    payload.push_back('T');
    payload.push_back('T');

    payload.push_back(0x04);

    payload.push_back(0x02);

    payload.push_back(static_cast<uint8_t>(keepalive >> 8));
    payload.push_back(static_cast<uint8_t>(keepalive & 0xFF));

    uint16_t id_len = static_cast<uint16_t>(client_id.size());
    payload.push_back(static_cast<uint8_t>(id_len >> 8));
    payload.push_back(static_cast<uint8_t>(id_len & 0xFF));
    payload.insert(payload.end(), client_id.begin(), client_id.end());

    std::vector<uint8_t> packet;
    packet.push_back(mqtt::packet::connect);
    auto rl = mqtt_encode_varint(payload.size());
    packet.insert(packet.end(), rl.begin(), rl.end());
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

static std::vector<uint8_t> build_subscribe(uint16_t packet_id,
                                             const std::string& filter) {
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(packet_id >> 8));
    payload.push_back(static_cast<uint8_t>(packet_id & 0xFF));
    uint16_t flen = static_cast<uint16_t>(filter.size());
    payload.push_back(static_cast<uint8_t>(flen >> 8));
    payload.push_back(static_cast<uint8_t>(flen & 0xFF));
    payload.insert(payload.end(), filter.begin(), filter.end());
    payload.push_back(mqtt::qos::at_most_once);

    std::vector<uint8_t> packet;
    packet.push_back(mqtt::packet::subscribe);
    auto rl = mqtt_encode_varint(payload.size());
    packet.insert(packet.end(), rl.begin(), rl.end());
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

static std::vector<uint8_t> build_publish(const std::string& topic,
                                           const std::string& payload_body) {
    std::vector<uint8_t> body;
    uint16_t tlen = static_cast<uint16_t>(topic.size());
    body.push_back(static_cast<uint8_t>(tlen >> 8));
    body.push_back(static_cast<uint8_t>(tlen & 0xFF));
    body.insert(body.end(), topic.begin(), topic.end());
    body.insert(body.end(), payload_body.begin(), payload_body.end());

    std::vector<uint8_t> packet;
    packet.push_back(mqtt::packet::publish);
    auto rl = mqtt_encode_varint(body.size());
    packet.insert(packet.end(), rl.begin(), rl.end());
    packet.insert(packet.end(), body.begin(), body.end());
    return packet;
}

static std::vector<uint8_t> build_pingreq() {
    return {mqtt::packet::pingreq, 0x00};
}

static std::vector<uint8_t> build_disconnect() {
    return {mqtt::packet::disconnect, 0x00};
}

static std::vector<uint8_t> build_unknown() {
    return {0xF0, 0x00};
}





static bool is_connack(const std::vector<uint8_t>& resp) {
    return resp.size() >= 4 &&
           resp[0] == mqtt::packet::connack && resp[1] == 0x02 &&
           resp[2] == 0x00 && resp[3] == mqtt::connect_return::accepted;
}

static bool is_pingresp(const std::vector<uint8_t>& resp) {
    return resp.size() >= 2 && resp[0] == mqtt::packet::pingresp && resp[1] == 0x00;
}

static bool is_suback(const std::vector<uint8_t>& resp) {
    return resp.size() >= 5 && resp[0] == mqtt::packet::suback;
}

static bool is_publish(const std::vector<uint8_t>& resp) {
    return !resp.empty() && (resp[0] & 0xF0) == mqtt::packet::publish;
}

static size_t parse_remaining_length(const std::vector<uint8_t>& packet,
                                     size_t& offset) {
    size_t value = 0;
    int multiplier = 1;
    offset = 1;
    for (int byte_index = 0; byte_index < 4 && offset < packet.size(); ++byte_index) {
        uint8_t byte = packet[offset];
        value += static_cast<size_t>(byte & 0x7F) * multiplier;
        multiplier *= 128;
        ++offset;
        if ((byte & 0x80) == 0) return value;
    }
    return 0;
}





static bool connect_and_verify(TestClient& client, uint16_t port,
                                const std::string& client_id) {
    if (!client.connect(port)) return false;
    if (!client.send_raw(build_connect(client_id))) return false;
    std::vector<uint8_t> resp;
    if (!client.recv_raw(resp, 4)) return false;
    return is_connack(resp);
}





class MqttServerTest : public ::testing::Test {
protected:
    mock_services::mqtt_server server;

    void SetUp() override {
        server.start();

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    void TearDown() override {
        server.stop();
    }

    uint16_t get_port() const { return server.port(); }
};





TEST_F(MqttServerTest, StartExposesDynamicPort) {
    uint16_t p = get_port();
    EXPECT_GT(p, 0);
    std::string url = server.base_url();
    EXPECT_EQ(url, "mqtt://127.0.0.1:" + std::to_string(p));
}

TEST_F(MqttServerTest, StopIsIdempotent) {
    server.stop();
    EXPECT_NO_THROW(server.stop());
}

TEST_F(MqttServerTest, StopWhileRunningIsSafe) {
    server.stop();
    EXPECT_NO_THROW(server.start());
    uint16_t p = get_port();
    EXPECT_GT(p, 0);
}

TEST_F(MqttServerTest, BaseUrlThrowsWhenNotRunning) {
    server.stop();
    EXPECT_THROW(server.base_url(), std::runtime_error);
}





TEST_F(MqttServerTest, ConnectReturnsConnack) {
    TestClient client;
    EXPECT_TRUE(connect_and_verify(client, get_port(), "test-client-01"));
}

TEST_F(MqttServerTest, MultipleConcurrentConnects) {

    uint16_t port = get_port();

    {
        TestClient c1;
        ASSERT_TRUE(connect_and_verify(c1, port, "multi-1"));
    }
    {
        TestClient c2;
        ASSERT_TRUE(connect_and_verify(c2, port, "multi-2"));
    }
    {
        TestClient c3;
        ASSERT_TRUE(connect_and_verify(c3, port, "multi-3"));
    }
}





TEST_F(MqttServerTest, PingreqReturnsPingresp) {
    TestClient client;
    ASSERT_TRUE(connect_and_verify(client, get_port(), "ping-test"));
    ASSERT_TRUE(client.send_raw(build_pingreq()));

    std::vector<uint8_t> resp;
    ASSERT_TRUE(client.recv_raw(resp, 2));
    EXPECT_TRUE(is_pingresp(resp));
}

TEST_F(MqttServerTest, DisconnectClosesSession) {
    TestClient client;
    ASSERT_TRUE(connect_and_verify(client, get_port(), "disc-test"));


    ASSERT_TRUE(client.send_raw(build_disconnect()));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));


    std::vector<uint8_t> resp;
    bool got_response = client.recv_some(resp, 500);
    EXPECT_FALSE(got_response)
        << "Server should have closed the connection after DISCONNECT";
}





TEST_F(MqttServerTest, SubscribeReturnsSuback) {
    TestClient client;
    ASSERT_TRUE(connect_and_verify(client, get_port(), "sub-test"));

    ASSERT_TRUE(client.send_raw(build_subscribe(1, "test/topic")));

    std::vector<uint8_t> resp;
    ASSERT_TRUE(client.recv_raw(resp, 5));
    EXPECT_TRUE(is_suback(resp));

    size_t offset = 0;
    parse_remaining_length(resp, offset);
    ASSERT_GE(resp.size(), offset + 2);
    EXPECT_EQ(resp[offset], 0);
    EXPECT_EQ(resp[offset + 1], 1);
    EXPECT_EQ(resp[offset + 2], mqtt::qos::at_most_once);
}





TEST_F(MqttServerTest, PublishExactMatchDelivery) {
    uint16_t port = get_port();
    TestClient sub, pub;
    ASSERT_TRUE(connect_and_verify(sub, port, "sub"));
    ASSERT_TRUE(connect_and_verify(pub, port, "pub"));

    ASSERT_TRUE(sub.send_raw(build_subscribe(1, "orders/created")));
    std::vector<uint8_t> resp;
    ASSERT_TRUE(sub.recv_raw(resp, 5));

    ASSERT_TRUE(pub.send_raw(build_publish("orders/created", "payload-1")));

    std::vector<uint8_t> delivered;
    ASSERT_TRUE(sub.recv_some(delivered));
    EXPECT_TRUE(is_publish(delivered));
}

TEST_F(MqttServerTest, PublishSingleLevelWildcard) {
    uint16_t port = get_port();
    TestClient sub, pub;
    ASSERT_TRUE(connect_and_verify(sub, port, "sub-plus"));
    ASSERT_TRUE(connect_and_verify(pub, port, "pub-plus"));

    ASSERT_TRUE(sub.send_raw(build_subscribe(1, "devices/+/status")));
    std::vector<uint8_t> resp;
    ASSERT_TRUE(sub.recv_raw(resp, 5));

    ASSERT_TRUE(pub.send_raw(build_publish("devices/a/status", "ok")));

    std::vector<uint8_t> delivered;
    ASSERT_TRUE(sub.recv_some(delivered));
    EXPECT_TRUE(is_publish(delivered));
}

TEST_F(MqttServerTest, PublishMultiLevelWildcard) {
    uint16_t port = get_port();
    TestClient sub, pub;
    ASSERT_TRUE(connect_and_verify(sub, port, "sub-hash"));
    ASSERT_TRUE(connect_and_verify(pub, port, "pub-hash"));

    ASSERT_TRUE(sub.send_raw(build_subscribe(1, "devices/#")));
    std::vector<uint8_t> resp;
    ASSERT_TRUE(sub.recv_raw(resp, 5));

    ASSERT_TRUE(pub.send_raw(build_publish("devices/a/b/c", "nested")));

    std::vector<uint8_t> delivered;
    ASSERT_TRUE(sub.recv_some(delivered));
    EXPECT_TRUE(is_publish(delivered));
}

TEST_F(MqttServerTest, PublishNonMatchingFilterNoDelivery) {
    uint16_t port = get_port();
    TestClient sub, pub;
    ASSERT_TRUE(connect_and_verify(sub, port, "sub-none"));
    ASSERT_TRUE(connect_and_verify(pub, port, "pub-none"));

    ASSERT_TRUE(sub.send_raw(build_subscribe(1, "devices/+/status")));
    std::vector<uint8_t> resp;
    ASSERT_TRUE(sub.recv_raw(resp, 5));


    ASSERT_TRUE(pub.send_raw(build_publish("devices/a/config", "no-match")));


    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::vector<uint8_t> delivered;
    bool got = sub.recv_some(delivered, 200);
    EXPECT_FALSE(got) << "Should not receive publish for non-matching topic";
}





TEST_F(MqttServerTest, PublishHistoryRecordsMessages) {
    uint16_t port = get_port();
    TestClient pub;
    ASSERT_TRUE(connect_and_verify(pub, port, "hist-test"));

    ASSERT_TRUE(pub.send_raw(build_publish("events/a", "x")));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_TRUE(pub.send_raw(build_publish("events/b", "y")));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto msgs = server.messages();
    ASSERT_GE(msgs.size(), static_cast<size_t>(2));
    bool found_a = false, found_b = false;
    for (const auto& m : msgs) {
        if (m.topic == "events/a" && m.payload == "x") found_a = true;
        if (m.topic == "events/b" && m.payload == "y") found_b = true;
    }
    EXPECT_TRUE(found_a);
    EXPECT_TRUE(found_b);
}

TEST_F(MqttServerTest, ClearMessagesResetsHistory) {
    uint16_t port = get_port();
    TestClient pub;
    ASSERT_TRUE(connect_and_verify(pub, port, "clear-test"));

    ASSERT_TRUE(pub.send_raw(build_publish("events/a", "x")));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_GT(server.messages().size(), static_cast<size_t>(0));

    server.clear_messages();
    EXPECT_EQ(server.messages().size(), static_cast<size_t>(0));
}





TEST_F(MqttServerTest, BadMqttHeaderClosesClientKeepsBroker) {
    uint16_t port = get_port();


    {
        TestClient bad_client;
        ASSERT_TRUE(bad_client.connect(port));
        ASSERT_TRUE(bad_client.send_raw(build_unknown()));


        std::this_thread::sleep_for(std::chrono::milliseconds(300));


        std::vector<uint8_t> resp;
        bool got = bad_client.recv_some(resp, 500);
        EXPECT_FALSE(got)
            << "Client should be disconnected after bad header";
    }


    TestClient good_client;
    EXPECT_TRUE(connect_and_verify(good_client, port, "after-bad"));
}





TEST_F(MqttServerTest, PortIsNonZero) {
    EXPECT_GT(get_port(), 0);
}
