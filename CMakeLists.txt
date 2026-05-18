cmake_minimum_required(VERSION 3.15)
project(mock-services VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(MOCK_SERVICES_IS_TOP_LEVEL ON)
else()
    set(MOCK_SERVICES_IS_TOP_LEVEL OFF)
endif()

option(MOCK_SERVICES_BUILD_TESTS "Build mock-services tests" ${MOCK_SERVICES_IS_TOP_LEVEL})
option(MOCK_SERVICES_BUILD_EXAMPLES "Build mock-services examples" ${MOCK_SERVICES_IS_TOP_LEVEL})
option(MOCK_SERVICES_ENABLE_SFTP "Build SFTP server support (requires libssh)" OFF)

# --- Dependencies ---
find_package(httplib CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_package(Threads REQUIRED)

# MinGW workaround: cpp-httplib Conan recipe declares cryptui as a system
# library on Windows, but MinGW does not provide libcryptui.a.
# The system lib comes via cpp-httplib_DEPS_TARGET's generator expressions,
# which get_target_property cannot evaluate. We create an empty stub instead.
if(MINGW)
    set(MOCK_SERVICES_CRYPTUI_STUB "${CMAKE_CURRENT_BINARY_DIR}/libcryptui.a")

    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cryptui_stub.cpp" "")
    add_custom_command(
        OUTPUT "${MOCK_SERVICES_CRYPTUI_STUB}"
        COMMAND ${CMAKE_CXX_COMPILER} -c "${CMAKE_CURRENT_BINARY_DIR}/cryptui_stub.cpp"
            -o "${CMAKE_CURRENT_BINARY_DIR}/cryptui_stub.o"
        COMMAND ${CMAKE_AR} crs "${MOCK_SERVICES_CRYPTUI_STUB}"
            "${CMAKE_CURRENT_BINARY_DIR}/cryptui_stub.o"
        COMMENT "Creating empty cryptui stub for MinGW"
        VERBATIM
    )
    add_custom_target(generate_cryptui_stub ALL
        DEPENDS "${MOCK_SERVICES_CRYPTUI_STUB}"
    )
    install(
        FILES "${MOCK_SERVICES_CRYPTUI_STUB}"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
        OPTIONAL
    )
endif()

# --- Library ---
add_library(${PROJECT_NAME}
    src/ftp_common.cpp
    src/http_common.cpp
    src/mqtt_server.cpp
    src/rest_server.cpp
    src/soap_server.cpp
    src/ftp_server.cpp
    src/temp_dir.cpp
)
add_library(${PROJECT_NAME}::${PROJECT_NAME} ALIAS ${PROJECT_NAME})

target_include_directories(${PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(${PROJECT_NAME}
    PUBLIC
        httplib::httplib
        pugixml::pugixml
        Threads::Threads
)

# FTP server uses Winsock2 on Windows
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PUBLIC ws2_32)
endif()

if(MINGW)
    if(TARGET generate_cryptui_stub)
        add_dependencies(${PROJECT_NAME} generate_cryptui_stub)
    endif()

    target_link_directories(${PROJECT_NAME}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_LIBDIR}>
    )
endif()

# --- SFTP server (optional, requires libssh + mbedtls) ---
if(MOCK_SERVICES_ENABLE_SFTP)
    target_sources(${PROJECT_NAME} PRIVATE src/sftp_server.cpp src/sftp_common.cpp src/sftp_handlers.cpp)
    add_subdirectory(third_party)

    # libssh and mbedtls are downloaded at configure-time via FetchContent
    # (see third_party/CMakeLists.txt). libssh's CMake handles platform
    # system libs (ws2_32, iphlpapi) privately; mbedtls handles pthreads
    # via LINK_WITH_PTHREAD.
    #
    # BUILD_INTERFACE only: these targets exist during build but are NOT
    # installed as part of mock-services. Consumers who install
    # mock-services must provide these dependencies separately.
    target_link_libraries(${PROJECT_NAME} PUBLIC
        $<BUILD_INTERFACE:ssh::ssh>
        $<BUILD_INTERFACE:MbedTLS::mbedtls>
        $<BUILD_INTERFACE:MbedTLS::mbedx509>
        $<BUILD_INTERFACE:MbedTLS::mbedcrypto>
    )
endif()
# --- Tests ---
if(MOCK_SERVICES_IS_TOP_LEVEL)
    include(CTest)
endif()

if(MOCK_SERVICES_BUILD_TESTS AND (NOT DEFINED BUILD_TESTING OR BUILD_TESTING))
    enable_testing()
    add_subdirectory(test)
endif()

# --- Examples ---
if(MOCK_SERVICES_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# --- Install ---
install(
    TARGETS ${PROJECT_NAME}
    EXPORT ${PROJECT_NAME}Targets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(
    DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(
    EXPORT ${PROJECT_NAME}Targets
    NAMESPACE ${PROJECT_NAME}::
    FILE ${PROJECT_NAME}Targets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)
