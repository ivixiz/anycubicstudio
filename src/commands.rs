//! Only commands whose `type`, `action`, and payload shape occur in
//! `accloud.c` are exposed as convenience constructors.

use serde_json::{Value, json};

use crate::error::{Error, Result};
use crate::protocol::{CommandEnvelope, start_print_data};

/// Exposure modes accepted by Workshop's dedicated LCD-test command.
/// Their visual meaning is deliberately left unnamed: the decompiled desktop
/// client proves the raw values 1, 2 and 4, but not stable human-readable
/// names across printer firmware versions.
pub const BUILTIN_EXPOSURE_TYPES: [u8; 3] = [1, 2, 4];

/// Safety policy of this client, not a firmware/protocol limit.
pub const CLIENT_MAX_EXPOSURE_SECONDS: u32 = 300;

pub(crate) fn validate_builtin_exposure(exposure_type: u8, duration_s: u32) -> Result<()> {
    if !BUILTIN_EXPOSURE_TYPES.contains(&exposure_type) {
        return Err(Error::Protocol(
            "exposure type must be one of the confirmed raw values 1, 2 or 4",
        ));
    }
    if !(1..=CLIENT_MAX_EXPOSURE_SECONDS).contains(&duration_s) {
        return Err(Error::Protocol(
            "exposure duration must be 1..=300 seconds (client safety limit)",
        ));
    }
    Ok(())
}

#[derive(Clone, Debug)]
pub struct Command {
    pub message_type: &'static str,
    pub action: &'static str,
    pub data: Value,
}

impl Command {
    pub fn envelope(&self) -> CommandEnvelope<'_> {
        CommandEnvelope::new(self.message_type, self.action, self.data.clone())
    }

    pub fn status_query() -> Self {
        Self::query("status")
    }
    pub fn lan_info_query() -> Self {
        Self::query("lanInfo")
    }
    pub fn print_query() -> Self {
        Self::query("print")
    }
    pub fn peripherie_query() -> Self {
        Self::query("peripherie")
    }
    pub fn light_query() -> Self {
        Self::query("light")
    }
    pub fn auto_operation_status() -> Self {
        Self {
            message_type: "autoOperation",
            action: "getStatus",
            data: Value::Null,
        }
    }
    pub fn signal_strength_query() -> Self {
        Self {
            message_type: "properties",
            action: "read",
            data: json!(["signal_strength"]),
        }
    }
    pub fn connection_properties_query() -> Self {
        Self {
            message_type: "properties",
            action: "read",
            data: json!(["connect"]),
        }
    }

    pub fn pause() -> Self {
        Self {
            message_type: "print",
            action: "pause",
            data: Value::Null,
        }
    }
    pub fn resume() -> Self {
        Self {
            message_type: "print",
            action: "resume",
            data: Value::Null,
        }
    }
    pub fn stop() -> Self {
        Self {
            message_type: "print",
            action: "stop",
            data: Value::Null,
        }
    }

    pub fn light(status: bool) -> Self {
        Self {
            message_type: "light",
            action: "control",
            data: json!({"type": 3, "status": u8::from(status)}),
        }
    }

    /// Start the printer's built-in LCD exposure test without entering the
    /// normal print path. Confirmed by `FUN_1800e1720 @ 0x1800e1720` and
    /// `FUN_1800d5ee0` in Workshop 4.1.8.
    ///
    /// This command selects one of three firmware-provided test patterns. It
    /// cannot carry a custom bitmap, PWSZ layer or Gerber image.
    pub fn exposure_start(exposure_type: u8, duration_s: u32) -> Result<Self> {
        validate_builtin_exposure(exposure_type, duration_s)?;
        Ok(Self {
            message_type: "exposure",
            action: "start",
            data: json!({
                "exposure_type": exposure_type,
                "last_time": duration_s,
            }),
        })
    }

    /// Cancel the dedicated LCD exposure test. The scalar `0` payload is the
    /// exact cancel branch serialized by `FUN_1800e1720 @ 0x1800e1720`.
    pub fn exposure_cancel() -> Self {
        Self {
            message_type: "exposure",
            action: "cancel",
            data: json!(0),
        }
    }

    /// Start data has confirmed field names, but the numeric/string semantics
    /// of `task_mode`, `filetype`, and `project_type` are not yet established.
    /// They are caller-provided rather than guessed.
    #[allow(clippy::too_many_arguments)]
    pub fn start_print(
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
    ) -> Self {
        Self {
            message_type: "print",
            action: "start",
            data: start_print_data(
                taskid,
                task_mode,
                url,
                filename,
                filesize,
                md5,
                filetype,
                filepath,
                project_type,
                task_settings,
            ),
        }
    }

    fn query(message_type: &'static str) -> Self {
        Self {
            message_type,
            action: "query",
            data: Value::Null,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn exposure_commands_match_confirmed_mqtt_shape() {
        let start = Command::exposure_start(2, 60).unwrap();
        assert_eq!(start.message_type, "exposure");
        assert_eq!(start.action, "start");
        assert_eq!(start.data, json!({"exposure_type": 2, "last_time": 60}));

        let cancel = Command::exposure_cancel();
        assert_eq!(cancel.message_type, "exposure");
        assert_eq!(cancel.action, "cancel");
        assert_eq!(cancel.data, json!(0));
    }

    #[test]
    fn exposure_command_rejects_unknown_or_unsafe_values() {
        assert!(Command::exposure_start(3, 60).is_err());
        assert!(Command::exposure_start(1, 0).is_err());
        assert!(Command::exposure_start(4, 301).is_err());
    }
}
