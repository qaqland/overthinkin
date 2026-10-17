CREATE TABLE brands
(      id    INTEGER PRIMARY KEY
     , name  TEXT NOT NULL
);

CREATE TABLE products
(      id           INTEGER PRIMARY KEY
     , title        TEXT NOT NULL
     , subtitle     TEXT
     , price_cents  INTEGER NOT NULL
     , status       TEXT NOT NULL
     , brand_id     INTEGER REFERENCES brands(id)
);

CREATE TABLE tags
(      id    INTEGER PRIMARY KEY
     , name  TEXT NOT NULL
);

CREATE TABLE product_tags
(      product_id  INTEGER
     , tag_id      INTEGER
);

INSERT INTO brands
VALUES
    (1, 'Acme');

INSERT INTO products
VALUES
    (1, 'Phone', 'Black', 299900, 'published', 1),
    (2, 'USB-C Cable', NULL, 3900, 'published', NULL),
    (3, 'Draft Item', NULL, 1000, 'draft', 1);

INSERT INTO tags
VALUES
    (1, 'electronics'),
    (2, 'phone');

INSERT INTO product_tags
VALUES
    (1, 1),
    (1, 2),
    (1, 1);
