cmake_minimum_required(VERSION 3.21)

# 根工程名对齐 Mine 项目目录名 chandao，使用 Visual Studio 生成器时产出 chandao.sln
project(chandao VERSION 0.1.0 LANGUAGES CXX)

add_subdirectory(main)
