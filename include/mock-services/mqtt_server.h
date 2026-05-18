#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mock_services {

struct mqtt_message {
    std::string topic;
    std::string payload;
};

class mqtt_server {
public:
    mqtt_server();
    ~mqtt_server();

    mqtt_server(const mqtt_server&) = delete;
    mqtt_server& operator=(const mqtt_server&) = delete;

    void start();
    void stop();

    std::string base_url() const;
    uint16_t port() const;

    std::vector<mqtt_message> messages() const;
    void clear_messages();

private:
    class implementation;
    std::unique_ptr<implementation> implementation_;
};

}
