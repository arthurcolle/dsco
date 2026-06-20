-- Market & execution layer: turns priced deals into quotable two-way markets,
-- records the desk's axes, client RFQs, and actual traded prints (FINRA TRACE
-- Securitized Products). This is the bridge from model value to execution.

-- Desk axes: the levels the desk is showing on a class (its own inventory bias).
CREATE TABLE IF NOT EXISTS desk_axe (
    deal_key   TEXT NOT NULL,
    class_name TEXT NOT NULL,
    side       TEXT NOT NULL,        -- bid | offer (better buyer | better seller)
    level      REAL,                 -- price (per 100) or spread (bps) per unit
    unit       TEXT DEFAULT 'price', -- price | spread
    size_mm    REAL,                 -- offered/wanted size ($MM face)
    trader     TEXT,
    ts         TEXT DEFAULT (datetime('now')),
    PRIMARY KEY (deal_key, class_name, side)
);

-- Client / street RFQ log: a bid-wanted or offer-wanted into the desk.
CREATE TABLE IF NOT EXISTS rfq (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    deal_key   TEXT NOT NULL,
    class_name TEXT,
    side       TEXT,                 -- bid_wanted | offer_wanted
    size_mm    REAL,
    client     TEXT,
    quote      REAL,                 -- the level the desk responded with
    status     TEXT DEFAULT 'open',  -- open | quoted | done | passed
    ts         TEXT DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_rfq_deal ON rfq(deal_key);

-- Actual traded prints from FINRA TRACE (Securitized Products). Populated only
-- from real TRACE exports — never synthesized. Gives the desk realized levels to
-- mark against. exec_ts is the report time; price is per 100 face.
CREATE TABLE IF NOT EXISTS trace_print (
    cusip      TEXT,
    deal_key   TEXT,
    class_name TEXT,
    side       TEXT,                 -- B (buy) | S (sell) | D (dealer)
    price      REAL,
    yield      REAL,
    size_mm    REAL,
    exec_ts    TEXT,
    src        TEXT DEFAULT 'TRACE',
    ingested   TEXT DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_trace_deal  ON trace_print(deal_key);
CREATE INDEX IF NOT EXISTS idx_trace_cusip ON trace_print(cusip);
