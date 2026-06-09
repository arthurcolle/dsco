-- FINRA BrokerCheck mirror: real registered broker-dealer firms and the
-- individuals registered with them. Sourced only from FINRA's public BrokerCheck
-- API (FINRA Rule 8312 public data) — never synthesized. CRDs are FINRA's own
-- Central Registration Depository numbers and are the stable join key here.

-- Broker-dealer / investment-adviser firms, keyed by FINRA firm CRD.
CREATE TABLE IF NOT EXISTS bc_firm (
    crd          INTEGER PRIMARY KEY,   -- FINRA firm CRD (the integer we walk)
    name         TEXT,
    other_names  TEXT,                  -- DBA / former names, ; joined
    sec_number   TEXT,                  -- e.g. 8-41342
    bd_scope     TEXT,                  -- ACTIVE | INACTIVE | NULL (broker-dealer)
    ia_scope     TEXT,                  -- ACTIVE | INACTIVE | NULL (adviser)
    branches     INTEGER,
    roster_total INTEGER,               -- active individuals reported by the API
    rostered     INTEGER DEFAULT 0,     -- 1 once we've pulled this firm's brokers
    src          TEXT DEFAULT 'crawl',  -- crawl | search | underwriter
    ts           TEXT DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_bc_firm_name ON bc_firm(name);

-- Registered individuals, keyed by FINRA individual CRD.
CREATE TABLE IF NOT EXISTS bc_broker (
    crd          INTEGER PRIMARY KEY,   -- FINRA individual CRD
    first_name   TEXT,
    middle_name  TEXT,
    last_name    TEXT,
    scope        TEXT,                  -- Active | InActive
    detailed     INTEGER DEFAULT 0,     -- 1 once full registration history pulled
    ts           TEXT DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_bc_broker_last ON bc_broker(last_name);

-- Employment / registration edges: which broker was at which firm and when.
-- One row per (broker, firm, begin) so re-registrations are kept distinct.
CREATE TABLE IF NOT EXISTS bc_registration (
    broker_crd INTEGER NOT NULL,
    firm_crd   INTEGER,
    firm_name  TEXT,
    begin_date TEXT,
    end_date   TEXT,                    -- NULL while current
    current    INTEGER DEFAULT 0,
    PRIMARY KEY (broker_crd, firm_crd, begin_date)
);
CREATE INDEX IF NOT EXISTS idx_bc_reg_firm   ON bc_registration(firm_crd);
CREATE INDEX IF NOT EXISTS idx_bc_reg_broker ON bc_registration(broker_crd);

-- Resume markers for the integer CRD walk (so a crawl is restartable).
CREATE TABLE IF NOT EXISTS bc_progress (
    kind     TEXT PRIMARY KEY,          -- e.g. 'firm_walk'
    last_crd INTEGER,
    ts       TEXT DEFAULT (datetime('now'))
);
