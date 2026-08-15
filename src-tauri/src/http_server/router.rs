use std::sync::Arc;
use axum::{
    routing::{get, post},
    Router,
};
use tower_http::cors::CorsLayer;
use tower_http::trace::TraceLayer;

use crate::http_server::handler::{create_user, root, AppState};
use crate::log_info;
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
        .route("/", get(root))
        // 嵌套用户路由到 /api/user 路径下
        .nest("/api/user", build_user_routes())
        .layer(CorsLayer::permissive())
        .layer(TraceLayer::new_for_http())
        // 注入应用共享状态
        .with_state(state)
}

/// 构建用户相关路由
fn build_user_routes() -> Router<Arc<AppState>> {
    Router::new()
        .route("/create", post(create_user))
}