use std::sync::Arc;
use axum::extract::State;
use axum::http::StatusCode;
use axum::Json;
use serde::{Deserialize, Serialize};

use crate::application::create_user::{CreateUserCommand, CreateUserUseCase};
use crate::domain::entities::User;
use crate::infrastructure::database::DatabaseConnection;
use crate::infrastructure::repository::SqliteUserRepository;

/// 应用状态，包含所有依赖
pub struct AppState {
    pub db: Arc<DatabaseConnection>,
    pub create_user_use_case: Arc<CreateUserUseCase<SqliteUserRepository>>,
}

impl AppState {
    /// 创建应用状态并初始化所有依赖
    pub async fn new(database_url: &str) -> anyhow::Result<Self> {
        // 1. 创建数据库连接
        let db = Arc::new(DatabaseConnection::new(database_url).await?);
        
        // 2. 创建仓储实现
        let user_repo = SqliteUserRepository::new(db.pool().clone());
        
        // 3. 创建用例
        let create_user_use_case = Arc::new(CreateUserUseCase::new(user_repo));
        
        Ok(Self {
            db,
            create_user_use_case,
        })
    }
}

/// 创建用户请求DTO
#[derive(Deserialize)]
pub struct CreateUserRequest {
    pub name: String,
    pub email: String,
    pub password: String,
}

/// 用户响应DTO
#[derive(Serialize)]
pub struct UserResponse {
    pub id: String,
    pub name: String,
    pub email: String,
    pub created_at: i64,
}

impl From<User> for UserResponse {
    fn from(user: User) -> Self {
        Self {
            id: user.id.0,
            name: user.name,
            email: user.email,
            created_at: user.created_at,
        }
    }
}

/// API统一响应格式
#[derive(Serialize)]
pub struct ApiResponse<T> {
    pub success: bool,
    pub data: Option<T>,
    pub error: Option<String>,
}

impl<T> ApiResponse<T> {
    pub fn success(data: T) -> Self {
        Self {
            success: true,
            data: Some(data),
            error: None,
        }
    }
    
    pub fn error(message: String) -> Self {
        Self {
            success: false,
            data: None,
            error: Some(message),
        }
    }
}

/// 创建用户HTTP处理器
pub async fn create_user(
    State(state): State<Arc<AppState>>,
    Json(request): Json<CreateUserRequest>,
) -> Result<Json<ApiResponse<UserResponse>>, StatusCode> {
    // 1. DTO → Command
    let command = CreateUserCommand {
        name: request.name,
        email: request.email,
        password: request.password,
    };
    
    // 2. 调用应用层用例
    let user = state
        .create_user_use_case
        .execute(command)
        .await
        .map_err(|e| {
            tracing::error!("Failed to create user: {}", e);
            StatusCode::BAD_REQUEST
        })?;
    
    // 3. Domain → DTO
    let response = UserResponse::from(user);
    
    // 4. 返回HTTP响应
    Ok(Json(ApiResponse::success(response)))
}