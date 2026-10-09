use std::collections::BTreeSet;
use std::time::Duration;

use anyhow::{anyhow, bail, Result};
use russh::client::Msg;
use russh::{Channel, ChannelMsg};
use tracing::debug;

/// Dump /proc/net/tcp (IPv4) immediately, then every 2 seconds.
pub const PROBE_SCRIPT: &str = "\
while true; do \
      cat /proc/net/tcp || exit 1; \
      echo ---SYNC---; \
      sleep 2; \
done";

const SYNC_MARKER: &[u8] = b"---SYNC---";
const MAX_PROBE_FRAME_BYTES: usize = 1024 * 1024;

/// Wait until a full `---SYNC---`-terminated frame has been received.
/// Any EOF, close, timeout, or oversized frame means the connection or remote
/// shell is unusable.
pub async fn read_frame(
    chan: &mut Channel<Msg>,
    buf: &mut Vec<u8>,
    timeout: Duration,
) -> Result<Vec<u8>> {
    let deadline = tokio::time::Instant::now() + timeout;
    loop {
        if let Some(pos) = buf
            .windows(SYNC_MARKER.len())
            .position(|w| w == SYNC_MARKER)
        {
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
        let msg = tokio::time::timeout_at(deadline, chan.wait())
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

/// One parsed probe frame -> set of listening ports we care about.
pub fn parse_frame(frame: &[u8]) -> Result<BTreeSet<u16>> {
    let text = String::from_utf8_lossy(frame);
    let mut ports = BTreeSet::new();
    let mut saw_content = false;
    let mut recognized = false;
    for line in text.lines().map(str::trim) {
        if line.is_empty() {
            continue;
        }
        saw_content = true;
        match parse_proc_line(line) {
            Some(Some(port)) => {
                recognized = true;
                ports.insert(port);
            }
            Some(None) => recognized = true,
            None => debug!("unparsed probe line: {line}"),
        }
    }
    if saw_content && !recognized {
        bail!("frame did not match /proc/net/tcp format");
    }
    Ok(ports)
}

/// /proc/net/tcp line: `sl local_address rem_address st ...`
/// local_address is little-endian hex "AABBCCDD:PPPP". st 0A = LISTEN.
fn parse_proc_line(line: &str) -> Option<Option<u16>> {
    if line.starts_with("sl") {
        return Some(None);
    }
    let fields: Vec<&str> = line.split_whitespace().collect();
    if fields.len() < 4 {
        return None;
    }
    let (addr_hex, port_hex) = fields[1].split_once(':')?;
    if addr_hex.len() != 8 || port_hex.len() != 4 {
        return None;
    }
    let port = u16::from_str_radix(port_hex, 16).ok()?;
    if fields[3] != "0A" {
        return Some(None);
    }
    let bytes = u32::from_str_radix(addr_hex, 16).ok()?.to_le_bytes();
    Some((bytes == [127, 0, 0, 1] || bytes == [0, 0, 0, 0]).then_some(port))
}

#[cfg(test)]
mod tests {
    use super::*;

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
    fn parse_garbage_frame_fails() {
        assert!(parse_frame(b"cat: /proc/net/tcp: No such file or directory").is_err());
        assert_eq!(parse_frame(b"\n").unwrap(), BTreeSet::new());
    }

    #[test]
    fn limits_probe_buffer_growth() {
        let mut buffer = vec![0; MAX_PROBE_FRAME_BYTES];
        assert!(append_probe_data(&mut buffer, &[0]).is_err());
    }
}
