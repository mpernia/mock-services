#include <cstdlib>
#include <iostream>

#include "mock-services/mqtt_server.h"

int main() {
    mock_services::mqtt_server server;

    server.start();
    std::cout << "MQTT server running at " << server.base_url() << '\n';

    std::cout << "\nUsage example (mosquitto_pub / mosquitto_sub):\n"
              << "  Subscribe: mosquitto_sub -h 127.0.0.1 -p "
              << server.port() << " -t \"sensor/#\" -q 0\n"
              << "  Publish:   mosquitto_pub -h 127.0.0.1 -p "
              << server.port() << " -t \"sensor/temp\" -q 0 \\\n"
              << "               -m '{\"value\": 23.5}'\n";

    std::cout << "\nWildcard filters supported:\n"
              << "  sensor/temperature     exact match\n"
              << "  sensor/+/temperature   single-level (+)\n"
              << "  sensor/#               multi-level (#)\n";

    std::cout << "\nPress Enter to stop the server...";
    std::cin.get();

    server.stop();

    std::cout << "Published messages captured: "
              << server.messages().size() << '\n';
    for (const auto& msg : server.messages()) {
        std::cout << "  " << msg.topic << ": " << msg.payload << '\n';
    }

    std::cout << "Server stopped.\n";
    return EXIT_SUCCESS;
}
