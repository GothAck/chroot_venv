cmake_minimum_required(VERSION 3.13)

project(chroot_venv CXX)

set(CMAKE_CXX_STANDARD 20)

add_subdirectory(procmounts)

add_executable(
  chroot_venv
  main.cpp
)

add_library(
    chroot_config
    OBJECT
    config.cpp
)

install(TARGETS chroot_venv DESTINATION libexec PERMISSIONS WORLD_EXECUTE SETUID)

target_link_libraries(chroot_venv chroot_config procmounts docopt stdc++fs)
target_link_libraries(chroot_config yaml-cpp stdc++fs)

target_include_directories(chroot_config PUBLIC .)
