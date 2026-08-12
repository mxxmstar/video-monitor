// http/handler.rs
// pub async fn create_user(
//     State(state): State<AppState>,
//     Json(req): Json<CreateUserRequest>,
// ) -> Result<ApiResponse<UserDto>, AppError> {
//     // 1. DTO → Command
//     let cmd = CreateUserCommand {
//         name: req.name,
//         email: req.email,
//     };
    
//     // 2. 调用应用层
//     let handler = state.create_user_handler.clone();
//     let dto = handler.execute(cmd).await?;
    
//     // 3. 返回 HTTP 响应
//     Ok(ApiResponse::success(dto))
// }