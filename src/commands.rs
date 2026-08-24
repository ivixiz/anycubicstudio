//! Only commands whose `type`, `action`, and payload shape occur in
//! `accloud.c` are exposed as convenience constructors.

use serde_json::{Value, json};

use crate::protocol::{CommandEnvelope, start_print_data};

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
