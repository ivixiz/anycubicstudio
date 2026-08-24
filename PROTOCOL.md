# Anycubic Photon Workshop 4.1.8: LAN protocol

The labels used below are deliberate:

- **Confirmed** — directly visible in the supplied `accloud.c`.
- **Inference** — follows data flow in that binary.
- **External** — corroborates architecture but is not evidence for identical
  Photon firmware behaviour.
- **Live-validated** — a request and its resulting state transition were
  observed on the Photon Mono M7 Pro used for this analysis.
- **Unknown** — is not implemented as a guessed constant or enum.

No account token, certificate, or decrypted broker credential is included.
The LAN response contains live credentials and must stay in memory only.

## Supplement: `QCloudSDK.dll` and `mqtt_client.dll`

The newly supplied files clarify the separation between the two application
paths, but do not change the direct-LAN protocol above.

`QCloudSDK.dll` is an Anycubic **cloud** client, not a generic third-party
`qcloud` library: its embedded source paths name
`cloud_connect_plugin/cloud_sdk_cpp/detail/impl/cloud_client_{user,printer,file}.cpp`.
It contains `CloudClient::loginImpl`, `printerList`, `printerStatus`,
`addPrinter`, `remotePrint`, `getSlicerConfig`, and `slicerConfigPreSign`.
The login request builds the JSON keys `device_type`, `access_token`, and
`pc_id` (visible near `0x180007568`). This is **confirmed** cloud-account
functionality and explains why a valid login/time can be required for the
Workshop cloud view. It is not evidence that the direct `accloud.dll` LAN
bootstrap needs the user's email/password.

`mqtt_client.dll` is the lower-level bundled MQTT implementation. It has
named packet functions `mqtt_connect_to_broker` (`0x18000196a`),
`mqtt_publish` (`0x180003fee`), `mqtt_subscribe` (`0x1800058bc`), and
`mqtt_unsubscribe` (`0x180007405`). Its bundled Paho-like implementation also
contains `WebSocket.c`, including the WebSocket path `/mqtt` and
`Sec-WebSocket-Protocol: mqtt`. This establishes
only a **library capability**. `ACLanClient` still passes broker host/port and
TLS credentials to QMQTT, and no caller proves that the Photon LAN broker uses
WebSocket rather than raw TLS MQTT. The Rust client therefore keeps raw MQTT
as the implemented, code-supported transport and does not speculate a WS URL.

## Cloud account transport and PWSZ delivery

This is a path separate from direct LAN MQTT. It is needed for the M7 Pro in
this test because its SSDP/LAN bootstrap was not advertised, while the
account's Cloud API did list the printer.

### Authentication

**Confirmed from `QCloudSDK.dll`.** `CloudClient::getNonce` at
`0x180135d60` generates 16 random bytes and formats a UUID. `getSignature` at
`0x180136330` takes the MD5 of the concatenation:

```text
APP_ID || timestamp_ms || "V3.0.0" || APP_SECRET || UUID_nonce || APP_ID
```

The constants embedded in the SDK are `APP_ID =
f9b3528877c94d5c9c5af32245db46ef` and `APP_SECRET =
0cf75926606049a3937f56b0373b99fb`; the client uses the `XX-*` headers
`Device-Type`, `IS-CN`, `Nonce`, `Signature`, `Timestamp`, `Version`,
`LANGUAGE`, and then `Token` after login. OAuth access tokens from Workshop's
Casdoor login are exchanged with:

```http
POST /v3/public/loginWithAccessToken
Content-Type: application/json

{"device_type":"pc","access_token":"…"}
```

The original 4.1.8 `environment.ini` decodes to the historical root
`https://cloud-platform.anycubiccloud.com/p/p/workbench/api`. That hostname
was no longer resolvable during this work. The current root
`https://cloud-universe.anycubic.com/p/p/workbench/api` accepted the saved
Workshop OAuth token and returned the printer list. This is a live observation,
not a claim that a legacy installer contained the new hostname.

`APP_SECRET` is a misleading name in a desktop application. It is a fixed,
shared **Photon Workshop application credential**, present verbatim at
`QCloudSDK.c:534056` inside `FUN_180136330 @ 0x180136330` (and at a second
inlined copy near `QCloudSDK.c:101993`). It is not an account secret, is not
derived from an email/password, and is the same for every user of this SDK
build. The account-bearing values are instead the OAuth `accessToken`, the
short-lived `XX-Token`, and any pre-signed upload URL. Those values must never
be committed or logged.

**Confirmed from `accloud.c`.** Workshop opens
`https://uc.makeronline.com/login/oauth/authorize` with client id
`25ec844d72180b556acc`, response type `code`, scope `read`, application
`ac_slicer_lcd`, and redirect URI `acpws://login` (constructed around
`accloud.c:220091-220117`). After the redirect, code flow reaches
`ACCloudNetRequest::getAccessTokenByCode`, `FUN_180141da0 @ 0x180141da0`,
which calls `NetworkRequest::AccessToken`. The user's password is entered into
the Anycubic/Casdoor web page; it is not an input to the Cloud API signature
and should not be collected by this Rust library. A reusable client should
therefore launch the system browser, receive the custom-scheme callback, and
store only the resulting OAuth token in OS credential storage.

The code exchange is also present in the supplied DLL. The exported wrapper
`NetworkRequest::AccessToken @ 0x18000bc5d` dispatches to
`CloudClient::accessToken`, recovered as `FUN_180165340 @ 0x180165340`. That
function constructs a one-field object whose literal key is `code` and sends
URL index `0x31`; the embedded endpoint table contains
`/v3/public/getoauthToken`. Its response reflection lists `expires`,
`refresh_token`, `access_token`, `scope`, `token_type`, and `id_token`
(`QCloudSDK.c:558300` onward). The association of URL index `0x31` with that
specific adjacent endpoint is a strong inference, not yet a captured request,
so the Rust library does not pretend the independent browser login is
live-validated.

The strict clock requirement has two concrete causes: the Cloud signature
contains `timestamp_ms`, and OAuth tokens also have time-based validity. It is
not evidence that the password is used to derive `APP_SECRET`.

### File delivery

The exact `CloudClient::sendToPrinters` request serializer is
`FUN_1801fa110 @ 0x1801fa110` in `QCloudSDK.dll`. It writes only:

```json
{"file_id": 123, "printer_ids": [456]}
```

The function named `sendToPrinters` invokes Cloud URL-index `0x3d`; the SDK's
embedded endpoint list contains `/v3/printer/sendFile`. The endpoint-to-index
mapping is an inference, but it was validated in this environment: an actual
PWSZ delivery returned success from
`POST /v3/printer/sendFile` with precisely that JSON.

The upload preparation sequence below is corroborated by the current open
source [anycubic-cloud-api](https://github.com/Nino6689/anycubic-cloud-api)
and the identically named routes embedded in `QCloudSDK.dll`; it is therefore
labelled **external + live-validated**, not a complete decompilation of the
SDK's `HttpClient::UploadFile` internals.

1. `POST /v2/cloud_storage/lockStorageSpace` with
   `{"size": N,"name":"file.pwsz","is_temp_file":0}` returns `data.id`
   and bearer-like `data.preSignUrl`.
2. HTTP `PUT` of the raw PWSZ bytes to that pre-signed URL, with no Cloud
   `XX-*` headers.
3. `POST /v2/profile/newUploadFile` with
   `{"user_lock_space_id": lock_id}` returns `data.id` (the Cloud file id).
4. `POST /v2/cloud_storage/unlockStorageSpace` releases the reservation.
5. `POST /v3/printer/sendFile` addresses the Cloud file to `printer_ids`.

`sendFile` is **not** a print start. Workshop's `ACCloudManager::remotePrintStart`
(`FUN_1800b8540 @ 0x1800b8540`) has a distinct subsequent branch.

### Cloud print control

Workshop's start path is `FUN_1800d8570 @ 0x1800d8570`. It serializes its
start data through `FUN_1800d97f0 @ 0x1800d97f0` and calls the generic printer
control request with `project_id = 0` and `order_id = 1`. Direct UI call sites
confirm `order_id = 2` for pause (`accloud.c:106655`), `3` for resume
(`accloud.c:106742`), and `4` for stop (`accloud.c:95693,107114`) through
`FUN_1800d7ab0 @ 0x1800d7ab0`. The generic request reaches
`POST /work/operation/sendOrder` through `FUN_18027a960 @ 0x18027a960`.

The following start request combines the fields confirmed by the Workshop
serializer with the nullable fields used by the current public Cloud client.
It is now **live-validated** on the M7 Pro:

```json
{
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
}
```

The server response contains `data.msgid`, which proves order acceptance but
not execution. `GET /work/project/getProjects` must then be polled until it
returns the new nonzero project `id`; that id, not the Cloud file id, is used
for later control. The live stop request was:

```json
{
  "order_id": 4,
  "printer_id": 609126,
  "project_id": 115977478,
  "data": null,
  "ams_info": null,
  "settings": null
}
```

Workshop 4.1.8 itself constructs a small default data object in
`FUN_1800d7ab0`, while the maintained public client sends `null`; the server
accepted the latter. Pause/resume ids are code-confirmed but were not sent to
the physical machine in this test, so only start and stop are labelled
live-validated.

## Transport map

| Purpose | Transport | Endpoint / topic | Evidence |
|---|---|---|---|
| Discovery | UDP SSDP | `239.255.255.250:1900` | **Confirmed**, `FUN_180151200 @ 0x180151200` |
| Discovery request | UDP payload | `M-SEARCH`, `ST: ac:3dprinter:lcd` | **Confirmed**, same function |
| HTTP metadata | HTTP | `LOCATION`, or `http://{ip}:{port}/info` | **Confirmed**, `FUN_18014e4a0`, `FUN_18014e670` |
| Bootstrap | HTTP POST | `ctrlInfoUrl?ts&nonce&did&sign` | **Confirmed**, `FUN_18014d8e0 @ 0x18014d8e0` |
| Control and reports | MQTT/TLS | host and port from decrypted `LanInfo` | **Confirmed**, `FUN_1800c2e40 @ 0x1800c2e40` |
| Camera | HTTP | `http://{ip}:18088/live/0` | **Confirmed**, same constructor |
| File transfer | HTTP(S) raw POST | `fileUploadUrl + target` | **Confirmed**, `FUN_1800d6950 @ 0x1800d6950` |

`FUN_18014aaf0 @ 0x18014aaf0` uses **8883** (`0x22b3`) as a fallback
broker port, but parses an explicit port from `broker`. It is not safe to call
8883 universal. No LAN WebSocket transport was found. `mach_mqtt.dll` and
`QMqttSDK.dll` represent a separate cloud path, not direct device control.

## Discovery and credential bootstrap

The exact UDP payload in `FUN_180151200` is:

```text
M-SEARCH * HTTP/1.1\r\n
Host: 239.255.255.250:1900\r\n
ST: ac:3dprinter:lcd\r\n
Man: "ssdp:discover"\r\n
MX: 5\r\n
```

The search timer is 2500 ms and sends up to four probes
(`FUN_180151c20 @ 0x180151c20`). `FUN_18014bcf0 @ 0x18014bcf0` extracts only
`ST`, `USN`, and `LOCATION`. The binary does not prove a universal `/info`
port: the port carried in `LOCATION` is authoritative.

`FUN_18014b690 @ 0x18014b690` parses this `/info` JSON shape:

```text
deviceType, modelId, modelName, cn, ctrlType, ctrlInfoUrl,
ip, deviceName, zone, env, token
```

It creates:

```text
ts    = current epoch in milliseconds
nonce = UUID with hyphens removed
did   = host identifier
sign  = MD5(hex(MD5(token[0..16])) || ts || nonce)
```

The signature construction is **confirmed** by `FUN_1800856f0 @ 0x1800856f0`.
`FUN_180086ca0 @ 0x180086ca0` constructs the Workshop 4.1.8 `did` from the
first preferred Windows Ethernet (`6`) or Wi-Fi (`0x47`) MAC plus raw bytes
`54 09 27 4e 66 12`, then returns lowercase MD5 hex. The Windows adapter
selection is material, so the Rust crate offers `workshop_did_from_mac` and
does not silently substitute a random/Linux `did`.

The POST has an empty body and content type
`application/x-www-form-urlencoded`. A valid response is:

```json
{"code": 200, "data": {"info": "<base64>", "token": "<string>"}}
```

`FUN_18014aaf0` confirms Base64 decoding, AES machinery and padding removal,
then parses the result as JSON. It extracts:

```text
encodedInfo, encodedToten, deviceType, accountDeleteUrl, broker,
clientId, deviceId, username, password, devicecrt, devicepk, cacrt,
modeId, modelName, ip, mac, fileUploadUrl, locationInfo
```

AES-CBC with PKCS#7 is a high-confidence inference from the binary. A public
implementation uses `AES-128-CBC`, key `location.token[16..32]`, IV = returned
control `data.token` (first 16 bytes). The Rust code implements that ordering
as an **external compatibility choice**; a captured Photon response is needed
before declaring it universal.

## MQTT

The `ACLanClient` constructor consumes `ip`, port, `clientId`, `username`,
`password`, `devicecrt`, `devicepk`, and `cacrt`. The mapping of its three
unnamed QMQTT setters to client ID/user/password is an inference from those
fields and standard MQTT API conventions.

`FUN_1800ca830 @ 0x1800ca830` subscribes at QoS 0 to:

```text
anycubic/anycubicCloud/v1/printer/+/{deviceId}/{modeId}/+/report
```

`FUN_1800d2770 @ 0x1800d2770` builds command topics:

```text
anycubic/anycubicCloud/v1/pc/printer/{modeId}/{deviceId}/{type}
```

The device/model order is reinforced by the x64 `QString::arg` call at
`0x1800d281a`. The client assigns incrementing 16-bit packet identifiers.
Subscription QoS 0 is confirmed; retain and the MQTT keep-alive period are
not recovered. `onPingresp` merely logs (`FUN_1800cae30`).

Every builder serialises a JSON envelope:

```json
{
  "type": "status",
  "action": "query",
  "timestamp": 1710000000000,
  "msgid": "UUID",
  "data": null
}
```

`FUN_1800ca830` initially sends `status/query`, `lanInfo/query`, and
`print/query`. `FUN_1800cae80 @ 0x1800cae80` dispatches reports by:

```text
axis, status, print, file, lanInfo, autoOperation, resin, exposure,
smartResinVat, properties, releaseFilm, peripherie, residual, video, light
```

The Rust report model deliberately keeps `data` as JSON. Verified convenience
fields are print states (`preheating`, `stirring`, `copying`, `printing`,
`waiting`, `finished`, `pausing`, `paused`, `resuming`, `resumed`, `stopping`,
`stoped`, `failed`), `data.progress` while copying (`FUN_1800cfde0`), and
auto-operation `total_space`, `free_space`, `resin_temp`, `signal_strength`,
`layers`, `times` (`FUN_1800cf2b0`).

## Confirmed commands

| Purpose | `type` / `action` | `data` | Evidence |
|---|---|---|---|
| State | `status/query` | `null` | `FUN_1800d39a0` |
| LAN metadata | `lanInfo/query` | `null` | `FUN_1800d2770` |
| Print status | `print/query` | `null` | `FUN_1800d3bc0` |
| Pause, resume, stop | `print/pause`, `print/resume`, `print/stop` | null/task-aware builder | `FUN_1800d14f0`, `FUN_1800e36f0` |
| Light status | `light/query` | `null` | `FUN_1800d3ff0` |
| Light control | `light/control` | `{"type":3,"status":0|1}` | `FUN_1800e07a0` |
| Connection property | `properties/read` | `["connect"]` | `FUN_1800e28a0` |
| RSSI property | `properties/read` | `["signal_strength"]` | `FUN_1800e3300` |
| Automation state | `autoOperation/getStatus` | `null` | `FUN_1800d2ff0` |
| Video capture | `video/startCapture`, `video/stopCapture` | `null` | `ACLanClient` call sites |

`print/start` has confirmed field names:

```text
taskid, task_mode, url, filename, filesize, md5, filetype, filepath,
project_type, task_settings
```

`task_settings` includes `ai_settings`, `timelapse` (`status`, `mode`) and
`stir_settings` (`stirTime`). Numeric/string meanings of `task_mode`,
`filetype`, `project_type`, URL scheme, and the upload-to-start sequence are
**unknown**. Callers must supply them; the crate does not invent defaults.

Observed but not yet exposed as high-level safety-critical commands: axis,
resin, residual, exposure, release-film. File operations include
`file/deleteBatch` (`files`: `path`, `filename`), `listLocal`, `listUdisk`,
and `listVideo`.

## Upload and TLS

`FUN_1800d6950` sends:

```text
POST {fileUploadUrl}{target}
Content-Type: application/octet-stream
Expect:

<raw file bytes>
```

The device supplies the upload URL; no fixed upload port is known. The Qt code
has progress, finished, and error handlers. The Rust module performs the exact
raw stream and returns final HTTP status/bytes; response-body semantics and
incremental progress await a live capture.

`FUN_1800ca230 @ 0x1800ca230` finally calls Qt
`setPeerVerifyMode(0)` (`VerifyNone`). The default Rust
`TlsVerification::WorkshopCompatible` explicitly mirrors it and provides the
device client certificate/key. It is suitable only on a trusted isolated LAN;
it does not authenticate the broker. `TlsVerification::Strict` is available,
but may fail if the device certificate does not name its IP.

## External correlation

[Nino6689/hass-anycubic](https://github.com/Nino6689/hass-anycubic) independently
documents signed HTTP LAN bootstrap, encrypted credentials, and local MQTT;
it reports basic support for Photon Mono M5s/M7 Pro. Its verified hardware is
a Kobra S1 and its later `web/printer` and `printer/public` topics must not
replace the Workshop 4.1.8 `pc/printer` topic above. The older
[WaresWichall cloud integration](https://github.com/WaresWichall/hass-anycubic_cloud)
also reports basic M5s/M7 Pro MQTT support, but it is cloud-oriented.

## `plugins/printer.dll` result

The separately decompiled `printer.c` was inspected after this report's first
revision. It is a printer-profile, resin-parameter, model-catalogue and update
module—not the printer-network transport. Its only network imports are Qt
HTTP classes (`QNetworkAccessManager`, `QNetworkRequest`, `QNetworkReply`),
used for product/update downloads and signed cloud API headers such as
`XX-Timestamp`, `XX-Nonce`, and `XX-Signature` (`FUN_18000b0c0 @ 0x18000b0c0`).

It has no `QTcpSocket`, `QUdpSocket`, WebSocket, Winsock, MQTT, Telnet,
`11311`, `6000`, `8883`, or `18088` string/import. The `LocalPrinter` and
`LanPrinterInfomationRespone` data models in it describe local printer profiles
and do not open network connections. Thus it supplies no protocol for the
open ports observed on `192.168.0.101`.

## Rust handoff

```text
src/
├── lib.rs        public API
├── cloud.rs       Cloud OAuth exchange, PWSZ delivery, start/stop control
├── discovery.rs  SSDP, GET /info, signed bootstrap
├── protocol.rs   topics, envelopes, hashes and JSON models
├── transport.rs  async TLS MQTT and report broadcast
├── printer.rs    initial queries and command facade
├── commands.rs   only confirmed command constructors
├── upload.rs     raw HTTP file upload
├── pwsz.rs        conservative PWSZ ZIP metadata reader
├── status.rs     conservative report helpers
└── error.rs      typed errors
```

`cargo test` (12 tests) and `cargo clippy --all-targets -- -D warnings` pass.
No-argument CLI runs exact SSDP discovery. `cargo run -- <host> <info-port>`
does only `GET /info`, never writes state or prints secrets.

## Live LAN test: `192.168.0.101`

All direct-LAN tests were read-only:

- route is via `wlo1`, local source `192.168.0.67`; ICMP succeeds with TTL 64,
  14.7 ms; ARP has a MAC address;
- exact SSDP received no response;
- full TCP connect scan found only `7` (echo), `13` (daytime), `23`
  (tcpwrapped), `37` (time), `5355`, and `11311`; the recovered HTTP, MQTT,
  camera, and SSDP/TCP ports (`80`, `443`, `1883`, `8883`, `9883`, `18088`,
  `18910`, `1900`) are closed;
- no bootstrap, MQTT login, status or telemetry request was possible; no
  command, upload, movement, heating, or print action was sent.

This points to an inactive LAN service, a sleeping/offline printer network
stack, or a legacy/different network module. Confirm the printer's on-screen
IP, exact model and network/LAN-cloud mode, then rerun SSDP. Do not toggle a
LAN mode setting blindly: the external Kobra implementation reports that a
newer, different firmware can unpair cloud registration when LAN-only mode is
enabled.

## Live Cloud test: 2026-08-24

The account listed one printer, Cloud id `609126`, model and name `Anycubic
Photon Mono M7 Pro`. Its Cloud fields after transfer were `device_status = 1`,
`ready_status = 0`, and `is_printing = 1`. The public Cloud implementation
interprets the first as online and `is_printing = 1` as available; this
interpretation is **external** and agrees with the user's confirmation that
the device was idle.

`example.pwsz` was locally validated as an M7 Pro job (13312×5120 LCD,
223.642×126.480×230 mm, 326 layers), then all five Cloud transfer stages
above succeeded. The returned Cloud file id was `86140277`; Cloud parsing
completed with `gcode_id = 115366672`.

At the user's explicit request, the Rust client then sent start order `1`.
Cloud created project `115977478`, linked to the expected printer and gcode,
with `print_status = 1`. Stop order `4` was sent for that exact project after
approximately one minute. The project transitioned to `print_status = 3`,
`progress = 0`, with server timestamps 64 seconds apart; a final printer-list
read again returned `device_status = 1` and `is_printing = 1` (online and idle
according to the externally corroborated enum). No pause/resume, motion,
heating, exposure override, or direct MQTT command was sent.
