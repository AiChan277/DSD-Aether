//! Opt-in localhost WebSocket JSON-RPC server (`ws://127.0.0.1:19190/v1`, bearer-token auth, §7.8).

use core::pin::pin;
use std::sync::Arc;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use ahash::RandomState;
use futures_util::future::{Either, select};
use futures_util::{SinkExt, StreamExt};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{broadcast, watch};
use tokio_tungstenite::accept_hdr_async;
use tokio_tungstenite::tungstenite::Message;
use tokio_tungstenite::tungstenite::handshake::server::{ErrorResponse, Request, Response};

use aether_ffi::{AETHER_HOST_EVT_HOTKEY_EVENT, AETHER_HOST_EVT_WS_COMMAND};

use crate::events::{HostEventQueue, SharedParamBank};
use crate::io::osc::{map_expression_name_to_slot, map_param_id_to_slot};

/// Telemetry snapshot broadcast at `4 Hz` to subscribed WebSocket clients (§7.8).
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct WsTelemetryFrame {
    /// Message discriminator (`"telemetry"`).
    #[serde(rename = "type")]
    pub kind: String,
    /// End-to-end latency estimate (ms).
    pub latency_ms: f32,
    /// Render frame rate.
    pub fps: f32,
    /// Active execution provider ID.
    pub ep: u32,
}

/// Parsed inbound WebSocket JSON-RPC control command (§7.8).
#[derive(Clone, Debug, Deserialize)]
#[serde(tag = "type")]
pub enum WsClientMsg {
    /// `{"type":"param.set","id":"render.flatness","value":0.75}`
    #[serde(rename = "param.set")]
    ParamSet { id: String, value: f32 },
    /// `{"type":"expression.trigger","name":"Joy"}`
    #[serde(rename = "expression.trigger")]
    ExpressionTrigger {
        name: String,
        #[serde(default = "default_expr_weight")]
        value: f32,
    },
    /// `{"type":"subscribe","topics":["telemetry"]}`
    #[serde(rename = "subscribe")]
    Subscribe { topics: Vec<String> },
}

const fn default_expr_weight() -> f32 {
    1.0
}

/// Generates a random unpredictable 32-char hex session bearer token using OS CSPRNG via `ahash::RandomState`.
#[must_use]
pub fn generate_session_token() -> String {
    let mut hasher = Sha256::new();
    for i in 0..4_u64 {
        let state = RandomState::new();
        let h = state.hash_one(i);
        hasher.update(h.to_le_bytes());
    }
    hasher.update(std::process::id().to_le_bytes());
    let now_ns = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_nanos());
    hasher.update(now_ns.to_le_bytes());
    let digest = hasher.finalize();
    let mut tok = String::with_capacity(32);
    for b in &digest[..16] {
        use core::fmt::Write as _;
        let _ = write!(tok, "{b:02x}");
    }
    tok
}

fn verify_bearer_request(req: &Request, expected_token: &str) -> bool {
    let path = req.uri().path();
    if path != "/v1" {
        return false;
    }
    if let Some(auth) = req
        .headers()
        .get("Authorization")
        .and_then(|v| v.to_str().ok())
    {
        if let Some(tok) = auth
            .strip_prefix("Bearer ")
            .or_else(|| auth.strip_prefix("bearer "))
        {
            if constant_time_eq(tok.trim().as_bytes(), expected_token.as_bytes()) {
                return true;
            }
        }
    }
    if let Some(query) = req.uri().query() {
        for pair in query.split('&') {
            if let Some(tok) = pair.strip_prefix("token=") {
                if constant_time_eq(tok.as_bytes(), expected_token.as_bytes()) {
                    return true;
                }
            }
        }
    }
    false
}

fn constant_time_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut diff = 0_u8;
    for (&x, &y) in a.iter().zip(b.iter()) {
        diff |= x ^ y;
    }
    diff == 0
}

/// Parses and executes one inbound WebSocket JSON-RPC client message cleanly.
pub fn handle_ws_text(
    text: &str,
    params: &SharedParamBank,
    events: &HostEventQueue,
    subscribed: &mut bool,
) -> Result<(), String> {
    if text.len() > 64 * 1024 {
        return Err("WebSocket message exceeds 64 KiB limit".to_owned());
    }
    let msg: WsClientMsg =
        serde_json::from_str(text).map_err(|e| format!("invalid WS JSON: {e}"))?;
    match msg {
        WsClientMsg::ParamSet { id, value } => {
            if !value.is_finite() {
                return Err("non-finite param value".to_owned());
            }
            let slot = usize::try_from(map_param_id_to_slot(&id)).unwrap_or(0);
            params.set_external_param(slot, value);
            events.push_ok(
                AETHER_HOST_EVT_WS_COMMAND,
                u64::from(map_param_id_to_slot(&id)),
                value,
                &format!("{id}={value}"),
            );
        }
        WsClientMsg::ExpressionTrigger { name, value } => {
            let slot_u32 = map_expression_name_to_slot(&name);
            let slot = usize::try_from(slot_u32).unwrap_or(0);
            params.set_expression_override(slot, value);
            events.push_ok(
                AETHER_HOST_EVT_HOTKEY_EVENT,
                u64::from(slot_u32),
                value,
                &format!("expr:{name}"),
            );
        }
        WsClientMsg::Subscribe { topics } => {
            if topics.iter().any(|t| t == "telemetry") {
                *subscribed = true;
            }
        }
    }
    Ok(())
}

enum ConnStep {
    Shutdown,
    Read(Option<Result<Message, tokio_tungstenite::tungstenite::Error>>),
    Tick,
}

enum AcceptStep {
    Shutdown,
    EnableChanged,
    Accepted(Option<TcpStream>),
}

/// Runs a single authenticated WebSocket connection until disconnect or shutdown.
#[allow(clippy::result_large_err)]
async fn serve_connection(
    stream: TcpStream,
    expected_token: String,
    params: Arc<SharedParamBank>,
    events: Arc<HostEventQueue>,
    mut telemetry_rx: broadcast::Receiver<String>,
    mut shutdown_rx: watch::Receiver<bool>,
) {
    let tok_check = expected_token;
    let callback = |req: &Request, resp: Response| -> Result<Response, ErrorResponse> {
        if verify_bearer_request(req, &tok_check) {
            Ok(resp)
        } else {
            let mut err = ErrorResponse::new(Some("Unauthorized".to_owned()));
            *err.status_mut() = tokio_tungstenite::tungstenite::http::StatusCode::UNAUTHORIZED;
            Err(err)
        }
    };

    let Ok(ws_stream) = accept_hdr_async(stream, callback).await else {
        return;
    };
    let (mut write, mut read) = ws_stream.split();
    let mut subscribed = false;
    let mut throttle = tokio::time::interval(Duration::from_millis(250));

    loop {
        if *shutdown_rx.borrow() {
            break;
        }
        let is_sub = subscribed;
        let step = {
            let sd_fut = pin!(shutdown_rx.changed());
            let read_fut = pin!(read.next());
            let read_or_sd = pin!(select(sd_fut, read_fut));
            let th = &mut throttle;
            let tick_fut = pin!(async move {
                if is_sub {
                    th.tick().await;
                } else {
                    core::future::pending::<tokio::time::Instant>().await;
                }
            });
            match select(read_or_sd, tick_fut).await {
                Either::Left((Either::Left(_), _)) => ConnStep::Shutdown,
                Either::Left((Either::Right((msg, _)), _)) => ConnStep::Read(msg),
                Either::Right(_) => ConnStep::Tick,
            }
        };
        match step {
            ConnStep::Shutdown => break,
            ConnStep::Read(msg) => match msg {
                Some(Ok(Message::Text(txt))) => {
                    let _ = handle_ws_text(txt.as_str(), &params, &events, &mut subscribed);
                }
                Some(Ok(Message::Close(_)) | Err(_)) | None => break,
                Some(Ok(_)) => {}
            },
            ConnStep::Tick => {
                if let Ok(payload) = telemetry_rx.try_recv() {
                    if write.send(Message::Text(payload.into())).await.is_err() {
                        break;
                    }
                }
            }
        }
    }
}

/// Runs the opt-in `ws://127.0.0.1:<port>/v1` listener whenever `enable_rx` becomes `true`.
pub async fn run_ws_supervisor(
    port: u16,
    token: String,
    params: Arc<SharedParamBank>,
    events: Arc<HostEventQueue>,
    telemetry_tx: broadcast::Sender<String>,
    mut enable_rx: watch::Receiver<bool>,
    mut shutdown_rx: watch::Receiver<bool>,
) {
    loop {
        if *shutdown_rx.borrow() {
            return;
        }
        if !*enable_rx.borrow() {
            let en_fut = pin!(enable_rx.changed());
            let sd_fut = pin!(shutdown_rx.changed());
            match select(en_fut, sd_fut).await {
                Either::Left(_) => {}
                Either::Right(_) => return,
            }
            continue;
        }

        let bind_addr = format!("127.0.0.1:{port}");
        let Ok(listener) = TcpListener::bind(&bind_addr).await else {
            tokio::time::sleep(Duration::from_millis(250)).await;
            continue;
        };

        loop {
            if *shutdown_rx.borrow() || !*enable_rx.borrow() {
                break;
            }
            let step = {
                let sd_fut = pin!(shutdown_rx.changed());
                let en_fut = pin!(enable_rx.changed());
                let acc_fut = pin!(listener.accept());
                let inner = pin!(select(en_fut, acc_fut));
                match select(sd_fut, inner).await {
                    Either::Left(_) => AcceptStep::Shutdown,
                    Either::Right((Either::Left(_), _)) => AcceptStep::EnableChanged,
                    Either::Right((Either::Right((accept_res, _)), _)) => {
                        AcceptStep::Accepted(accept_res.ok().map(|(s, _)| s))
                    }
                }
            };
            match step {
                AcceptStep::Shutdown => return,
                AcceptStep::EnableChanged => {
                    if !*enable_rx.borrow() {
                        break;
                    }
                }
                AcceptStep::Accepted(Some(stream)) => {
                    let t_rx = telemetry_tx.subscribe();
                    let s_rx = shutdown_rx.clone();
                    let tok = token.clone();
                    let p = Arc::clone(&params);
                    let e = Arc::clone(&events);
                    tokio::spawn(serve_connection(stream, tok, p, e, t_rx, s_rx));
                }
                AcceptStep::Accepted(None) => {}
            }
        }
    }
}
