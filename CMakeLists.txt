cmake_minimum_required(VERSION 3.29)

project(
    Frost
    VERSION 0.1.0
    LANGUAGES CXX
)

include(cmake/Dependencies.cmake)

add_subdirectory(engine)
add_subdirectory(game)