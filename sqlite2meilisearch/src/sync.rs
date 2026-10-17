use std::{
    collections::HashSet,
    env,
    future::Future,
    io::{self, Write},
    path::Path,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use anyhow::{Context, Result, bail, ensure};
use meilisearch_sdk::{
    client::Client,
    documents::DocumentsQuery,
    errors::{Error, ErrorCode},
    indexes::Index,
    settings::Settings,
    task_info::TaskInfo,
    tasks::Task,
};
use serde::Serialize;
use serde_json::Value;
use tempfile::NamedTempFile;

use crate::{
    cli::Cli,
    source::{Counts, Document, Documents, Source, read_text},
    transform::document_id,
};

const MAX_BATCH_DOCUMENTS: usize = 1024;
const MAX_BATCH_BYTES: usize = 8 * 1024 * 1024;
const REQUEST_TIMEOUT: Duration = Duration::from_secs(30);
const TASK_TIMEOUT: Duration = Duration::from_secs(3600);

pub async fn run(args: Cli) -> Result<()> {
    let index_uid = args
        .index
        .as_deref()
        .expect("--index is required without --preview");
    let started_at_unix = SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs();
    let started = Instant::now();
    let source = Source::open(&args.input)?;
    let sqlite_path = args
        .input
        .sqlite
        .canonicalize()
        .with_context(|| format!("resolve SQLite path {}", args.input.sqlite.display()))?;
    let mut statement = source.prepare()?;
    let mut documents =
        Documents::new(&mut statement, &source.transformer, &args.input.primary_key)?;
    let settings = args.settings.as_deref().map(load_settings).transpose()?;
    let report_file = args.report.as_deref().map(prepare_report).transpose()?;
    let mut next = documents.next()?;
    ensure!(
        next.is_some() || args.allow_empty,
        "no documents generated; use --allow-empty to permit clearing the target index"
    );

    let client = client_from_env()?;
    let index = target_index(&client, index_uid, &args.input.primary_key).await?;
    eprintln!("syncing {}; target must have no other writers", index_uid);

    if let Some(settings) = settings {
        let task = request(index.set_settings(&settings))
            .await
            .context("submit index settings")?;
        wait(&client, task, "apply settings").await?;
    }

    let mut batch = Batch::default();
    let mut batch_number = 0;
    while let Some(document) = next {
        if batch.would_overflow(&document) {
            batch_number += 1;
            upload(&client, &index, &batch, batch_number).await?;
            batch = Batch::default();
        }
        batch.push(document);
        next = documents.next()?;
    }
    if !batch.values.is_empty() {
        batch_number += 1;
        upload(&client, &index, &batch, batch_number).await?;
    }
    let deleted_documents =
        remove_stale(&client, &index, &args.input.primary_key, &documents.ids).await?;
    let stats = request(index.get_stats())
        .await
        .context("read final index stats")?;
    ensure!(
        stats.number_of_documents as u64 == documents.counts.documents,
        "document count mismatch: generated {}, indexed {}",
        documents.counts.documents,
        stats.number_of_documents
    );
    let report = Report {
        status: "succeeded",
        sqlite: sqlite_path.display().to_string(),
        index: index_uid,
        primary_key: &args.input.primary_key,
        counts: &documents.counts,
        deleted_documents,
        indexed_documents: stats.number_of_documents,
        started_at_unix,
        finished_at_unix: SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs(),
        elapsed_seconds: started.elapsed().as_secs_f64(),
    };
    let mut json = serde_json::to_vec_pretty(&report)?;
    json.push(b'\n');
    if let Some(file) = report_file {
        let path = args.report.as_deref().expect("prepared report has a path");
        publish_report(file, path, &json)
            .with_context(|| format!("publish success report {}", path.display()))?;
    } else {
        io::stdout()
            .lock()
            .write_all(&json)
            .context("write success report to stdout")?;
    }
    eprintln!(
        "ready: {} ({} documents, {} deleted); no live resources switched",
        index_uid, documents.counts.documents, deleted_documents
    );
    Ok(())
}

fn client_from_env() -> Result<Client> {
    let meili_url = match env::var("MEILI_URL") {
        Ok(url) => crate::cli::server_url(&url)
            .map_err(anyhow::Error::msg)
            .context("invalid MEILI_URL")?,
        Err(env::VarError::NotPresent) => bail!("MEILI_URL is not set"),
        Err(_) => bail!("MEILI_URL is not valid UTF-8"),
    };
    let api_key = match env::var("MEILI_KEY") {
        Ok(key) if key.is_empty() => None,
        Ok(key) => Some(crate::cli::api_key(&key).map_err(anyhow::Error::msg)?),
        Err(env::VarError::NotPresent) => None,
        Err(_) => bail!("MEILI_KEY is not valid UTF-8"),
    };
    Client::new(&meili_url, api_key.as_deref()).context("initialize Meilisearch client")
}

async fn target_index(client: &Client, uid: &str, primary_key: &str) -> Result<Index> {
    match request(client.get_index(uid)).await {
        Ok(index) => {
            ensure!(
                index.primary_key.as_deref() == Some(primary_key),
                "target primary key {:?} does not match {primary_key:?}; refusing to modify index",
                index.primary_key
            );
            Ok(index)
        }
        Err(error) if is_index_not_found(&error) => {
            eprintln!("creating candidate index {uid}");
            let task = request(client.create_index(uid, Some(primary_key)))
                .await
                .context("submit index creation")?;
            wait(client, task, "create index").await?;
            Ok(client.index(uid))
        }
        Err(error) => Err(error.context("check target index")),
    }
}

async fn request<T>(future: impl Future<Output = Result<T, Error>>) -> Result<T> {
    tokio::time::timeout(REQUEST_TIMEOUT, future)
        .await
        .context("Meilisearch request timed out; server state may be unknown")?
        .map_err(Into::into)
}

fn is_index_not_found(error: &anyhow::Error) -> bool {
    matches!(
            error.downcast_ref::<Error>(),
            Some(Error::Meilisearch(e)) if e.error_code == ErrorCode::IndexNotFound
    )
}

async fn wait(client: &Client, info: TaskInfo, stage: &str) -> Result<()> {
    let uid = info.task_uid;
    eprintln!("{stage}: task {uid}");
    // See https://github.com/meilisearch/meilisearch-rust/issues/809
    let result = tokio::time::timeout(
        TASK_TIMEOUT,
        info.wait_for_completion(
            client,
            Some(Duration::from_millis(250)),
            Some(Duration::MAX),
        ),
    )
    .await
    .with_context(|| format!("{stage}: task {uid} timed out; server state may be unknown"))?
    .with_context(|| format!("{stage}: wait for task {uid}"))?;
    match result {
        Task::Succeeded { .. } => Ok(()),
        Task::Failed { content } => bail!("{stage}: task {uid} failed: {}", content.error),
        _ => bail!("{stage}: task {uid} did not succeed"),
    }
}

async fn upload(client: &Client, index: &Index, batch: &Batch, number: u64) -> Result<()> {
    let stage = format!(
        "batch {number} ({} documents, {} bytes)",
        batch.values.len(),
        batch.bytes
    );
    let task = request(index.add_or_replace(&batch.values, None))
        .await
        .with_context(|| format!("submit {stage}"))?;
    wait(client, task, &stage).await
}

async fn remove_stale(
    client: &Client,
    index: &Index,
    primary_key: &str,
    ids: &HashSet<String>,
) -> Result<u64> {
    let mut stale = Vec::new();
    let mut offset = 0;
    let mut retained = 0;
    loop {
        let mut query = DocumentsQuery::new(index);
        query
            .with_fields([primary_key])
            .with_limit(MAX_BATCH_DOCUMENTS)
            .with_offset(offset);
        let page = request(index.get_documents_with::<Value>(&query))
            .await
            .context("enumerate target documents before cleanup")?;
        if page.results.is_empty() {
            break;
        }
        offset += page.results.len();
        for document in page.results {
            let id = document_id(&document, primary_key)
                .context("read target primary key before cleanup")?;
            if ids.contains(&id) {
                retained += 1;
            } else {
                stale.push(id);
            }
        }
        if offset >= page.total as usize {
            break;
        }
    }
    ensure!(
        retained == ids.len(),
        "target is missing synchronized documents: expected {}, found {}; refusing cleanup",
        ids.len(),
        retained
    );
    // Finish enumeration before deleting: deletions would shift subsequent page offsets.
    for (number, batch) in stale.chunks(MAX_BATCH_DOCUMENTS).enumerate() {
        let stage = format!("delete batch {} ({} documents)", number + 1, batch.len());
        let task = request(index.delete_documents(batch))
            .await
            .with_context(|| format!("submit {stage}"))?;
        wait(client, task, &stage).await?;
    }
    Ok(stale.len() as u64)
}

struct Batch {
    values: Vec<Value>,
    bytes: usize,
}

impl Default for Batch {
    fn default() -> Self {
        Self {
            values: Vec::new(),
            bytes: 2,
        }
    }
}

impl Batch {
    fn would_overflow(&self, document: &Document) -> bool {
        self.values.len() >= MAX_BATCH_DOCUMENTS
            || self.bytes + document.bytes + usize::from(!self.values.is_empty()) > MAX_BATCH_BYTES
    }

    fn push(&mut self, document: Document) {
        self.bytes += document.bytes + usize::from(!self.values.is_empty());
        self.values.push(document.value);
    }
}

fn load_settings(path: &Path) -> Result<Settings> {
    let text = read_text(path)?;
    let mut unknown = Vec::new();
    let mut deserializer = serde_json::Deserializer::from_str(&text);
    let settings =
        serde_ignored::deserialize(&mut deserializer, |path| unknown.push(path.to_string()))
            .with_context(|| format!("parse settings {}", path.display()))?;
    deserializer
        .end()
        .with_context(|| format!("trailing content in settings {}", path.display()))?;
    ensure!(
        unknown.is_empty(),
        "unknown or SDK-unsupported settings in {}: {}",
        path.display(),
        unknown.join(", ")
    );
    Ok(settings)
}

fn prepare_report(path: &Path) -> Result<NamedTempFile> {
    ensure!(
        !path
            .try_exists()
            .with_context(|| format!("check report path {}", path.display()))?
            && !path.is_symlink(),
        "report path {} already exists; refusing to overwrite",
        path.display()
    );
    let parent = path
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    NamedTempFile::new_in(parent).with_context(|| format!("prepare report in {}", parent.display()))
}

fn publish_report(mut file: NamedTempFile, path: &Path, json: &[u8]) -> Result<()> {
    file.write_all(json).context("write success report")?;
    file.as_file().sync_all().context("sync success report")?;
    file.persist_noclobber(path)
        .context("publish success report without overwriting")?;
    Ok(())
}

#[derive(Serialize)]
struct Report<'a> {
    status: &'static str,
    sqlite: String,
    index: &'a str,
    primary_key: &'a str,
    #[serde(flatten)]
    counts: &'a Counts,
    deleted_documents: u64,
    indexed_documents: usize,
    started_at_unix: u64,
    finished_at_unix: u64,
    elapsed_seconds: f64,
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use std::fs;

    #[test]
    fn batch_limits_include_array_and_commas() {
        let value = json!({"id":"p_1","title":"é\""});
        let bytes = serde_json::to_vec(&value).unwrap().len();
        let mut batch = Batch::default();
        for _ in 0..MAX_BATCH_DOCUMENTS {
            let doc = Document {
                value: value.clone(),
                bytes,
            };
            assert!(!batch.would_overflow(&doc));
            batch.push(doc);
        }
        assert_eq!(
            batch.bytes,
            serde_json::to_vec(&batch.values).unwrap().len()
        );
        assert!(batch.would_overflow(&Document {
            value: value.clone(),
            bytes
        }));
        let mut batch = Batch::default();
        let max = Document {
            value: Value::Null,
            bytes: MAX_BATCH_BYTES - 2,
        };
        assert!(!batch.would_overflow(&max));
        batch.push(max);
        assert!(batch.would_overflow(&Document { value, bytes }));
    }

    #[test]
    fn settings_reject_unknown_fields() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("settings.json");
        fs::write(&path, r#"{"searchableAttributes":["title"]}"#).unwrap();
        assert!(load_settings(&path).is_ok());
        for (text, message) in [
            (
                r#"{"searchableAttribute":["title"]}"#,
                "searchableAttribute",
            ),
            (
                r#"{"pagination":{"maxTotalHits":1,"maxTotalHit":1}}"#,
                "unknown or SDK-unsupported settings",
            ),
        ] {
            fs::write(&path, text).unwrap();
            let error = format!("{:#}", load_settings(&path).unwrap_err());
            assert!(error.contains(message), "{text}: {error}");
        }
    }

    #[test]
    fn report_never_overwrites() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("report.json");
        let pending = prepare_report(&path).unwrap();
        fs::write(&path, "existing data").unwrap();
        assert!(prepare_report(&path).is_err());
        let error = publish_report(pending, &path, b"new report").unwrap_err();
        assert!(
            error.to_string().contains("without overwriting"),
            "{error:#}"
        );
        assert_eq!(fs::read_to_string(path).unwrap(), "existing data");
    }
}
