use thiserror::Error;

/// 领域错误
#[derive(Error, Debug)]
pub enum DomainError {
    #[error("Invalid user name: {0}")]
    InvalidName(String),
    
    #[error("Invalid email format: {0}")]
    InvalidEmail(String),
    
    #[error("Password is too short: {0}")]
    PasswordTooShort(String),
    
    #[error("Password and confirm password do not match: {0}")]
    PasswordMismatch(String),

    #[error("Email already exists: {0}")]
    EmailAlreadyExists(String),
    
    #[error("User not found: {0}")]
    UserNotFound(String),
    
    #[error("Database error: {0}")]
    DatabaseError(String),
}