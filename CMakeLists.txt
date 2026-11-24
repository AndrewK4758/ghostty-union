cmake_minimum_required(VERSION 3.24)
project(ghostty-union VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network DBus Test)
find_package(KSysGuard CONFIG REQUIRED)

find_package(ghostty-vt 1.0 CONFIG QUIET)
if(NOT TARGET ghostty-vt::ghostty-vt)
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
endif()

add_library(ghostty-union-core STATIC
    src/MainWindow.cpp
    src/MainWindow.h
    src/SessionState.cpp
    src/SessionState.h
    src/StatsBarWidget.cpp
    src/StatsBarWidget.h
    src/TerminalTabPane.cpp
    src/TerminalTabPane.h
    src/TerminalWidget.cpp
    src/TerminalWidget.h
    src/TerminalLink.cpp
    src/TerminalLink.h
    src/KeyMap.cpp
    src/KeyMap.h
    src/Theme.h
)

target_include_directories(ghostty-union-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(ghostty-union-core PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::DBus
    KSysGuard::SystemStats
    KSysGuard::Formatter
)
if(TARGET ghostty-vt::ghostty-vt)
    target_link_libraries(ghostty-union-core PUBLIC ghostty-vt::ghostty-vt)
else()
    target_include_directories(ghostty-union-core PUBLIC ${GHOSTTY_VT_INCLUDE_DIR})
    target_link_libraries(ghostty-union-core PUBLIC ${GHOSTTY_VT_LIBRARY})
endif()

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

include(GNUInstallDirs)

add_test(NAME ghostty_union_smoke COMMAND ghostty-union-tests)
set_tests_properties(ghostty_union_smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;GHOSTTY_UNION_BUILD_DESKTOP=${CMAKE_CURRENT_BINARY_DIR}/ghostty-union.desktop"
)

set(GHOSTTY_UNION_EXEC "${CMAKE_INSTALL_FULL_BINDIR}/ghostty-union")
set(GHOSTTY_UNION_ICON "com.mitchellh.ghostty")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/ghostty-union.desktop.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ghostty-union.desktop"
    @ONLY
)

install(TARGETS ghostty-union RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/ghostty-union.svg"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")

# Configure ghostty-union.desktop at `cmake --install` time so `--prefix ~/.local`
# (which is often absent from KDE Plasma / systemd user session PATH) always embeds
# the exact absolute binary path in TryExec= and Exec=.
install(CODE "
    set(GHOSTTY_UNION_EXEC \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/ghostty-union\")
    set(GHOSTTY_UNION_ICON \"ghostty-union\")
    if(\"\$ENV{DESTDIR}\" STREQUAL \"\")
        if(EXISTS \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/icons/candy-icons/apps/scalable/com.mitchellh.ghostty.svg\"
           OR EXISTS \"/usr/share/icons/hicolor/512x512/apps/com.mitchellh.ghostty.png\"
           OR EXISTS \"/usr/share/icons/hicolor/256x256/apps/com.mitchellh.ghostty.png\")
            set(GHOSTTY_UNION_ICON \"com.mitchellh.ghostty\")
        endif()
    endif()
    configure_file(
        \"${CMAKE_CURRENT_SOURCE_DIR}/ghostty-union.desktop.in\"
        \"${CMAKE_CURRENT_BINARY_DIR}/ghostty-union.install.desktop\"
        @ONLY
    )
    file(INSTALL
        DESTINATION \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/applications\"
        TYPE FILE
        RENAME \"ghostty-union.desktop\"
        FILES \"${CMAKE_CURRENT_BINARY_DIR}/ghostty-union.install.desktop\"
    )
")
