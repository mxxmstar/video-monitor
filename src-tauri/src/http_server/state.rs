


// pub struct AppState {
//     // 应用层 Handler
//     pub create_user_handler: Arc<CreateUserHandler>,
//     pub update_user_handler: Arc<UpdateUserHandler>,
//     pub list_users_handler: Arc<ListUsersHandler>,
// }

// impl AppState {
//     pub fn new() -> Self {
//         // 1. 创建基础设施
//         let user_repo = Arc::new(InMemoryUserRepository::new());
        
//         // 2. 注入到应用层
//         Self {
//             create_user_handler: Arc::new(CreateUserHandler::new(user_repo.clone())),
//             update_user_handler: Arc::new(UpdateUserHandler::new(user_repo.clone())),
//             list_users_handler: Arc::new(ListUsersHandler::new(user_repo)),
//         }
//     }
// }