-- Agency CMO / REMIC model.
-- A REMIC deal (series) re-tranches pass-through collateral into classes whose
-- principal/interest cashflows are carved by a waterfall. This schema captures
-- the issued universe (deal + class) plus monthly factors (the live balances).

CREATE TABLE IF NOT EXISTS deal (
    deal_id     TEXT PRIMARY KEY,   -- canonical series, e.g. GNMA-1994-001
    issuer      TEXT NOT NULL,      -- GNMA | FNMA | FHLMC
    series      TEXT,               -- issuer's series number (1994-001)
    year        INTEGER,
    n_classes   INTEGER DEFAULT 0,
    orig_balance REAL DEFAULT 0,    -- sum of class original balances
    first_seen  TEXT,               -- report month first observed (YYYYMM)
    UNIQUE(issuer, series)
);

CREATE TABLE IF NOT EXISTS class (
    cusip        TEXT PRIMARY KEY,
    deal_id      TEXT NOT NULL REFERENCES deal(deal_id),
    class_id     TEXT,              -- tranche letter(s): A, PK, Z, FA, S, IO ...
    class_type   TEXT,             -- derived: SEQ/PAC/TAC/SUP/Z/IO/PO/FLT/INV/RES
    coupon       REAL,
    orig_balance REAL,
    maturity     TEXT,              -- YYYYMMDD
    issue_date   TEXT,
    UNIQUE(deal_id, class_id)
);

-- One row per class per report month: the factor and the resulting balance.
CREATE TABLE IF NOT EXISTS factor (
    cusip        TEXT NOT NULL REFERENCES class(cusip),
    report_month TEXT NOT NULL,     -- YYYYMM
    factor       REAL,              -- fraction of original still outstanding
    cur_balance  REAL,
    PRIMARY KEY (cusip, report_month)
);

CREATE INDEX IF NOT EXISTS idx_class_deal   ON class(deal_id);
CREATE INDEX IF NOT EXISTS idx_class_type   ON class(class_type);
CREATE INDEX IF NOT EXISTS idx_factor_month ON factor(report_month);
CREATE INDEX IF NOT EXISTS idx_deal_issuer  ON deal(issuer);
