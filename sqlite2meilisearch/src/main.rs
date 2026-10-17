mod cli;
mod source;
mod sync;
mod transform;

use std::{
    io::{self, BufWriter, Write},
    process::ExitCode,
};

use anyhow::{Context, Result};
use clap::Parser;

use crate::{
    cli::{Cli, Input},
    source::{Documents, Source},
};

#[tokio::main(flavor = "current_thread")]
async fn main() -> ExitCode {
    let cli = Cli::parse();
    let result = if cli.preview {
        preview(&cli.input)
    } else {
        sync::run(cli)
            .await
            .context("synchronization failed; target may be partially modified, do not publish")
    };
    if let Err(error) = result {
        eprintln!("error: {error:#}");
        ExitCode::FAILURE
    } else {
        ExitCode::SUCCESS
    }
}

fn preview(input: &Input) -> Result<()> {
    let source = Source::open(input)?;
    let mut statement = source.prepare()?;
    let mut documents = Documents::new(&mut statement, &source.transformer, &input.primary_key)?;
    let mut output = BufWriter::new(io::stdout().lock());
    if let Some(document) = documents.next()? {
        serde_json::to_writer(&mut output, &document.value).context("write preview JSON")?;
        writeln!(output).context("write preview newline")?;
    }
    output.flush().context("flush preview output")?;
    eprintln!(
        "preview: {} rows read, {} skipped, {} documents; only inspected rows were validated",
        documents.counts.rows_read, documents.counts.rows_skipped, documents.counts.documents
    );
    Ok(())
}
