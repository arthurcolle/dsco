-- Deal-contents model: the collateral agents buy and the bonds agents make.
-- EDGAR ABS-EE EX-102 gives loan-level asset data (the collateral tape); the
-- 424B5 prospectus gives the tranche stack + underwriting syndicate (the bonds).

-- Pool-level summary of one asset tape (what a trader prices to bid the pool).
CREATE TABLE IF NOT EXISTS collateral_pool (
    accession    TEXT PRIMARY KEY,
    cik          INTEGER,
    deal_name    TEXT,
    asset_type   TEXT,            -- autoloan | autolease | cmbs | rmbs | debt ...
    report_begin TEXT,
    report_end   TEXT,
    n_loans      INTEGER,
    orig_balance REAL,
    cur_balance  REAL,
    wac          REAL,            -- weighted-avg coupon (by current balance)
    wam          REAL,            -- weighted-avg remaining term (months)
    wa_fico      REAL,            -- weighted-avg obligor credit score
    wa_pti       REAL,            -- weighted-avg payment-to-income
    pct_new      REAL,            -- balance share of new vehicles
    pct_delinq   REAL,            -- balance share 30+ days delinquent
    top_state    TEXT,
    top_state_pct REAL
);

-- Loan-level rows (optional; large). One obligor loan per row.
CREATE TABLE IF NOT EXISTS collateral_loan (
    accession     TEXT NOT NULL,
    asset_number  TEXT NOT NULL,
    asset_type    TEXT,
    originator    TEXT,
    orig_balance  REAL,
    cur_balance   REAL,
    orig_rate     REAL,
    orig_term     INTEGER,
    rem_term      INTEGER,
    fico          INTEGER,
    pti           REAL,
    state         TEXT,
    vehicle_make  TEXT,
    vehicle_model TEXT,
    vehicle_year  INTEGER,
    new_used      INTEGER,        -- 1 new, 2 used
    delinquency   INTEGER,        -- days
    zero_balance_code INTEGER,
    PRIMARY KEY (accession, asset_number)
);
CREATE INDEX IF NOT EXISTS idx_loan_acc   ON collateral_loan(accession);
CREATE INDEX IF NOT EXISTS idx_loan_state ON collateral_loan(state);
CREATE INDEX IF NOT EXISTS idx_loan_fico  ON collateral_loan(fico);

-- Bond structure: one row per offered class in a deal (the tranche stack).
CREATE TABLE IF NOT EXISTS tranche (
    accession     TEXT NOT NULL,
    cik           INTEGER,
    deal_name     TEXT,
    class_name    TEXT NOT NULL,
    orig_balance  REAL,
    coupon        REAL,
    coupon_type   TEXT,           -- FIXED | FLOAT | WAC | variable
    rating        TEXT,
    wal           REAL,           -- weighted-avg life (years)
    credit_support REAL,          -- subordination / enhancement %
    PRIMARY KEY (accession, class_name)
);
CREATE INDEX IF NOT EXISTS idx_tranche_acc ON tranche(accession);

-- Underwriting syndicate per deal (the broker-dealers that placed the bonds).
CREATE TABLE IF NOT EXISTS underwriter (
    accession TEXT NOT NULL,
    name      TEXT NOT NULL,
    role      TEXT,               -- lead | co-manager
    PRIMARY KEY (accession, name)
);
CREATE INDEX IF NOT EXISTS idx_uw_name ON underwriter(name);
