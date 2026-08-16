/// @file test_process.cpp
/// @brief Process 模块测试，使用 ZLMediaKit MediaServer 作为测试目标

#include "common/process/process.h"

#include <boost/asio.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;

// ZLMediaKit MediaServer 路径
const std::string ZLMEDIAKIT_PATH = "E:/project/video-monitor/third_apps/win32/zlmediakit/MediaServer.exe";

/// @brief 测试基本启动和终止
void TestStartAndTerminate() {
    std::cout << "=== Test: Start and Terminate ===" << std::endl;
    
    boost::asio::io_context io_context;
    common::process::Process process(io_context.get_executor());
    
    common::process::ProcessOptions options;
    options.executable = ZLMEDIAKIT_PATH;
    options.console_mode = common::process::ConsoleMode::NewConsole;
    options.terminal_mode = common::process::TerminalMode::NewTerminal;
    options.terminal_title = "ZLMediaKit Test";
    
    std::cout << "Starting MediaServer..." << std::endl;
    if (!process.Start(options)) {
        std::cerr << "Failed to start process: " << process.LastError() << std::endl;
        return;
    }
    
    std::cout << "Process started, PID: " << process.Pid() << std::endl;
    std::cout << "IsRunning: " << (process.IsRunning() ? "true" : "false") << std::endl;
    
    // 等待 3 秒
    std::cout << "Waiting 3 seconds..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(3));
    
    // 终止进程
    std::cout << "Terminating process..." << std::endl;
    boost::system::error_code ec;
    if (process.Terminate(ec)) {
        std::cout << "Process terminated successfully" << std::endl;
    } else {
        std::cerr << "Failed to terminate: " << ec.message() << std::endl;
    }
    
    std::cout << "IsRunning after terminate: " << (process.IsRunning() ? "true" : "false") << std::endl;
    std::cout << std::endl;
}

/// @brief 测试异步等待
void TestAsyncWait() {
    std::cout << "=== Test: Async Wait ===" << std::endl;
    
    boost::asio::io_context io_context;
    common::process::Process process(io_context.get_executor());
    
    common::process::ProcessOptions options;
    options.executable = ZLMEDIAKIT_PATH;
    options.console_mode = common::process::ConsoleMode::NewConsole;
    options.terminal_mode = common::process::TerminalMode::NewTerminal;
    options.terminal_title = "ZLMediaKit AsyncWait Test";
    
    std::cout << "Starting MediaServer..." << std::endl;
    if (!process.Start(options)) {
        std::cerr << "Failed to start process: " << process.LastError() << std::endl;
        return;
    }
    
    std::cout << "Process started, PID: " << process.Pid() << std::endl;
    
    // 设置异步等待
    process.AsyncWait([&io_context](const boost::system::error_code& ec, int exit_code) {
        if (ec) {
            std::cerr << "AsyncWait error: " << ec.message() << std::endl;
        } else {
            std::cout << "Process exited with code: " << exit_code << std::endl;
        }
        io_context.stop();
    });
    
    // 2 秒后请求退出
    std::cout << "Will request exit in 2 seconds..." << std::endl;
    boost::asio::steady_timer timer(io_context);
    timer.expires_after(std::chrono::seconds(2));
    timer.async_wait([&process](const boost::system::error_code& ec) {
        if (!ec) {
            std::cout << "Requesting process exit..." << std::endl;
            boost::system::error_code exit_ec;
            process.RequestExit(exit_ec);
        }
    });
    
    // 运行 io_context
    io_context.run();
    std::cout << std::endl;
}

/// @brief 测试异常版本
void TestExceptionVersion() {
    std::cout << "=== Test: Exception Version ===" << std::endl;
    
    boost::asio::io_context io_context;
    common::process::Process process(io_context.get_executor());
    
    common::process::ProcessOptions options;
    options.executable = ZLMEDIAKIT_PATH;
    options.console_mode = common::process::ConsoleMode::NewConsole;
    options.terminal_mode = common::process::TerminalMode::NewTerminal;
    options.terminal_title = "ZLMediaKit Exception Test";
    
    try {
        std::cout << "Starting MediaServer (exception version)..." << std::endl;
        process.Start(options);  // 无 ec 版本，失败时抛异常
        
        std::cout << "Process started, PID: " << process.Pid() << std::endl;
        std::cout << "IsRunning: " << (process.IsRunning() ? "true" : "false") << std::endl;
        
        // 等待 2 秒
        std::this_thread::sleep_for(std::chrono::seconds(2));
        
        // 终止并等待
        boost::system::error_code ec;
        process.Terminate(ec);
        process.Wait(ec);
        
        std::cout << "Process terminated and waited" << std::endl;
        
    } catch (const boost::system::system_error& e) {
        std::cerr << "Exception caught: " << e.what() << std::endl;
    }
    
    std::cout << std::endl;
}

/// @brief 测试错误路径（应该失败）
void TestInvalidPath() {
    std::cout << "=== Test: Invalid Path ===" << std::endl;
    
    boost::asio::io_context io_context;
    common::process::Process process(io_context.get_executor());
    
    common::process::ProcessOptions options;
    options.executable = "C:/nonexistent/path/to/executable.exe";
    
    boost::system::error_code ec;
    if (!process.Start(options, ec)) {
        std::cout << "Expected failure: " << ec.message() << std::endl;
    } else {
        std::cerr << "Unexpected success!" << std::endl;
    }
    
    std::cout << std::endl;
}

int main() {
    std::cout << "Process Module Test" << std::endl;
    std::cout << "===================" << std::endl;
    
    // 检查可执行文件是否存在
    if (!fs::exists(ZLMEDIAKIT_PATH)) {
        std::cerr << "Warning: MediaServer.exe not found at: " << ZLMEDIAKIT_PATH << std::endl;
        std::cerr << "Tests will fail!" << std::endl;
        return 1;
    }
    
    TestStartAndTerminate();
    TestAsyncWait();
    TestExceptionVersion();
    TestInvalidPath();
    
    std::cout << "All tests completed!" << std::endl;
    return 0;
}