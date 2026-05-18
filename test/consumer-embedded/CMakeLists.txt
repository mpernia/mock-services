cmake_minimum_required(VERSION 3.15)
project(mock_services_embedded_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(MOCK_SERVICES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MOCK_SERVICES_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../.." mock-services-build)

add_executable(embedded_consumer main.cpp)
target_link_libraries(embedded_consumer PRIVATE mock-services::mock-services)
