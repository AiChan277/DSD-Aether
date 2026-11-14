//! External control and telemetry I/O channels: OSC (`127.0.0.1:9001/9002`) and opt-in WebSocket (`:19190`).

pub mod osc;
pub mod ws;

pub use osc::{
    OSC_MAX_MSG_PER_SEC, OscControlMsg, OscRateLimiter, encode_osc_float, encode_osc_int,
    map_expression_name_to_slot, map_param_id_to_slot, parse_osc_packet,
};
pub use ws::{
    WsClientMsg, WsTelemetryFrame, generate_session_token, handle_ws_text, run_ws_supervisor,
};
