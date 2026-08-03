use time::OffsetDateTime;
use tracing_subscriber::fmt::{format, FormatEvent, FormatFields};

/// 自定义日志格式器
pub struct CustomFormatter;

impl<S, N> FormatEvent<S, N> for CustomFormatter
where
    S: tracing::Subscriber + for<'a> tracing_subscriber::registry::LookupSpan<'a>,
    N: for<'a> FormatFields<'a> + 'static,
{
    fn format_event(
        &self,
        ctx: &tracing_subscriber::fmt::FmtContext<'_, S, N>,
        mut writer: format::Writer<'_>,
        event: &tracing::Event<'_>,
    ) -> std::fmt::Result {
        // 时间戳
        let timestamp = OffsetDateTime::now_utc();
        let format_description = time::format_description::parse_borrowed::<2>(
            "[year]-[month]-[day] [hour]:[minute]:[second].[subsecond digits:3]",
        )
        .map_err(|_| std::fmt::Error)?;
        let time_str = timestamp
            .format(&format_description)
            .map_err(|_| std::fmt::Error)?;
        write!(writer, "{}", time_str)?;

        // 日志级别
        let level = event.metadata().level();
        write!(writer, "[{}]", level)?;

        // 线程ID
        let thread_id = std::thread::current().id();
        write!(writer, "[{:?}]", thread_id)?;

        // 目标模块信息
        if event.metadata().target() != "" {
            write!(writer, "{}: ", event.metadata().target())?;
        }

        // 日志消息
        ctx.field_format().format_fields(writer.by_ref(), event)?;
        writeln!(writer)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_formatter_compiles() {
        let _formatter = CustomFormatter;
    }
}
