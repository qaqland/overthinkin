mod wayland;

use std::sync::Arc;

use axum::{
    Router,
    extract::{
        State, WebSocketUpgrade,
        ws::{Message, WebSocket},
    },
    http::{HeaderMap, StatusCode},
    response::{Html, IntoResponse, Response},
    routing::get,
};
use calloop::channel;
use serde::Deserialize;
use serde_json::{Value, json};
use tokio::sync::{Mutex, oneshot, watch};

use wayland::{Command, Status};

type Result<T> = std::result::Result<T, Box<dyn std::error::Error + Send + Sync>>;

#[derive(Clone)]
struct App {
    commands: channel::Sender<Command>,
    status: watch::Receiver<Status>,
    controller: Arc<Mutex<()>>,
}

#[derive(Deserialize)]
#[serde(tag = "type", rename_all = "lowercase")]
enum Input {
    Draft { epoch: u64, text: String },
    Send { epoch: u64, text: String, id: u64 },
}

#[tokio::main]
async fn main() -> Result<()> {
    let listen = std::env::args()
        .nth(1)
        .unwrap_or_else(|| "0.0.0.0:8080".into());
    if listen == "--help" || listen == "-h" {
        println!("Usage: remote-text-input [LISTEN_ADDRESS]\nDefault: 0.0.0.0:8080");
        return Ok(());
    }
    let listener = tokio::net::TcpListener::bind(&listen).await?;
    let address = listener.local_addr()?;
    let (commands, receiver) = channel::channel();
    let (publisher, status) = watch::channel(Status::default());
    let app = Router::new()
        .route("/", get(index))
        .route("/ws", get(websocket))
        .with_state(App {
            commands: commands.clone(),
            status,
            controller: Arc::new(Mutex::new(())),
        });

    println!("Listening on {address}");
    println!("Open http://127.0.0.1:{}/", address.port());
    println!("手机访问：将 127.0.0.1 换成电脑的局域网 IP。首个会话可输入，其他会话排队。");
    println!("仅用于可信局域网：HTTP 未加密。Ctrl+C 退出。");

    let mut backend = tokio::task::spawn_blocking(move || wayland::run(receiver, publisher));
    let result: Result<()> = tokio::select! {
        result = &mut backend => return result?,
        result = axum::serve(listener, app) => result.map_err(Into::into),
        result = tokio::signal::ctrl_c() => result.map_err(Into::into),
    };
    let _ = commands.send(Command::Stop);
    backend.await??;
    result
}

async fn index() -> impl IntoResponse {
    (
        [
            ("cache-control", "no-store"),
            ("referrer-policy", "no-referrer"),
            ("x-frame-options", "DENY"),
        ],
        Html(include_str!("../web/index.html")),
    )
}

async fn websocket(
    State(app): State<App>,
    headers: HeaderMap,
    upgrade: WebSocketUpgrade,
) -> Response {
    let host = headers.get("host").and_then(|v| v.to_str().ok());
    let origin = headers.get("origin").and_then(|v| v.to_str().ok());
    let same_origin = host.is_some_and(|host| origin == Some(format!("http://{host}").as_str()));
    if !same_origin {
        return (StatusCode::FORBIDDEN, "Invalid origin").into_response();
    }
    upgrade
        .max_message_size(32 * 1024)
        .max_frame_size(32 * 1024)
        .on_upgrade(move |socket| session(socket, app))
}

async fn send(socket: &mut WebSocket, value: Value) -> bool {
    socket
        .send(Message::Text(value.to_string().into()))
        .await
        .is_ok()
}

async fn session(mut socket: WebSocket, mut app: App) {
    if !send(&mut socket, json!({ "type": "waiting" })).await {
        return;
    }
    // Keep the same lock future queued: Tokio's mutex hands control over in FIFO order.
    let acquire = app.controller.clone().lock_owned();
    tokio::pin!(acquire);
    let _controller = loop {
        tokio::select! {
            biased;
            message = socket.recv() => match message {
                Some(Ok(Message::Text(_) | Message::Binary(_) | Message::Ping(_) | Message::Pong(_))) => {},
                _ => return,
            },
            controller = &mut acquire => break controller,
        }
    };
    let initial = app.status.borrow_and_update().clone();
    if !send(&mut socket, json!({ "type": "state", "state": initial })).await {
        return;
    }
    loop {
        let response = tokio::select! {
            changed = app.status.changed() => {
                if changed.is_err() { break; }
                let status = app.status.borrow_and_update().clone();
                json!({ "type": "state", "state": status })
            }
            message = socket.recv() => {
                match message {
                    Some(Ok(Message::Text(text))) => process(&app, &text).await,
                    Some(Ok(Message::Ping(_) | Message::Pong(_))) => continue,
                    _ => break,
                }
            }
        };
        if !send(&mut socket, response).await {
            break;
        }
    }
    let _ = app.commands.send(Command::Clear);
}

async fn process(app: &App, payload: &str) -> Value {
    let input = match serde_json::from_str::<Input>(payload) {
        Ok(input) => input,
        Err(_) => return json!({ "type": "error", "message": "无效消息" }),
    };
    let (epoch, text, id) = match input {
        Input::Draft { epoch, text } => (epoch, text, None),
        Input::Send { epoch, text, id } => (epoch, text, Some(id)),
    };
    let (reply, response) = oneshot::channel();
    if app
        .commands
        .send(Command::Update {
            epoch,
            text,
            submit: id.is_some(),
            reply,
        })
        .is_err()
    {
        return json!({ "type": "error", "id": id, "message": "Wayland 连接已关闭" });
    }
    match response.await {
        Ok(Ok(())) => json!({ "type": "ack", "id": id }),
        Ok(Err(message)) => json!({ "type": "error", "id": id, "message": message }),
        Err(_) => json!({ "type": "error", "id": id, "message": "Wayland 连接已关闭" }),
    }
}
