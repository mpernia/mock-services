#include "mock-services/mqtt_server.h"
#include "mock-services/detail/platform_socket.h"
#include "mock-services/detail/thread_utils.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace mock_services {

enum mqtt_packet_type : uint8_t {
    MQTT_CONNECT    = 1,
    MQTT_CONNACK    = 2,
    MQTT_PUBLISH    = 3,
    MQTT_SUBSCRIBE  = 8,
    MQTT_SUBACK     = 9,
    MQTT_PINGREQ    = 12,
    MQTT_PINGRESP   = 13,
    MQTT_DISCONNECT = 14,
};

static bool decode_remaining_length(const uint8_t* data, size_t size,
                                     size_t& decoded_length,
                                     size_t& bytes_consumed) {
    decoded_length = 0;
    bytes_consumed = 0;
    int multiplier = 1;
    for (size_t byte_index = 0; byte_index < 4 && byte_index < size; ++byte_index) {
        uint8_t byte = data[byte_index];
        decoded_length += static_cast<size_t>(byte & 0x7F) * multiplier;
        multiplier *= 128;
        ++bytes_consumed;
        if ((byte & 0x80) == 0) return true;
    }
    return false;
}

static std::vector<uint8_t> encode_remaining_length(size_t length) {
    std::vector<uint8_t> out;
    do {
        uint8_t byte = static_cast<uint8_t>(length % 128);
        length /= 128;
        if (length > 0) byte |= 0x80;
        out.push_back(byte);
    } while (length > 0);
    return out;
}

static std::vector<uint8_t> frame_packet(uint8_t type_and_flags,
                                          const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> packet;
    packet.push_back(type_and_flags);
    auto remaining_length = encode_remaining_length(payload.size());
    packet.insert(packet.end(), remaining_length.begin(), remaining_length.end());
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

static bool recv_exact(raw_socket socket, uint8_t* buffer, size_t length) {
    while (length > 0) {
        int received_count = ::recv(socket, reinterpret_cast<char*>(buffer),
                       static_cast<int>(length), 0);
        if (received_count <= 0) return false;
        buffer += received_count;
        length -= static_cast<size_t>(received_count);
    }
    return true;
}

static bool send_all(raw_socket socket, const uint8_t* data, size_t length) {
    while (length > 0) {
        int sent_count = ::send(socket, reinterpret_cast<const char*>(data),
                       static_cast<int>(length), 0);
        if (sent_count <= 0) return false;
        data += sent_count;
        length -= static_cast<size_t>(sent_count);
    }
    return true;
}

struct connect_packet {
    std::string client_id;
    uint16_t keepalive;
    uint8_t flags;
};

static bool decode_connect(const uint8_t* data, size_t size,
                            connect_packet& out) {
    if (size < 10) return false;
    uint16_t name_len = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    if (name_len != 4 || data[2] != 'M' || data[3] != 'Q' ||
        data[4] != 'T' || data[5] != 'T')
        return false;
    if (data[6] != 4) return false;
    out.flags = data[7];
    out.keepalive = (static_cast<uint16_t>(data[8]) << 8) | data[9];
    size_t offset = 10;
    if (offset + 2 > size) return false;
    uint16_t client_len = (static_cast<uint16_t>(data[offset]) << 8) |
                           data[offset + 1];
    offset += 2;
    if (offset + client_len > size) return false;
    out.client_id.assign(reinterpret_cast<const char*>(data + offset),
                          client_len);
    return true;
}

struct publish_packet {
    std::string topic;
    std::string payload;
};

static bool decode_publish(const uint8_t* data, size_t size,
                            publish_packet& out) {
    if (size < 2) return false;
    uint16_t topic_len = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    if (topic_len == 0) return false;
    size_t offset = 2;
    if (offset + topic_len > size) return false;
    out.topic.assign(reinterpret_cast<const char*>(data + offset), topic_len);
    offset += topic_len;
    out.payload.assign(reinterpret_cast<const char*>(data + offset),
                        size - offset);
    return true;
}

struct subscribe_packet {
    uint16_t packet_id;
    std::vector<std::pair<std::string, uint8_t>> filters;
};

static bool decode_subscribe(const uint8_t* data, size_t size,
                              subscribe_packet& out) {
    if (size < 2) return false;
    out.packet_id = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    size_t offset = 2;
    while (offset < size) {
        if (offset + 2 > size) return false;
        uint16_t filter_len = (static_cast<uint16_t>(data[offset]) << 8) |
                               data[offset + 1];
        offset += 2;
        if (offset + filter_len + 1 > size) return false;
        std::string filter(reinterpret_cast<const char*>(data + offset),
                            filter_len);
        offset += filter_len;
        uint8_t qos = data[offset];
        ++offset;
        out.filters.push_back(std::make_pair(filter, qos));
    }
    return true;
}

static std::vector<uint8_t> encode_connack() {
    return frame_packet(MQTT_CONNACK << 4, {0x00, 0x00});
}

static std::vector<uint8_t> encode_suback(uint16_t packet_id,
                                           const std::vector<uint8_t>& codes) {
    std::vector<uint8_t> payload;
    payload.push_back(static_cast<uint8_t>(packet_id >> 8));
    payload.push_back(static_cast<uint8_t>(packet_id & 0xFF));
    payload.insert(payload.end(), codes.begin(), codes.end());
    return frame_packet(MQTT_SUBACK << 4, payload);
}

static std::vector<uint8_t> encode_publish(const std::string& topic,
                                            const std::string& payload) {
    std::vector<uint8_t> body;
    uint16_t topic_len = static_cast<uint16_t>(topic.size());
    body.push_back(static_cast<uint8_t>(topic_len >> 8));
    body.push_back(static_cast<uint8_t>(topic_len & 0xFF));
    body.insert(body.end(), topic.begin(), topic.end());
    body.insert(body.end(), payload.begin(), payload.end());
    return frame_packet(MQTT_PUBLISH << 4, body);
}

static std::vector<uint8_t> encode_pingresp() {
    return {0xD0, 0x00};
}

static std::vector<std::string> split_topic(const std::string& topic) {
    std::vector<std::string> segments;
    std::string current_segment;
    for (size_t topic_index = 0; topic_index <= topic.size(); ++topic_index) {
        char character = (topic_index < topic.size()) ? topic[topic_index] : '/';
        if (character == '/') {
            segments.push_back(current_segment);
            current_segment.clear();
        } else {
            current_segment += character;
        }
    }
    return segments;
}

static bool topic_matches(const std::string& filter,
                           const std::string& topic) {
    auto filter_segments = split_topic(filter);
    auto topic_segments = split_topic(topic);

    for (size_t filter_index = 0; filter_index < filter_segments.size(); ++filter_index) {
        if (filter_segments[filter_index] == "#" && filter_index != filter_segments.size() - 1)
            return false;
    }

    size_t filter_index = 0;
    size_t topic_index = 0;
    while (filter_index < filter_segments.size() && topic_index < topic_segments.size()) {
        const std::string& segment = filter_segments[filter_index];
        if (segment == "#")
            return true;
        if (segment == "+") {
            ++filter_index; ++topic_index;
            continue;
        }
        if (segment != topic_segments[topic_index])
            return false;
        ++filter_index;
        ++topic_index;
    }

    return (filter_index == filter_segments.size() && topic_index == topic_segments.size()) ||
           (filter_index < filter_segments.size() && filter_segments[filter_index] == "#");
}

struct session {
    std::string client_id;
    raw_socket sock;
    std::vector<std::string> filters;
};

class mqtt_server::implementation {
public:
    implementation()
        : listen_sock_(kInvalidSock), port_(0), running_(false) {
        wsa_init();
    }

    ~implementation() {
        stop();
        wsa_fini();
    }

    void start() {
        if (running_.load())
            throw std::runtime_error("mqtt_server is already running");

        listen_sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_sock_ == kInvalidSock)
            throw std::runtime_error("socket creation failed");

        set_reuseaddr(listen_sock_);

        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = 0;

        if (::bind(listen_sock_, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) == kSockErr) {
            sock_close(listen_sock_);
            listen_sock_ = kInvalidSock;
            throw std::runtime_error("bind failed");
        }

        sockaddr_in bound;
        socklen_t alen = sizeof(bound);
        if (::getsockname(listen_sock_, reinterpret_cast<sockaddr*>(&bound),
                          &alen) == kSockErr) {
            sock_close(listen_sock_);
            listen_sock_ = kInvalidSock;
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(bound.sin_port);

        if (::listen(listen_sock_, 10) == kSockErr) {
            sock_close(listen_sock_);
            listen_sock_ = kInvalidSock;
            throw std::runtime_error("listen failed");
        }

        running_.store(true);
        acceptor_ = std::thread([this] { accept_loop(); });
    }

    void stop() {
        if (!running_.load()) return;
        running_.store(false);


        if (listen_sock_ != kInvalidSock) {
            sock_close(listen_sock_);
            listen_sock_ = kInvalidSock;
        }

        if (acceptor_.joinable())
            acceptor_.join();


        {
            std::lock_guard<std::mutex> lock(sessions_mtx_);
            for (auto& session_entry : sessions_) {
                if (session_entry.second.sock != kInvalidSock) {
                    sock_shutdown(session_entry.second.sock);
                    sock_close(session_entry.second.sock);
                    session_entry.second.sock = kInvalidSock;
                }
            }
            sessions_.clear();
        }


        client_thread_group_.join_all();
    }

    std::string base_url() const {
        if (!running_.load())
            throw std::runtime_error("mqtt_server is not running");
        return "mqtt://127.0.0.1:" + std::to_string(port_);
    }

    uint16_t port() const { return port_; }

    std::vector<mqtt_message> messages() const {
        std::lock_guard<std::mutex> lock(history_mtx_);
        return history_;
    }

    void clear_messages() {
        std::lock_guard<std::mutex> lock(history_mtx_);
        history_.clear();
    }

private:
    raw_socket listen_sock_;
    uint16_t port_;
    std::atomic<bool> running_;
    std::thread acceptor_;

    mutable std::mutex sessions_mtx_;
    std::map<std::string, session> sessions_;

    detail::thread_group client_thread_group_;

    mutable std::mutex history_mtx_;
    std::vector<mqtt_message> history_;





    void accept_loop() {
        while (running_.load()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(listen_sock_, &rfds);
            timeval tv{1, 0};
            int ready_count = ::select(static_cast<int>(listen_sock_ + 1),
                             &rfds, nullptr, nullptr, &tv);
            if (ready_count == kSockErr) break;
            if (ready_count == 0) continue;

            sockaddr_in client_addr;
            socklen_t client_addr_len = sizeof(client_addr);
            raw_socket client_sock = ::accept(listen_sock_,
                                      reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
            if (client_sock == kInvalidSock) continue;






            auto done = detail::thread_group::make_stop_flag();
            client_thread_group_.add(
                std::thread([this, client_sock, done]() {
                    client_handler(client_sock);
                    done->store(true);
                }), done);
            client_thread_group_.join_finished();
        }
    }





    void client_handler(raw_socket socket) {

        uint8_t header;
        if (!recv_exact(socket, &header, 1)) { sock_close(socket); return; }

        uint8_t packet_type = header >> 4;

        size_t remaining_length = 0;
        if (!read_remaining_length(socket, remaining_length)) {
            sock_close(socket);
            return;
        }

        if (packet_type != MQTT_CONNECT) {
            sock_close(socket);
            return;
        }

        std::vector<uint8_t> payload(remaining_length);
        if (!recv_exact(socket, payload.data(), remaining_length)) {
            sock_close(socket);
            return;
        }

        connect_packet connect_data;
        if (!decode_connect(payload.data(), remaining_length, connect_data)) {
            sock_close(socket);
            return;
        }

        std::string client_id = connect_data.client_id;
        if (client_id.empty()) {

            client_id = "anon-"
                      + std::to_string(reinterpret_cast<uintptr_t>(&socket));
        }


        {
            std::lock_guard<std::mutex> lock(sessions_mtx_);
            sessions_[client_id] = session{client_id, socket, {}};
        }


        auto connack = encode_connack();
        if (!send_all(socket, connack.data(), connack.size())) {
            remove_session(client_id);
            sock_close(socket);
            return;
        }


        std::vector<uint8_t> buffer(4096);

        while (running_.load()) {
            if (!recv_exact(socket, buffer.data(), 1)) break;

            packet_type = buffer[0] >> 4;

            if (!read_remaining_length(socket, remaining_length)) break;

            if (remaining_length > 0) {
                if (remaining_length > buffer.size())
                    buffer.resize(remaining_length);
                if (!recv_exact(socket, buffer.data(), remaining_length)) break;
            }

            switch (packet_type) {
            case MQTT_PUBLISH:
                handle_publish(socket, buffer.data(), remaining_length, client_id);
                break;

            case MQTT_SUBSCRIBE:
                handle_subscribe(socket, buffer.data(), remaining_length, client_id);
                break;

            case MQTT_PINGREQ: {
                auto pingresp = encode_pingresp();
                send_all(socket, pingresp.data(), pingresp.size());
                break;
            }

            case MQTT_DISCONNECT:
                remove_session(client_id);
                sock_close(socket);
                return;

            default:

                remove_session(client_id);
                sock_close(socket);
                return;
            }
        }


        remove_session(client_id);
        sock_close(socket);
    }





    bool read_remaining_length(raw_socket socket, size_t& out_length) {
        out_length = 0;
        int multiplier = 1;
        for (int byte_index = 0; byte_index < 4; ++byte_index) {
            uint8_t byte;
            if (!recv_exact(socket, &byte, 1)) return false;
            out_length += static_cast<size_t>(byte & 0x7F) * multiplier;
            multiplier *= 128;
            if ((byte & 0x80) == 0) return true;
        }
        return false;
    }





    void handle_publish(raw_socket , const uint8_t* data,
                        size_t size, const std::string& ) {
        publish_packet publish_data;
        if (!decode_publish(data, size, publish_data)) return;


        {
            std::lock_guard<std::mutex> lock(history_mtx_);
            history_.push_back({publish_data.topic, publish_data.payload});
        }


        route_publish(publish_data.topic, publish_data.payload);
    }





    void handle_subscribe(raw_socket socket, const uint8_t* data, size_t size,
                            const std::string& client_id) {
        subscribe_packet subscribe_data;
        if (!decode_subscribe(data, size, subscribe_data)) {

            remove_session(client_id);
            sock_close(socket);
            return;
        }

        std::vector<uint8_t> return_codes;
        {
            std::lock_guard<std::mutex> lock(sessions_mtx_);
            auto it = sessions_.find(client_id);
            if (it != sessions_.end()) {
                for (size_t filter_index = 0; filter_index < subscribe_data.filters.size(); ++filter_index) {
                    it->second.filters.push_back(subscribe_data.filters[filter_index].first);
                    return_codes.push_back(0x00);
                }
            }
        }

        auto suback = encode_suback(subscribe_data.packet_id, return_codes);
        send_all(socket, suback.data(), suback.size());
    }





    void route_publish(const std::string& topic,
                        const std::string& payload) {

        std::vector<raw_socket> targets;
        {
            std::lock_guard<std::mutex> lock(sessions_mtx_);
            for (auto& session_entry : sessions_) {
                for (const auto& filter : session_entry.second.filters) {
                    if (topic_matches(filter, topic)) {
                        targets.push_back(session_entry.second.sock);
                        break;
                    }
                }
            }
        }


        auto publish_frame = encode_publish(topic, payload);
        for (auto target_socket : targets) {
            send_all(target_socket, publish_frame.data(), publish_frame.size());
        }
    }





    void remove_session(const std::string& client_id) {
        std::lock_guard<std::mutex> lock(sessions_mtx_);
        auto it = sessions_.find(client_id);
        if (it != sessions_.end()) {
            if (it->second.sock != kInvalidSock) {
                sock_shutdown(it->second.sock);
                sock_close(it->second.sock);
            }
            sessions_.erase(it);
        }
    }
};

mqtt_server::mqtt_server() : implementation_(new implementation()) {}
mqtt_server::~mqtt_server() = default;

void mqtt_server::start() { implementation_->start(); }
void mqtt_server::stop() { implementation_->stop(); }
std::string mqtt_server::base_url() const { return implementation_->base_url(); }
uint16_t mqtt_server::port() const { return implementation_->port(); }

std::vector<mqtt_message> mqtt_server::messages() const {
    return implementation_->messages();
}
void mqtt_server::clear_messages() { implementation_->clear_messages(); }

}
