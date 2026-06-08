-- SEC EDGAR deal-universe model.
-- Every structured-finance deal (RMBS/CMBS/ABS/agency CMO) is registered with the
-- SEC. The issuing entity (a trust) files a prospectus (424B*) at issuance and a
-- distribution report (10-D) every period thereafter, plus asset-level data
-- (ABS-EE). This captures the full filing universe so we can reverse-engineer who
-- issues what, on what cadence, and through which dealer shelf.

CREATE TABLE IF NOT EXISTS entity (
    cik         INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,        -- issuing entity / depositor as EDGAR records it
    dealer      TEXT,                 -- derived sponsor/shelf: GS, MS, JPM, WELLS ...
    asset_class TEXT,                 -- derived: CMBS | RMBS | ABS | AGENCY | OTHER
    first_filed TEXT,                 -- earliest filing date seen
    last_filed  TEXT                  -- latest filing date seen
);

CREATE TABLE IF NOT EXISTS filing (
    accession   TEXT PRIMARY KEY,     -- 0001888524-25-000053
    cik         INTEGER NOT NULL REFERENCES entity(cik),
    form        TEXT NOT NULL,        -- 424B5 | 10-D | ABS-EE | ABS-15G | FWP ...
    filed_date  TEXT NOT NULL,        -- YYYY-MM-DD
    path        TEXT                  -- edgar/data/CIK/accession.txt
);

CREATE INDEX IF NOT EXISTS idx_filing_cik   ON filing(cik);
CREATE INDEX IF NOT EXISTS idx_filing_form  ON filing(form);
CREATE INDEX IF NOT EXISTS idx_filing_date  ON filing(filed_date);
CREATE INDEX IF NOT EXISTS idx_entity_dealer ON entity(dealer);
CREATE INDEX IF NOT EXISTS idx_entity_class  ON entity(asset_class);
