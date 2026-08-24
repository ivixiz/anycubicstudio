//! MQTT transport for an already authenticated printer.

use std::{io::Cursor, sync::Arc, time::Duration};

use rumqttc::{
    AsyncClient, Event, EventLoop, MqttOptions, Packet, QoS, TlsConfiguration, Transport,
};
use tokio::{sync::broadcast, task::JoinHandle};

use crate::{
    error::{Error, Result},
    protocol::{LanInfo, ReportEnvelope},
};

use rumqttc::tokio_rustls::rustls::{
    self, DigitallySignedStruct,
    client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier},
    crypto::{WebPkiSupportedAlgorithms, verify_tls12_signature, verify_tls13_signature},
    pki_types::{CertificateDer, ServerName, UnixTime},
};

/// TLS material supplied by the encrypted LAN response.  Photon Workshop loads
/// the same fields into `QSslConfiguration` in `FUN_1800ca230`.
#[derive(Clone, Debug, Default)]
pub struct TlsCredentials {
    pub ca_certificate_pem: Option<Vec<u8>>,
    pub client_certificate_pem: Option<Vec<u8>>,
    pub client_private_key_pem: Option<Vec<u8>>,
}

/// The Qt client invokes `QSslConfiguration::setPeerVerifyMode(0)` in
/// `FUN_1800ca230`, so compatibility needs certificate verification disabled.
/// On a trusted, isolated LAN that is a deliberate compatibility choice—not a
/// statement that it is cryptographically safe. Prefer [`TlsVerification::Strict`]
/// if a capture proves that the supplied CA and server identity validate.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum TlsVerification {
    #[default]
    WorkshopCompatible,
    Strict,
}

impl From<&LanInfo> for TlsCredentials {
    fn from(info: &LanInfo) -> Self {
        Self {
            ca_certificate_pem: info.cacrt.as_ref().map(|text| text.as_bytes().to_vec()),
            client_certificate_pem: info.devicecrt.as_ref().map(|text| text.as_bytes().to_vec()),
            client_private_key_pem: info.devicepk.as_ref().map(|text| text.as_bytes().to_vec()),
        }
    }
}

/// A connected transport owns an event-loop task and offers a broadcast stream
/// of lossless-as-possible raw report envelopes.  Slow consumers receive a
/// `Lagged` indication and can issue fresh queries.
#[derive(Clone)]
pub struct MqttTransport {
    client: AsyncClient,
    reports: broadcast::Sender<ReportEnvelope>,
}

impl MqttTransport {
    pub async fn connect(info: &LanInfo) -> Result<Self> {
        Self::connect_with_tls_verification(info, TlsVerification::WorkshopCompatible).await
    }

    pub async fn connect_with_tls_verification(
        info: &LanInfo,
        verification: TlsVerification,
    ) -> Result<Self> {
        let (host, port) = info.broker_address()?;
        let mut options = MqttOptions::new(&info.client_id, host, port);
        options.set_credentials(&info.username, &info.password);
        options.set_keep_alive(Duration::from_secs(60));
        options.set_clean_session(true);
        options.set_transport(tls_transport(TlsCredentials::from(info), verification)?);

        let (client, event_loop) = AsyncClient::new(options, 32);
        let (reports, _) = broadcast::channel(128);
        spawn_event_loop(event_loop, reports.clone());

        // `onConnected` in `FUN_1800ca830` subscribes at QoS 0 before issuing
        // status/lanInfo/print queries.  Awaiting this request ensures it is
        // serialised by rumqttc before the caller can publish a query.
        client
            .subscribe(info.report_topic_filter(), QoS::AtMostOnce)
            .await?;
        Ok(Self { client, reports })
    }

    pub fn subscribe_reports(&self) -> broadcast::Receiver<ReportEnvelope> {
        self.reports.subscribe()
    }

    pub async fn publish_json(&self, topic: String, payload: Vec<u8>) -> Result<()> {
        self.client
            .publish(topic, QoS::AtMostOnce, false, payload)
            .await?;
        Ok(())
    }

    pub async fn disconnect(&self) -> Result<()> {
        self.client.disconnect().await?;
        Ok(())
    }
}

fn tls_transport(credentials: TlsCredentials, verification: TlsVerification) -> Result<Transport> {
    if verification == TlsVerification::WorkshopCompatible {
        return insecure_mtls_transport(credentials);
    }
    let client_auth = match (
        credentials.client_certificate_pem,
        credentials.client_private_key_pem,
    ) {
        (Some(certificate), Some(key)) => Some((certificate, key)),
        (None, None) => None,
        _ => {
            return Err(Error::Protocol(
                "incomplete MQTT client certificate credentials",
            ));
        }
    };
    let ca = credentials.ca_certificate_pem.unwrap_or_default();
    Ok(Transport::tls_with_config(TlsConfiguration::Simple {
        ca,
        alpn: None,
        client_auth,
    }))
}

/// A verifier that preserves handshake signatures but deliberately accepts the
/// server certificate. This is constrained to the explicit compatibility mode.
#[derive(Debug)]
struct AcceptAnyServerCertificate {
    algorithms: WebPkiSupportedAlgorithms,
}

impl ServerCertVerifier for AcceptAnyServerCertificate {
    fn verify_server_cert(
        &self,
        _end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> std::result::Result<ServerCertVerified, rustls::Error> {
        Ok(ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> std::result::Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls12_signature(message, cert, dss, &self.algorithms)
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> std::result::Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls13_signature(message, cert, dss, &self.algorithms)
    }

    fn supported_verify_schemes(&self) -> Vec<rustls::SignatureScheme> {
        self.algorithms.supported_schemes()
    }
}

fn insecure_mtls_transport(credentials: TlsCredentials) -> Result<Transport> {
    let certificate = credentials
        .client_certificate_pem
        .ok_or(Error::Protocol("LAN response has no devicecrt"))?;
    let private_key = credentials
        .client_private_key_pem
        .ok_or(Error::Protocol("LAN response has no devicepk"))?;
    let certificates = rustls_pemfile::certs(&mut Cursor::new(certificate))
        .collect::<std::result::Result<Vec<_>, _>>()
        .map_err(|_| Error::Protocol("invalid devicecrt PEM"))?;
    let key = rustls_pemfile::private_key(&mut Cursor::new(private_key))
        .map_err(|_| Error::Protocol("invalid devicepk PEM"))?
        .ok_or(Error::Protocol("devicepk PEM has no private key"))?;
    let provider = rustls::crypto::ring::default_provider();
    let verifier = Arc::new(AcceptAnyServerCertificate {
        algorithms: provider.signature_verification_algorithms,
    });
    let config = rustls::ClientConfig::builder_with_provider(Arc::new(provider))
        .with_safe_default_protocol_versions()
        .map_err(|_| Error::Protocol("no supported TLS protocol versions"))?
        .dangerous()
        .with_custom_certificate_verifier(verifier)
        .with_client_auth_cert(certificates, key)
        .map_err(|_| Error::Protocol("invalid MQTT client certificate credentials"))?;
    Ok(Transport::tls_with_config(TlsConfiguration::Rustls(
        Arc::new(config),
    )))
}

fn spawn_event_loop(
    mut event_loop: EventLoop,
    reports: broadcast::Sender<ReportEnvelope>,
) -> JoinHandle<()> {
    tokio::spawn(async move {
        loop {
            match event_loop.poll().await {
                Ok(Event::Incoming(Packet::Publish(publish))) => {
                    if let Ok(report) = serde_json::from_slice::<ReportEnvelope>(&publish.payload) {
                        let _ = reports.send(report);
                    }
                }
                Ok(_) => {}
                // A disconnected broadcast source is how consumers learn that
                // their current monitor stream is no longer valid. Reconnect
                // policy is intentionally a higher-level application choice.
                Err(_) => break,
            }
        }
    })
}
