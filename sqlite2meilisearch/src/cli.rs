use std::path::PathBuf;

use clap::{Args, Parser};

#[derive(Parser)]
#[command(version, about)]
// Meilisearch URL and API key are read only from MEILI_URL and MEILI_KEY
pub struct Cli {
    #[command(flatten)]
    pub input: Input,

    /// Print the first generated document as JSON without contacting Meilisearch
    #[arg(long)]
    pub preview: bool,

    /// Target index UID (created if missing; must have no other writers)
    #[arg(
        long,
        value_parser = index_uid,
        required_unless_present = "preview",
        conflicts_with = "preview",
    )]
    pub index: Option<String>,

    /// Optional Meilisearch settings JSON file
    #[arg(long, conflicts_with = "preview")]
    pub settings: Option<PathBuf>,

    /// Write success report atomically to a NEW file instead of stdout
    #[arg(long, conflicts_with = "preview")]
    pub report: Option<PathBuf>,

    /// Allow zero generated documents, deleting all existing target documents
    #[arg(long, conflicts_with = "preview")]
    pub allow_empty: bool,
}

#[derive(Args)]
pub struct Input {
    /// Existing SQLite database, opened read-only
    #[arg(long)]
    pub sqlite: PathBuf,

    /// File containing one read-only SQL query
    #[arg(long)]
    pub query: PathBuf,

    /// Rhai file defining fn transform(row); omit to pass rows through unchanged
    #[arg(long)]
    pub transform: Option<PathBuf>,

    /// Top-level primary key in the transformed document
    #[arg(long, default_value = "id", value_parser = nonempty)]
    pub primary_key: String,
}

fn nonempty(value: &str) -> Result<String, String> {
    if value.is_empty() {
        Err("must not be empty".into())
    } else {
        Ok(value.into())
    }
}

fn index_uid(value: &str) -> Result<String, String> {
    if value.is_empty()
        || !value
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
    {
        Err("index UID must contain only ASCII letters, digits, '-' or '_'".into())
    } else {
        Ok(value.into())
    }
}

pub fn server_url(value: &str) -> Result<String, String> {
    let url = url::Url::parse(value).map_err(|_| "invalid Meilisearch URL")?;
    if !matches!(url.scheme(), "http" | "https")
        || url.host_str().is_none()
        || !url.username().is_empty()
        || url.password().is_some()
        || url.query().is_some()
        || url.fragment().is_some()
    {
        return Err("use an HTTP(S) URL without credentials, query or fragment".into());
    }
    Ok(url.as_str().trim_end_matches('/').into())
}

pub fn api_key(value: &str) -> Result<String, String> {
    if value.bytes().all(|b| (b' '..=b'~').contains(&b)) {
        Ok(value.into())
    } else {
        Err("MEILI_KEY must contain printable ASCII only".into())
    }
}
