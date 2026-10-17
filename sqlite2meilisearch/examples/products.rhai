fn transform(row) {
    if row.status != "published" {
        return ();
    }

    let title = row.title;
    if row.subtitle != () && row.subtitle != "" {
        title += " · " + row.subtitle;
    }

    let brand = row.brand_name;
    if brand == () || brand == "" {
        brand = "Unknown Brand";
    }

    let tags = [];
    for tag in json_parse(row.tags_json) {
        if !tags.contains(tag) {
            tags.push(tag);
        }
    }

    #{
        id: row.id,
        title: title,
        brand: #{ name: brand },
        tags: tags,
        price_cents: row.price_cents,
        price_range: if row.price_cents < 10000 { "budget" } else { "standard" }
    }
}
