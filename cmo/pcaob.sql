-- PCAOB Registered Public Accounting Firms mirror.
-- Sourced from PCAOB's public registration page (https://pcaobus.org/about/
-- administration/registration/), which publishes the full current list as CSV.
-- Public-record basis: Sarbanes-Oxley Act § 102 — any firm preparing audit
-- reports for SEC-registered issuers must register with the PCAOB, and that
-- registration list is public. Nothing synthesized.
-- firm_id is the PCAOB Firm ID; the integer is the stable join key.

CREATE TABLE IF NOT EXISTS pcaob_firm (
    firm_id            INTEGER PRIMARY KEY,
    name               TEXT,
    country            TEXT,                 -- HQ country
    status             TEXT,                 -- Registered | Withdrawn | ...
    registration_date  TEXT,
    scope              TEXT,                 -- Issuer | Broker-Dealer | Both | None
    headquarters_city  TEXT,
    headquarters_state TEXT,
    ts                 TEXT DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_pcaob_firm_name    ON pcaob_firm(name);
CREATE INDEX IF NOT EXISTS idx_pcaob_firm_country ON pcaob_firm(country);
CREATE INDEX IF NOT EXISTS idx_pcaob_firm_status  ON pcaob_firm(status);

-- Per-year inspection report metadata. Populated by --refresh-inspections,
-- which scrapes https://pcaobus.org/oversight/inspections/firm-inspection-reports.
CREATE TABLE IF NOT EXISTS pcaob_inspection (
    firm_id         INTEGER NOT NULL,
    inspection_year INTEGER NOT NULL,
    report_url      TEXT,
    defects_found   INTEGER,                 -- count of audit deficiencies
    PRIMARY KEY (firm_id, inspection_year)
);
CREATE INDEX IF NOT EXISTS idx_pcaob_insp_year ON pcaob_inspection(inspection_year);
