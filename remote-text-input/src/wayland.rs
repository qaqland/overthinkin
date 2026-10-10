use calloop::{EventLoop, LoopSignal, channel};
use calloop_wayland_source::WaylandSource;
use serde::Serialize;
use tokio::sync::{oneshot, watch};
use wayland_client::{
    Connection, Dispatch, QueueHandle, delegate_noop,
    protocol::{wl_registry, wl_seat},
};
use wayland_protocols_misc::zwp_input_method_v2::client::{
    zwp_input_method_manager_v2::ZwpInputMethodManagerV2,
    zwp_input_method_v2::{self, ZwpInputMethodV2},
};

use crate::Result;

pub const MAX_TEXT_BYTES: usize = 3900;

#[derive(Clone, Default, Serialize)]
pub struct Status {
    pub active: bool,
    pub epoch: u64,
}

pub enum Command {
    Update {
        epoch: u64,
        text: String,
        submit: bool,
        reply: oneshot::Sender<std::result::Result<(), &'static str>>,
    },
    Clear,
    Stop,
}

struct InputMethod {
    manager: Option<ZwpInputMethodManagerV2>,
    seat: Option<wl_seat::WlSeat>,
    method: Option<ZwpInputMethodV2>,
    status: Status,
    publisher: watch::Sender<Status>,
    pending_active: bool,
    pending_reset: bool,
    serial: u32,
    unavailable: bool,
    stop: LoopSignal,
}

pub fn run(commands: channel::Channel<Command>, publisher: watch::Sender<Status>) -> Result<()> {
    let connection = Connection::connect_to_env()?;
    let mut queue = connection.new_event_queue();
    let qh = queue.handle();
    let mut event_loop = EventLoop::try_new()?;
    let mut state = InputMethod {
        manager: None,
        seat: None,
        method: None,
        status: Status::default(),
        publisher,
        pending_active: false,
        pending_reset: false,
        serial: 0,
        unavailable: false,
        stop: event_loop.get_signal(),
    };

    connection.display().get_registry(&qh, ());
    queue.roundtrip(&mut state)?;
    let manager = state
        .manager
        .as_ref()
        .ok_or("Compositor does not expose zwp_input_method_manager_v2")?;
    let seat = state.seat.as_ref().ok_or("No Wayland seat available")?;
    state.method = Some(manager.get_input_method(seat, &qh, ()));
    queue.roundtrip(&mut state)?;
    if state.unavailable {
        return Err("Wayland input method is unavailable".into());
    }

    WaylandSource::new(connection.clone(), queue)
        .insert(event_loop.handle())
        .map_err(|err| err.error)?;
    event_loop
        .handle()
        .insert_source(commands, |event, _, state| match event {
            channel::Event::Msg(Command::Update {
                epoch,
                text,
                submit,
                reply,
            }) => {
                let _ = reply.send(state.update(epoch, &text, submit));
            }
            channel::Event::Msg(Command::Clear) => state.clear(),
            channel::Event::Msg(Command::Stop) | channel::Event::Closed => state.stop.stop(),
        })
        .map_err(|err| err.error)?;

    event_loop.run(None, &mut state, |_| {})?;
    if state.unavailable {
        return Err("Wayland input method became unavailable".into());
    }
    state.clear();
    if let Some(method) = &state.method {
        method.destroy();
    }
    connection.flush()?;
    Ok(())
}

impl InputMethod {
    fn update(
        &self,
        epoch: u64,
        text: &str,
        submit: bool,
    ) -> std::result::Result<(), &'static str> {
        if !self.status.active || epoch != self.status.epoch {
            return Err("输入焦点已改变，请重新输入或发送");
        }
        if text.len() > MAX_TEXT_BYTES || text.contains('\0') {
            return Err("文本超过 3900 UTF-8 字节或包含 NUL");
        }
        let method = self.method.as_ref().ok_or("输入法尚未就绪")?;
        if submit {
            method.set_preedit_string(String::new(), 0, 0);
            method.commit_string(text.to_owned());
        } else {
            let cursor = text.len() as i32;
            method.set_preedit_string(text.to_owned(), cursor, cursor);
        }
        // commit applies protocol state; only commit_string inserts finalized text.
        method.commit(self.serial);
        Ok(())
    }

    fn clear(&self) {
        if self.status.active
            && !self.unavailable
            && let Some(method) = &self.method
        {
            method.set_preedit_string(String::new(), 0, 0);
            method.commit(self.serial);
        }
    }
}

impl Dispatch<wl_registry::WlRegistry, ()> for InputMethod {
    fn event(
        state: &mut Self,
        registry: &wl_registry::WlRegistry,
        event: wl_registry::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wl_registry::Event::Global {
            name, interface, ..
        } = event
        {
            match interface.as_str() {
                "zwp_input_method_manager_v2" => {
                    state.manager = Some(registry.bind(name, 1, qh, ()));
                }
                // shortcut: use the first seat, add seat selection if multi-seat is needed.
                "wl_seat" if state.seat.is_none() => {
                    state.seat = Some(registry.bind(name, 1, qh, ()));
                }
                _ => {}
            }
        }
    }
}

impl Dispatch<ZwpInputMethodV2, ()> for InputMethod {
    fn event(
        state: &mut Self,
        _: &ZwpInputMethodV2,
        event: zwp_input_method_v2::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            zwp_input_method_v2::Event::Activate => {
                state.pending_active = true;
                state.pending_reset = true;
            }
            zwp_input_method_v2::Event::Deactivate => {
                state.pending_active = false;
                state.pending_reset = true;
            }
            zwp_input_method_v2::Event::Done => {
                state.serial = state.serial.wrapping_add(1);
                state.status.active = state.pending_active;
                if state.pending_reset {
                    state.status.epoch += 1;
                    state.pending_reset = false;
                }
                state.publisher.send_replace(state.status.clone());
            }
            zwp_input_method_v2::Event::Unavailable => {
                state.unavailable = true;
                state.status.active = false;
                state.publisher.send_replace(state.status.clone());
                state.stop.stop();
            }
            _ => {}
        }
    }
}

delegate_noop!(InputMethod: ignore wl_seat::WlSeat);
delegate_noop!(InputMethod: ignore ZwpInputMethodManagerV2);
