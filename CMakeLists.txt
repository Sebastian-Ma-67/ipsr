cmake_minimum_required(VERSION 2.6 FATAL_ERROR)

project(IPSR)

SET(CMAKE_BUILD_TYPE Debug)

file (GLOB MY_SRC Src/*.h
                  Src/*.cpp)

add_executable(IPSR "${MY_SRC}")
