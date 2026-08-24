//! Wire-format primitives proven by `plugins/accloud.dll` in Photon Workshop 4.1.8.
//!
//! The types intentionally retain unknown JSON as [`serde_json::Value`].  That
//! makes model-specific additions observable instead of silently inventing a
//! Rust interpretation for them.

use std::{
    fmt,
    time::{SystemTime, UNIX_EPOCH},
};

use aes::Aes128;
use base64::{Engine, engine::general_purpose::STANDARD as BASE64};
use cbc::{
    Decryptor,
    cipher::{BlockDecryptMut, KeyIvInit, block_padding::Pkcs7},
};
use md5::{Digest, Md5};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use url::Url;
use uuid::Uuid;

use crate::error::{Error, Result};

pub const TOPIC_PREFIX: &str = "anycubic/anycubicCloud/v1";
pub const SSDP_MULTICAST: &str = "239.255.255.250:1900";
pub const SSDP_SERVICE_TYPE: &str = "ac:3dprinter:lcd";
pub const CAMERA_URL_TEMPLATE: &str = "http://{host}:18088/live/0";
pub const DEFAULT_WORKSHOP_MQTT_PORT: u16 = 8883;

/// The exact bytes constructed by `SsdpManager::onSearchTimeout`
/// (`FUN_180151200 @ 0x180151200`).
pub const SSDP_SEARCH_REQUEST: &[u8] = b"M-SEARCH * HTTP/1.1\r\nHost: 239.255.255.250:1900\r\nST: ac:3dprinter:lcd\r\nMan: \"ssdp:discover\"\r\nMX: 5\r\n";

/// Parsed `/info` discovery document.  The fields mirror
/// `FUN_18014b690 @ 0x18014b690`; optionality is intentional because different
/// firmwares omit fields.
#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LocationInfo {
    pub device_type: Option<String>,
    pub model_id: Option<String>,
    pub model_name: Option<String>,
    pub cn: Option<String>,
    pub ctrl_type: Option<String>,
    pub ctrl_info_url: Option<String>,
    pub ip: Option<String>,
    pub device_name: Option<String>,
    pub zone: Option<String>,
    pub env: Option<String>,
    pub token: Option<String>,
    /// Present on some newer firmwares and useful for a stable physical ID.
    pub usn: Option<String>,
}

/// Decrypted broker credential record, parsed by
/// `FUN_18014aaf0 @ 0x18014aaf0`.
#[derive(Clone, Default, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LanInfo {
    pub encoded_info: Option<String>,
    #[serde(rename = "encodedToten")]
    pub encoded_token: Option<String>,
    pub device_type: Option<String>,
    pub account_delete_url: Option<String>,
    pub broker: String,
    pub client_id: String,
    pub device_id: String,
    pub username: String,
    pub password: String,
    pub devicecrt: Option<String>,
    pub devicepk: Option<String>,
    pub cacrt: Option<String>,
    pub mode_id: String,
    pub model_name: Option<String>,
    pub ip: Option<String>,
    pub mac: Option<String>,
    pub file_upload_url: Option<String>,
    #[serde(default)]
    pub location_info: Option<Value>,
}

impl fmt::Debug for LanInfo {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("LanInfo")
            .field("broker", &self.broker)
            .field("client_id", &self.client_id)
            .field("device_id", &self.device_id)
            .field("username", &"<redacted>")
            .field("password", &"<redacted>")
            .field("mode_id", &self.mode_id)
            .field("model_name", &self.model_name)
            .field("ip", &self.ip)
            .finish()
    }
}

impl LanInfo {
    /// Accept both the 4.1.8 `host:port` form and a later `mqtts://host:port`
    /// form seen in external implementations.
    pub fn broker_address(&self) -> Result<(String, u16)> {
        let value = self.broker.trim();
        if value.is_empty() {
            return Err(Error::Protocol("empty broker"));
        }
        let candidate = if value.contains("://") {
            value.to_owned()
        } else {
            format!("mqtts://{value}")
        };
        let url = Url::parse(&candidate)?;
        let host = url
            .host_str()
            .ok_or(Error::Protocol("broker has no host"))?;
        Ok((
            host.to_owned(),
            url.port().unwrap_or(DEFAULT_WORKSHOP_MQTT_PORT),
        ))
    }

    pub fn command_topic(&self, message_type: &str) -> String {
        format!(
            "{TOPIC_PREFIX}/pc/printer/{}/{}/{}",
            self.mode_id, self.device_id, message_type
        )
    }

    pub fn report_topic_filter(&self) -> String {
        format!(
            "{TOPIC_PREFIX}/printer/+/{}/{}/+/report",
            self.device_id, self.mode_id
        )
    }

    pub fn camera_url(&self) -> Option<String> {
        self.ip
            .as_ref()
            .map(|host| CAMERA_URL_TEMPLATE.replace("{host}", host))
    }
}

/// JSON carried in every MQTT command (`FUN_1800e2d40` and related builders).
#[derive(Clone, Debug, Serialize)]
pub struct CommandEnvelope<'a> {
    #[serde(rename = "type")]
    pub message_type: &'a str,
    pub action: &'a str,
    pub timestamp: i64,
    pub msgid: String,
    pub data: Value,
}

impl<'a> CommandEnvelope<'a> {
    pub fn new(message_type: &'a str, action: &'a str, data: Value) -> Self {
        Self {
            message_type,
            action,
            timestamp: unix_millis(),
            msgid: Uuid::new_v4().to_string(),
            data,
        }
    }
}

/// JSON received from the printer.  `data` must remain untyped until a report
/// has been captured for the particular firmware.
#[derive(Clone, Debug, Deserialize, Serialize)]
pub struct ReportEnvelope {
    #[serde(rename = "type")]
    pub message_type: Option<String>,
    pub action: Option<String>,
    pub state: Option<String>,
    pub timestamp: Option<i64>,
    pub msgid: Option<String>,
    #[serde(default)]
    pub data: Value,
}

pub fn unix_millis() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis()
        .try_into()
        .unwrap_or(i64::MAX)
}

pub fn md5_hex(bytes: &[u8]) -> String {
    format!("{:x}", Md5::digest(bytes))
}

/// `sign` generated by `FUN_1800856f0 @ 0x1800856f0`:
/// `MD5( hex(MD5(token[0..16])) || ts || nonce )`.
pub fn bootstrap_sign(token: &str, timestamp: i64, nonce: &str) -> Result<String> {
    let prefix = token.get(..16).ok_or(Error::Protocol(
        "bootstrap token shorter than 16 characters",
    ))?;
    let first = md5_hex(prefix.as_bytes());
    Ok(md5_hex(format!("{first}{timestamp}{nonce}").as_bytes()))
}

/// Reproduces the `did` hash in `FUN_180086ca0 @ 0x180086ca0` once its chosen
/// adapter MAC is known.  The adapter-selection rule is Windows-specific
/// (first Ethernet/Wi-Fi adapter from `GetAdaptersInfo`), so it is deliberately
/// separate from this pure function.
pub fn workshop_did_from_mac(mac: [u8; 6]) -> String {
    let mut source = Vec::with_capacity(12);
    source.extend_from_slice(&mac);
    source.extend_from_slice(&[0x54, 0x09, 0x27, 0x4e, 0x66, 0x12]);
    md5_hex(&source)
}

/// Decrypt the documented LAN credential exchange.  This matches the external
/// LAN reference implementation (AES-128-CBC, PKCS#7): key = `local_token[16..32]`,
/// IV = returned control token's first sixteen bytes.  `accloud.c` confirms AES,
/// Base64 and padding removal, but its decompiler output should be checked with
/// a captured Photon response before treating this particular key/IV ordering
/// as universal across firmware generations.
pub fn decrypt_bootstrap_info(
    ciphertext_b64: &str,
    response_token: &str,
    local_token: &str,
) -> Result<Value> {
    let key = local_token.as_bytes().get(16..32).ok_or(Error::Protocol(
        "local token does not contain a 16-byte AES key",
    ))?;
    let mut iv = [0_u8; 16];
    let copy_len = response_token.len().min(iv.len());
    iv[..copy_len].copy_from_slice(&response_token.as_bytes()[..copy_len]);
    let mut ciphertext = BASE64.decode(ciphertext_b64)?;
    let plain = Decryptor::<Aes128>::new_from_slices(key, &iv)
        .map_err(|_| Error::Protocol("invalid AES key or IV"))?
        .decrypt_padded_mut::<Pkcs7>(&mut ciphertext)
        .map_err(|_| Error::Protocol("invalid encrypted bootstrap padding"))?;
    Ok(serde_json::from_slice(plain)?)
}

pub fn null_query(message_type: &str) -> CommandEnvelope<'_> {
    CommandEnvelope::new(message_type, "query", Value::Null)
}

#[allow(clippy::too_many_arguments)]
pub fn start_print_data(
    taskid: String,
    task_mode: Value,
    url: String,
    filename: String,
    filesize: u64,
    md5: String,
    filetype: Value,
    filepath: Option<String>,
    project_type: Value,
    task_settings: Value,
) -> Value {
    json!({
        "taskid": taskid,
        "task_mode": task_mode,
        "url": url,
        "filename": filename,
        "filesize": filesize,
        "md5": md5,
        "filetype": filetype,
        "filepath": filepath,
        "project_type": project_type,
        "task_settings": task_settings,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn command_and_report_topics_follow_the_workshop_template() {
        let info = LanInfo {
            broker: "192.168.0.101:8883".to_owned(),
            client_id: "client".to_owned(),
            device_id: "device".to_owned(),
            username: "user".to_owned(),
            password: "password".to_owned(),
            mode_id: "model".to_owned(),
            ..Default::default()
        };
        assert_eq!(
            info.command_topic("status"),
            "anycubic/anycubicCloud/v1/pc/printer/model/device/status"
        );
        assert_eq!(
            info.report_topic_filter(),
            "anycubic/anycubicCloud/v1/printer/+/device/model/+/report"
        );
    }

    #[test]
    fn bootstrap_signature_matches_the_md5_construction() {
        assert_eq!(
            bootstrap_sign("abcdefghijklmnopqrstuvwxyz", 123, "nonce").unwrap(),
            "a7cef6796b8472a885753062f4977e1c"
        );
    }

    #[test]
    fn workshop_did_keeps_the_binary_salt() {
        assert_eq!(
            workshop_did_from_mac([1, 2, 3, 4, 5, 6]),
            "055b9e4be4fa5c0e9e0b7b6d1b212e8e"
        );
    }
}
