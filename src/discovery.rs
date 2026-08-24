//! SSDP discovery and the HTTP credential bootstrap.
//!
//! Discovery is safe and read-only: it sends the exact M-SEARCH datagram used
//! by Photon Workshop and optionally performs `GET /info`.  The signed POST is
//! separate because it returns short-lived connection credentials.

use std::{collections::BTreeMap, net::SocketAddr, time::Duration};

use reqwest::{Client, header::CONTENT_TYPE};
use serde::Deserialize;
use tokio::{
    net::UdpSocket,
    time::{Instant, timeout},
};
use url::Url;
use uuid::Uuid;

use crate::{
    error::{Error, Result},
    protocol::{
        LanInfo, LocationInfo, SSDP_MULTICAST, SSDP_SEARCH_REQUEST, bootstrap_sign,
        decrypt_bootstrap_info, unix_millis,
    },
};

/// An SSDP response and the UDP endpoint that sent it.
#[derive(Clone, Debug)]
pub struct SsdpAdvertisement {
    pub sender: SocketAddr,
    pub headers: BTreeMap<String, String>,
    pub raw: Vec<u8>,
}

impl SsdpAdvertisement {
    pub fn location(&self) -> Option<&str> {
        self.headers.get("LOCATION").map(String::as_str)
    }
    pub fn usn(&self) -> Option<&str> {
        self.headers.get("USN").map(String::as_str)
    }
}

/// Send four SSDP searches over about ten seconds in the original application.
/// The default below keeps the same number of sends and 2.5-second spacing.
pub async fn discover(timeout_total: Duration) -> Result<Vec<SsdpAdvertisement>> {
    let socket = UdpSocket::bind("0.0.0.0:0").await?;
    let deadline = Instant::now() + timeout_total;
    let mut next_search = Instant::now();
    let mut sent = 0_u8;
    let mut found = BTreeMap::<SocketAddr, SsdpAdvertisement>::new();
    let mut buf = vec![0_u8; 8192];

    while Instant::now() < deadline {
        if sent < 4 && Instant::now() >= next_search {
            socket.send_to(SSDP_SEARCH_REQUEST, SSDP_MULTICAST).await?;
            sent += 1;
            next_search += Duration::from_millis(2500);
        }
        let wake = next_search.min(deadline);
        let duration = wake.saturating_duration_since(Instant::now());
        match timeout(duration, socket.recv_from(&mut buf)).await {
            Ok(Ok((size, sender))) => {
                let raw = buf[..size].to_vec();
                found.insert(
                    sender,
                    SsdpAdvertisement {
                        sender,
                        headers: parse_ssdp_headers(&raw),
                        raw,
                    },
                );
            }
            Ok(Err(error)) => return Err(error.into()),
            Err(_) => {}
        }
    }
    Ok(found.into_values().collect())
}

/// `FUN_18014bcf0 @ 0x18014bcf0` looks specifically for ST, USN, and LOCATION.
pub fn parse_ssdp_headers(raw: &[u8]) -> BTreeMap<String, String> {
    let text = String::from_utf8_lossy(raw);
    text.lines()
        .filter_map(|line| line.split_once(':'))
        .map(|(name, value)| (name.trim().to_ascii_uppercase(), value.trim().to_owned()))
        .filter(|(name, _)| matches!(name.as_str(), "ST" | "USN" | "LOCATION"))
        .collect()
}

pub fn info_url(host: &str, port: u16) -> Result<Url> {
    Ok(Url::parse(&format!("http://{host}:{port}/info"))?)
}

/// Reads the discovery document.  The Content-Type cannot be trusted—the Qt
/// code asks for `application/x-www-form-urlencoded`, then parses the body as
/// JSON—so this method parses bytes directly too.
pub async fn fetch_location_info(client: &Client, host: &str, port: u16) -> Result<LocationInfo> {
    let response = client
        .get(info_url(host, port)?)
        .header(CONTENT_TYPE, "application/x-www-form-urlencoded")
        .send()
        .await?
        .error_for_status()?;
    Ok(serde_json::from_slice(&response.bytes().await?)?)
}

#[derive(Debug, Deserialize)]
struct EncryptedControlResponse {
    code: i32,
    #[serde(default)]
    message: String,
    data: Option<EncryptedControlData>,
}

#[derive(Debug, Deserialize)]
struct EncryptedControlData {
    info: String,
    token: String,
}

/// Full bootstrap after `/info` is known.  `did` is explicit because Workshop
/// 4.1.8 derives it from the Windows host's selected NIC; generating a random
/// replacement would not faithfully reproduce that build.
pub async fn authenticate(client: &Client, location: &LocationInfo, did: &str) -> Result<LanInfo> {
    let local_token = location
        .token
        .as_deref()
        .ok_or(Error::Protocol("/info has no token"))?;
    let control_url = location
        .ctrl_info_url
        .as_deref()
        .ok_or(Error::Protocol("/info has no ctrlInfoUrl"))?;
    if did.is_empty() {
        return Err(Error::Protocol("empty bootstrap did"));
    }

    let timestamp = unix_millis();
    let nonce = Uuid::new_v4().simple().to_string();
    let sign = bootstrap_sign(local_token, timestamp, &nonce)?;
    let mut url = Url::parse(control_url)?;
    url.query_pairs_mut()
        .append_pair("ts", &timestamp.to_string())
        .append_pair("nonce", &nonce)
        .append_pair("did", did)
        .append_pair("sign", &sign);

    let response: EncryptedControlResponse = client
        .post(url)
        .header(CONTENT_TYPE, "application/x-www-form-urlencoded")
        .body(Vec::new())
        .send()
        .await?
        .error_for_status()?
        .json()
        .await?;
    if response.code != 200 {
        return Err(Error::BootstrapRejected(response.message));
    }
    let encrypted = response
        .data
        .ok_or(Error::Protocol("control reply has no data"))?;
    let value = decrypt_bootstrap_info(&encrypted.info, &encrypted.token, local_token)?;
    let mut info: LanInfo = serde_json::from_value(value)?;
    if info.ip.is_none() {
        info.ip = location.ip.clone();
    }
    Ok(info)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_the_ssdp_headers_consumed_by_workshop_are_retained() {
        let headers = parse_ssdp_headers(
            b"HTTP/1.1 200 OK\r\nST: ac:3dprinter:lcd\r\nUSN: uuid:test\r\nLOCATION: http://192.168.0.101:18910/info\r\nServer: ignored\r\n\r\n",
        );
        assert_eq!(headers["ST"], "ac:3dprinter:lcd");
        assert_eq!(headers["USN"], "uuid:test");
        assert_eq!(headers["LOCATION"], "http://192.168.0.101:18910/info");
        assert!(!headers.contains_key("SERVER"));
    }
}
