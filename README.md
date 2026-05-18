# mock-services — fake external services for C++ tests

Stop wrestling with Docker containers, daemon processes, or network setup just to test code that talks to REST, SOAP, FTP, MQTT, or SFTP servers.

`mock-services` embeds lightweight mock servers directly in your test process. One `server.start()` call — no ports to configure, no external dependencies to install, no CI flakiness.

```cpp
mock_services::rest_server server;
server.when(mock_services::method::get, "/hello")
    .then_return(mock_services::response_builder{}
        .status(mock_services::status::ok)
        .content_type(mock_services::content_type::json)
        .body(R"({"message":"Hello, world!"})"));
server.start();
// → http://127.0.0.1:54321

auto res = httplib::Client(server.base_url()).Get("/hello");
// res->status == 200
```

---

## At a glance

| Mock server | Protocol | Key features |
|-------------|----------|-------------|
| **REST** | HTTP/1.1 | Route matching, declarative/programmatic responses, request history |
| **SOAP** | SOAP 1.1 / 1.2 | Action matching, fault factory, request history |
| **FTP** | RFC 959 | User auth, permissions, virtual filesystem, session tracking |
| **MQTT** | 3.1.1 | Topic pub/sub, wildcard matching (`+` / `#`), message history |
| **SFTP** ⚠️ | SFTP v3 | User auth, permissions, activity log, virtual fs *(not in pre-built releases; needs custom build)* |

**Language**: C++11 · **Platforms**: Linux (x86_64), Windows (i686 MinGW)  
**Delivery**: Static library (`.a`), Conan package, CMake targets  
**License**: [MIT](LICENSE)

---

## Quick start

All servers follow the same lifecycle.

```cpp
// 1. Create
mock_services::rest_server server;

// 2. Declare routes
server.when(mock_services::method::get, "/hello")
    .then_return(mock_services::response_builder{}
        .status(mock_services::status::ok)
        .body(R"({"message":"Hello!"})"));

// 3. Start (dynamic port, 127.0.0.1)
server.start();

// 4. Use the server
httplib::Client cli(server.base_url());
auto res = cli.Get("/hello");

// 5. Stop (also called by destructor)
server.stop();
```

---

## Servers

### REST

HTTP/1.1 mock server with route matching and request history.

```cpp
mock_services::rest_server server;

// Declarative route
server.when(mock_services::method::get, "/hello")
    .then_return(mock_services::response_builder{}
        .status(mock_services::status::ok)
        .content_type(mock_services::content_type::json)
        .body(R"({"message":"Hello"})"));

// Programmatic route
server.route(mock_services::method::post, "/echo",
    [](const mock_services::request& req) {
        return mock_services::response{
            mock_services::status::ok,
            req.body,
            mock_services::content_type::text};
    });

// Typed constants
//   method::   get, post, put, del, patch, head, options
//   status::   ok(200), created(201), no_content(204),
//              bad_request(400), not_found(404), internal_server_error(500)
//   content_type:: json, html, text, xml, form_urlencoded

server.start();

// Inspect history
std::vector<mock_services::request> history = server.requests();
server.clear_requests();

server.stop();
```

**Full example**: [`examples/rest_example.cpp`](examples/rest_example.cpp)

---

### SOAP

SOAP 1.1 / 1.2 mock server with action matching, declarative routes, and built-in fault factory.

```cpp
mock_services::soap_server server;

server.route("/Calculator", "Add",
    [](const mock_services::soap_request& req) {
        return mock_services::soap_response{
            200,
            R"(<soap:Envelope ...><soap:Body>
                <AddResponse><Result>42</Result></AddResponse>
            </soap:Body></soap:Envelope>)"};
    });

// Declarative
server.when("/Greeting", "SayHello")
    .then_return(mock_services::soap_response{
        200, R"(<soap:Envelope ...>...</soap:Envelope>)"});

// Fault helper
auto fault = mock_services::soap_response::fault(
    500, "Receiver", "Something went wrong",
    mock_services::soap_version::v1_2);

server.start();
std::vector<mock_services::soap_request> history = server.requests();
server.stop();
```

**Full example**: [`examples/soap_example.cpp`](examples/soap_example.cpp)

---

### FTP

FTP (RFC 959) mock server with user authentication, permission control, and virtual filesystem.

```cpp
mock_services::ftp_server server;

server.add_user("alice", "secret", "/tmp/alice",
                mock_services::ftp_permission::all);
server.add_anonymous_user("/tmp/pub",
                          mock_services::ftp_permission::read);

// Permissions: none(0), read(1), write(2), all(3)
server.start();

uint16_t port = server.port();
std::string temp_root = server.temp_root();
std::vector<std::string> active = server.connected_users();

server.stop();
```

**Full example**: [`examples/ftp_example.cpp`](examples/ftp_example.cpp)

---

### MQTT

MQTT 3.1.1 broker mock server with topic subscription, wildcard matching (`+` single-level, `#` multi-level), and message history.

```cpp
mock_services::mqtt_server server;
server.start();

// Any MQTT client can publish/subscribe.
// Topics: sensor/temperature, sensor/+/temperature, sensor/#

// Inspect history
std::vector<mock_services::mqtt_message> msgs = server.messages();
server.clear_messages();

server.stop();
```

**Full example**: [`examples/mqtt_example.cpp`](examples/mqtt_example.cpp)

---

### SFTP

SFTP v3 mock server (via libssh) with user authentication, permission control, and activity logging. **Not included in pre-built releases** — requires building from source with `MOCK_SERVICES_ENABLE_SFTP=ON`.

```cpp
mock_services::sftp_server server;
server.add_user("alice", "secret", "/tmp/alice",
                mock_services::sftp_permission::all);
server.start();

std::vector<mock_services::sftp_activity> log = server.activity();
server.clear_activity();

server.stop();
```

**Full example**: [`examples/sftp_example.cpp`](examples/sftp_example.cpp)

---

## Common API

| Method | Description |
|--------|-------------|
| `start()` | Bind to `127.0.0.1`, pick a dynamic port, start serving |
| `stop()` | Graceful shutdown, safe to call when already stopped |
| `base_url()` | Returns `protocol://127.0.0.1:<port>` (throws if not started) |

- `start()` throws if already running
- `stop()` is idempotent — safe to call multiple times
- Destructor calls `stop()` automatically
- Servers are not copyable

---

## Build

### Using Make (recommended for day-to-day)

The Makefile is a thin wrapper over Conan + CMake. It auto-detects your OS and runs `conan install` if needed — even on a fresh clone.

```bash
make              # build + test
make build        # compile library + tests
make test         # run tests
make examples     # build standalone examples
make install      # cmake --install --prefix /usr/local
make release      # build + package into dist/ (see Releases)
make clean        # remove compiled objects (keeps Conan generators)
make distclean    # remove build/ entirely
make mrproper     # remove build/ + third_party/ + test output
```

### Using CMake directly

**With Conan** (recommended for dependency management):

```bash
conan install . --build=missing -c tools.cmake.cmaketoolchain:generator="Unix Makefiles"
cmake --preset conan-release
cmake --build --preset conan-release
ctest --preset conan-release
```

**With SFTP support:**

```bash
conan install . --build=missing \
    -o "&:with_sftp=True" \
    -c tools.cmake.cmaketoolchain:generator="Unix Makefiles"
cmake --preset conan-release -DMOCK_SERVICES_ENABLE_SFTP=ON
cmake --build --preset conan-release
```

**Without Conan** (dependencies must be discoverable by CMake):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DMOCK_SERVICES_BUILD_TESTS=OFF \
    -DMOCK_SERVICES_BUILD_EXAMPLES=OFF
cmake --build build
```

---

## Consumption

### Option A: Via Conan (recommended)

Conan resolves all dependencies (`httplib`, `pugixml`) automatically.

```python
# conanfile.py
def requirements(self):
    self.test_requires("mock-services/0.1.0")
```

Then in your CMakeLists.txt:

```cmake
find_package(mock-services CONFIG REQUIRED)
target_link_libraries(my_tests PRIVATE mock-services::mock-services)
```

### Option B: Via release tarball

Each [GitHub release](https://github.com/mpernia/mock-services/releases) publishes pre-built archives containing the static library **and** all its dependencies — no Conan needed.

> **SFTP note**: Pre-built releases are compiled **without** SFTP support (requires libssh + mbedtls). To use the SFTP server, build from source with `MOCK_SERVICES_ENABLE_SFTP=ON`.

```bash
# Download and extract
tar xzf mock-services-v0.1.0-linux-x86_64.tar.gz        # Linux
unzip mock-services-v0.1.0-win32.zip                     # Windows

# Compile and link
g++ -I mock-services/include -L mock-services/lib test.cpp \
    -lmock-services -lpugixml -lpthread -lm
```

The archive layout is self-contained:

```
mock-services/
├── lib/
│   ├── libmock-services.a     (stripped)
│   └── libpugixml.a           (stripped)
├── include/
│   ├── httplib/httplib.h       ← header-only dependency
│   ├── mock-services/          ← library headers
│   │   ├── rest_server.h
│   │   ├── soap_server.h
│   │   ├── ftp_server.h
│   │   ├── mqtt_server.h
│   │   ├── sftp_server.h       (requires SFTP build)
│   │   ├── sftp_common.h       (requires SFTP build)
│   │   ├── http_common.h
│   │   ├── ftp_common.h
│   │   ├── mqtt_common.h
│   │   └── detail/
│   │       ├── temp_dir.h
│   │       ├── platform_socket.h
│   │       ├── thread_utils.h
│   │       ├── path_utils.h
│   │       └── sftp_handlers.h (requires SFTP build)
│   ├── pugiconfig.hpp          ← pugixml header
│   └── pugixml.hpp             ← pugixml header
└── LICENSE
```

### Option C: As a vendored subdirectory

```cmake
set(MOCK_SERVICES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MOCK_SERVICES_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/mock-services)
target_link_libraries(my_tests PRIVATE mock-services::mock-services)
```

Dependencies required: `httplib::httplib`, `pugixml::pugixml`, `Threads::Threads`.

---

## Project

- [LICENSE](LICENSE) — MIT
- Repository: <https://github.com/mpernia/mock-services>
