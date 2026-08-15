use std::sync::Arc;
use axum::extract::State;
use axum::http::StatusCode;
use axum::Json;
use serde::{Deserialize, Serialize};

use crate::application::create_user::{CreateUserCommand, CreateUserUseCase};
use crate::domain::entities::User;
use crate::infrastructure::database::DatabaseConnection;
use crate::infrastructure::repository::SqliteUserRepository;

pub const CODE_SUCCESS: i32 = 0;
pub const CODE_USER_CREATED: i32 = 10001;


/// 应用状态，包含所有依赖
#[derive(Clone)]
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
    /// 业务状态码：0 表示成功，非 0 表示错误
    pub code: i32,
    /// 提示信息
    pub message: String,
    /// 响应数据（成功时有值，失败时为 None）
    pub data: Option<T>,
}

impl<T> ApiResponse<T> {
    /// 成功响应
    pub fn success(data: T) -> Self {
        Self {
            code: CODE_SUCCESS,
            message: "success".into(),
            data: Some(data),
        }
    }
    
    /// 错误响应
    pub fn error(code: i32, message: String) -> Self {
        Self {
            code,
            message,
            data: None,
        }
    }
}

/// 创建用户HTTP处理器
pub async fn create_user(
    State(state): State<Arc<AppState>>,
    Json(request): Json<CreateUserRequest>,
) -> Json<ApiResponse<UserResponse>> {
    // 1. DTO → Command
    let command = CreateUserCommand {
        name: request.name,
        email: request.email,
        password: request.password,
    };
    
    // 2. 调用应用层用例
    let user = match state.create_user_use_case.execute(command).await {
        Ok(user) => user,
        Err(e) => {
            tracing::error!("Failed to create user: {}", e);
            let code = match e {
                crate::domain::errors::DomainError::EmailAlreadyExists(_) => CODE_USER_CREATED,
                _ => CODE_SUCCESS,
            };
            return Json(ApiResponse::error(code, e.to_string()));
        }
    };
    
    // 3. Domain → DTO
    let response = UserResponse::from(user);
    
    // 4. 返回HTTP响应
    Json(ApiResponse::success(response))
}