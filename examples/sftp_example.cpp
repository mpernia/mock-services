#include <cstdlib>
#include <iostream>

#include "mock-services/sftp_server.h"

int main() {
    mock_services::sftp_server server;

    server.add_user("alice", "secret", "/tmp/alice",
                    mock_services::sftp_permission::all);

    server.start();
    std::cout << "SFTP server running at " << server.base_url() << '\n';
    std::cout << "  temp root: " << server.temp_root() << '\n';

    std::cout << "\nUsers:\n"
              << "  alice  (password: secret)  "
              << "root: /tmp/alice  permission: all\n";

    std::cout << "\nUsage example:\n"
              << "  sftp://alice:secret@127.0.0.1:" << server.port() << "/\n";

    std::cout << "\nPress Enter to stop the server...";
    std::cin.get();

    server.stop();

    for (const auto& entry : server.activity()) {
        std::cout << "  " << entry.user << " | " << entry.operation
                  << " " << entry.path << " -> " << entry.result << '\n';
    }

    std::cout << "Server stopped.\n";
    return EXIT_SUCCESS;
}
