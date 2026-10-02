//! High-level printer facade built only from verified protocol primitives.

use std::time::Duration;

use serde_json::to_vec;
use tokio::{sync::broadcast, time::timeout};

use crate::{
    commands::Command,
    error::{Error, Result},
    protocol::{LanInfo, ReportEnvelope},
    transport::MqttTransport,
};

#[derive(Clone)]
pub struct Printer {
    info: LanInfo,
    transport: MqttTransport,
}

impl Printer {
    pub async fn connect(info: LanInfo) -> Result<Self> {
        let transport = MqttTransport::connect(&info).await?;
        let printer = Self { info, transport };
        // Same safe startup queries issued by `ACLanClient::onConnected`.
        printer.query(Command::status_query()).await?;
        printer.query(Command::lan_info_query()).await?;
        printer.query(Command::print_query()).await?;
        Ok(printer)
    }

    pub fn info(&self) -> &LanInfo {
        &self.info
    }

    pub fn reports(&self) -> broadcast::Receiver<ReportEnvelope> {
        self.transport.subscribe_reports()
    }

    pub async fn query(&self, command: Command) -> Result<String> {
        self.send(command).await
    }

    pub async fn pause(&self) -> Result<String> {
        self.send(Command::pause()).await
    }
    pub async fn resume(&self) -> Result<String> {
        self.send(Command::resume()).await
    }
    pub async fn stop(&self) -> Result<String> {
        self.send(Command::stop()).await
    }

    /// Run one of the printer firmware's built-in LCD test patterns. Unlike
    /// `print/start`, the command itself contains no Z-axis operation.
    pub async fn start_exposure(&self, exposure_type: u8, duration_s: u32) -> Result<String> {
        self.send(Command::exposure_start(exposure_type, duration_s)?)
            .await
    }

    /// Cancel a dedicated LCD exposure test.
    pub async fn cancel_exposure(&self) -> Result<String> {
        self.send(Command::exposure_cancel()).await
    }

    pub async fn send(&self, command: Command) -> Result<String> {
        let envelope = command.envelope();
        let msgid = envelope.msgid.clone();
        self.transport
            .publish_json(
                self.info.command_topic(command.message_type),
                to_vec(&envelope)?,
            )
            .await?;
        Ok(msgid)
    }

    /// Wait for a report which echoes a particular `msgid`.  Not all old
    /// firmware reports echo it, therefore a timeout means “unconfirmed”, not
    /// necessarily that a command failed.
    pub async fn wait_for_msgid(&self, msgid: &str, limit: Duration) -> Result<ReportEnvelope> {
        let mut reports = self.reports();
        timeout(limit, async {
            loop {
                match reports.recv().await {
                    Ok(report) if report.msgid.as_deref() == Some(msgid) => return Ok(report),
                    Ok(_) | Err(broadcast::error::RecvError::Lagged(_)) => {}
                    Err(broadcast::error::RecvError::Closed) => return Err(Error::Disconnected),
                }
            }
        })
        .await
        .map_err(|_| Error::Timeout("matching printer report"))?
    }

    pub async fn disconnect(&self) -> Result<()> {
        self.transport.disconnect().await
    }
}
