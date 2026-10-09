use std::collections::{BTreeSet, HashMap};
use std::io::ErrorKind;
use std::sync::Arc;
use std::time::{Duration, Instant};

use anyhow::{Context, Result};
use russh::client::Handle;
use tokio::net::{TcpListener, TcpStream};
use tokio::task::{JoinHandle, JoinSet};
use tokio_util::sync::CancellationToken;
use tracing::{debug, info, warn};

use crate::Handler;

const RETRY_DELAY: Duration = Duration::from_secs(30);
const REMAP_SEARCH_RADIUS: u32 = 200;
const MIN_REMAP_PORT: u32 = 1024;
const MAX_REMAP_PORT: u32 = 65535;

struct Forward {
    local_port: u16,
    cancellation: CancellationToken,
    task: JoinHandle<()>,
}

impl Forward {
    async fn stop(self) {
        self.cancellation.cancel();
        if let Err(error) = self.task.await {
            warn!("forward listener task terminated unexpectedly: {error}");
        }
    }
}

enum ForwardSetup {
    Started(Forward),
    Skipped,
    RetryLater,
}

#[derive(Default)]
pub struct ForwardManager {
    desired: BTreeSet<u16>,
    forwards: HashMap<u16, Forward>,
    skipped: BTreeSet<u16>,
    retry_at: HashMap<u16, Instant>,
}

impl ForwardManager {
    pub async fn reconcile(
        &mut self,
        session: &Arc<Handle<Handler>>,
        target: BTreeSet<u16>,
        skip: bool,
    ) {
        let removed: Vec<u16> = self.desired.difference(&target).copied().collect();

        for port in removed {
            if let Some(forward) = self.forwards.remove(&port) {
                let local_port = forward.local_port;
                forward.stop().await;
                info!("- forward 127.0.0.1:{local_port} removed (remote :{port} closed)");
            }
            self.skipped.remove(&port);
            self.retry_at.remove(&port);
        }

        let now = Instant::now();
        let pending: Vec<u16> = target
            .iter()
            .filter(|port| !self.forwards.contains_key(port))
            .filter(|port| !self.skipped.contains(port))
            .filter(|port| {
                self.retry_at
                    .get(port)
                    .is_none_or(|retry_at| *retry_at <= now)
            })
            .copied()
            .collect();

        for port in pending {
            match add_forward(session, port, skip).await {
                Ok(ForwardSetup::Started(forward)) => {
                    self.forwards.insert(port, forward);
                    self.retry_at.remove(&port);
                }
                Ok(ForwardSetup::Skipped) => {
                    self.skipped.insert(port);
                    self.retry_at.remove(&port);
                }
                Ok(ForwardSetup::RetryLater) => {
                    self.retry_at.insert(port, now + RETRY_DELAY);
                }
                Err(error) => {
                    warn!(
                        "cannot forward remote :{port}: {error:#}; retrying in {}s",
                        RETRY_DELAY.as_secs()
                    );
                    self.retry_at.insert(port, now + RETRY_DELAY);
                }
            }
        }

        self.desired = target;
    }

    pub async fn stop_all(self) {
        for (port, forward) in self.forwards {
            forward.stop().await;
            debug!("tore down forward for remote :{port}");
        }
    }
}

async fn try_bind(port: u16) -> Result<Option<TcpListener>> {
    match TcpListener::bind(("127.0.0.1", port)).await {
        Ok(listener) => Ok(Some(listener)),
        Err(error) if error.kind() == ErrorKind::AddrInUse => Ok(None),
        Err(error) => Err(error).with_context(|| format!("bind 127.0.0.1:{port} failed")),
    }
}

#[cfg(test)]
#[tokio::test]
async fn occupied_port_is_a_conflict() {
    let listener = try_bind(0).await.unwrap().unwrap();
    let port = listener.local_addr().unwrap().port();
    assert!(try_bind(port).await.unwrap().is_none());
    drop(listener);
    assert!(try_bind(port).await.unwrap().is_some());
}

#[cfg(test)]
#[tokio::test]
async fn stop_all_cancels_and_waits_for_listeners() {
    let listener = try_bind(0).await.unwrap().unwrap();
    let local_port = listener.local_addr().unwrap().port();
    let cancellation = CancellationToken::new();
    let cancelled = cancellation.clone();
    let task = tokio::spawn(async move {
        cancelled.cancelled().await;
        drop(listener);
    });
    let mut manager = ForwardManager::default();
    manager.forwards.insert(
        local_port,
        Forward {
            local_port,
            cancellation: cancellation.clone(),
            task,
        },
    );
    manager.stop_all().await;
    assert!(cancellation.is_cancelled());
    assert!(try_bind(local_port).await.unwrap().is_some());
}

/// Nearest free local port: +1, -1, +2, -2, ... within 1024-65535.
async fn find_free_near(port: u16) -> Result<Option<(TcpListener, u16)>> {
    for offset in 1..=REMAP_SEARCH_RADIUS {
        for candidate in [port as u32 + offset, (port as u32).saturating_sub(offset)] {
            if (MIN_REMAP_PORT..=MAX_REMAP_PORT).contains(&candidate) {
                if let Some(listener) = try_bind(candidate as u16).await? {
                    return Ok(Some((listener, candidate as u16)));
                }
            }
        }
    }
    Ok(None)
}

async fn add_forward(
    session: &Arc<Handle<Handler>>,
    port: u16,
    skip: bool,
) -> Result<ForwardSetup> {
    let (listener, local_port, remapped) = match try_bind(port).await? {
        Some(listener) => (listener, port, false),
        None if skip => {
            warn!("cannot bind local port {port}; skipping forward for remote :{port}");
            return Ok(ForwardSetup::Skipped);
        }
        None => match find_free_near(port).await? {
            Some((listener, local_port)) => (listener, local_port, true),
            None => {
                warn!(
                    "no free local port near {port}; retrying forward for remote :{port} in {}s",
                    RETRY_DELAY.as_secs()
                );
                return Ok(ForwardSetup::RetryLater);
            }
        },
    };

    if remapped {
        info!("+ forward 127.0.0.1:{local_port} -> remote :{port} (local {port} unavailable)");
    } else {
        info!("+ forward 127.0.0.1:{local_port} -> remote :{port}");
    }

    let cancellation = CancellationToken::new();
    let task = tokio::spawn(accept_loop(
        listener,
        session.clone(),
        port,
        cancellation.clone(),
    ));
    Ok(ForwardSetup::Started(Forward {
        local_port,
        cancellation,
        task,
    }))
}

/// Accept local connections and track all direct-tcpip tasks so a removed
/// forward also cancels its active tunnels.
async fn accept_loop(
    listener: TcpListener,
    session: Arc<Handle<Handler>>,
    remote_port: u16,
    cancellation: CancellationToken,
) {
    let mut connections = JoinSet::new();

    loop {
        tokio::select! {
            _ = cancellation.cancelled() => break,
            result = connections.join_next(), if !connections.is_empty() => {
                if let Some(Err(error)) = result {
                    warn!("tunnel task for remote :{remote_port} failed: {error}");
                }
            }
            result = listener.accept() => match result {
                Ok((socket, peer)) => {
                    debug!("local connection from {peer} on forward for remote :{remote_port}");
                    connections.spawn(proxy_connection(
                        socket,
                        session.clone(),
                        remote_port,
                        cancellation.clone(),
                    ));
                }
                Err(error) => {
                    warn!("accept error on forward for remote :{remote_port}: {error}");
                    tokio::select! {
                        _ = cancellation.cancelled() => break,
                        _ = tokio::time::sleep(Duration::from_millis(100)) => {}
                    }
                }
            },
        }
    }

    while let Some(result) = connections.join_next().await {
        if let Err(error) = result {
            warn!("tunnel task for remote :{remote_port} failed during shutdown: {error}");
        }
    }
}

async fn proxy_connection(
    mut socket: TcpStream,
    session: Arc<Handle<Handler>>,
    remote_port: u16,
    cancellation: CancellationToken,
) {
    let channel = tokio::select! {
        _ = cancellation.cancelled() => return,
        result = session.channel_open_direct_tcpip("127.0.0.1", remote_port as u32, "127.0.0.1", 0) => {
            match result {
                Ok(channel) => channel,
                Err(error) => {
                    debug!("cannot open direct-tcpip channel to :{remote_port}: {error}");
                    return;
                }
            }
        }
    };

    let mut stream = channel.into_stream();
    tokio::select! {
        _ = cancellation.cancelled() => {
            debug!("closing tunnel for remote :{remote_port}");
        }
        result = tokio::io::copy_bidirectional(&mut socket, &mut stream) => {
            if let Err(error) = result {
                debug!("tunnel for remote :{remote_port} ended with error: {error}");
            }
        }
    }
}
