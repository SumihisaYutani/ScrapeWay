cmake_minimum_required(VERSION 3.21)
project(ScrapeWay VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network Concurrent)
qt_standard_project_setup()

if(MSVC)
    add_compile_options(/utf-8 /permissive-)
endif()

# 取得・保存の本体 (画面なし)。アプリと確認ツールで共有する
qt_add_library(scrapeway_core STATIC
    src/net.cpp src/net.h
    src/util.cpp src/util.h
    src/textcodec.cpp src/textcodec.h
    src/model.h
    src/paths.cpp src/paths.h
    src/pixiv.cpp src/pixiv.h
    src/wayback.cpp src/wayback.h
)
qt_add_resources(scrapeway_core "templates" PREFIX "/" BASE resources FILES resources/reader.html)
target_link_libraries(scrapeway_core PUBLIC Qt6::Core Qt6::Network)

qt_add_executable(ScrapeWay WIN32
    src/main.cpp
    src/mainwindow.cpp src/mainwindow.h
    src/chromelogin.cpp src/chromelogin.h
    src/cdpsocket.cpp src/cdpsocket.h
    src/jobs.cpp src/jobs.h
)
target_compile_definitions(ScrapeWay PRIVATE SCRAPEWAY_VERSION="${PROJECT_VERSION}")
target_link_libraries(ScrapeWay PRIVATE scrapeway_core Qt6::Widgets Qt6::Concurrent)

# 開発用の確認ツール (配布物には含めない)
qt_add_executable(scrapeway_cli tests/cli.cpp)
target_link_libraries(scrapeway_cli PRIVATE scrapeway_core)

qt_add_executable(cdptest tests/cdptest.cpp src/cdpsocket.cpp src/cdpsocket.h)
target_link_libraries(cdptest PRIVATE Qt6::Core Qt6::Network)
