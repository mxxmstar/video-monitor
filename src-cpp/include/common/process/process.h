#pragma once
/// @file process.h
/// @brief 进程模块(基于 boost::process)，用于管理进程相关操作


#include <boost/asio/any_io_executor.hpp>
#include <boost/system/error_code.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace common::process {

/// @brief 控制台模式
enum class ConsoleMode {
    Inherit,     ///< 继承父进程的控制台
    NewConsole,  ///< 创建新的控制台
};

/// @brief 窗口模式
enum class WindowMode {
    Default, ///< 默认窗口模式
    Normal,  ///< 正常窗口模式
    Minimized, ///< 最小化窗口模式
    Maximized, ///< 最大化窗口模式
    Hidden,    ///< 隐藏窗口模式
};

enum class TerminalMode {
    None, ///< 不使用终端模式
    NewTerminal, ///< 创建新的终端模式
};

/// @brief 进程选项
struct ProcessOptions {
    std::filesystem::path executable; ///< 可执行文件路径
    std::vector<std::string> args; ///< 命令行参数
    std::filesystem::path working_dir; ///< 工作目录    
    // std::filesystem::path env_file; ///< 环境变量文件
    ConsoleMode console_mode{ConsoleMode::Inherit}; ///< 控制台模式, 默认继承父进程的控制台
    WindowMode window_mode{WindowMode::Default}; ///< 窗口模式, 默认窗口模式
    TerminalMode terminal_mode{TerminalMode::None}; ///< 终端模式, 默认不使用终端模式
    std::string terminal_title; ///< 终端名称
};

class Process {
public:
    /// @brief 进程退出回调函数
    using ExitHandler = std::function<void(const boost::system::error_code& ec, int)>;

    explicit Process(boost::asio::any_io_executor executor);
    ~Process();

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    Process(Process&&) noexcept;
    Process& operator=(Process&&) noexcept;

    /// @brief 启动进程
    bool Start(const ProcessOptions& options);
    bool Start(const ProcessOptions& options, boost::system::error_code& ec) noexcept;

    /// @brief 检查进程是否已打开
    bool IsOpen() const noexcept;
    
    /// @brief 检查进程是否正在运行
    bool IsRunning();
    bool IsRunning(boost::system::error_code& ec) noexcept;

    /// @brief 获取进程ID
    std::uint64_t Pid() const noexcept;

    int ExitCode() const noexcept;

    /// @brief 异步等待进程退出
    void AsyncWait(ExitHandler handler);

    /// @brief 请求进程退出
    bool RequestExit(boost::system::error_code& ec) noexcept;

    /// @brief 终止进程
    bool Terminate(boost::system::error_code& ec) noexcept;
    
    /// @brief 等待进程退出
    int Wait();
    int Wait(boost::system::error_code& ec) noexcept;

    /// @brief 重置进程状态
    void Reset() noexcept;

    /// @brief 获取进程错误信息
    const std::string& LastError() const noexcept;

private:
    class ProcessImpl;
    std::unique_ptr<ProcessImpl> impl_;
};


} // namespace common::process