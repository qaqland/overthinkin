//! pw-micwatch
//!
//! Watches PipeWire and prints which clients request to use the microphone
//! (capture streams with media.class = Stream/Input/Audio, aka source-outputs),
//! when they start and when they stop.

use std::cell::RefCell;
use std::collections::HashMap;
use std::env;
use std::rc::Rc;

use chrono::Local;
use log::{LevelFilter, Metadata, Record, debug, info, trace};
use pipewire as pw;
use pipewire::node::{Node, NodeInfoRef, NodeListener, NodeState};
use pipewire::registry::GlobalObject;
use pipewire::types::ObjectType;
use pw::spa::utils::dict::DictRef;

/// Simple logger: `[local-time LEVEL] message`, no env-var control.
struct SimpleLogger {
    level: LevelFilter,
}

impl log::Log for SimpleLogger {
    fn enabled(&self, metadata: &Metadata) -> bool {
        metadata.level() <= self.level
    }

    fn log(&self, record: &Record) {
        if !self.enabled(record.metadata()) {
            return;
        }
        let now = Local::now().format("%H:%M:%S");
        println!("[{} {:<5}] {}", now, record.level(), record.args());
    }

    fn flush(&self) {}
}

/// Formats all key/value pairs of a dict as an indented multi-line string.
fn dump_props(props: &DictRef) -> String {
    let mut items: Vec<(&str, &str)> = props.iter().collect();
    items.sort();
    if items.is_empty() {
        "  (no props)".to_string()
    } else {
        items
            .iter()
            .map(|(k, v)| format!("  {k} = {v}"))
            .collect::<Vec<_>>()
            .join("\n")
    }
}

fn app_name(props: &DictRef) -> &str {
    props
        .get("application.name")
        .or_else(|| props.get("application.process.binary"))
        .or_else(|| props.get("node.name"))
        .unwrap_or("(unknown)")
}

fn state_str(state: NodeState) -> String {
    match state {
        NodeState::Error(e) => format!("error({e})"),
        s => format!("{s:?}").to_lowercase(),
    }
}

fn is_capture_stream(global: &GlobalObject<&DictRef>) -> bool {
    global.type_ == ObjectType::Node
        && global.props.and_then(|props| props.get("media.class")) == Some("Stream/Input/Audio")
}

/// Per-stream state, used to detect prop/state changes.
struct CaptureState {
    app: String,
    last_props: String,
    last_state: Option<String>,
}

fn handle_node_info(info: &NodeInfoRef, captures: &RefCell<HashMap<u32, CaptureState>>) {
    let id = info.id();
    let props = match info.props() {
        Some(props) => props,
        None => return,
    };
    let app = app_name(props).to_string();
    let props_str = dump_props(props);
    let state = state_str(info.state());

    let mut captures = captures.borrow_mut();
    match captures.get_mut(&id) {
        None => {
            info!(
                "mic capture request: {} (id={}, client.id={})",
                app,
                id,
                props.get("client.id").unwrap_or("-")
            );
            debug!(
                "mic capture props: {} (id={}, state={})\n{}",
                app, id, state, props_str
            );
            captures.insert(
                id,
                CaptureState {
                    app,
                    last_props: props_str,
                    last_state: Some(state),
                },
            );
        }
        Some(capture) => {
            if capture.last_state.as_deref() != Some(state.as_str()) {
                info!(
                    "mic capture state: {} (id={}) -> {}",
                    capture.app, id, state
                );
                capture.last_state = Some(state.clone());
            }
            if capture.last_props != props_str {
                debug!(
                    "mic capture props updated: {} (id={}, state={})\n{}",
                    capture.app, id, state, props_str
                );
                capture.last_props = props_str;
            }
        }
    }
}

fn print_usage(opts: &getopts::Options) {
    let brief = "Usage: pw-micwatch [options]";
    println!("{}", opts.usage(brief));
}

fn main() {
    // getopts (modelled after C getopt): -v repeatable, -vv counts as 2
    let mut opts = getopts::Options::new();
    opts.optflagmulti("v", "verbose", "-v add properties, -vv add other events");
    opts.optflag("h", "help", "show help");

    let matches = match opts.parse(env::args().skip(1)) {
        Ok(m) => m,
        Err(f) => {
            eprintln!("{}", f);
            return;
        }
    };
    if matches.opt_present("h") {
        print_usage(&opts);
        return;
    }
    let verbose = matches.opt_count("v");

    let level = match verbose {
        0 => LevelFilter::Info,
        1 => LevelFilter::Debug,
        _ => LevelFilter::Trace,
    };

    log::set_boxed_logger(Box::new(SimpleLogger { level })).expect("failed to init logger");
    log::set_max_level(level);

    pw::init();

    let mainloop = pw::main_loop::MainLoop::new(None).expect("failed to create MainLoop");
    let context = pw::context::Context::new(&mainloop).expect("failed to create Context");
    let core = context.connect(None).expect("failed to connect PipeWire");
    let registry = Rc::new(core.get_registry().expect("failed to get Registry"));
    let registry_weak = Rc::downgrade(&registry);

    // capture streams: global id -> state
    let captures: Rc<RefCell<HashMap<u32, CaptureState>>> = Rc::new(RefCell::new(HashMap::new()));
    // bound node proxies and their listeners must stay alive
    let nodes: Rc<RefCell<HashMap<u32, (Node, NodeListener)>>> =
        Rc::new(RefCell::new(HashMap::new()));

    let _listener = {
        let captures_for_remove = captures.clone();
        let nodes_for_remove = nodes.clone();

        registry
            .add_listener_local()
            .global(move |g: &GlobalObject<&DictRef>| {
                let registry = match registry_weak.upgrade() {
                    Some(r) => r,
                    None => return,
                };
                if !is_capture_stream(g) {
                    trace!("ignored global id={} type={}", g.id, g.type_.to_str());
                    return;
                }

                // Bind the node to receive full info (complete props + state).
                let node: Node = match registry.bind(g) {
                    Ok(n) => n,
                    Err(e) => {
                        debug!("failed to bind node id={}: {}", g.id, e);
                        return;
                    }
                };

                let captures = captures.clone();
                let obj_listener = node
                    .add_listener_local()
                    .info(move |info: &NodeInfoRef| handle_node_info(info, &captures))
                    .register();

                nodes.borrow_mut().insert(g.id, (node, obj_listener));
            })
            .global_remove(move |id| {
                // drop the proxy/listener first
                nodes_for_remove.borrow_mut().remove(&id);
                if let Some(st) = captures_for_remove.borrow_mut().remove(&id) {
                    info!("mic recording stopped: {} (id={})", st.app, id);
                }
            })
            .register()
    };

    info!("watching for mic capture requests (Ctrl+C to exit)");
    mainloop.run();
}
