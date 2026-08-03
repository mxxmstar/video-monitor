use serde::{Deserialize, Serialize};

#[derive(Debug, Deserialize, Serialize, Clone)]
pub struct LogConfig {
    pub module_name: String,      // 模块名称
    pub level: String,            // 日志级别
    pub json_format: bool,        // 是否输出 JSON 格式日志
    pub with_thread_id: bool,     // 是否显示线程ID
    pub with_thread_name: bool,   // 是否显示线程名称
    pub with_target: bool,        // 是否显示目标模块信息
    pub with_file_line: bool,     // 是否显示文件名和行号
    pub with_ansi: bool,          // 是否显示 ANSI 颜色
    pub log_file: Option<String>, // 日志文件路径
    pub rotation: Option<String>, // 日志文件轮转策略
}

impl Default for LogConfig {
    fn default() -> Self {
        Self {
            module_name: "main".to_string(),
            level: "info".to_string(),
            json_format: false,
            with_thread_id: true,
            with_thread_name: false,
            with_target: true,
            with_file_line: true,
            with_ansi: true,
            log_file: None,
            rotation: None,
        }
    }
}
