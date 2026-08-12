use crate::domain::entities::User;
use crate::domain::errors::DomainError;
use async_trait::async_trait;

/// 用户仓储接口（定义在领域层）
#[async_trait]
pub trait UserRepository: Send + Sync {
    /// 保存用户
    async fn save(&self, user: User) -> Result<User, DomainError>;
    
    /// 根据ID查找用户
    async fn find_by_id(&self, id: &str) -> Result<Option<User>, DomainError>;
    
    /// 根据邮箱查找用户
    async fn find_by_email(&self, email: &str) -> Result<Option<User>, DomainError>;
}