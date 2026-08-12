use crate::domain::entities::User;
use crate::domain::errors::DomainError;
use crate::domain::repository::UserRepository;

/// 创建用户用例的输入参数
pub struct CreateUserCommand {
    pub name: String,
    pub email: String,
    pub password: String,
}

/// 创建用户用例
pub struct CreateUserUseCase<R: UserRepository> {
    repository: R,
}

impl<R: UserRepository> CreateUserUseCase<R> {
    /// 创建新的用例实例
    pub fn new(repository: R) -> Self {
        Self { repository }
    }
    
    /// 执行创建用户用例
    pub async fn execute(&self, command: CreateUserCommand) -> Result<User, DomainError> {
        // 1. 业务规则校验
        self.validate(&command)?;
        
        // 2. 检查邮箱是否已存在
        let existing = self.repository.find_by_email(&command.email).await?;
        if existing.is_some() {
            return Err(DomainError::EmailAlreadyExists(command.email));
        }
        
        // 3. 创建领域对象
        let user = User::new(command.name, command.email, command.password);
        
        // 4. 持久化
        self.repository.save(user).await
    }
    
    /// 业务规则校验
    fn validate(&self, command: &CreateUserCommand) -> Result<(), DomainError> {
        // 用户名不能为空
        if command.name.trim().is_empty() {
            return Err(DomainError::InvalidName("Name cannot be empty".to_string()));
        }
        
        // 用户名长度限制
        if command.name.len() > 30 {
            return Err(DomainError::InvalidName("Name too long (max 30 characters)".to_string()));
        }
        
        // 邮箱格式简单校验
        if !command.email.contains('@') {
            return Err(DomainError::InvalidEmail("Invalid email format".to_string()));
        }
        
        Ok(())
    }
}