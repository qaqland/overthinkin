use std::{fs, process::Command};

use rusqlite::Connection;
use serde_json::{Value, json};
use tempfile::TempDir;

struct Fixture {
    dir: TempDir,
    has_transform: bool,
}

impl Fixture {
    fn new(query: &str, rhai: Option<&str>) -> Self {
        let dir = tempfile::tempdir().unwrap();
        Connection::open(dir.path().join("data.sqlite")).unwrap();
        fs::write(dir.path().join("query.sql"), query).unwrap();
        if let Some(script) = rhai {
            fs::write(dir.path().join("map.rhai"), script).unwrap();
        }
        Self {
            dir,
            has_transform: rhai.is_some(),
        }
    }

    fn seed(&self, sql: &str) {
        Connection::open(self.dir.path().join("data.sqlite"))
            .unwrap()
            .execute_batch(sql)
            .unwrap();
    }

    fn command(&self) -> Command {
        let mut command = Command::new(env!("CARGO_BIN_EXE_sqlite2meilisearch"));
        command
            .current_dir(self.dir.path())
            .env_remove("MEILI_URL")
            .env_remove("MEILI_KEY")
            .args([
                "--sqlite",
                "data.sqlite",
                "--query",
                "query.sql",
                "--preview",
            ]);
        if self.has_transform {
            command.args(["--transform", "map.rhai"]);
        }
        command
    }
}

#[test]
fn preview_prints_first() {
    let fixture = Fixture::new(
        include_str!("../examples/products.sql"),
        Some(include_str!("../examples/products.rhai")),
    );
    fixture.seed(include_str!("../examples/seed.sql"));
    let output = fixture.command().output().unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    assert_eq!(
        serde_json::from_slice::<Value>(&output.stdout).unwrap(),
        json!({
                "id":"1",
                "title": "Phone · Black",
                "brand": {"name": "Acme"},
                "tags": ["electronics", "phone"],
                "price_cents": 299900,
                "price_range": "standard"
        }),
    );
}

#[test]
fn preview_skips_rows() {
    let fixture = Fixture::new(
        "SELECT 1 AS id UNION ALL SELECT 2",
        Some("fn transform(row) { if row.id == 1 { return (); } row }"),
    );
    let output = fixture.command().output().unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    assert_eq!(
        serde_json::from_slice::<Value>(&output.stdout).unwrap(),
        json!({"id": 2})
    );
}

#[test]
fn transform_default() {
    let fixture = Fixture::new("SELECT 1 AS id, 'a' AS name", None);
    let output = fixture.command().output().unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    assert_eq!(
        serde_json::from_slice::<Value>(&output.stdout).unwrap(),
        json!({"id": 1, "name": "a"})
    );
}
