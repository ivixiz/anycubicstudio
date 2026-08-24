//! Foundation client for the LAN protocol used by Anycubic Photon Workshop 4.1.8.
//!
//! This crate separates discovery/bootstrap from MQTT control.  Do not persist
//! a decrypted [`protocol::LanInfo`]: it contains live credentials.

pub mod cloud;
pub mod commands;
pub mod discovery;
pub mod error;
pub mod printer;
pub mod protocol;
pub mod pwsz;
pub mod slicer;
pub mod status;
pub mod transport;
pub mod upload;

pub use error::{Error, Result};
pub use printer::Printer;
pub use protocol::{LanInfo, LocationInfo, ReportEnvelope};
