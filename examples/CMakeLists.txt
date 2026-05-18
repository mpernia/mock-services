if(NOT TARGET mock-services::mock-services)
    find_package(mock-services CONFIG REQUIRED)
endif()

add_executable(rest_example
    rest_example.cpp
)
target_link_libraries(rest_example
    PRIVATE mock-services::mock-services
)

add_executable(soap_example
    soap_example.cpp
)
target_link_libraries(soap_example
    PRIVATE mock-services::mock-services
)

add_executable(ftp_example
    ftp_example.cpp
)
target_link_libraries(ftp_example
    PRIVATE mock-services::mock-services
)

add_executable(mqtt_example
    mqtt_example.cpp
)
target_link_libraries(mqtt_example
    PRIVATE mock-services::mock-services
)

if(MOCK_SERVICES_ENABLE_SFTP)
    add_executable(sftp_example
        sftp_example.cpp
    )
    target_link_libraries(sftp_example
        PRIVATE mock-services::mock-services
    )
endif()
