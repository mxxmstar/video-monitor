use async_trait::async_trait;
use sqlx::SqlitePool;

use crate::domain::entities::{User, UserId};
use crate::domain::errors::DomainError;
use crate::domain::repository::UserRepository;
use crate::{ log_info, log_error, log_debug };
use chrono::Utc;

/// SQLite 用户仓储实现
/// 
/// 负责用户数据的持久化操作，实现领域层定义的 UserRepository 接口。
/// 使用 sqlx 进行数据库操作，通过连接池管理数据库连接。
pub struct SqliteUserRepository {
    /// SQLite 连接池，sqlx 会自动管理连接的获取和归还
    pool: SqlitePool,
}

impl SqliteUserRepository {
    /// 创建新的用户仓储实例
    /// 
    /// # 参数
    /// - `pool`: SQLite 连接池，由外部传入（通常由 DatabaseConnection 提供）
    pub fn new(pool: SqlitePool) -> Self {
        Self { pool }
    }
}

#[async_trait]
impl UserRepository for SqliteUserRepository {
    /// 保存用户到数据库
    /// 
    /// 保存前会检查邮箱是否已存在，如果存在则返回错误。
    /// sqlx 会自动从连接池获取连接、执行操作、归还连接。
    /// 
    /// # 参数
    /// - `user`: 要保存的用户实体
    /// 
    /// # 返回
    /// - `Ok(User)`: 保存成功，返回用户实体
    /// - `Err(DomainError::EmailAlreadyExists)`: 邮箱已被使用
    /// - `Err(DomainError::DatabaseError)`: 数据库操作失败
    async fn save(&self, user: User) -> Result<User, DomainError> {
        // 步骤1：检查邮箱是否已存在（防止重复注册）
        // query_as 用于查询并映射结果到指定类型
        // (String,) 表示查询结果是一个单列元组
        // fetch_optional 表示查询可能返回 None（用户不存在）
        let existing = sqlx::query_as::<_, (String,)>(
            "SELECT id FROM users WHERE email = ?"
        )
        .bind(&user.email)                    // 绑定参数，防止 SQL 注入
        .fetch_optional(&self.pool)           // 从连接池获取连接并执行查询
        .await                                // 异步等待结果
        .map_err(|e| DomainError::DatabaseError(e.to_string()))?;  // 错误转换
        
        // 如果邮箱已存在，返回业务错误        
        if existing.is_some() {
            log_error!("Email {} already exists", user.email);
            return Err(DomainError::EmailAlreadyExists(user.email));
        }
        
        // 步骤2：插入新用户记录
        // query 用于执行不返回结果的 SQL（INSERT/UPDATE/DELETE）
        sqlx::query(
            "INSERT INTO users (id, name, email, created_at) VALUES (?, ?, ?, ?)"
        )
        .bind(&user.id.0)                     // 绑定用户 ID（UserId 内部的 String）
        .bind(&user.name)                     // 绑定用户名
        .bind(&user.email)                    // 绑定邮箱
        .bind(user.created_at)                // 绑定创建时间
        .execute(&self.pool)                  // 执行 SQL，自动管理连接
        .await
        .map_err(|e| DomainError::DatabaseError(e.to_string()))?;
        
        // 记录成功日志
        log_info!("User {:?} saved successfully", user);
        
        // 返回保存的用户实体
        Ok(user)
    }
    
    /// 根据用户 ID 查找用户
    /// 
    /// # 参数
    /// - `id`: 用户 ID 字符串
    /// 
    /// # 返回
    /// - `Ok(Some(User))`: 找到用户
    /// - `Ok(None)`: 用户不存在
    /// - `Err(DomainError::DatabaseError)`: 数据库操作失败
    async fn find_by_id(&self, id: &str) -> Result<Option<User>, DomainError> {
        // 查询用户记录，结果映射为四元组
        // query_as 的泛型参数指定了结果类型
        let result = sqlx::query_as::<_, (String, String, String, chrono::DateTime<Utc>)>(
            "SELECT id, name, email, created_at FROM users WHERE id = ?"
        )
        .bind(id)                             // 绑定查询参数
        .fetch_optional(&self.pool)           // 执行查询（可能返回 None）
        .await
        .map_err(|e| DomainError::DatabaseError(e.to_string()))?;
        
        // 处理查询结果
        match result {
            Some((id, name, email, created_at)) => {
                // 找到用户，将数据库记录转换为领域实体
                log_info!("User {} found", id);
                Ok(Some(User {
                    id: UserId(id),           // 包装为 UserId 类型
                    name,                     // 字段名相同可简写
                    email,
                    created_at: created_at.timestamp() as i64,  // 转换为时间戳
                }))
            }
            None => {
                // 用户不存在，记录调试日志
                log_debug!("User {} not found", id);
                Ok(None)
            }
        }
    }
    
    /// 根据邮箱查找用户
    /// 
    /// 常用于登录验证、邮箱唯一性检查等场景。
    /// 
    /// # 参数
    /// - `email`: 用户邮箱地址
    /// 
    /// # 返回
    /// - `Ok(Some(User))`: 找到用户
    /// - `Ok(None)`: 该邮箱未注册
    /// - `Err(DomainError::DatabaseError)`: 数据库操作失败
    async fn find_by_email(&self, email: &str) -> Result<Option<User>, DomainError> {
        // 按邮箱查询用户记录
        let result = sqlx::query_as::<_, (String, String, String, chrono::DateTime<Utc>)>(
            "SELECT id, name, email, created_at FROM users WHERE email = ?"
        )
        .bind(email)                          // 绑定邮箱参数
        .fetch_optional(&self.pool)           // 执行查询
        .await
        .map_err(|e| DomainError::DatabaseError(e.to_string()))?;
        
        // 处理查询结果
        match result {
            Some((id, name, email, created_at)) => {
                // 找到用户，转换为领域实体
                log_info!("email {} found", email);
                Ok(Some(User {
                    id: UserId(id),
                    name,
                    email,
                    created_at: created_at.timestamp() as i64,
                }))
            }
            None => {
                // 邮箱未注册
                log_debug!("User {} not found", email);
                Ok(None)
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::domain::entities::UserId;
    use sqlx::SqlitePool;

    async fn setup_test_db() -> SqliteUserRepository {
        let pool = SqlitePool::connect("sqlite::memory:")
            .await
            .expect("Failed to create in-memory database");
        
        sqlx::query(
            "CREATE TABLE users (
                id TEXT PRIMARY KEY,
                name TEXT NOT NULL,
                email TEXT NOT NULL UNIQUE,
                created_at DATETIME NOT NULL
            )"
        )
        .execute(&pool)
        .await
        .expect("Failed to create users table");
        
        SqliteUserRepository::new(pool)
    }

    #[tokio::test]
    async fn test_save_user_success() {
        let repo = setup_test_db().await;
        
        let user = User {
            id: UserId("test-1".to_string()),
            name: "Test User".to_string(),
            email: "test@example.com".to_string(),
            created_at: Utc::now().timestamp() as i64,
        };
        
        let result = repo.save(user).await;
        assert!(result.is_ok());
        let saved_user = result.unwrap();
        assert_eq!(saved_user.id.0, "test-1");
        assert_eq!(saved_user.email, "test@example.com");
    }

    #[tokio::test]
    async fn test_save_user_duplicate_email() {
        let repo = setup_test_db().await;
        
        let user1 = User {
            id: UserId("test-1".to_string()),
            name: "User 1".to_string(),
            email: "duplicate@example.com".to_string(),
            created_at: Utc::now().timestamp() as i64,
        };
        
        repo.save(user1).await.expect("Failed to save first user");
        
        let user2 = User {
            id: UserId("test-2".to_string()),
            name: "User 2".to_string(),
            email: "duplicate@example.com".to_string(),
            created_at: Utc::now().timestamp() as i64,
        };
        
        let result = repo.save(user2).await;
        assert!(result.is_err());
        match result.unwrap_err() {
            DomainError::EmailAlreadyExists(email) => {
                assert_eq!(email, "duplicate@example.com");
            }
            _ => panic!("Expected EmailAlreadyExists error"),
        }
    }

    #[tokio::test]
    async fn test_find_by_id_existing() {
        let repo = setup_test_db().await;
        
        let user = User {
            id: UserId("find-by-id-test".to_string()),
            name: "Find Me".to_string(),
            email: "findme@example.com".to_string(),
            created_at: Utc::now().timestamp() as i64,
        };
        
        repo.save(user).await.expect("Failed to save user");
        
        let result = repo.find_by_id("find-by-id-test").await;
        assert!(result.is_ok());
        let found = result.unwrap();
        assert!(found.is_some());
        let found_user = found.unwrap();
        assert_eq!(found_user.name, "Find Me");
        assert_eq!(found_user.email, "findme@example.com");
    }

    #[tokio::test]
    async fn test_find_by_id_nonexistent() {
        let repo = setup_test_db().await;
        
        let result = repo.find_by_id("nonexistent-id").await;
        assert!(result.is_ok());
        assert!(result.unwrap().is_none());
    }

    #[tokio::test]
    async fn test_find_by_email_existing() {
        let repo = setup_test_db().await;
        
        let user = User {
            id: UserId("find-email-test".to_string()),
            name: "Email Test".to_string(),
            email: "searchme@example.com".to_string(),
            created_at: Utc::now().timestamp() as i64,
        };
        
        repo.save(user).await.expect("Failed to save user");
        
        let result = repo.find_by_email("searchme@example.com").await;
        assert!(result.is_ok());
        let found = result.unwrap();
        assert!(found.is_some());
        let found_user = found.unwrap();
        assert_eq!(found_user.id.0, "find-email-test");
    }

    #[tokio::test]
    async fn test_find_by_email_nonexistent() {
        let repo = setup_test_db().await;
        
        let result = repo.find_by_email("nonexistent@example.com").await;
        assert!(result.is_ok());
        assert!(result.unwrap().is_none());
    }

    #[tokio::test]
    async fn test_save_and_retrieve_user() {
        let repo = setup_test_db().await;
        
        let original_user = User {
            id: UserId("integration-test".to_string()),
            name: "Integration Test".to_string(),
            email: "integration@example.com".to_string(),
            created_at: Utc::now().timestamp() as i64,
        };
        
        repo.save(original_user.clone())
            .await
            .expect("Failed to save user");
        
        let found_by_id = repo.find_by_id("integration-test").await.unwrap();
        assert!(found_by_id.is_some());
        assert_eq!(found_by_id.unwrap().name, "Integration Test");
        
        let found_by_email = repo.find_by_email("integration@example.com").await.unwrap();
        assert!(found_by_email.is_some());
        assert_eq!(found_by_email.unwrap().id.0, "integration-test");
    }
}