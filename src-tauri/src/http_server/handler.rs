pub use super::state::{create_user, AppState, ApiResponse, CreateUserRequest, UserResponse};
use std::sync::Arc;
use axum::Json;
use axum::response::IntoResponse;

/// 根路径欢迎页
pub async fn root(
    _state: axum::extract::State<Arc<AppState>>,
) -> impl IntoResponse {
    let data: serde_json::Value = serde_json::json!({
        "service": "video-monitor",
        "version": "0.1.0",
        "docs": ">.<",
    });
    Json(ApiResponse::success(data))
}