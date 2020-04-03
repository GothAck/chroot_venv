find_package(Qt5 COMPONENTS Core Network REQUIRED)

add_library(
  procmounts
  OBJECT
  procmounts.cpp
)

target_include_directories(procmounts PUBLIC include PRIVATE .)
