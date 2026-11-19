cmake_minimum_required(VERSION 3.24)
project(ghostty-union VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network DBus Test)
find_package(KSysGuard CONFIG REQUIRED)

find_path(GHOSTTY_VT_INCLUDE_DIR
    NAMES ghostty/vt.h
    PATHS /usr/include /usr/local/include
    REQUIRED
)
find_library(GHOSTTY_VT_LIBRARY
    NAMES ghostty-vt
    PATHS /usr/lib /usr/local/lib
    REQUIRED
)

add_library(ghostty-union-core STATIC
    src/MainWindow.cpp
    src/MainWindow.h
    src/StatsBarWidget.cpp
    src/StatsBarWidget.h
    src/TerminalTabPane.cpp
    src/TerminalTabPane.h
    src/TerminalWidget.cpp
    src/TerminalWidget.h
    src/KeyMap.cpp
    src/KeyMap.h
    src/Theme.h
)

target_include_directories(ghostty-union-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${GHOSTTY_VT_INCLUDE_DIR}
)

target_link_libraries(ghostty-union-core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::DBus
    KSysGuard::SystemStats
    KSysGuard::Formatter
    ${GHOSTTY_VT_LIBRARY}
)

target_compile_definitions(ghostty-union-core PUBLIC
    QT_NO_KEYWORDS
)

target_compile_options(ghostty-union-core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(ghostty-union
    src/main.cpp
)
target_link_libraries(ghostty-union PRIVATE ghostty-union-core)
target_compile_options(ghostty-union PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_executable(ghostty-union-tests
    tests/smoke_test.cpp
)
target_link_libraries(ghostty-union-tests PRIVATE ghostty-union-core Qt6::Test)
target_compile_options(ghostty-union-tests PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME ghostty_union_smoke COMMAND ghostty-union-tests)
set_tests_properties(ghostty_union_smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

install(TARGETS ghostty-union RUNTIME DESTINATION bin)
