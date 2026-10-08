use clap::{Parser, ValueEnum};

pub(crate) const MIN_POLL_INTERVAL: f64 = 0.2;
pub(crate) const MAX_POLL_INTERVAL: f64 = 10.0;

#[derive(Copy, Clone, Debug, ValueEnum)]
pub(crate) enum OnConflict {
    /// Remap to the nearest free local port.
    Remap,
    /// Skip forwarding this port.
    Skip,
}

#[derive(Parser, Debug)]
#[command(
    name = "ssh-auto-port",
    about = "Auto-forward remote listening ports to localhost over a single SSH connection",
    version
)]
pub(crate) struct Cli {
    /// Host alias as defined in ~/.ssh/config (user@host is NOT accepted).
    pub(crate) host: String,

    /// Only forward remote ports inside this inclusive range.
    #[arg(long, default_value = "1024-65535", value_parser = parse_port_range)]
    pub(crate) port_range: (u16, u16),

    /// Ports to never forward (comma separated or repeated).
    #[arg(long, value_delimiter = ',', default_value = "22")]
    pub(crate) exclude: Vec<u16>,

    /// Base polling interval in seconds (0.2s to 10s).
    #[arg(long, default_value_t = 1.5, value_parser = parse_interval)]
    pub(crate) interval: f64,

    /// What to do when the matching local port is already in use.
    #[arg(long, value_enum, default_value_t = OnConflict::Remap)]
    pub(crate) on_conflict: OnConflict,

    /// Verbose logging.
    #[arg(short, long)]
    pub(crate) verbose: bool,
}

fn parse_port_range(s: &str) -> Result<(u16, u16), String> {
    let (lo, hi) = s
        .split_once('-')
        .ok_or_else(|| "expected format LOW-HIGH, e.g. 1024-65535".to_string())?;
    let lo: u16 = lo
        .trim()
        .parse()
        .map_err(|_| "invalid LOW port".to_string())?;
    let hi: u16 = hi
        .trim()
        .parse()
        .map_err(|_| "invalid HIGH port".to_string())?;
    if lo > hi {
        return Err("LOW must be <= HIGH".to_string());
    }
    Ok((lo, hi))
}

fn parse_interval(s: &str) -> Result<f64, String> {
    let interval: f64 = s
        .parse()
        .map_err(|_| "interval must be a number of seconds".to_string())?;
    if !interval.is_finite() || !(MIN_POLL_INTERVAL..=MAX_POLL_INTERVAL).contains(&interval) {
        return Err(format!(
            "interval must be a finite value between {MIN_POLL_INTERVAL} and {MAX_POLL_INTERVAL} seconds"
        ));
    }
    Ok(interval)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_only_finite_intervals_in_range() {
        assert_eq!(parse_interval("0.2").unwrap(), MIN_POLL_INTERVAL);
        assert_eq!(parse_interval("10").unwrap(), MAX_POLL_INTERVAL);
        assert!(parse_interval("0.1").is_err());
        assert!(parse_interval("NaN").is_err());
        assert!(parse_interval("inf").is_err());
    }

    #[test]
    fn validates_port_range_order() {
        assert_eq!(parse_port_range("1024-65535").unwrap(), (1024, 65535));
        assert!(parse_port_range("65535-1024").is_err());
    }
}
