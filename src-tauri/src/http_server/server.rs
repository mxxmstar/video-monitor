use std::sync::Arc;

use crate::http_server;

/// 启动 HTTP 服务器
/// 
/// 该函数负责：
/// 1. 初始化应用状态（包含数据库连接）
/// 2. 创建路由
/// 3. 绑定并监听指定端口
/// 
/// # 返回
/// - `Ok(())`: 服务器正常关闭
/// - `Err(anyhow::Error)`: 服务器启动或运行失败
pub async fn start_http_server() -> anyhow::Result<()> {
    // 使用项目目录存储数据库文件
    let manifest_dir = env!("CARGO_MANIFEST_DIR");
    let db_path = std::path::Path::new(manifest_dir).join("users.db");
    
    // 确保数据库文件存在（SQLite 需要文件已存在或能创建）
    if !db_path.exists() {
        std::fs::File::create(&db_path)
            .map_err(|e| anyhow::anyhow!("Failed to create database file: {}", e))?;
        tracing::info!("Created new database file at: {:?}", db_path);
    }
    
    // 使用正斜杠格式
    let db_path_str = db_path.to_string_lossy().replace('\\', "/");
    let database_url = format!("sqlite:{}", db_path_str);
    
    tracing::info!("Database path: {}", db_path_str);
    
    // 1. 初始化应用状态（包含数据库连接）
    let state = Arc::new(
        http_server::state::AppState::new(&database_url)
            .await?
    );
    
    // 2. 创建路由
    let app = http_server::router::create_router(state);
    
    // 3. 启动服务器
    let listener = tokio::net::TcpListener::bind("127.0.0.1:33337")
        .await?;
    
    tracing::info!("HTTP server listening on http://127.0.0.1:33337");
    
    axum::serve(listener, app).await?;
    
    Ok(())
}