#include <cstdlib>
#include <iostream>
#include <thread>

#include "mock-services/ftp_server.h"

int main() {
    mock_services::ftp_server server;

    server.add_user("alice", "secret", "/tmp/alice",
                    mock_services::ftp_permission::all);

    server.add_anonymous_user("/tmp/pub",
                              mock_services::ftp_permission::read);

    server.start();
    std::cout << "FTP server running at " << server.base_url() << '\n';
    std::cout << "  temp root: " << server.temp_root() << '\n';

    std::cout << "\nUsers:\n"
              << "  alice  (password: secret)  "
              << "root: /tmp/alice  permission: all\n"
              << "  anon   (no password)       "
              << "root: /tmp/pub    permission: read\n";

    std::cout << "\nUsage example:\n"
              << "  ftp://alice:secret@127.0.0.1:" << server.port() << "/\n"
              << "  ftp://anonymous:@127.0.0.1:" << server.port() << "/\n";

    std::cout << "\nPress Enter to stop the server...";
    std::cin.get();

    server.stop();

    std::cout << "Connected users at shutdown: "
              << server.connected_users().size() << '\n';
    std::cout << "Server stopped.\n";
    return EXIT_SUCCESS;
}
