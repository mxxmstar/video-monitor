// Learn more about Tauri commands at https://tauri.app/develop/calling-rust/
pub mod log;
pub mod http_server;
pub mod domain;
pub mod application;
pub mod infrastructure;

#[tauri::command]
fn greet(name: &str) -> String {
    format!("Hello, {}! You've been greeted from Rust!", name)
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    // 初始化日志系统
    if let Err(e) = log::init_logger_default() {
        eprintln!("Failed to initialize logger: {}", e);
    }
    
    // 创建运行时
    let rt = tokio::runtime::Runtime::new().expect("Failed to create Tokio runtime");
    
    // 启动HTTP服务器
    rt.spawn(async {
        if let Err(e) = http_server::server::start_http_server().await {
            tracing::error!("HTTP server failed: {}", e);
        }
    });
    
    // 运行Tauri应用
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .invoke_handler(tauri::generate_handler![greet])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}