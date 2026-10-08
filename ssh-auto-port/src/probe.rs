use std::collections::BTreeSet;
use std::time::Duration;

use anyhow::{anyhow, bail, Result};
use russh::client::Msg;
use russh::{Channel, ChannelMsg};
use tracing::debug;

use crate::cli::MAX_POLL_INTERVAL;

/// Remote loop: every time we send one line (the sleep duration) on stdin it
/// dumps the listening sockets, prints the frame separator and sleeps.
/// `ss` is preferred; fall back to parsing /proc/net/tcp{,6} (hex format).
pub(crate) const PROBE_SCRIPT: &str = "while read -r d; do \
    ss -ltnH 2>/dev/null || cat /proc/net/tcp /proc/net/tcp6; \
    echo ---SYNC---; sleep \"$d\"; done";

const SYNC_MARKER: &[u8] = b"---SYNC---";
const MAX_PROBE_FRAME_BYTES: usize = 1024 * 1024;

/// Adaptive polling interval: speed up to 1s for 5 rounds after a change,
/// back off gradually (x1.5, capped at 10s) after 10 stable rounds.
pub(crate) struct Adaptive {
    base: f64,
    cur: f64,
    fast_left: u32,
    stable: u32,
}

impl Adaptive {
    pub(crate) fn new(base: f64) -> Self {
        debug_assert!(base.is_finite() && base > 0.0);
        Self {
            base,
            cur: base,
            fast_left: 0,
            stable: 0,
        }
    }

    pub(crate) fn current(&self) -> f64 {
        self.cur
    }

    pub(crate) fn next(&mut self, changed: bool) -> f64 {
        if changed {
            self.cur = 1.0;
            self.fast_left = 5;
            self.stable = 0;
            debug!("ports changed; polling sped up to 1s for 5 rounds");
        } else if self.fast_left > 0 {
            self.fast_left -= 1;
            if self.fast_left == 0 {
                self.cur = self.base;
            }
        } else {
            self.stable += 1;
            if self.stable > 10 {
                self.cur = (self.cur * 1.5).min(MAX_POLL_INTERVAL);
                debug!("polling backed off to {:.2}s", self.cur);
            }
        }
        self.cur
    }
}

/// Wait until a full `---SYNC---`-terminated frame has been received.
/// Any EOF, close, timeout, or oversized frame means the connection or remote
/// shell is unusable.
pub(crate) async fn read_frame(
    chan: &mut Channel<Msg>,
    buf: &mut Vec<u8>,
    timeout: Duration,
) -> Result<Vec<u8>> {
    loop {
        if let Some(pos) = find_subsequence(buf, SYNC_MARKER) {
            let frame = buf[..pos].to_vec();
            let mut end = pos + SYNC_MARKER.len();
            if buf.get(end) == Some(&b'\r') {
                end += 1;
            }
            if buf.get(end) == Some(&b'\n') {
                end += 1;
            }
            buf.drain(..end);
            return Ok(frame);
        }
        let msg = tokio::time::timeout(timeout, chan.wait())
            .await
            .map_err(|_| anyhow!("probe timed out; connection presumed dead"))?
            .ok_or_else(|| anyhow!("probe channel closed (remote shell exited)"))?;
        match msg {
            ChannelMsg::Data { data } => append_probe_data(buf, &data)?,
            ChannelMsg::ExtendedData { data, .. } => {
                debug!("remote probe stderr: {}", String::from_utf8_lossy(&data))
            }
            ChannelMsg::Eof | ChannelMsg::Close => {
                bail!("remote probe shell exited (EOF)")
            }
            ChannelMsg::ExitStatus { exit_status } => {
                bail!("remote probe shell exited with status {exit_status}")
            }
            ChannelMsg::Failure => bail!("remote exec request failed"),
            _ => {}
        }
    }
}

fn append_probe_data(buf: &mut Vec<u8>, data: &[u8]) -> Result<()> {
    if buf.len().saturating_add(data.len()) > MAX_PROBE_FRAME_BYTES {
        bail!("probe frame exceeds {MAX_PROBE_FRAME_BYTES} bytes");
    }
    buf.extend_from_slice(data);
    Ok(())
}

fn find_subsequence(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    haystack
        .windows(needle.len())
        .position(|window| window == needle)
}

/// One parsed probe frame -> set of listening ports we care about.
pub(crate) fn parse_frame(frame: &[u8]) -> Result<BTreeSet<u16>> {
    let text = String::from_utf8_lossy(frame);
    let mut ports = BTreeSet::new();
    let mut saw_content = false;
    let mut recognized = false;
    for line in text.lines().map(str::trim) {
        if line.is_empty() {
            continue;
        }
        saw_content = true;
        match parse_ss_line(line).or_else(|| parse_proc_line(line)) {
            Some(Some(port)) => {
                recognized = true;
                ports.insert(port);
            }
            Some(None) => recognized = true,
            None => debug!("unparsed probe line: {line}"),
        }
    }
    if saw_content && !recognized {
        bail!("frame did not match ss or /proc/net/tcp format");
    }
    Ok(ports)
}

/// `Some(Some(p))` = ss line, port accepted; `Some(None)` = ss line, filtered;
/// `None` = not an ss line.
fn parse_ss_line(line: &str) -> Option<Option<u16>> {
    let fields: Vec<&str> = line.split_whitespace().collect();
    if fields.len() < 4 || fields[0] != "LISTEN" {
        return None;
    }
    let local = fields[3];
    let (addr, port_s) = local.rsplit_once(':')?;
    let addr = addr.trim_start_matches('[').trim_end_matches(']');
    let port: u16 = port_s.parse().ok()?;
    Some(wanted_addr(addr).then_some(port))
}

/// /proc/net/tcp{,6} line: `sl local_address rem_address st ...`
/// local_address is little-endian hex "AABBCCDD:PPPP" (tcp) or 32 hex chars
/// (tcp6, four little-endian u32 words). st 0A = LISTEN.
fn parse_proc_line(line: &str) -> Option<Option<u16>> {
    if line.starts_with("sl") {
        return Some(None);
    }
    let fields: Vec<&str> = line.split_whitespace().collect();
    if fields.len() < 4 {
        return None;
    }
    let (addr_hex, port_hex) = fields[1].split_once(':')?;
    if !(addr_hex.len() == 8 || addr_hex.len() == 32) || port_hex.len() != 4 {
        return None;
    }
    let port = u16::from_str_radix(port_hex, 16).ok()?;
    if fields[3] != "0A" {
        return Some(None);
    }
    let ok = if addr_hex.len() == 8 {
        let raw = u32::from_str_radix(addr_hex, 16).ok()?;
        let bytes = raw.to_le_bytes();
        bytes == [127, 0, 0, 1] || bytes == [0, 0, 0, 0]
    } else {
        let mut bytes = [0u8; 16];
        for (index, chunk) in bytes.chunks_mut(4).enumerate() {
            let word = u32::from_str_radix(addr_hex.get(index * 8..index * 8 + 8)?, 16).ok()?;
            chunk.copy_from_slice(&word.to_le_bytes());
        }
        bytes == [0u8; 16]
            || bytes == {
                let mut loopback = [0u8; 16];
                loopback[15] = 1;
                loopback
            }
    };
    Some(ok.then_some(port))
}

fn wanted_addr(addr: &str) -> bool {
    matches!(addr, "127.0.0.1" | "0.0.0.0" | "::1" | "::" | "*")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_ss_output() {
        let frame = b"LISTEN 0 128 127.0.0.1:3000 0.0.0.0:*\n\
                      LISTEN 0 128 0.0.0.0:8080 0.0.0.0:*\n\
                      LISTEN 0 128 [::1]:9000 [::]:*\n\
                      LISTEN 0 128 [::]:5432 [::]:*\n\
                      LISTEN 0 128 192.168.1.5:6666 0.0.0.0:*\n\
                      LISTEN 0 128 127.0.0.1:3000 0.0.0.0:*\n";
        let ports = parse_frame(frame).unwrap();
        assert_eq!(ports, BTreeSet::from([3000, 8080, 9000, 5432]));
    }

    #[test]
    fn parse_proc_net_tcp_hex() {
        let frame = b"  sl  local_address rem_address   st tx_queue ...\n\
                      0: 0100007F:0BB8 00000000:0000 0A 00000000:00000000 00:00000000 00000000 1000 0 0 0\n\
                      1: 00000000:1F90 00000000:0000 0A 00000000:00000000 00:00000000 00000000 1000 0 0 0\n\
                      2: AC101C05:170C 00000000:0000 0A 00000000:00000000 00:00000000 00000000 1000 0 0 0\n\
                      3: 0100007F:0BB9 00000000:0000 01 00000000:00000000 00:00000000 00000000 1000 0 0 0\n";
        let ports = parse_frame(frame).unwrap();
        assert_eq!(ports, BTreeSet::from([3000, 8080]));
    }

    #[test]
    fn parse_proc_net_tcp6_hex() {
        let frame = b"  sl  local_address                         remote_address                st ...\n\
                      0: 00000000000000000000000001000000:2328 00000000000000000000000000000000:0000 0A 0 0\n\
                      1: 00000000000000000000000000000000:1634 00000000000000000000000000000000:0000 0A 0 0\n\
                      2: 0000000000000000FFFF00000100007F:0401 00000000000000000000000000000000:0000 0A 0 0\n";
        let ports = parse_frame(frame).unwrap();
        assert_eq!(ports, BTreeSet::from([9000, 5684]));
    }

    #[test]
    fn parse_garbage_frame_fails() {
        assert!(parse_frame(b"cat: /proc/net/tcp: No such file or directory").is_err());
        assert_eq!(parse_frame(b"\n").unwrap(), BTreeSet::new());
    }

    #[test]
    fn limits_probe_buffer_growth() {
        let mut buffer = vec![0; MAX_PROBE_FRAME_BYTES];
        assert!(append_probe_data(&mut buffer, &[0]).is_err());
    }

    #[test]
    fn adaptive_interval() {
        let mut adaptive = Adaptive::new(1.5);
        assert_eq!(adaptive.current(), 1.5);
        assert_eq!(adaptive.next(true), 1.0);
        for _ in 0..4 {
            assert_eq!(adaptive.next(false), 1.0);
        }
        assert_eq!(adaptive.next(false), 1.5);
        for _ in 0..10 {
            adaptive.next(false);
        }
        assert!(adaptive.next(false) > 1.5);
        for _ in 0..50 {
            adaptive.next(false);
        }
        assert_eq!(adaptive.current(), MAX_POLL_INTERVAL);
        assert_eq!(adaptive.next(true), 1.0);
    }
}
