//! HTTP upload used by `ACLanClient::upload` (`FUN_1800d6950`).

use std::path::Path;

use reqwest::{
    Client,
    header::{CONTENT_TYPE, EXPECT},
};
use tokio_util::io::ReaderStream;

use crate::{
    error::{Error, Result},
    protocol::LanInfo,
};

#[derive(Debug, Clone)]
pub struct UploadResult {
    pub http_status: u16,
    pub bytes_sent: u64,
}

/// Uploads a file as an unframed `application/octet-stream` POST body.  The
/// target is concatenated to `fileUploadUrl`, matching the Qt source—not URL
/// joined—because the exact slash convention is device supplied.
pub async fn upload_file(
    client: &Client,
    info: &LanInfo,
    local_file: impl AsRef<Path>,
    target: &str,
) -> Result<UploadResult> {
    let base = info
        .file_upload_url
        .as_deref()
        .ok_or(Error::Protocol("LAN response has no fileUploadUrl"))?;
    let path = local_file.as_ref();
    let metadata = tokio::fs::metadata(path).await?;
    let stream = ReaderStream::new(tokio::fs::File::open(path).await?);
    let response = client
        .post(format!("{base}{target}"))
        .header(CONTENT_TYPE, "application/octet-stream")
        // `QNetworkRequest::setRawHeader("Expect", "")` in the original.
        .header(EXPECT, "")
        .body(reqwest::Body::wrap_stream(stream))
        .send()
        .await?;
    let status = response.status();
    if !status.is_success() {
        return Err(Error::Protocol("printer rejected file upload"));
    }
    Ok(UploadResult {
        http_status: status.as_u16(),
        bytes_sent: metadata.len(),
    })
}
