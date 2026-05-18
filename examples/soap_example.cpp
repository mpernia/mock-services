#include <cstdlib>
#include <iostream>

#include <httplib.h>

#include "mock-services/soap_server.h"

int main() {
    mock_services::soap_server server;

    server.route("/Calculator", "Add",
        [](const mock_services::soap_request& request) {
            return mock_services::soap_response{
                200,
                R"(<soap:Envelope xmlns:soap="http://schemas.xmlsoap.org/soap/envelope/">
  <soap:Body>
    <AddResponse><Result>42</Result></AddResponse>
  </soap:Body>
</soap:Envelope>)"};
        });

    server.when("/Greeting", "SayHello")
        .then_return(mock_services::soap_response{
            200,
            R"(<soap:Envelope xmlns:soap="http://schemas.xmlsoap.org/soap/envelope/">
  <soap:Body>
    <SayHelloResponse><Message>Hello from mock SOAP</Message></SayHelloResponse>
  </soap:Body>
</soap:Envelope>)"});

    server.start();
    std::cout << "SOAP server running at " << server.base_url() << '\n';

    std::cout << "\nRegistered routes:\n"
              << "  POST /Calculator  SOAPAction: Add\n"
              << "  POST /Greeting    SOAPAction: SayHello\n";

    std::cout << "\nSOAP 1.2 example:\n"
              << "  curl -X POST " << server.base_url() << "/Calculator \\\n"
              << "    -H \"Content-Type: application/soap+xml\" \\\n"
              << "    -H \"SOAPAction: Add\" \\\n"
              << "    -d '<soap:Envelope ...><soap:Body><Add>..."
              << " </Add></soap:Body></soap:Envelope>'\n";

    std::cout << "\nPress Enter to stop the server...";
    std::cin.get();

    server.stop();
    std::cout << "Request history: " << server.requests().size()
              << " request(s)\n";
    std::cout << "Server stopped.\n";
    return EXIT_SUCCESS;
}
