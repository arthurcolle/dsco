-- CMS Open Payments — General Payments mirror.
-- Sourced from the Centers for Medicare & Medicaid Services public Open
-- Payments data at https://www.cms.gov/openpayments/data (annual files,
-- hundreds of MB to several GB each). Public-record basis: Affordable Care
-- Act § 6002 (the Physician Payments Sunshine Act) — every covered transfer
-- of value from drug / device manufacturers and GPOs to physicians and
-- teaching hospitals must be disclosed publicly. Nothing synthesized.
--
-- record_id is the CMS-published per-payment identifier.

CREATE TABLE IF NOT EXISTS cms_op_general (
    record_id                                            BIGINT PRIMARY KEY,
    covered_recipient_npi                                TEXT,    -- 10-digit NPI; joins to people/npi.db
    covered_recipient_first_name                         TEXT,
    covered_recipient_last_name                          TEXT,
    recipient_state                                      TEXT,
    applicable_manufacturer_or_applicable_gpo_name       TEXT,    -- joins to SEC filer corpus by name
    payment_date                                         TEXT,
    total_amount_of_payment                              REAL,    -- USD
    payment_form                                         TEXT,    -- "Cash or cash equivalent", "In-kind items", ...
    nature_of_payment                                    TEXT,    -- "Consulting Fee", "Speaker", "Food and Beverage", ...
    related_product_indicator                            TEXT,    -- Drug | Device | Biological | Medical Supply | None
    related_product_name                                 TEXT,
    program_year                                         INTEGER,
    ts                                                   TEXT DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_op_npi          ON cms_op_general(covered_recipient_npi);
CREATE INDEX IF NOT EXISTS idx_op_state        ON cms_op_general(recipient_state);
CREATE INDEX IF NOT EXISTS idx_op_year         ON cms_op_general(program_year);
CREATE INDEX IF NOT EXISTS idx_op_manufacturer ON cms_op_general(applicable_manufacturer_or_applicable_gpo_name);
CREATE INDEX IF NOT EXISTS idx_op_nature       ON cms_op_general(nature_of_payment);

-- Per-year progress / size table. Lets a refresh know what's already loaded.
CREATE TABLE IF NOT EXISTS cms_op_program_year (
    program_year         INTEGER PRIMARY KEY,
    n_records            BIGINT,
    total_amount_dollars REAL,
    last_refreshed       TEXT
);
