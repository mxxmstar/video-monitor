use super::config::LogConfig;
use super::layers::LayerBuilder;
use time::OffsetDateTime;
use tracing_subscriber::{
    layer::Identity, layer::SubscriberExt, util::SubscriberInitExt, EnvFilter, Layer, Registry,
};

fn generate_log_file_name(module_name: &str) -> String {
    let now = OffsetDateTime::now_utc();
    let date_str = now
        .format(&time::format_description::parse_borrowed::<2>("[year][month][day]").unwrap())
        .unwrap();
    format!("log_{}_{}.log", module_name, date_str)
}

pub fn init_logger(config: &LogConfig) -> anyhow::Result<()> {
    let log_file = generate_log_file_name(&config.module_name);
    let config = LogConfig {
        log_file: Some(format!("logs/{}", log_file)),
        ..config.clone()
    };

    // 日志过滤器，优先使用环境变量RUST_LOG中的配置，如果没有则使用配置文件中的日志级别
    let env_filter =
        EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new(&config.level));

    let builder = LayerBuilder::new(config.clone());
    let (layers, _guards) = builder.build()?;

    // 将所有动态层通过 and_then 组合成单个层，再与 env_filter 组合
    let combined_layer = layers.into_iter().fold(
        Box::new(Identity::new()) as Box<dyn Layer<Registry> + Send + Sync>,
        |combined, layer| Box::new(combined.and_then(layer)),
    );

    let subscriber = Registry::default().with(combined_layer).with(env_filter);

    subscriber.try_init()?;
    Ok(())
}

pub fn init_logger_default() -> anyhow::Result<()> {
    let config = LogConfig::default();
    init_logger(&config)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Once;

    static INIT: Once = Once::new();

    fn init_test_logger() {
        INIT.call_once(|| {
            let config = LogConfig {
                module_name: "test".to_string(),
                level: "debug".to_string(),
                json_format: false,
                with_thread_id: true,
                with_thread_name: false,
                with_target: true,
                with_file_line: true,
                with_ansi: true,
                log_file: None,
                rotation: None,
            };
            let _ = init_logger(&config);
        });
    }

    #[test]
    fn test_generate_log_file_name_format() {
        init_test_logger();
        let module_name = "test_module";
        let result = generate_log_file_name(module_name);
        
        tracing::info!("测试日志文件名生成: {}", result);
        
        assert!(result.starts_with("log_test_module_"));
        assert!(result.ends_with(".log"));
        
        let date_part = result
            .trim_start_matches("log_test_module_")
            .trim_end_matches(".log");
        assert_eq!(date_part.len(), 8);
        assert!(date_part.chars().all(|c| c.is_ascii_digit()));
    }

    #[test]
    fn test_generate_log_file_name_different_modules() {
        init_test_logger();
        let name1 = generate_log_file_name("module_a");
        let name2 = generate_log_file_name("module_b");
        
        tracing::debug!("模块A日志文件名: {}", name1);
        tracing::debug!("模块B日志文件名: {}", name2);
        
        assert!(name1.contains("module_a"));
        assert!(name2.contains("module_b"));
        assert_ne!(name1, name2);
    }

    #[test]
    fn test_generate_log_file_name_contains_current_date() {
        init_test_logger();
        let result = generate_log_file_name("test");
        let now = OffsetDateTime::now_utc();
        
        let year = now.year().to_string();
        let month = format!("{:02}", now.month() as u8);
        
        tracing::info!("当前日期: {}-{}, 文件名: {}", year, month, result);
        
        assert!(result.contains(&year));
        assert!(result.contains(&month));
    }

    #[test]
    fn test_logging_output() {
        init_test_logger();
        
        tracing::trace!("这是一条 TRACE 级别的日志");
        tracing::debug!("这是一条 DEBUG 级别的日志");
        tracing::info!("这是一条 INFO 级别的日志");
        tracing::warn!("这是一条 WARN 级别的日志");
        tracing::error!("这是一条 ERROR 级别的日志");
        
        assert!(true);
    }
}