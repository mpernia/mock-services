cmake_minimum_required(VERSION 3.15)
project(mock_services_installed_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(mock-services CONFIG REQUIRED)

add_executable(installed_consumer main.cpp)
target_link_libraries(installed_consumer PRIVATE mock-services::mock-services)
