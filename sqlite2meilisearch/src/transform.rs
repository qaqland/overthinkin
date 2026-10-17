use anyhow::{Context, Result, anyhow, bail, ensure};
use rhai::{AST, Array, Dynamic, Engine, EvalAltResult, ImmutableString, Map, Scope};
use serde_json::{Number, Value};

pub const MAX_DOCUMENT_BYTES: usize = 8 * 1024 * 1024 - 2;
const MAX_DEPTH: usize = 64;

pub struct Transformer {
    engine: Engine,
    ast: AST,
}

impl Transformer {
    pub fn compile(script: &str) -> Result<Self> {
        let mut engine = Engine::new();
        engine.disable_symbol("import");
        engine.disable_symbol("eval");
        engine.set_max_operations(1_000_000);
        engine.set_max_call_levels(32);
        engine.set_max_expr_depths(64, 32);
        engine.set_max_string_size(MAX_DOCUMENT_BYTES);
        engine.set_max_array_size(100_000);
        engine.set_max_map_size(10_000);
        engine.on_print(|text| eprintln!("[rhai] {text}"));
        engine.on_debug(|text, _, position| eprintln!("[rhai {position}] {text}"));
        engine.register_fn("json_parse", json_parse);
        let ast = engine.compile(script).map_err(|e| anyhow!("{e}"))?;
        ensure!(
            ast.iter_functions()
                .any(|f| f.name == "transform" && f.params.len() == 1),
            "script must define fn transform(row)"
        );
        Ok(Self { engine, ast })
    }

    pub fn apply(&self, row: Map) -> Result<Option<Value>> {
        let result = self
            .engine
            .call_fn::<Dynamic>(&mut Scope::new(), &self.ast, "transform", (row,))
            .map_err(|e| anyhow!("{e}"))?;
        if result.is_unit() {
            return Ok(None);
        }
        ensure!(
            result.is::<Map>(),
            "transform must return a Map or (), got {}",
            result.type_name()
        );
        dynamic_to_json(result, 0).map(Some)
    }
}

fn json_parse(text: &str) -> Result<Dynamic, Box<EvalAltResult>> {
    let result = (|| {
        ensure!(
            text.len() <= MAX_DOCUMENT_BYTES,
            "JSON input exceeds document size limit"
        );
        let value: Value = serde_json::from_str(text).context("invalid JSON")?;
        json_to_dynamic(value, 0)
    })();
    result.map_err(|e| format!("json_parse: {e:#}").into())
}

fn json_to_dynamic(value: Value, depth: usize) -> Result<Dynamic> {
    ensure!(depth <= MAX_DEPTH, "JSON nesting exceeds {MAX_DEPTH}");
    Ok(match value {
        Value::Null => Dynamic::UNIT,
        Value::Bool(v) => v.into(),
        Value::String(v) => v.into(),
        Value::Number(v) => {
            if let Some(i) = v.as_i64() {
                i.into()
            } else if v.is_f64() {
                let f = v
                    .as_f64()
                    .filter(|f| f.is_finite())
                    .context("non-finite JSON number")?;
                f.into()
            } else {
                bail!("JSON integer outside signed 64-bit range; encode it as a string");
            }
        }
        Value::Array(values) => {
            ensure!(values.len() <= 100_000, "JSON array is too large");
            values
                .into_iter()
                .map(|v| json_to_dynamic(v, depth + 1))
                .collect::<Result<Array>>()?
                .into()
        }
        Value::Object(values) => {
            ensure!(values.len() <= 10_000, "JSON object is too large");
            values
                .into_iter()
                .map(|(k, v)| Ok((k.into(), json_to_dynamic(v, depth + 1)?)))
                .collect::<Result<Map>>()?
                .into()
        }
    })
}

fn dynamic_to_json(value: Dynamic, depth: usize) -> Result<Value> {
    ensure!(depth <= MAX_DEPTH, "document nesting exceeds {MAX_DEPTH}");
    let value = value.flatten();
    Ok(if value.is_unit() {
        Value::Null
    } else if value.is::<bool>() {
        Value::Bool(value.cast())
    } else if value.is::<i64>() {
        Value::Number(value.cast::<i64>().into())
    } else if value.is::<f64>() {
        Value::Number(Number::from_f64(value.cast()).context("non-finite number in document")?)
    } else if value.is::<ImmutableString>() {
        Value::String(value.cast::<ImmutableString>().into())
    } else if value.is::<Array>() {
        Value::Array(
            value
                .cast::<Array>()
                .into_iter()
                .map(|v| dynamic_to_json(v, depth + 1))
                .collect::<Result<_>>()?,
        )
    } else if value.is::<Map>() {
        Value::Object(
            value
                .cast::<Map>()
                .into_iter()
                .map(|(k, v)| Ok((k.into(), dynamic_to_json(v, depth + 1)?)))
                .collect::<Result<_>>()?,
        )
    } else {
        bail!("unsupported JSON value type: {}", value.type_name());
    })
}

pub fn document_id(document: &Value, primary_key: &str) -> Result<String> {
    let value = document
        .get(primary_key)
        .with_context(|| format!("missing primary key {primary_key:?}"))?;
    let id = match value {
        Value::String(id) => id.clone(),
        Value::Number(n) if n.is_i64() || n.is_u64() => n.to_string(),
        _ => bail!("primary key {primary_key:?} must be a string or integer"),
    };
    ensure!(
        !id.is_empty()
            && id
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_'),
        "invalid primary key {primary_key:?}: {id:?}; use ASCII letters, digits, '-' or '_'"
    );
    Ok(id)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn run(script: &str) -> Result<Option<Value>> {
        Transformer::compile(script)?.apply(Map::new())
    }

    #[test]
    fn nested_json_and_nulls() {
        let value = run(r#"
        fn transform(row) {
            let doc = json_parse(`
            {
                "id": "p_1",
                "tags": ["a", null],
                "nested": {
                    "n": 9223372036854775807,
                    "ratio": 1.5,
                    "active": true
                }
            }`);
            doc.extra = ();
            doc
        }"#);
        assert_eq!(
            value.unwrap().unwrap(),
            json!({
                "id":"p_1",
                "tags": ["a", null],
                "nested": {
                    "n": i64::MAX,
                    "ratio": 1.5,
                    "active": true
                },
                "extra": null
            })
        );
    }

    #[test]
    fn primary_keys() {
        let cases = [
            (json!({}), "id", None), // missing key
            (json!({"id": null}), "id", None),
            (json!({"id": ""}), "id", None),
            (json!({"id": true}), "id", None),
            (json!({"id": 1.5}), "id", None),
            (json!({"id": "a/b"}), "id", None), // '/' is not allowed
            (json!({"id": "é"}), "id", None),   // non-ASCII
            (json!({"id": -42}), "id", Some("-42")),
            (json!({"key": "p_1"}), "key", Some("p_1")),
        ];
        for (doc, key, expected) in cases {
            assert_eq!(document_id(&doc, key).ok().as_deref(), expected, "{doc}");
        }
    }
}
