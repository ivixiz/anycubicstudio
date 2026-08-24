//! Anycubic Cloud authentication, file delivery and print control.
//!
//! This is deliberately separate from the LAN transport.  Workshop 4.1.8
//! authenticates through Casdoor OAuth, then exchanges its OAuth access token
//! for a short-lived Cloud API token at `/v3/public/loginWithAccessToken`.
//! The exact header construction is present in `QCloudSDK.dll`:
//! `getNonce` (16 random bytes rendered as a UUID) at `0x180135d60`, and
//! `getSignature` at `0x180136330`.
//!
//! `CloudClient` never logs its OAuth access token or short-lived Cloud token.
//! Upload, start and stop were exercised against a Photon Mono M7 Pro; callers
//! must still treat all control methods as active hardware operations.

use std::{
    fs,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

use md5::{Digest, Md5};
use reqwest::header::{CONTENT_TYPE, HeaderMap, HeaderValue};
use serde_json::{Value, json};
use tokio::sync::Mutex;
use uuid::Uuid;

use crate::error::{Error, Result};

/// Legacy production root decoded from `[cloud_api_prod]` in Workshop 4.1.8's
/// `environment.ini`: `https://cloud-platform.anycubiccloud.com/p/p/workbench/api`.
/// DNS for this host no longer resolves in the test environment.
pub const WORKSHOP_4_1_8_CLOUD_API_ROOT: &str =
    "https://cloud-platform.anycubiccloud.com/p/p/workbench/api";

/// Current production root verified against the user's Workshop OAuth token
/// and printer list on 2026-08-24.  It is intentionally distinct from the
/// historical embedded value above.
pub const CURRENT_CLOUD_API_ROOT: &str = "https://cloud-universe.anycubic.com/p/p/workbench/api";

// Shared application credentials embedded verbatim in QCloudSDK.dll.  They
// identify Photon Workshop as an API client; they are not generated from, or
// unique to, an Anycubic user account.  Unlike the OAuth and XX-Token values,
// these constants therefore carry no access to one particular user's account.
const WORKSHOP_APP_ID: &str = "f9b3528877c94d5c9c5af32245db46ef";
const WORKSHOP_APP_SECRET: &str = "0cf75926606049a3937f56b0373b99fb";
const CLIENT_VERSION: &str = "V3.0.0";

/// OAuth access token produced by the Anycubic Casdoor web login.  Do not
/// derive `Debug`; it is a bearer credential.
#[derive(Clone)]
pub struct WorkshopAccessToken(String);

impl WorkshopAccessToken {
    pub fn new(value: impl Into<String>) -> Result<Self> {
        let value = value.into();
        if value.trim().is_empty() {
            return Err(Error::Protocol("empty Workshop access token"));
        }
        Ok(Self(value))
    }

    /// Read the `accessToken` key from a Workshop `global_config.ini` without
    /// persisting or printing it.
    pub fn from_global_config(path: impl AsRef<Path>) -> Result<Self> {
        let source = fs::read_to_string(path)?;
        let value = source
            .lines()
            .find_map(|line| line.strip_prefix("accessToken="))
            .ok_or(Error::Protocol(
                "Workshop global_config.ini has no accessToken",
            ))?;
        Self::new(value.trim())
    }

    fn as_str(&self) -> &str {
        &self.0
    }
}

/// Non-secret fields returned by `GET /work/printer/getPrinters`.  Unknown
/// JSON is retained privately rather than mapped to invented meanings.
#[derive(Clone, Debug)]
pub struct CloudPrinter {
    pub id: Option<String>,
    pub name: Option<String>,
    pub model: Option<String>,
    pub firmware_version: Option<String>,
    pub machine_version: Option<String>,
    /// Cloud `device_status`; its meanings are Cloud API-specific.  In the
    /// current public API implementation, `1` denotes an online device, but
    /// callers should retain the raw code rather than treating it as a
    /// universal enum until a response capture from this printer is recorded.
    pub device_status: Option<u64>,
    /// Cloud `ready_status`, if supplied by this route.
    pub ready_status: Option<u64>,
    /// Cloud `is_printing`.  This is a numeric state code, not a JSON boolean.
    /// The Workshop 4.1.8 SDK also contains the separately-spelled
    /// `is_printring` field, so both are preserved without equating them.
    pub is_printing: Option<u64>,
    /// The server's field is misspelled `is_printring`; preserve that observed
    /// spelling instead of assuming another boolean has the same meaning.
    pub is_printring: Option<bool>,
    pub progress_percent: Option<u64>,
    pub current_layer: Option<u64>,
    pub total_layers: Option<u64>,
    pub ip: Option<String>,
    raw: Value,
}

/// Result of sending a Cloud file to one printer.  A successful transfer does
/// not start a print: `QCloudSDK.dll` names this operation `sendToPrinters`,
/// while Workshop calls its distinct remote-print-start path only afterwards.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CloudFileTransfer {
    /// Stable Cloud-side identifier returned by `/v2/profile/newUploadFile`.
    pub cloud_file_id: u64,
    /// Account-local printer identifier supplied to `/v3/printer/sendFile`.
    pub printer_id: u64,
    /// Source archive; retained solely for the caller's audit trail.
    pub source: PathBuf,
    pub bytes_sent: u64,
}

/// Non-sensitive summary returned by `POST /work/index/files`.
///
/// URLs, object-store keys, hashes and account identifiers are deliberately
/// not retained: they are unnecessary for selecting a file and may be bearer
/// data on some Cloud revisions.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CloudFile {
    pub id: u64,
    pub name: Option<String>,
    pub size: Option<u64>,
    pub status: Option<u64>,
    pub gcode_id: Option<u64>,
    pub extension: Option<String>,
    pub parse_status: Option<u64>,
}

/// Project/job summary returned by `GET /work/project/getProjects`.
/// Numeric state fields are kept raw until observed on the M7 Pro.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CloudProject {
    pub id: u64,
    pub task_id: Option<u64>,
    pub printer_id: Option<u64>,
    pub gcode_id: Option<u64>,
    pub name: Option<String>,
    pub progress_percent: Option<u64>,
    pub print_status: Option<u64>,
    pub status: Option<u64>,
    pub create_time: Option<u64>,
    pub start_time: Option<u64>,
    pub end_time: Option<u64>,
}

/// A Cloud command was accepted for delivery to the printer.  Acceptance is
/// not the same as completion; callers must observe a project/status update.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CloudOrderAck {
    pub message_id: String,
}

/// Result of an automatically resolved project-control command.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CloudProjectOrderAck {
    pub project_id: u64,
    pub message_id: String,
}

impl CloudPrinter {
    /// Original per-printer JSON, useful for adding a field only after it has
    /// been observed on a real machine.  It must not be logged wholesale,
    /// because providers may add credentials to future responses.
    pub fn raw(&self) -> &Value {
        &self.raw
    }

    fn from_response(value: Value) -> Self {
        Self {
            id: field_as_string(
                &value,
                &[
                    "id",
                    "printer_id",
                    "printerID",
                    "printerId",
                    "printer_unicode",
                ],
            ),
            name: field_as_string(&value, &["printer_name", "name", "machine_name"]),
            model: field_as_string(&value, &["model", "model_name", "machine_type"]),
            firmware_version: field_as_string(&value, &["firmware_version"]),
            machine_version: field_as_string(&value, &["machine_version"]),
            device_status: field_as_u64(&value, &["device_status"]),
            ready_status: field_as_u64(&value, &["ready_status"]),
            is_printing: field_as_u64(&value, &["is_printing"]),
            is_printring: value.get("is_printring").and_then(Value::as_bool),
            progress_percent: field_as_u64(&value, &["progress"]),
            current_layer: field_as_u64(&value, &["curr_layer", "current_layer"]),
            total_layers: field_as_u64(&value, &["total_layers"]),
            ip: field_as_string(&value, &["ip", "printer_ip"]),
            raw: value,
        }
    }
}

/// Cloud client authenticated as Photon Workshop (`device_type=pc`).
pub struct CloudClient {
    http: reqwest::Client,
    api_root: String,
    access_token: WorkshopAccessToken,
    // A CLI operation can call several endpoints in quick succession.  Reuse
    // the short-lived token for this client instance instead of exchanging
    // the same OAuth token before every request and triggering rate limits.
    api_token_cache: Mutex<Option<String>>,
}

impl CloudClient {
    pub fn new(access_token: WorkshopAccessToken) -> Result<Self> {
        Self::with_api_root(access_token, CURRENT_CLOUD_API_ROOT)
    }

    pub fn from_global_config(path: impl AsRef<Path>) -> Result<Self> {
        Self::new(WorkshopAccessToken::from_global_config(path)?)
    }

    pub fn with_api_root(
        access_token: WorkshopAccessToken,
        api_root: impl Into<String>,
    ) -> Result<Self> {
        let api_root = api_root.into().trim_end_matches('/').to_owned();
        if !api_root.starts_with("https://") {
            return Err(Error::Protocol("Cloud API root must use HTTPS"));
        }
        let http = reqwest::Client::builder().https_only(true).build()?;
        Ok(Self {
            http,
            api_root,
            access_token,
            api_token_cache: Mutex::new(None),
        })
    }

    /// Exchange the OAuth token for the API token expected in `XX-Token`.
    /// This request is an authentication state change on the Cloud service but
    /// cannot start, alter, or upload a print.
    async fn api_token(&self) -> Result<String> {
        let mut cache = self.api_token_cache.lock().await;
        if let Some(token) = cache.as_ref() {
            return Ok(token.clone());
        }
        let body = json!({
            "device_type": "pc",
            "access_token": self.access_token.as_str(),
        });
        let value = self
            .request_json(
                "POST",
                "/v3/public/loginWithAccessToken",
                None,
                Some(body),
                None,
            )
            .await?;
        let token = value
            .get("data")
            .and_then(|data| data.get("token"))
            .and_then(Value::as_str)
            .ok_or_else(|| cloud_error(&value, "login response had no API token"))?;
        let token = token.to_owned();
        *cache = Some(token.clone());
        Ok(token)
    }

    /// Read the account's printer list.  This is the least invasive Cloud
    /// operation that establishes the printer identity/model and current
    /// advertised state before any command is considered.
    pub async fn list_printers(&self) -> Result<Vec<CloudPrinter>> {
        let token = self.api_token().await?;
        self.list_at_path("/work/printer/getPrinters", &token).await
    }

    /// Read the detailed status list.  The route is the exact literal from
    /// `QCloudSDK.dll` shipped with Workshop 4.1.8.  Its numeric status codes
    /// are deliberately not interpreted here.
    pub async fn list_printer_statuses(&self) -> Result<Vec<CloudPrinter>> {
        let token = self.api_token().await?;
        self.list_at_path("/v2/printer/printersStatus", &token)
            .await
    }

    /// List the account's newest Cloud files without exposing their object
    /// storage URLs or account metadata.
    pub async fn list_cloud_files(&self, limit: u64) -> Result<Vec<CloudFile>> {
        if limit == 0 {
            return Err(Error::Protocol("Cloud file-list limit must be non-zero"));
        }
        let token = self.api_token().await?;
        self.list_cloud_files_with_token(&token, limit).await
    }

    async fn list_cloud_files_with_token(&self, token: &str, limit: u64) -> Result<Vec<CloudFile>> {
        let value = self
            .request_json(
                "POST",
                "/work/index/files",
                None,
                Some(json!({ "page": 1, "limit": limit })),
                Some(token),
            )
            .await?;
        let data = value
            .get("data")
            .and_then(Value::as_array)
            .ok_or_else(|| cloud_error(&value, "Cloud file list had no data array"))?;
        data.iter().map(CloudFile::from_response).collect()
    }

    /// List recent print projects.  Workshop uses a project's `id` as the
    /// `project_id` of pause/resume/stop orders; a file id cannot substitute
    /// for it.
    pub async fn list_projects(&self, limit: u64) -> Result<Vec<CloudProject>> {
        if limit == 0 {
            return Err(Error::Protocol("Cloud project-list limit must be non-zero"));
        }
        let token = self.api_token().await?;
        self.list_projects_with_token(&token, limit).await
    }

    async fn list_projects_with_token(&self, token: &str, limit: u64) -> Result<Vec<CloudProject>> {
        let page = "1";
        let limit = limit.to_string();
        let value = self
            .request_json(
                "GET",
                "/work/project/getProjects",
                Some(&[("page", page), ("limit", limit.as_str())]),
                None,
                Some(token),
            )
            .await?;
        let data = value
            .get("data")
            .and_then(Value::as_array)
            .ok_or_else(|| cloud_error(&value, "Cloud project list had no data array"))?;
        data.iter().map(CloudProject::from_response).collect()
    }

    /// Start a Cloud file on a printer.  This is an active hardware command.
    ///
    /// The envelope and order id are confirmed by Photon Workshop 4.1.8's
    /// `CloudClient::contrlPrinter` and `FUN_1800d8570 @ 0x1800d8570`.
    /// The current server's additional nullable slice fields are corroborated
    /// by the maintained public Anycubic Cloud client.  No account password
    /// is involved once the OAuth token has been obtained.
    pub async fn start_cloud_print(
        &self,
        printer_id: u64,
        cloud_file_id: u64,
    ) -> Result<CloudOrderAck> {
        let token = self.api_token().await?;
        // Refuse an identifier which is not currently present in this
        // account.  This also catches an upload that has not finished being
        // claimed/parsed before a hardware command is sent.
        let file = self
            .list_cloud_files_with_token(&token, 50)
            .await?
            .into_iter()
            .find(|file| file.id == cloud_file_id)
            .ok_or(Error::Protocol(
                "Cloud file id is not present in the current account",
            ))?;
        if file.parse_status == Some(0) {
            return Err(Error::Protocol("Cloud file has not finished parsing"));
        }
        self.send_order_with_token(&token, start_cloud_file_payload(cloud_file_id, printer_id))
            .await
    }

    /// Stop one specific Cloud project.  The project id is mandatory: the
    /// Workshop UI obtains it from the current job before issuing order 4.
    pub async fn stop_cloud_print(
        &self,
        printer_id: u64,
        project_id: u64,
    ) -> Result<CloudOrderAck> {
        self.control_cloud_project(printer_id, project_id, 4).await
    }

    /// Pause one specific Cloud project (`order_id = 2`).  The order id is
    /// confirmed by the Workshop UI call at `accloud.c:106655`; the null data
    /// object is corroborated by the maintained public Cloud client.  Unlike
    /// start/stop, this transition has not been exercised on the test M7 Pro.
    pub async fn pause_cloud_print(
        &self,
        printer_id: u64,
        project_id: u64,
    ) -> Result<CloudOrderAck> {
        self.control_cloud_project(printer_id, project_id, 2).await
    }

    /// Resume one specific Cloud project (`order_id = 3`).  Evidence and
    /// live-validation limits are the same as for [`Self::pause_cloud_print`].
    pub async fn resume_cloud_print(
        &self,
        printer_id: u64,
        project_id: u64,
    ) -> Result<CloudOrderAck> {
        self.control_cloud_project(printer_id, project_id, 3).await
    }

    /// Pause the only known active project for this printer.  No command is
    /// sent when there are zero or multiple candidates, or when Cloud reports
    /// an unknown non-terminal state.
    pub async fn pause_active_cloud_print(&self, printer_id: u64) -> Result<CloudProjectOrderAck> {
        self.control_active_cloud_project(printer_id, 2).await
    }

    /// Resume the only known active project for this printer.  Resolution has
    /// the same fail-closed rules as [`Self::pause_active_cloud_print`].
    pub async fn resume_active_cloud_print(&self, printer_id: u64) -> Result<CloudProjectOrderAck> {
        self.control_active_cloud_project(printer_id, 3).await
    }

    /// Stop the only known active project for this printer.  This is the safe
    /// high-level cancellation entry point for interactive clients.
    pub async fn stop_active_cloud_print(&self, printer_id: u64) -> Result<CloudProjectOrderAck> {
        self.control_active_cloud_project(printer_id, 4).await
    }

    async fn control_active_cloud_project(
        &self,
        printer_id: u64,
        order_id: u64,
    ) -> Result<CloudProjectOrderAck> {
        if !matches!(order_id, 2..=4) {
            return Err(Error::Protocol("unsupported project-control order"));
        }
        let token = self.api_token().await?;
        let projects = self.list_projects_with_token(&token, 50).await?;
        let project_id = single_active_project(&projects, printer_id)?.id;
        let ack = self
            .send_order_with_token(
                &token,
                project_control_payload(order_id, printer_id, project_id),
            )
            .await?;
        Ok(CloudProjectOrderAck {
            project_id,
            message_id: ack.message_id,
        })
    }

    async fn control_cloud_project(
        &self,
        printer_id: u64,
        project_id: u64,
        order_id: u64,
    ) -> Result<CloudOrderAck> {
        if project_id == 0 {
            return Err(Error::Protocol(
                "refusing a project-control order without project_id",
            ));
        }
        if !matches!(order_id, 2..=4) {
            return Err(Error::Protocol("unsupported project-control order"));
        }
        let token = self.api_token().await?;
        self.send_order_with_token(
            &token,
            project_control_payload(order_id, printer_id, project_id),
        )
        .await
    }

    async fn send_order_with_token(&self, token: &str, payload: Value) -> Result<CloudOrderAck> {
        let value = self
            .request_json(
                "POST",
                "/work/operation/sendOrder",
                None,
                Some(payload),
                Some(token),
            )
            .await?;
        let message_id = value
            .get("data")
            .and_then(|data| data.get("msgid"))
            .and_then(Value::as_str)
            .filter(|value| !value.is_empty())
            .ok_or_else(|| cloud_error(&value, "Cloud order response had no msgid"))?;
        Ok(CloudOrderAck {
            message_id: message_id.to_owned(),
        })
    }

    /// Upload a prepared PWSZ archive to Cloud storage and request Cloud-side
    /// delivery to a specific printer.  This implements the sequence observed
    /// in the bundled Cloud SDK and independently corroborated by the current
    /// public Cloud client:
    ///
    /// 1. `lockStorageSpace` returns a one-time `preSignUrl` and lock id;
    /// 2. the bytes are PUT to that URL;
    /// 3. `newUploadFile` claims the uploaded object as a Cloud file;
    /// 4. `sendFile` receives `{file_id, printer_ids}`.
    ///
    /// The method deliberately does **not** submit a remote start order.  It
    /// is suitable only for a job already checked against the target model.
    /// A failed upload releases its just-created reservation and asks Cloud to
    /// discard the unfinished object; a successfully claimed file is kept.
    pub async fn upload_pwsz_to_printer(
        &self,
        printer_id: u64,
        archive: impl AsRef<Path>,
    ) -> Result<CloudFileTransfer> {
        let archive = archive.as_ref();
        if archive.extension().and_then(|value| value.to_str()) != Some("pwsz") {
            return Err(Error::Protocol(
                "Cloud transfer currently accepts a .pwsz archive",
            ));
        }
        let file_name = archive
            .file_name()
            .and_then(|value| value.to_str())
            .ok_or(Error::Protocol("PWSZ archive has no UTF-8 file name"))?;
        let payload = tokio::fs::read(archive).await?;
        if payload.is_empty() {
            return Err(Error::Protocol("refusing to upload an empty PWSZ archive"));
        }
        let bytes_sent = payload.len() as u64;

        let token = self.api_token().await?;
        let reservation = self
            .reserve_cloud_upload(&token, file_name, payload.len())
            .await?;
        let transfer_result: Result<u64> = async {
            self.put_to_presigned_url(&reservation.pre_sign_url, payload)
                .await?;
            let cloud_file_id = self.claim_cloud_upload(&token, reservation.id).await?;
            Ok(cloud_file_id)
        }
        .await;

        let cleanup_object = transfer_result.is_err();
        let unlock_result = self
            .release_cloud_upload(&token, reservation.id, cleanup_object)
            .await;
        let cloud_file_id = transfer_result?;
        unlock_result?;

        self.send_cloud_file_to_printer(&token, cloud_file_id, printer_id)
            .await?;
        Ok(CloudFileTransfer {
            cloud_file_id,
            printer_id,
            source: archive.to_owned(),
            bytes_sent,
        })
    }

    async fn list_at_path(&self, path: &str, token: &str) -> Result<Vec<CloudPrinter>> {
        let value = self
            .request_json("GET", path, None, None, Some(token))
            .await?;
        let data = value
            .get("data")
            .and_then(Value::as_array)
            .ok_or_else(|| cloud_error(&value, "printer list response had no data array"))?;
        Ok(data
            .iter()
            .cloned()
            .map(CloudPrinter::from_response)
            .collect())
    }

    async fn reserve_cloud_upload(
        &self,
        token: &str,
        file_name: &str,
        bytes: usize,
    ) -> Result<CloudUploadReservation> {
        let value = self
            .request_json(
                "POST",
                "/v2/cloud_storage/lockStorageSpace",
                None,
                Some(json!({
                    "size": bytes,
                    "name": file_name,
                    "is_temp_file": 0,
                })),
                Some(token),
            )
            .await?;
        let data = value
            .get("data")
            .ok_or_else(|| cloud_error(&value, "Cloud upload reservation had no data"))?;
        Ok(CloudUploadReservation {
            id: required_u64(data, "id", "Cloud upload reservation had no id")?,
            pre_sign_url: required_string(
                data,
                "preSignUrl",
                "Cloud upload reservation had no preSignUrl",
            )?,
        })
    }

    async fn put_to_presigned_url(&self, url: &str, payload: Vec<u8>) -> Result<()> {
        // The pre-signed URL carries its own authorization.  Workshop's
        // `HttpClient::UploadFile` does not attach Cloud API headers here.
        self.http
            .put(url)
            .body(payload)
            .send()
            .await?
            .error_for_status()?;
        Ok(())
    }

    async fn claim_cloud_upload(&self, token: &str, reservation_id: u64) -> Result<u64> {
        let value = self
            .request_json(
                "POST",
                "/v2/profile/newUploadFile",
                None,
                Some(json!({ "user_lock_space_id": reservation_id })),
                Some(token),
            )
            .await?;
        let data = value
            .get("data")
            .ok_or_else(|| cloud_error(&value, "Cloud upload claim had no data"))?;
        required_u64(data, "id", "Cloud upload claim had no file id")
    }

    async fn release_cloud_upload(
        &self,
        token: &str,
        reservation_id: u64,
        delete_remote_object: bool,
    ) -> Result<()> {
        self.request_json(
            "POST",
            "/v2/cloud_storage/unlockStorageSpace",
            None,
            Some(json!({
                "id": reservation_id,
                "is_delete_cos": u8::from(delete_remote_object),
            })),
            Some(token),
        )
        .await?;
        Ok(())
    }

    async fn send_cloud_file_to_printer(
        &self,
        token: &str,
        cloud_file_id: u64,
        printer_id: u64,
    ) -> Result<()> {
        self.request_json(
            "POST",
            "/v3/printer/sendFile",
            None,
            Some(send_file_payload(cloud_file_id, printer_id)),
            Some(token),
        )
        .await?;
        Ok(())
    }

    async fn request_json(
        &self,
        method: &str,
        path: &str,
        query: Option<&[(&str, &str)]>,
        body: Option<Value>,
        api_token: Option<&str>,
    ) -> Result<Value> {
        let url = format!("{}{path}", self.api_root);
        let mut request = match method {
            "GET" => self.http.get(url),
            "POST" => self.http.post(url),
            _ => return Err(Error::Protocol("unsupported Cloud request method")),
        }
        .headers(self.headers(api_token)?);
        if let Some(query) = query {
            request = request.query(query);
        }
        if let Some(body) = body {
            request = request.json(&body);
        }
        let response = request.send().await?.error_for_status()?;
        Ok(response.json().await?)
    }

    fn headers(&self, api_token: Option<&str>) -> Result<HeaderMap> {
        let timestamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_millis()
            .to_string();
        // `getNonce` renders 16 random bytes as a canonical 36-byte UUID.
        let nonce = Uuid::new_v4().hyphenated().to_string();
        let signature_source = format!(
            "{WORKSHOP_APP_ID}{timestamp}{CLIENT_VERSION}{WORKSHOP_APP_SECRET}{nonce}{WORKSHOP_APP_ID}"
        );
        let signature = format!("{:x}", Md5::digest(signature_source.as_bytes()));

        let mut headers = HeaderMap::new();
        insert_header(&mut headers, "XX-Device-Type", "pc")?;
        insert_header(&mut headers, "XX-IS-CN", "1")?;
        insert_header(&mut headers, "XX-Nonce", &nonce)?;
        insert_header(&mut headers, "XX-Signature", &signature)?;
        insert_header(&mut headers, "XX-Timestamp", &timestamp)?;
        insert_header(&mut headers, "XX-Version", CLIENT_VERSION)?;
        insert_header(&mut headers, "XX-LANGUAGE", "US")?;
        headers.insert(CONTENT_TYPE, HeaderValue::from_static("application/json"));
        if let Some(api_token) = api_token {
            insert_header(&mut headers, "XX-Token", api_token)?;
        }
        Ok(headers)
    }
}

impl CloudFile {
    fn from_response(value: &Value) -> Result<Self> {
        Ok(Self {
            id: required_u64(value, "id", "Cloud file had no id")?,
            name: field_as_string(value, &["old_filename", "filename", "name"]),
            size: field_as_u64(value, &["size", "file_size"]),
            status: field_as_u64(value, &["status"]),
            gcode_id: field_as_u64(value, &["gcode_id"]),
            extension: field_as_string(value, &["file_extension"]),
            parse_status: field_as_u64(value, &["is_parse", "slice_status"]),
        })
    }
}

impl CloudProject {
    fn from_response(value: &Value) -> Result<Self> {
        Ok(Self {
            id: required_u64(value, "id", "Cloud project had no id")?,
            task_id: field_as_u64(value, &["taskid", "task_id"]),
            printer_id: field_as_u64(value, &["printer_id"]),
            gcode_id: field_as_u64(value, &["gcode_id"]),
            name: field_as_string(value, &["gcode_name", "name", "post_title"]),
            progress_percent: field_as_u64(value, &["progress"]),
            print_status: field_as_u64(value, &["print_status"]),
            status: field_as_u64(value, &["status"]),
            create_time: field_as_u64(value, &["create_time"]),
            start_time: field_as_u64(value, &["start_time"]),
            end_time: field_as_u64(value, &["end_time"]),
        })
    }
}

struct CloudUploadReservation {
    id: u64,
    // Do not derive Debug: this is a bearer URL for an individual upload.
    pre_sign_url: String,
}

fn insert_header(headers: &mut HeaderMap, name: &'static str, value: &str) -> Result<()> {
    let value =
        HeaderValue::from_str(value).map_err(|_| Error::Protocol("invalid Cloud header"))?;
    headers.insert(name, value);
    Ok(())
}

fn cloud_error(response: &Value, fallback: &str) -> Error {
    let message = response
        .get("msg")
        .and_then(Value::as_str)
        .unwrap_or(fallback);
    Error::Cloud(message.to_owned())
}

fn field_as_string(value: &Value, names: &[&str]) -> Option<String> {
    names.iter().find_map(|name| match value.get(*name) {
        Some(Value::String(value)) if !value.is_empty() => Some(value.to_owned()),
        Some(Value::Number(value)) => Some(value.to_string()),
        _ => None,
    })
}

fn field_as_u64(value: &Value, names: &[&str]) -> Option<u64> {
    names.iter().find_map(|name| match value.get(*name) {
        Some(Value::Number(value)) => value.as_u64(),
        Some(Value::String(value)) => value.parse().ok(),
        _ => None,
    })
}

fn required_u64(value: &Value, field: &str, fallback: &'static str) -> Result<u64> {
    field_as_u64(value, &[field]).ok_or(Error::Protocol(fallback))
}

fn required_string(value: &Value, field: &str, fallback: &'static str) -> Result<String> {
    field_as_string(value, &[field]).ok_or(Error::Protocol(fallback))
}

fn send_file_payload(cloud_file_id: u64, printer_id: u64) -> Value {
    json!({
        "file_id": cloud_file_id,
        "printer_ids": [printer_id],
    })
}

fn start_cloud_file_payload(cloud_file_id: u64, printer_id: u64) -> Value {
    json!({
        "order_id": 1,
        "printer_id": printer_id,
        "project_id": 0,
        "data": {
            "filetype": 0,
            "file_key": "",
            "file_name": "",
            "task_settings": {
                "ai_detect": 0,
                "camera_timelapse": 0,
            },
            "file_id": cloud_file_id,
            "hollow_param": null,
            "is_delete_file": 0,
            "matrix": "",
            "project_type": 1,
            "punching_param": null,
            "slice_param": null,
            "slice_size": null,
            "template_id": 0,
        },
        "ams_info": null,
        "settings": null,
    })
}

fn project_control_payload(order_id: u64, printer_id: u64, project_id: u64) -> Value {
    json!({
        "order_id": order_id,
        "printer_id": printer_id,
        "project_id": project_id,
        "data": null,
        "ams_info": null,
        "settings": null,
    })
}

fn single_active_project(projects: &[CloudProject], printer_id: u64) -> Result<&CloudProject> {
    let mut candidates = Vec::new();
    let mut unknown_non_terminal = false;

    for project in projects
        .iter()
        .filter(|project| project.printer_id == Some(printer_id))
    {
        if project.end_time.is_some_and(|end| end != 0)
            || matches!(project.print_status, Some(2 | 3))
        {
            continue;
        }
        if matches!(project.print_status, Some(1 | 4 | 5 | 6 | 7 | 9)) {
            candidates.push(project);
        } else {
            unknown_non_terminal = true;
        }
    }

    if unknown_non_terminal {
        return Err(Error::Protocol(
            "refusing automatic control: printer has an unknown non-terminal project state",
        ));
    }
    match candidates.as_slice() {
        [project] => Ok(*project),
        [] => Err(Error::Protocol("printer has no active Cloud project")),
        _ => Err(Error::Protocol(
            "refusing automatic control: printer has multiple active Cloud projects",
        )),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_access_token_without_accepting_an_empty_value() {
        let token = WorkshopAccessToken::new("token").unwrap();
        assert_eq!(token.as_str(), "token");
        assert!(WorkshopAccessToken::new("  ").is_err());
    }

    #[test]
    fn retains_only_observed_summary_fields() {
        let printer = CloudPrinter::from_response(json!({
            "id": 7,
            "printer_name": "M7 Pro",
            "firmware_version": "4.0.8.6",
            "device_status": 1,
            "ready_status": "0",
            "is_printing": 1,
            "is_printring": false,
            "curr_layer": 2,
            "total_layers": "50"
        }));
        assert_eq!(printer.id.as_deref(), Some("7"));
        assert_eq!(printer.name.as_deref(), Some("M7 Pro"));
        assert_eq!(printer.firmware_version.as_deref(), Some("4.0.8.6"));
        assert_eq!(printer.device_status, Some(1));
        assert_eq!(printer.ready_status, Some(0));
        assert_eq!(printer.is_printing, Some(1));
        assert_eq!(printer.is_printring, Some(false));
        assert_eq!(printer.current_layer, Some(2));
        assert_eq!(printer.total_layers, Some(50));
    }

    #[test]
    fn accepts_numeric_or_string_cloud_identifiers() {
        let object = json!({"number": 42, "string": "7", "not_number": "abc"});
        assert_eq!(required_u64(&object, "number", "missing").unwrap(), 42);
        assert_eq!(required_u64(&object, "string", "missing").unwrap(), 7);
        assert!(required_u64(&object, "not_number", "missing").is_err());
    }

    #[test]
    fn send_file_payload_uses_the_sdk_field_names() {
        assert_eq!(
            send_file_payload(123, 456),
            json!({"file_id": 123, "printer_ids": [456]})
        );
    }

    #[test]
    fn parses_safe_cloud_file_and_project_summaries() {
        let file = CloudFile::from_response(&json!({
            "id": "86140277",
            "old_filename": "example.pwsz",
            "size": 3354087,
            "gcode_id": 123,
            "url": "must-not-be-retained"
        }))
        .unwrap();
        assert_eq!(file.id, 86140277);
        assert_eq!(file.name.as_deref(), Some("example.pwsz"));
        assert_eq!(file.size, Some(3354087));

        let project = CloudProject::from_response(&json!({
            "id": 42,
            "taskid": "43",
            "printer_id": "609126",
            "gcode_name": "example.pwsz",
            "progress": 7
        }))
        .unwrap();
        assert_eq!(project.id, 42);
        assert_eq!(project.task_id, Some(43));
        assert_eq!(project.printer_id, Some(609126));
        assert_eq!(project.progress_percent, Some(7));
    }

    #[test]
    fn start_and_stop_payloads_keep_file_and_project_ids_distinct() {
        assert_eq!(
            start_cloud_file_payload(86140277, 609126),
            json!({
                "order_id": 1,
                "printer_id": 609126,
                "project_id": 0,
                "data": {
                    "filetype": 0,
                    "file_key": "",
                    "file_name": "",
                    "task_settings": {"ai_detect": 0, "camera_timelapse": 0},
                    "file_id": 86140277,
                    "hollow_param": null,
                    "is_delete_file": 0,
                    "matrix": "",
                    "project_type": 1,
                    "punching_param": null,
                    "slice_param": null,
                    "slice_size": null,
                    "template_id": 0
                },
                "ams_info": null,
                "settings": null
            })
        );
        assert_eq!(
            project_control_payload(4, 609126, 42),
            json!({
                "order_id": 4,
                "printer_id": 609126,
                "project_id": 42,
                "data": null,
                "ams_info": null,
                "settings": null
            })
        );
        assert_eq!(project_control_payload(2, 609126, 42)["order_id"], 2);
        assert_eq!(project_control_payload(3, 609126, 42)["order_id"], 3);
    }

    #[test]
    fn automatic_control_requires_exactly_one_known_active_project() {
        let active = CloudProject::from_response(&json!({
            "id": 10,
            "printer_id": 7,
            "print_status": 1,
            "end_time": 0
        }))
        .unwrap();
        let cancelled = CloudProject::from_response(&json!({
            "id": 9,
            "printer_id": 7,
            "print_status": 3,
            "end_time": 123
        }))
        .unwrap();
        assert_eq!(
            single_active_project(&[active.clone(), cancelled], 7)
                .unwrap()
                .id,
            10
        );
        assert!(single_active_project(&[], 7).is_err());
        assert!(single_active_project(&[active.clone(), active], 7).is_err());

        let unknown = CloudProject::from_response(&json!({
            "id": 11,
            "printer_id": 7,
            "print_status": 8,
            "end_time": 0
        }))
        .unwrap();
        assert!(single_active_project(&[unknown], 7).is_err());
    }
}
