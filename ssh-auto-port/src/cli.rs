use clap::Parser;

#[derive(Parser, Debug)]
#[command(
    name = "ssh-auto-port",
    about = "Auto-forward remote listening ports to localhost over a single SSH connection",
    version
)]
pub struct Cli {
    /// Host alias as defined in ~/.ssh/config (user@host is NOT accepted).
    pub host: String,

    /// Only forward remote ports inside this inclusive range.
    #[arg(long, default_value = "1024-65535", value_parser = parse_port_range)]
    pub port_range: (u16, u16),

    /// Ports to never forward (comma separated or repeated).
    #[arg(long, value_delimiter = ',', default_value = "22")]
    pub exclude: Vec<u16>,

    /// Skip occupied local ports instead of remapping to the nearest free port.
    #[arg(long)]
    pub skip: bool,

    /// Enable debug logging.
    #[arg(long)]
    pub debug: bool,
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn validates_port_range_order() {
        assert_eq!(parse_port_range("1024-65535").unwrap(), (1024, 65535));
        assert!(parse_port_range("65535-1024").is_err());
    }
}
