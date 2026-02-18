cmake_minimum_required(VERSION 3.20)
project(CppDownloader VERSION 1.0.0 LANGUAGES CXX)

# ==============================================================================
# グローバル設定
# ==============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Windows での Unicode 対応
if(WIN32)
    add_compile_definitions(UNICODE _UNICODE)
    # Windows での min/max マクロ問題を回避
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ==============================================================================
# コンパイラ警告設定
# ==============================================================================
if(MSVC)
    add_compile_options(
        /W4          # 警告レベル 4
        /WX          # 警告をエラーとして扱う
        /permissive- # 準拠モード
        /utf-8       # ソースファイルを UTF-8 として扱う
        # 外部ライブラリのヘッダーで発生する警告を抑制
        /wd4100      # 未参照の仮引数
    )
else()
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
    )
endif()

# ==============================================================================
# curl の検索
# CMAKE_PREFIX_PATH または vcpkg toolchain で解決することを推奨
# 例: cmake -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ..
# ==============================================================================
find_package(CURL REQUIRED)

# ==============================================================================
# Downloader ライブラリ (静的ライブラリとして定義)
# main.cpp とテストから共通して使用する
# ==============================================================================
add_library(DownloaderLib STATIC
    src/Downloader.cpp
    src/CurlHandle.cpp
)

target_include_directories(DownloaderLib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(DownloaderLib
    PUBLIC
        CURL::libcurl
)

# Windows での追加リンク設定
if(WIN32)
    target_link_libraries(DownloaderLib
        PUBLIC
            ws2_32       # Winsock
            wldap32      # LDAP (curl が使用)
            crypt32      # 暗号化 API
            normaliz     # IDN サポート
    )
endif()

# ==============================================================================
# メイン実行ファイル
# ==============================================================================
add_executable(downloader
    src/main.cpp
)

target_link_libraries(downloader
    PRIVATE
        DownloaderLib
)

# ==============================================================================
# ユニットテスト
# ==============================================================================
option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS)
    # GoogleTest の取得（FetchContent を使用 - vcpkg の代替）
    include(FetchContent)

    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0  # 安定リリースを固定
    )

    # Windows: DLL の共有ランタイムを回避
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(googletest)

    # テスト実行ファイル
    add_executable(DownloaderTests
        tests/DownloaderTest.cpp
    )

    target_include_directories(DownloaderTests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )

    target_link_libraries(DownloaderTests
        PRIVATE
            DownloaderLib
            GTest::gtest
            GTest::gmock
    )

    # CTest への登録
    include(GoogleTest)
    enable_testing()
    gtest_discover_tests(DownloaderTests
        TIMEOUT 60  # テスト全体のタイムアウト
    )

    # IDE でテストを見やすくするフォルダ設定
    set_target_properties(DownloaderTests PROPERTIES FOLDER "Tests")
endif()

# ==============================================================================
# インストール設定 (オプション)
# ==============================================================================
install(TARGETS downloader
    RUNTIME DESTINATION bin
)

install(FILES
    include/Downloader.h
    include/IDownloaderObserver.h
    include/ICurlHandle.h
    DESTINATION include/downloader
)

# ==============================================================================
# 設定のサマリー表示
# ==============================================================================
message(STATUS "=== CppDownloader Build Configuration ===")
message(STATUS "  C++ Standard    : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Build Tests     : ${BUILD_TESTS}")
message(STATUS "  CURL Version    : ${CURL_VERSION_STRING}")
message(STATUS "==========================================")
