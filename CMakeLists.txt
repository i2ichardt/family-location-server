cmake_minimum_required(VERSION 3.16)
project(FamilyLocationServer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${PROJECT_SOURCE_DIR}/include)
add_executable(FamilyLocationServer server.cpp)

find_package(Threads REQUIRED)
target_link_libraries(FamilyLocationServer PRIVATE Threads::Threads)

if (WIN32)
    target_link_libraries(FamilyLocationServer PRIVATE ws2_32 crypt32)
endif()
