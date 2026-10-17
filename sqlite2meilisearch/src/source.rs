use std::{collections::HashSet, fs, path::Path};

use anyhow::{Context, Result, bail, ensure};
use rhai::{Dynamic, Map};
use rusqlite::{Connection, OpenFlags, Row, Rows, Statement, types::ValueRef};
use serde::Serialize;
use serde_json::Value;

use crate::{
    cli::Input,
    transform::{MAX_DOCUMENT_BYTES, Transformer, document_id},
};

pub struct Source {
    connection: Connection,
    sql: String,
    pub transformer: Transformer,
}

impl Source {
    pub fn open(input: &Input) -> Result<Self> {
        let sql = read_text(&input.query)?;
        let transformer = match &input.transform {
            Some(path) => Transformer::compile(&read_text(path)?)
                .with_context(|| format!("compile {}", path.display()))?,
            None => Transformer::compile("fn transform(row) { row }")?,
        };
        let connection = Connection::open_with_flags(
            &input.sqlite,
            OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX,
        )
        .with_context(|| format!("open SQLite {} read-only", input.sqlite.display()))?;
        Ok(Self {
            connection,
            sql,
            transformer,
        })
    }

    pub fn prepare(&self) -> Result<Statement<'_>> {
        let statement = self
            .connection
            .prepare(&self.sql)
            .context("prepare read-only SQL query")?;
        ensure!(
            statement.readonly() && statement.column_count() > 0,
            "SQL must be one read-only query returning columns"
        );
        ensure!(
            statement.parameter_count() == 0,
            "SQL query must not contain unbound parameters"
        );
        let names = statement.column_names();
        let mut seen = HashSet::new();
        ensure!(
            names.iter().all(|name| seen.insert(name)),
            "duplicate SQL column names; give each column a unique alias"
        );
        Ok(statement)
    }
}

pub fn read_text(path: &Path) -> Result<String> {
    fs::read_to_string(path).with_context(|| format!("read {}", path.display()))
}

#[derive(Default, Serialize)]
pub struct Counts {
    pub rows_read: u64,
    pub rows_skipped: u64,
    pub documents: u64,
}

pub struct Document {
    pub value: Value,
    pub bytes: usize,
}

pub struct Documents<'a> {
    rows: Rows<'a>,
    columns: Vec<String>,
    transformer: &'a Transformer,
    primary_key: &'a str,
    // shortcut: keep IDs in memory, use a disk-backed set if large datasets exhaust memory.
    pub ids: HashSet<String>,
    pub counts: Counts,
}

impl<'a> Documents<'a> {
    pub fn new(
        statement: &'a mut Statement<'_>,
        transformer: &'a Transformer,
        primary_key: &'a str,
    ) -> Result<Self> {
        let columns: Vec<String> = statement
            .column_names()
            .iter()
            .map(|s| s.to_string())
            .collect();
        Ok(Self {
            rows: statement.query([]).context("start SQL query")?,
            columns,
            transformer,
            primary_key,
            ids: HashSet::new(),
            counts: Counts::default(),
        })
    }

    pub fn next(&mut self) -> Result<Option<Document>> {
        loop {
            let row = self
                .rows
                .next()
                .with_context(|| format!("read SQL row {}", self.counts.rows_read + 1))?;
            let Some(row) = row else { return Ok(None) };
            self.counts.rows_read += 1;
            let input = row_to_map(row, &self.columns)
                .with_context(|| format!("decode SQL row {}", self.counts.rows_read))?;
            let converted = self
                .transformer
                .apply(input)
                .with_context(|| format!("transform SQL row {}", self.counts.rows_read))?;
            let Some(value) = converted else {
                self.counts.rows_skipped += 1;
                continue;
            };
            let id = document_id(&value, self.primary_key)
                .with_context(|| format!("validate SQL row {}", self.counts.rows_read))?;
            ensure!(
                self.ids.insert(id.clone()),
                "SQL row {}: duplicate document primary key {id:?}",
                self.counts.rows_read
            );
            let bytes = serde_json::to_vec(&value)
                .context("serialize document")?
                .len();
            ensure!(
                bytes <= MAX_DOCUMENT_BYTES,
                "SQL row {}: document {id:?} is too large: {bytes} bytes (max {MAX_DOCUMENT_BYTES})",
                self.counts.rows_read
            );
            self.counts.documents += 1;
            return Ok(Some(Document { value, bytes }));
        }
    }
}

fn row_to_map(row: &Row<'_>, columns: &[String]) -> Result<Map> {
    let mut input = Map::new();
    for (i, name) in columns.iter().enumerate() {
        let value = match row
            .get_ref(i)
            .with_context(|| format!("read column {name:?}"))?
        {
            ValueRef::Null => Dynamic::UNIT,
            ValueRef::Integer(v) => v.into(),
            ValueRef::Real(v) => {
                ensure!(
                    v.is_finite(),
                    "column {name:?} contains a non-finite number"
                );
                v.into()
            }
            ValueRef::Text(v) => {
                ensure!(
                    v.len() <= MAX_DOCUMENT_BYTES,
                    "column {name:?} exceeds size limit"
                );
                std::str::from_utf8(v)
                    .with_context(|| format!("column {name:?} is not UTF-8"))?
                    .to_owned()
                    .into()
            }
            ValueRef::Blob(_) => {
                bail!("column {name:?} is a BLOB; convert it explicitly in SQL")
            }
        };
        input.insert(name.into(), value);
    }
    Ok(input)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reject_duplicate_document_ids() {
        // SQLite treats 1 and '1' as distinct values, but both become the
        // Meilisearch document ID "1": uploading both would silently overwrite.
        let source = Source {
            connection: Connection::open_in_memory().unwrap(),
            sql: "SELECT 1 AS id UNION ALL SELECT '1' AS id".into(),
            transformer: Transformer::compile("fn transform(row) { row }").unwrap(),
        };
        let mut statement = source.prepare().unwrap();
        let mut rows = Documents::new(&mut statement, &source.transformer, "id").unwrap();

        assert!(rows.next().unwrap().is_some(), "first row is accepted");
        assert!(rows.next().is_err(), "duplicate id is rejected");
    }
}
