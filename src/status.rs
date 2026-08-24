//! Conservative helpers for reports observed in `ACLanClient::onReceived`.

use serde_json::Value;

use crate::protocol::ReportEnvelope;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PrintState {
    Preheating,
    Stirring,
    Copying,
    Printing,
    Waiting,
    Finished,
    Pausing,
    Paused,
    Resuming,
    Resumed,
    Stopping,
    Stopped,
    Failed,
    Other,
}

impl PrintState {
    pub fn parse(value: &str) -> Self {
        match value {
            "preheating" => Self::Preheating,
            "stirring" => Self::Stirring,
            "copying" => Self::Copying,
            "printing" => Self::Printing,
            "waiting" => Self::Waiting,
            "finished" => Self::Finished,
            "pausing" => Self::Pausing,
            "paused" => Self::Paused,
            "resuming" => Self::Resuming,
            "resumed" => Self::Resumed,
            "stopping" => Self::Stopping,
            // The Workshop firmware spells this on-wire value `stoped`.
            "stoped" => Self::Stopped,
            "failed" => Self::Failed,
            _ => Self::Other,
        }
    }
}

#[derive(Clone, Debug, Default)]
pub struct PrintProgress {
    pub state: Option<PrintState>,
    pub progress_percent: Option<u64>,
    pub task_id: Option<String>,
}

impl PrintProgress {
    pub fn from_report(report: &ReportEnvelope) -> Option<Self> {
        if report.message_type.as_deref()? != "print" {
            return None;
        }
        let data = &report.data;
        Some(Self {
            state: report.state.as_deref().map(PrintState::parse),
            progress_percent: data.get("progress").and_then(Value::as_u64),
            task_id: data
                .get("taskid")
                .and_then(Value::as_str)
                .map(ToOwned::to_owned),
        })
    }
}

/// Selected telemetry keys read by `FUN_1800cf2b0`; all other report data is
/// intentionally left in the original JSON envelope.
#[derive(Clone, Debug, Default)]
pub struct AutoOperationTelemetry {
    pub total_space: Option<f64>,
    pub free_space: Option<f64>,
    pub resin_temp: Option<f64>,
    pub signal_strength: Option<i64>,
    pub layers: Option<i64>,
    pub times: Option<i64>,
}

impl AutoOperationTelemetry {
    pub fn from_report(report: &ReportEnvelope) -> Option<Self> {
        if report.message_type.as_deref()? != "autoOperation" {
            return None;
        }
        let data = &report.data;
        Some(Self {
            total_space: data.get("total_space").and_then(Value::as_f64),
            free_space: data.get("free_space").and_then(Value::as_f64),
            resin_temp: data.get("resin_temp").and_then(Value::as_f64),
            signal_strength: data.get("signal_strength").and_then(Value::as_i64),
            layers: data.get("layers").and_then(Value::as_i64),
            times: data.get("times").and_then(Value::as_i64),
        })
    }
}
