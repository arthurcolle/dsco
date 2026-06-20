-- HHS OIG List of Excluded Individuals/Entities (LEIE) mirror.
-- Sourced from the Office of Inspector General's monthly-updated public file at
-- https://oig.hhs.gov/exclusions/exclusions_list.asp (CSV at /downloadables/
-- UPDATED.csv). Public-record basis: Social Security Act § 1128 — anyone
-- excluded from federal healthcare programs is identified on the public LEIE
-- by name, address, and (where known) NPI. Nothing synthesized.
-- exclusion_id is OIG's published identifier.

CREATE TABLE IF NOT EXISTS oig_exclusion (
    exclusion_id   TEXT PRIMARY KEY,
    last_name      TEXT,
    first_name     TEXT,
    midname        TEXT,
    business_name  TEXT,
    general        TEXT,            -- general practice / specialty bucket
    specialty      TEXT,
    upin           TEXT,            -- Unique Provider Identification Number (legacy)
    npi            TEXT,            -- 10-digit NPI; joins to people/npi.db
    dob            TEXT,
    address        TEXT,
    city           TEXT,
    state          TEXT,
    zip            TEXT,
    exclusion_type TEXT,            -- 1128a1, 1128b4, ...
    exclusion_date TEXT,
    reinstate_date TEXT,
    waiver_date    TEXT,
    waiver_state   TEXT,
    ts             TEXT DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_oig_npi    ON oig_exclusion(npi);
CREATE INDEX IF NOT EXISTS idx_oig_state  ON oig_exclusion(state);
CREATE INDEX IF NOT EXISTS idx_oig_type   ON oig_exclusion(exclusion_type);
CREATE INDEX IF NOT EXISTS idx_oig_lname  ON oig_exclusion(last_name);
CREATE INDEX IF NOT EXISTS idx_oig_bname  ON oig_exclusion(business_name);
