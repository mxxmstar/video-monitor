use std::sync::Arc;
use axum::{
    routing::post,
    Router,
};
use tower_http::cors::CorsLayer;
use tower_http::trace::TraceLayer;

use crate::http_server::handler::{create_user, AppState};

/// 创建 HTTP 路由器
/// 
/// 配置所有 API 路由、中间件和应用状态。
/// 
/// # 参数
/// - `state`: 应用状态，包含数据库连接和用例实例
/// 
/// # 返回
/// - `Router`: 配置好的 axum 路由器
pub fn create_router(state: Arc<AppState>) -> Router {
    Router::new()
        .route("/api/users", post(create_user))
        .layer(CorsLayer::permissive())
        .layer(TraceLayer::new_for_http())
        .with_state(state)
}