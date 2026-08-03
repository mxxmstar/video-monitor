use std::path::Path;
use tracing_appender::non_blocking::WorkerGuard;
use tracing_appender::rolling::{RollingFileAppender, Rotation};
use tracing_subscriber::{filter, fmt, Layer, Registry};

use super::config::LogConfig;

/// 控制台日志输出层
pub struct ConsoleLayer {
    pub layer: Box<dyn tracing_subscriber::Layer<Registry> + Send + Sync>,
}

impl ConsoleLayer {
    /// 构建控制台日志输出层
    pub fn new(config: &LogConfig) -> Self {
        let layer = if config.json_format {
            Box::new(
                fmt::layer()
                    .with_thread_ids(config.with_thread_id)
                    .with_thread_names(config.with_thread_name)
                    .with_target(config.with_target)
                    .with_file(config.with_file_line)
                    .with_line_number(config.with_file_line)
                    .with_ansi(config.with_ansi)
                    .json(),
            ) as Box<dyn tracing_subscriber::Layer<Registry> + Send + Sync>
        } else {
            Box::new(
                fmt::layer()
                    .with_thread_ids(config.with_thread_id)
                    .with_thread_names(config.with_thread_name)
                    .with_target(config.with_target)
                    .with_file(config.with_file_line)
                    .with_line_number(config.with_file_line)
                    .with_ansi(config.with_ansi),
            ) as Box<dyn tracing_subscriber::Layer<Registry> + Send + Sync>
        };

        Self { layer }
    }
}

/// 日志文件层配置
pub struct FileLayerConfig {
    pub log_dir: String,
    pub file_name: String,
    pub rotation: Rotation,
    pub json_format: bool,
    pub with_thread_id: bool,
    pub with_thread_name: bool,
    pub with_target: bool,
    pub with_file_line: bool,
}

impl From<&LogConfig> for FileLayerConfig {
    fn from(config: &LogConfig) -> Self {
        let log_file = config.log_file.as_deref().unwrap_or("logs/app.log");
        let path = Path::new(log_file);

        let log_dir = path
            .parent()
            .and_then(|p| p.to_str())
            .unwrap_or("logs")
            .to_string();

        let file_name = path
            .file_name()
            .and_then(|s| s.to_str())
            .unwrap_or("app.log")
            .to_string();

        let rotation = match config.rotation.as_deref() {
            Some("hourly") => Rotation::HOURLY,
            Some("daily") => Rotation::DAILY,
            Some("minutely") => Rotation::MINUTELY,
            _ => Rotation::NEVER,
        };

        Self {
            log_dir,
            file_name,
            rotation,
            json_format: config.json_format,
            with_thread_id: config.with_thread_id,
            with_thread_name: config.with_thread_name,
            with_target: config.with_target,
            with_file_line: config.with_file_line,
        }
    }
}

/// 日志文件层
pub struct FileLayer {
    pub guard: WorkerGuard,
    pub layer: Box<dyn tracing_subscriber::Layer<Registry> + Send + Sync>,
}

impl FileLayer {
    pub fn new(config: &FileLayerConfig) -> anyhow::Result<Self> {
        std::fs::create_dir_all(&config.log_dir)?;

        let appender =
            RollingFileAppender::new(config.rotation.clone(), &config.log_dir, &config.file_name);

        let (non_blocking, file_guard) = tracing_appender::non_blocking(appender);

        let layer: Box<dyn tracing_subscriber::Layer<Registry> + Send + Sync> =
            if config.json_format {
                Box::new(
                    fmt::layer()
                        .with_writer(non_blocking)
                        .with_thread_ids(config.with_thread_id)
                        .with_target(config.with_target)
                        .with_file(config.with_file_line)
                        .with_line_number(config.with_file_line)
                        .json(),
                ) as Box<dyn tracing_subscriber::Layer<Registry> + Send + Sync>
            } else {
                Box::new(
                    fmt::layer()
                        .with_writer(non_blocking)
                        .with_thread_names(config.with_thread_name)
                        .with_target(config.with_target)
                        .with_file(config.with_file_line)
                        .with_line_number(config.with_file_line),
                ) as Box<dyn tracing_subscriber::Layer<Registry> + Send + Sync>
            };
        Ok(Self {
            guard: file_guard,
            layer,
        })
    }
}

/// 错误日志层（单独记录错误）
pub struct ErrorLayer {
    pub guard: WorkerGuard,
    pub layer: Box<dyn tracing_subscriber::Layer<Registry> + Send + Sync>,
}

impl ErrorLayer {
    pub fn new(config: &FileLayerConfig) -> anyhow::Result<Self> {
        std::fs::create_dir_all(&config.log_dir)?;

        // 构建错误日志文件名, 基于日志文件名添加_error后缀
        let appender = RollingFileAppender::new(
            config.rotation.clone(),
            &config.log_dir,
            &format!(
                "{}_error.log",
                Path::new(&config.file_name)
                    .file_stem()
                    .and_then(|s| s.to_str())
                    .unwrap_or("error")
            ),
        );

        let (non_blocking, file_guard) = tracing_appender::non_blocking(appender);

        let layer = Box::new(
            fmt::layer()
                .with_writer(non_blocking)
                .with_thread_ids(config.with_thread_id)
                .with_thread_names(config.with_thread_name)
                .with_target(config.with_target)
                .with_file(config.with_file_line)
                .with_line_number(config.with_file_line)
                .json()
                .with_filter(filter::LevelFilter::ERROR),
        ) as Box<dyn tracing_subscriber::Layer<Registry> + Send + Sync>;

        Ok(Self {
            guard: file_guard,
            layer,
        })
    }
}

/// 多层组合构建器
pub struct LayerBuilder {
    console_enabled: bool,
    file_enabled: bool,
    error_file_enabled: bool,
    config: LogConfig,
}

impl LayerBuilder {
    pub fn new(config: LogConfig) -> Self {
        Self {
            console_enabled: true,
            file_enabled: config.log_file.is_some(),
            error_file_enabled: config.log_file.is_some(),
            config,
        }
    }

    pub fn without_console(mut self) -> Self {
        self.console_enabled = false;
        self
    }

    pub fn without_file(mut self) -> Self {
        self.file_enabled = false;
        self.error_file_enabled = false;
        self
    }

    pub fn without_error_file(mut self) -> Self {
        self.error_file_enabled = false;
        self
    }

    pub fn build(
        self,
    ) -> anyhow::Result<(
        Vec<Box<dyn tracing_subscriber::Layer<Registry> + Send + Sync>>,
        Vec<WorkerGuard>,
    )> {
        let mut layers: Vec<Box<dyn tracing_subscriber::Layer<Registry> + Send + Sync>> =
            Vec::new();
        let mut guards: Vec<WorkerGuard> = Vec::new();

        if self.console_enabled {
            let console_layer = ConsoleLayer::new(&self.config);
            layers.push(Box::new(console_layer.layer));
        }

        if self.file_enabled {
            let file_config = FileLayerConfig::from(&self.config);
            let file_layer = FileLayer::new(&file_config)?;
            guards.push(file_layer.guard);
            layers.push(Box::new(file_layer.layer));
        }

        if self.error_file_enabled {
            let file_config = FileLayerConfig::from(&self.config);
            let error_layer = ErrorLayer::new(&file_config)?;
            guards.push(error_layer.guard);
            layers.push(Box::new(error_layer.layer));
        }

        Ok((layers, guards))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_console_layer_creation() {
        let config = LogConfig::default();
        let _layer = ConsoleLayer::new(&config);
    }

    #[test]
    fn test_file_layer_config_from_log_config() {
        let config = LogConfig {
            log_file: Some("/var/logs/myapp/app.log".to_string()),
            rotation: Some("daily".to_string()),
            ..Default::default()
        };

        let file_config = FileLayerConfig::from(&config);
        assert_eq!(file_config.log_dir, "/var/logs/myapp");
        assert_eq!(file_config.file_name, "app.log");
        assert_eq!(file_config.rotation, Rotation::DAILY);
    }

    #[test]
    fn test_file_layer_config_default_path() {
        let config = LogConfig::default();
        let file_config = FileLayerConfig::from(&config);
        assert_eq!(file_config.log_dir, "logs");
        assert_eq!(file_config.file_name, "app.log");
    }

    #[test]
    fn test_rotation_parsing() {
        let test_cases = vec![
            ("hourly", Rotation::HOURLY),
            ("daily", Rotation::DAILY),
            ("minutely", Rotation::MINUTELY),
            ("never", Rotation::NEVER),
            ("invalid", Rotation::NEVER),
        ];

        for (input, expected) in test_cases {
            let config = LogConfig {
                rotation: Some(input.to_string()),
                log_file: Some("logs/test.log".to_string()),
                ..Default::default()
            };
            let file_config = FileLayerConfig::from(&config);
            assert_eq!(
                file_config.rotation, expected,
                "Failed for input: {}",
                input
            );
        }
    }

    #[test]
    fn test_layer_builder_default() {
        let config = LogConfig::default();
        let builder = LayerBuilder::new(config);
        assert!(builder.console_enabled);
        assert!(!builder.file_enabled);
    }

    #[test]
    fn test_layer_builder_with_file() {
        let config = LogConfig {
            log_file: Some("logs/test.log".to_string()),
            ..Default::default()
        };
        let builder = LayerBuilder::new(config);
        assert!(builder.console_enabled);
        assert!(builder.file_enabled);
        assert!(builder.error_file_enabled);
    }

    #[test]
    fn test_layer_builder_without_console() {
        let config = LogConfig::default();
        let builder = LayerBuilder::new(config).without_console();
        assert!(!builder.console_enabled);
    }

    #[test]
    fn test_layer_builder_without_file() {
        let config = LogConfig {
            log_file: Some("logs/test.log".to_string()),
            ..Default::default()
        };
        let builder = LayerBuilder::new(config).without_file();
        assert!(!builder.file_enabled);
        assert!(!builder.error_file_enabled);
    }
}
