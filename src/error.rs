use std::io;

use thiserror::Error;

/// Errors returned by the LAN discovery, HTTP bootstrap, and MQTT layers.
#[derive(Debug, Error)]
pub enum Error {
    #[error("network I/O: {0}")]
    Io(#[from] io::Error),
    #[error("HTTP: {0}")]
    Http(#[from] reqwest::Error),
    #[error("MQTT: {0}")]
    Mqtt(#[from] rumqttc::ClientError),
    #[error("MQTT connection: {0}")]
    MqttConnection(#[source] Box<rumqttc::ConnectionError>),
    #[error("JSON: {0}")]
    Json(#[from] serde_json::Error),
    #[error("ZIP archive: {0}")]
    Zip(#[from] zip::result::ZipError),
    #[error("invalid URL: {0}")]
    Url(#[from] url::ParseError),
    #[error("base64 payload: {0}")]
    Base64(#[from] base64::DecodeError),
    #[error("invalid protocol data: {0}")]
    Protocol(&'static str),
    #[error("invalid 3D model: {0}")]
    InvalidModel(String),
    #[error("unsupported model format: {0}")]
    UnsupportedModelFormat(String),
    #[error("unsupported slicer option: {0}")]
    UnsupportedSlicerOption(&'static str),
    #[error("external mesh converter failed: {0}")]
    MeshConverter(String),
    #[error("cloud API rejected request: {0}")]
    Cloud(String),
    #[error("printer rejected bootstrap request: {0}")]
    BootstrapRejected(String),
    #[error("timeout waiting for {0}")]
    Timeout(&'static str),
    #[error("MQTT report stream closed")]
    Disconnected,
}

pub type Result<T, E = Error> = std::result::Result<T, E>;
