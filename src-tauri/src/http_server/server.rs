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
    // 使用内存数据库（开发测试用）
    let database_url = "sqlite::memory:";
    
    tracing::info!("Using in-memory SQLite database");
    
    // 1. 初始化应用状态（包含数据库连接）
    let state = Arc::new(
        http_server::state::AppState::new(database_url)
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