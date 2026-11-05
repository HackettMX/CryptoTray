cmake_minimum_required(VERSION 3.20)
project(CryptoTray LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源码统一 UTF-8：不指定时 MSVC 会按系统代码页(GBK)解析中文注释，
# 个别汉字尾字节恰好是 0x5C，会把注释变成续行并吞掉后续代码。
if (MSVC)
    add_compile_options(/utf-8)
endif()

# 静态链接 MSVC 运行时，生成无需安装 VC++ 运行库的单文件 exe
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# 主程序：原生 Windows GUI（托盘 + 弹出小窗）
add_executable(CryptoTray WIN32
    src/main.cpp
    src/http.cpp
    src/market.cpp
    src/config.cpp
)
target_include_directories(CryptoTray PRIVATE src)
target_link_libraries(CryptoTray PRIVATE winhttp comctl32 shell32 dwmapi uxtheme)
set_target_properties(CryptoTray PROPERTIES WIN32_EXECUTABLE TRUE)

# 数据层冒烟测试（控制台）：验证 OKX 接口与解析
add_executable(test_okx
    test_okx.cpp
    src/http.cpp
    src/market.cpp
)
target_include_directories(test_okx PRIVATE src)
target_link_libraries(test_okx PRIVATE winhttp)

# 限频验证（控制台）：突发请求下客户端限流是否生效
add_executable(test_rate
    test_rate.cpp
    src/http.cpp
)
target_include_directories(test_rate PRIVATE src)
target_link_libraries(test_rate PRIVATE winhttp)

# 超时验证（控制台）：慢接口必须被 1 秒截止时间掐断
add_executable(test_timeout
    test_timeout.cpp
    src/http.cpp
)
target_include_directories(test_timeout PRIVATE src)
target_link_libraries(test_timeout PRIVATE winhttp)
