find_package(GTest REQUIRED)

add_executable(mock-services_test
    mqtt_server_test.cpp
    rest_server_test.cpp
    soap_server_test.cpp
    ftp_server_test.cpp
)
target_link_libraries(mock-services_test
    PRIVATE
        mock-services::mock-services
        GTest::gtest
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(mock-services_test
    DISCOVERY_TIMEOUT 30
)

# --- SFTP server tests (optional, requires libssh) ---
if(MOCK_SERVICES_ENABLE_SFTP)
    add_executable(mock-services-sftp-test sftp_server_test.cpp)
    target_link_libraries(mock-services-sftp-test PRIVATE
        mock-services::mock-services
        GTest::gtest
        GTest::gtest_main
        ssh::ssh
    )

    # Win32 needs Winsock for the test WSA guard
    if(WIN32)
        target_link_libraries(mock-services-sftp-test PRIVATE ws2_32)
    endif()

    add_test(NAME sftp_server_test COMMAND mock-services-sftp-test)
endif()
