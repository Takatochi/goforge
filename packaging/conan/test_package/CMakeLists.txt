cmake_minimum_required(VERSION 3.20)
project(goforge_test_package LANGUAGES CXX)

find_package(goforge REQUIRED CONFIG)

add_executable(test_package test_package.cpp)
target_link_libraries(test_package PRIVATE goforge::goforge)
target_compile_features(test_package PRIVATE cxx_std_20)
