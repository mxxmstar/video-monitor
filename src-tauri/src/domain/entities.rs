use chrono::{Utc};

/// 用户ID值对象
#[derive(Debug, Clone)]
pub struct UserId(pub String);

/// 用户实体
#[derive(Debug, Clone)]
pub struct User {
    pub id: UserId,
    pub name: String,
    pub email: String,
    pub password: String,
    pub created_at: i64,
}

impl User {
    /// 创建新用户
    pub fn new(name: String, email: String, password: String) -> Self {
        Self {
            id: UserId(uuid::Uuid::new_v4().to_string()),
            name,
            email,
            password,
            created_at: Utc::now().timestamp() as i64,
        }
    }
}