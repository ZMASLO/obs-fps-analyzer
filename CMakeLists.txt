cmake_minimum_required(VERSION 3.16)
project(obs-fps-analyzer VERSION 0.2.1)

find_package(libobs REQUIRED)

add_subdirectory(plugins/fps-analyzer)
