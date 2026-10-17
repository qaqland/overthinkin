use std::{
    fs,
    process::{Command, Output},
    time::Duration,
};

use anyhow::Result;
use meilisearch_sdk::{
    client::Client, documents::DocumentsQuery, indexes::Index, task_info::TaskInfo, tasks::Task,
};
use rusqlite::Connection;
use serde_json::{Value, json};
use tempfile::TempDir;

const IMAGE: &str = "getmeili/meilisearch:latest";

struct Container(String);

impl Drop for Container {
    fn drop(&mut self) {
        Command::new("docker")
            .args(["rm", "-f", &self.0])
            .output()
            .ok();
    }
}

struct Fixture {
    dir: TempDir,
    url: String,
    index: Index,
    _container: Container,
}

fn docker(args: &[&str]) -> Output {
    let output = Command::new("docker").args(args).output().unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    output
}

impl Fixture {
    async fn new() -> Option<Self> {
        let ok = |args: &[&str]| {
            Command::new("docker")
                .args(args)
                .output()
                .is_ok_and(|o| o.status.success())
        };
        if !ok(&["version"]) || !ok(&["image", "inspect", IMAGE]) {
            eprintln!("skipping: docker or image {IMAGE} unavailable");
            return None;
        }

        let output = docker(&["run", "-d", "--rm", "-p", "127.0.0.1::7700", IMAGE]).stdout;
        let container = Container(String::from_utf8(output).unwrap().trim().into());

        let output = docker(&["port", &container.0, "7700/tcp"]).stdout;
        let url = format!("http://{}", String::from_utf8(output).unwrap().trim());

        let client = Client::new(&url, None::<&str>).unwrap();
        let index = client.index("test");

        tokio::time::timeout(Duration::from_secs(10), async {
            while index.client.health().await.is_err() {
                tokio::time::sleep(Duration::from_millis(100)).await;
            }
        })
        .await
        .expect("meilisearch did not become healthy");

        let dir = tempfile::tempdir().unwrap();
        Connection::open(dir.path().join("data.sqlite")).unwrap();

        let fixture = Self {
            dir,
            url,
            index,
            _container: container,
        };
        Some(fixture)
    }

    fn source(&self, sql: &str, rhai: &str) {
        fs::write(self.dir.path().join("query.sql"), sql).unwrap();
        fs::write(self.dir.path().join("transform.rhai"), rhai).unwrap();
    }

    fn command(&self) -> Command {
        let mut command = Command::new(env!("CARGO_BIN_EXE_sqlite2meilisearch"));
        command
            .current_dir(self.dir.path())
            .env("MEILI_URL", &self.url)
            .env_remove("MEILI_KEY")
            .args([
                "--sqlite",
                "data.sqlite",
                "--query",
                "query.sql",
                "--transform",
                "transform.rhai",
                "--index",
                &self.index.uid,
            ]);
        command
    }

    async fn wait(&self, task: TaskInfo) {
        let task = tokio::time::timeout(
            Duration::from_secs(60),
            task.wait_for_completion(
                &self.index.client,
                Some(Duration::from_millis(100)),
                Some(Duration::MAX),
            ),
        )
        .await
        .expect("test task timed out")
        .unwrap();
        assert!(matches!(task, Task::Succeeded { .. }), "{task:?}");
    }

    async fn seed(&self, documents: &[Value], primary_key: &str) {
        self.wait(
            self.index
                .add_or_replace(documents, Some(primary_key))
                .await
                .unwrap(),
        )
        .await;
    }
}

fn success(output: Output) -> Value {
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    serde_json::from_slice(&output.stdout).unwrap()
}

fn failure(output: Output) {
    assert_eq!(output.status.code(), Some(1), "{output:?}");
    assert!(
        output.stdout.is_empty(),
        "failure must not emit a success report"
    );
}

#[tokio::test]
async fn creates_replaces_and_prunes_across_pages() -> Result<()> {
    let Some(fixture) = Fixture::new().await else {
        return Ok(());
    };
    // id    title  obsolete
    // 1     old    remove me
    // ...
    // 2105  old    remove me
    let sql = "\
        WITH RECURSIVE n(id) AS (
            SELECT 1
            UNION ALL
            SELECT id + 1 FROM n WHERE id < 2105
        )
        SELECT id, 'old' AS title, 'remove me' AS obsolete
        FROM n";
    let rhai = "fn transform(row) { row }";
    fixture.source(sql, rhai);
    let settings = json!({
        "searchableAttributes": ["title"],
        "displayedAttributes": ["title"]
    });
    fs::write(
        fixture.dir.path().join("settings.json"),
        settings.to_string(),
    )?;
    let report = success(
        fixture
            .command()
            .args(["--settings", "settings.json"])
            .output()?,
    );
    assert_eq!(report["documents"], 2105);
    assert_eq!(report["deleted_documents"], 0);
    assert_eq!(report["indexed_documents"], 2105);
    let original_settings = serde_json::to_value(fixture.index.get_settings().await?)?;

    // id    title
    // 1     new  (dropped by the transform)
    // 2     new
    // 4     new
    // ...
    // 2104  new
    // 3000  new
    let sql = "\
        WITH RECURSIVE n(id) AS (
            SELECT 1
            UNION ALL
            SELECT id + 1 FROM n WHERE id < 2105
        )
        SELECT id, 'new' AS title
        FROM n
        WHERE id % 2 = 0 OR id = 1
        UNION ALL
        SELECT 3000, 'new'";
    let rhai = "fn transform(row) { if row.id == 1 { return (); } row }";
    fixture.source(sql, rhai);
    let report = success(fixture.command().output()?);
    assert_eq!(report["rows_read"], 1054);
    assert_eq!(report["rows_skipped"], 1);
    assert_eq!(report["documents"], 1053);
    assert_eq!(report["deleted_documents"], 1053);
    assert_eq!(report["indexed_documents"], 1053);
    assert_eq!(
        serde_json::to_value(fixture.index.get_settings().await?)?,
        original_settings
    );

    let mut query = DocumentsQuery::new(&fixture.index);
    query.with_limit(4000);
    let documents = fixture.index.get_documents_with::<Value>(&query).await?;
    assert_eq!(documents.results.len(), 1053);
    for document in documents.results {
        let id = document["id"].as_u64().unwrap();
        assert!((2..=2104).contains(&id) && id % 2 == 0 || id == 3000);
        assert_eq!(document, json!({"id":id, "title":"new"}));
    }

    let output = fixture
        .command()
        .args(["--report", "report.json"])
        .output()?;
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(output.stdout.is_empty());
    let report: Value = serde_json::from_slice(&fs::read(fixture.dir.path().join("report.json"))?)?;
    assert_eq!(report["deleted_documents"], 0);
    assert_eq!(report["indexed_documents"], 1053);
    Ok(())
}

#[tokio::test]
async fn late_source_failure_does_not_delete_existing_documents() -> Result<()> {
    for (last_id, rhai) in [
        ("1", "fn transform(row) { row }"),
        (
            "1026",
            "fn transform(row) { if row.id == 1026 { throw \"bad row\"; } row }",
        ),
    ] {
        let Some(fixture) = Fixture::new().await else {
            return Ok(());
        };
        fixture
            .seed(&[json!({"id":99999}), json!({"id":1,"title":"old"})], "id")
            .await;
        // id        title
        // 1         new
        // ...
        // 1025      new
        // {last_id} new  (fails after the first batch)
        let sql = format!(
            "\
            WITH RECURSIVE n(id) AS (
                SELECT 1
                UNION ALL
                SELECT id + 1 FROM n WHERE id < 1025
            )
            SELECT id, 'new' AS title
            FROM n
            UNION ALL
            SELECT {last_id}, 'new'"
        );
        fixture.source(&sql, rhai);
        failure(
            fixture
                .command()
                .args(["--report", "report.json"])
                .output()?,
        );
        assert!(!fixture.dir.path().join("report.json").exists());
        assert_eq!(
            fixture.index.get_document::<Value>("99999").await?,
            json!({"id":99999})
        );
        assert_eq!(
            fixture.index.get_document::<Value>("1").await?["title"],
            "new"
        );
        assert_eq!(fixture.index.get_stats().await?.number_of_documents, 1025);
        // id  title
        // 1   fixed
        fixture.source(
            "SELECT 1 AS id, 'fixed' AS title",
            "fn transform(row) { row }",
        );
        let report = success(fixture.command().output()?);
        assert_eq!(report["deleted_documents"], 1024);
        assert_eq!(report["indexed_documents"], 1);
    }
    Ok(())
}

#[tokio::test]
async fn failed_upload_task_does_not_start_cleanup() -> Result<()> {
    let Some(fixture) = Fixture::new().await else {
        return Ok(());
    };
    fixture.seed(&[json!({"id":"keep"})], "id").await;
    // Meilisearch rejects document IDs longer than 511 bytes asynchronously.
    fixture.source(
        "SELECT printf('%.*c', 512, 'x') AS id",
        "fn transform(row) { row }",
    );
    failure(
        fixture
            .command()
            .args(["--report", "report.json"])
            .output()?,
    );
    assert!(!fixture.dir.path().join("report.json").exists());
    assert_eq!(
        fixture.index.get_document::<Value>("keep").await?,
        json!({"id":"keep"})
    );
    assert_eq!(fixture.index.get_stats().await?.number_of_documents, 1);
    Ok(())
}

#[tokio::test]
async fn empty_sync_requires_explicit_permission() -> Result<()> {
    let Some(fixture) = Fixture::new().await else {
        return Ok(());
    };
    fixture.seed(&[json!({"id":1})], "id").await;
    for (sql, rhai) in [
        // no rows
        ("SELECT 1 AS id WHERE 0", "fn transform(row) { row }"),
        // id 1, dropped by the transform
        ("SELECT 1 AS id", "fn transform(row) { () }"),
    ] {
        fixture.source(sql, rhai);
        failure(fixture.command().output()?);
        assert_eq!(fixture.index.get_stats().await?.number_of_documents, 1);
    }
    let report = success(fixture.command().arg("--allow-empty").output()?);
    assert_eq!(report["documents"], 0);
    assert_eq!(report["deleted_documents"], 1);
    assert_eq!(report["indexed_documents"], 0);
    assert_eq!(fixture.index.get_stats().await?.number_of_documents, 0);
    Ok(())
}

#[tokio::test]
async fn custom_primary_key_must_match_before_mutation() -> Result<()> {
    let Some(fixture) = Fixture::new().await else {
        return Ok(());
    };
    fixture
        .seed(&[json!({"key":"old", "title":"unchanged"})], "key")
        .await;
    // id  title
    // 1   new
    fixture.source(
        "SELECT 1 AS id, 'new' AS title",
        "fn transform(row) { row }",
    );
    let before = serde_json::to_value(fixture.index.get_settings().await?)?;
    fs::write(
        fixture.dir.path().join("settings.json"),
        r#"{"searchableAttributes":["title"]}"#,
    )?;
    failure(
        fixture
            .command()
            .args(["--settings", "settings.json"])
            .output()?,
    );
    assert_eq!(
        serde_json::to_value(fixture.index.get_settings().await?)?,
        before
    );
    assert_eq!(fixture.index.get_stats().await?.number_of_documents, 1);
    assert_eq!(
        fixture.index.get_document::<Value>("old").await?["title"],
        "unchanged"
    );
    // key  title
    // new  updated
    fixture.source(
        "SELECT 'new' AS key, 'updated' AS title",
        "fn transform(row) { row }",
    );
    let report = success(
        fixture
            .command()
            .args(["--primary-key", "key", "--settings", "settings.json"])
            .output()?,
    );
    assert_eq!(report["deleted_documents"], 1);
    assert_eq!(report["indexed_documents"], 1);
    assert_eq!(
        fixture.index.get_document::<Value>("new").await?,
        json!({"key":"new", "title":"updated"})
    );
    let after = serde_json::to_value(fixture.index.get_settings().await?)?;
    assert_eq!(after["searchableAttributes"], json!(["title"]));
    Ok(())
}
