//! ssh-auto-port: watch a remote host's listening TCP ports over a single SSH
//! connection (russh, pure-library, no `ssh` subprocess) and automatically
//! maintain matching local forwards (ssh -L 127.0.0.1:PORT:127.0.0.1:PORT).
//!
//! All connection parameters come from ~/.ssh/config (parsed via russh-config);
//! the only positional argument is a Host alias from that file.

mod cli;
mod forward;
mod probe;

use std::collections::BTreeSet;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::{anyhow, bail, Context, Result};
use clap::Parser;
use cli::{Cli, OnConflict};
use forward::ForwardManager;
use probe::{parse_frame, read_frame, Adaptive, PROBE_SCRIPT};
use russh::client::{self, Handle};
use russh::keys::agent::client::AgentClient;
use russh::keys::agent::AgentIdentity;
use russh::keys::{
    check_known_hosts_path, load_secret_key, PrivateKey, PrivateKeyWithHashAlg,
    PublicKeyOrCertificate,
};
use tracing::{debug, error, info, warn};

// ---------------------------------------------------------------------------
// ssh config alias validation
// ---------------------------------------------------------------------------

/// Make sure `alias` is a plain Host alias that actually matches some `Host`
/// entry in ~/.ssh/config (including `Host *` wildcards). Reject user@host.
fn ensure_host_alias(alias: &str) -> Result<PathBuf> {
    let config_path = home_dir()?.join(".ssh").join("config");

    if alias.contains('@') || alias.contains('/') || alias.contains(':') {
        bail!(
            "\"{alias}\" is not a Host alias from ~/.ssh/config (manual user@host forms are not accepted).\n\
             Please configure this host in ~/.ssh/config first."
        );
    }

    let text = std::fs::read_to_string(&config_path).with_context(|| {
        format!(
            "cannot read {}. Please configure this host in ~/.ssh/config first.",
            config_path.display()
        )
    })?;

    if !has_matching_host_alias(&text, alias) {
        bail!(
            "\"{alias}\" does not match any Host entry in {}. Please configure this host in ~/.ssh/config first.",
            config_path.display()
        );
    }
    Ok(config_path)
}

fn has_matching_host_alias(config: &str, alias: &str) -> bool {
    for line in config.lines().map(str::trim) {
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let mut parts = line.split_ascii_whitespace();
        let Some(key) = parts.next() else {
            continue;
        };
        if !key.eq_ignore_ascii_case("host") {
            continue;
        }

        let mut entry_matches = false;
        for pattern in parts {
            let (pattern, negated) = match pattern.strip_prefix('!') {
                Some(pattern) => (pattern, true),
                None => (pattern, false),
            };
            let is_match = globset::Glob::new(pattern)
                .map(|glob| glob.compile_matcher().is_match(alias))
                .unwrap_or(false);
            if is_match {
                if negated {
                    entry_matches = false;
                    break;
                }
                entry_matches = true;
            }
        }
        if entry_matches {
            return true;
        }
    }
    false
}

fn home_dir() -> Result<PathBuf> {
    std::env::home_dir().ok_or_else(|| anyhow!("cannot determine the home directory"))
}

// ---------------------------------------------------------------------------
// host key verification (strict known_hosts)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
enum KeyVerdict {
    Trusted,
    Unknown {
        algorithm: String,
        fingerprint: String,
    },
    Changed {
        line: usize,
    },
    Error(String),
}

pub(crate) struct Handler {
    host: String,
    port: u16,
    known_hosts: PathBuf,
    verdict: Arc<Mutex<Option<KeyVerdict>>>,
}

impl client::Handler for Handler {
    type Error = anyhow::Error;

    async fn check_server_key(
        &mut self,
        server_public_key: &PublicKeyOrCertificate,
    ) -> Result<bool, Self::Error> {
        let public_key = match server_public_key {
            PublicKeyOrCertificate::PublicKey { key, .. } => key.clone(),
            PublicKeyOrCertificate::Certificate(certificate) => {
                certificate.public_key().clone().into()
            }
        };
        let fingerprint = public_key
            .fingerprint(russh::keys::ssh_key::HashAlg::Sha256)
            .to_string();
        let algorithm = public_key.algorithm().to_string();

        let verdict = if !self.known_hosts.exists() {
            KeyVerdict::Unknown {
                algorithm,
                fingerprint,
            }
        } else {
            match check_known_hosts_path(&self.host, self.port, &public_key, &self.known_hosts) {
                Ok(true) => KeyVerdict::Trusted,
                Ok(false) => KeyVerdict::Unknown {
                    algorithm,
                    fingerprint,
                },
                Err(russh::keys::Error::KeyChanged { line }) => KeyVerdict::Changed { line },
                Err(error) => KeyVerdict::Error(error.to_string()),
            }
        };
        let trusted = matches!(verdict, KeyVerdict::Trusted);
        debug!(
            "host key verdict for {}:{} -> {:?}",
            self.host, self.port, verdict
        );
        *self.verdict.lock().expect("verdict mutex") = Some(verdict);
        Ok(trusted)
    }
}

// ---------------------------------------------------------------------------
// connect + authenticate
// ---------------------------------------------------------------------------

enum ConnError {
    /// Do not retry (host key problems, auth exhausted, bad config).
    Fatal(anyhow::Error),
    /// Network-level problems, worth retrying with backoff.
    Transient(anyhow::Error),
}

async fn connect_and_auth(
    cfg: &russh_config::Config,
    identity_files: &[PathBuf],
    known_hosts: &Path,
) -> Result<Arc<Handle<Handler>>, ConnError> {
    let stream = cfg
        .stream()
        .await
        .map_err(|error| ConnError::Transient(anyhow!("cannot open connection stream: {error}")))?;

    let verdict = Arc::new(Mutex::new(None));
    let handler = Handler {
        host: cfg.host().to_string(),
        port: cfg.port(),
        known_hosts: known_hosts.to_path_buf(),
        verdict: verdict.clone(),
    };

    let ssh_config = Arc::new(client::Config {
        nodelay: true,
        keepalive_interval: Some(Duration::from_secs(10)),
        keepalive_max: 3,
        ..Default::default()
    });

    let mut handle = match client::connect_stream(ssh_config, stream, handler).await {
        Ok(handle) => handle,
        Err(error) => {
            let verdict = verdict.lock().expect("verdict mutex").take();
            return Err(match verdict {
                Some(KeyVerdict::Unknown {
                    algorithm,
                    fingerprint,
                }) => ConnError::Fatal(anyhow!(
                    "The authenticity of host '{}:{}' can't be established.\n\
                     {algorithm} key fingerprint is {fingerprint}.\n\
                     This host is not trusted: no matching entry in {}.\n\
                     Strict host key checking is enabled; not connecting.\n\
                     Verify the fingerprint out-of-band and add the key to known_hosts \
                     (e.g. connect once with OpenSSH, or `ssh-keyscan {} >> {}`).",
                    cfg.host(),
                    cfg.port(),
                    known_hosts.display(),
                    cfg.host(),
                    known_hosts.display(),
                )),
                Some(KeyVerdict::Changed { line }) => ConnError::Fatal(anyhow!(
                    "REMOTE HOST KEY CHANGED for '{}:{}' ({} line {line}).\n\
                     This could indicate a man-in-the-middle attack. Not connecting.",
                    cfg.host(),
                    cfg.port(),
                    known_hosts.display(),
                )),
                Some(KeyVerdict::Error(message)) => ConnError::Fatal(anyhow!(
                    "host key verification error for '{}:{}': {message}",
                    cfg.host(),
                    cfg.port(),
                )),
                _ => ConnError::Transient(anyhow!(
                    "SSH connect to {}:{} failed: {error:#}",
                    cfg.host(),
                    cfg.port()
                )),
            });
        }
    };

    authenticate(&mut handle, &cfg.user(), identity_files)
        .await
        .map_err(ConnError::Fatal)?;

    Ok(Arc::new(handle))
}

/// Try each key source in priority order: IdentityFile entries, default key
/// paths, then ssh-agent. Public key only; password auth is not implemented.
async fn authenticate(
    handle: &mut Handle<Handler>,
    user: &str,
    identity_files: &[PathBuf],
) -> Result<()> {
    let mut candidates: Vec<PathBuf> = identity_files.to_vec();
    let ssh_dir = home_dir()?.join(".ssh");
    for name in ["id_ed25519", "id_rsa"] {
        let path = ssh_dir.join(name);
        if path.exists() && !candidates.contains(&path) {
            candidates.push(path);
        }
    }

    let mut tried = 0usize;
    for path in &candidates {
        if !path.exists() {
            warn!("IdentityFile {} does not exist, skipping", path.display());
            continue;
        }
        let Some(key) = load_key_interactive(path) else {
            continue;
        };
        tried += 1;
        let hash = handle
            .best_supported_rsa_hash()
            .await
            .ok()
            .flatten()
            .flatten();
        match handle
            .authenticate_publickey(user, PrivateKeyWithHashAlg::new(Arc::new(key), hash))
            .await
        {
            Ok(result) if result.success() => {
                info!("authenticated as {user} with key {}", path.display());
                return Ok(());
            }
            Ok(_) => debug!("key {} was rejected by the server", path.display()),
            Err(error) => debug!("auth with {} failed: {error:#}", path.display()),
        }
    }

    match AgentClient::connect_env().await {
        Ok(mut agent) => match agent.request_identities().await {
            Ok(identities) => {
                for identity in &identities {
                    let AgentIdentity::PublicKey { key, comment } = identity else {
                        debug!("skipping certificate identity from ssh-agent");
                        continue;
                    };
                    tried += 1;
                    let hash = handle
                        .best_supported_rsa_hash()
                        .await
                        .ok()
                        .flatten()
                        .flatten();
                    match handle
                        .authenticate_publickey_with(user, key.clone(), hash, &mut agent)
                        .await
                    {
                        Ok(result) if result.success() => {
                            info!("authenticated as {user} with ssh-agent key ({comment})");
                            return Ok(());
                        }
                        Ok(_) => debug!("agent key {comment} was rejected by the server"),
                        Err(error) => debug!("agent auth with {comment} failed: {error}"),
                    }
                }
            }
            Err(error) => debug!("ssh-agent request_identities failed: {error}"),
        },
        Err(error) => debug!("ssh-agent not available: {error}"),
    }

    bail!(
        "authentication failed (publickey only; password/keyboard-interactive are not supported). \
         Tried {tried} key(s) from IdentityFile/default paths/ssh-agent."
    )
}

/// Load a private key; if it is encrypted, ask for the passphrase on the
/// terminal (hidden input) up to 3 times.
fn load_key_interactive(path: &Path) -> Option<PrivateKey> {
    match load_secret_key(path, None) {
        Ok(key) => Some(key),
        Err(russh::keys::Error::KeyIsEncrypted) => {
            for _ in 0..3 {
                let prompt = format!("Enter passphrase for {}: ", path.display());
                let passphrase = match rpassword::prompt_password(prompt) {
                    Ok(passphrase) => passphrase,
                    Err(error) => {
                        warn!("cannot read passphrase: {error}");
                        return None;
                    }
                };
                match load_secret_key(path, Some(&passphrase)) {
                    Ok(key) => return Some(key),
                    Err(_) => error!("incorrect passphrase for {}", path.display()),
                }
            }
            warn!("giving up on encrypted key {}", path.display());
            None
        }
        Err(error) => {
            warn!("cannot load key {}: {error}", path.display());
            None
        }
    }
}

// ---------------------------------------------------------------------------
// main poll loop for one SSH session
// ---------------------------------------------------------------------------

struct SessionParams {
    port_lo: u16,
    port_hi: u16,
    exclude: BTreeSet<u16>,
    interval: f64,
    on_conflict: OnConflict,
}

/// Runs until the connection breaks; always returns Err (transient).
async fn poll_loop(session: Arc<Handle<Handler>>, params: &SessionParams) -> Result<()> {
    let mut channel = session
        .channel_open_session()
        .await
        .context("cannot open probe channel")?;
    channel
        .exec(true, PROBE_SCRIPT)
        .await
        .context("cannot start remote probe")?;

    let mut adaptive = Adaptive::new(params.interval);
    let mut buffer = Vec::new();
    let mut forwards = ForwardManager::new();

    channel
        .data_bytes(format!("{:.2}\n", adaptive.current()))
        .await
        .context("cannot trigger remote probe")?;

    let result = async {
        loop {
            let timeout = Duration::from_secs_f64(adaptive.current() * 3.0 + 60.0);
            let frame = read_frame(&mut channel, &mut buffer, timeout).await?;

            let changed = match parse_frame(&frame) {
                Ok(ports) => {
                    let target: BTreeSet<u16> = ports
                        .into_iter()
                        .filter(|port| {
                            *port >= params.port_lo
                                && *port <= params.port_hi
                                && !params.exclude.contains(port)
                        })
                        .collect();
                    debug!("remote listening ports (filtered): {target:?}");
                    forwards
                        .reconcile(&session, target, params.on_conflict)
                        .await
                }
                Err(error) => {
                    warn!("cannot parse probe frame, skipping round: {error:#}");
                    false
                }
            };

            let next = adaptive.next(changed);
            channel
                .data_bytes(format!("{next:.2}\n"))
                .await
                .context("cannot reach remote probe")?;
        }
        #[allow(unreachable_code)]
        Ok::<(), anyhow::Error>(())
    }
    .await;

    forwards.stop_all().await;
    result
}

// ---------------------------------------------------------------------------
// entry point / reconnect supervision
// ---------------------------------------------------------------------------

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();
    tracing_subscriber::fmt()
        .with_max_level(if cli.verbose {
            tracing::Level::DEBUG
        } else {
            tracing::Level::INFO
        })
        .with_target(false)
        .without_time()
        .init();

    ensure_host_alias(&cli.host)?;
    let config = russh_config::parse_home(&cli.host).context("failed to parse ~/.ssh/config")?;

    if config.host_config.proxy_jump.is_some() {
        error!(
            "host '{}' uses ProxyJump, which is not supported yet by this tool.",
            cli.host
        );
        bail!(
            "ProxyJump is not supported yet; please remove it from ~/.ssh/config or connect directly."
        );
    }
    if let Some(command) = &config.host_config.proxy_command {
        info!("using ProxyCommand from ssh config: {command}");
    }

    let home = home_dir()?;
    let known_hosts = config
        .host_config
        .user_known_hosts_file
        .clone()
        .unwrap_or_else(|| home.join(".ssh").join("known_hosts"));
    let identity_files = config.host_config.identity_file.clone().unwrap_or_default();

    let params = SessionParams {
        port_lo: cli.port_range.0,
        port_hi: cli.port_range.1,
        exclude: cli.exclude.iter().copied().collect(),
        interval: cli.interval,
        on_conflict: cli.on_conflict,
    };

    info!(
        "target: {}@{}:{} (alias \"{}\", known_hosts: {})",
        config.user(),
        config.host(),
        config.port(),
        cli.host,
        known_hosts.display()
    );
    info!(
        "watching remote ports {}-{} (exclude: {:?})",
        params.port_lo, params.port_hi, cli.exclude
    );

    let mut backoff = 1.0;
    loop {
        match connect_and_auth(&config, &identity_files, &known_hosts).await {
            Ok(session) => {
                info!("connected; (re)building forwards from remote state");
                backoff = 1.0;
                match poll_loop(session, &params).await {
                    Err(error) => warn!("connection lost: {error:#}"),
                    Ok(()) => unreachable!("poll_loop only returns on error"),
                }
                info!("all forwards torn down");
            }
            Err(ConnError::Fatal(error)) => {
                error!("{error:#}");
                return Err(error);
            }
            Err(ConnError::Transient(error)) => {
                warn!("connect failed: {error:#}");
            }
        }
        info!("reconnecting in {:.0}s", backoff);
        tokio::time::sleep(Duration::from_secs_f64(backoff)).await;
        backoff = (backoff * 2.0).min(30.0);
    }
}
