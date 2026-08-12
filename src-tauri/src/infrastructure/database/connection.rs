use sqlx::{SqlitePool, Pool, sqlite::Sqlite};
use anyhow::Result;

/// 数据库连接池封装
pub struct DatabaseConnection {
    pool: Pool<Sqlite>,
}

impl DatabaseConnection {
    /// 创建新的数据库连接
    pub async fn new(database_url: &str) -> Result<Self> {
        let pool = SqlitePool::connect(database_url).await?;
        
        // 初始化数据库表
        Self::init_tables(&pool).await?;
        
        Ok(Self { pool })
    }
    
    /// 获取连接池
    pub fn pool(&self) -> &Pool<Sqlite> {
        &self.pool
    }
    
    /// 初始化数据库表
    async fn init_tables(pool: &Pool<Sqlite>) -> Result<()> {
        sqlx::query(
            r#"
            CREATE TABLE IF NOT EXISTS users (
                id TEXT PRIMARY KEY,
                name TEXT NOT NULL,
                email TEXT NOT NULL UNIQUE,
                created_at DATETIME DEFAULT CURRENT_TIMESTAMP
            )
            "#
        )
        .execute(pool)
        .await?;
        
        Ok(())
    }
}