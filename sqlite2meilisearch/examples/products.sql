SELECT CAST(p.id AS TEXT) AS id
     , p.title
     , p.subtitle
     , p.price_cents
     , p.status
     , b.name AS brand_name
     , COALESCE((
           SELECT json_group_array(t.name)
             FROM product_tags AS pt
             JOIN tags AS t
               ON t.id = pt.tag_id
            WHERE pt.product_id = p.id
       ), '[]') AS tags_json
  FROM products AS p
  LEFT JOIN brands AS b
    ON b.id = p.brand_id
 ORDER BY p.id;
