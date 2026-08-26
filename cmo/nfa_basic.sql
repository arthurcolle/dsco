-- NFA BASIC (Background Affiliation Status Information Center) mirror.
-- Sourced from the National Futures Association's public BASIC system at
-- https://www.nfa.futures.org/basicnet/ — the canonical public registry for
-- US futures / swaps / FCM / IB / CPO / CTA registrants. Public-record basis:
-- Commodity Exchange Act § 17 (NFA is the SRO under CFTC oversight; registration
-- and disciplinary status are statutorily public). Nothing synthesized.
-- nfa_id is NFA's own identifier, the stable join key.

-- Registered firms (FCMs, IBs, CPOs, CTAs, RFEDs, SDs).
CREATE TABLE IF NOT EXISTS nfa_firm (
    nfa_id              INTEGER PRIMARY KEY,
    name                TEXT,
    registration_type   TEXT,           -- FCM | IB | CPO | CTA | RFED | SD | ...
    status              TEXT,           -- Approved | Withdrawn | Suspended | ...
    address_city        TEXT,
    address_state       TEXT,
    registered_categories TEXT,         -- ; joined when a firm holds multiple categories
    ts                  TEXT DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_nfa_firm_name   ON nfa_firm(name);
CREATE INDEX IF NOT EXISTS idx_nfa_firm_type   ON nfa_firm(registration_type);
CREATE INDEX IF NOT EXISTS idx_nfa_firm_status ON nfa_firm(status);

-- Registered individuals (APs — associated persons — and principals).
CREATE TABLE IF NOT EXISTS nfa_individual (
    nfa_id                INTEGER PRIMARY KEY,
    name                  TEXT,
    status                TEXT,         -- Approved | Withdrawn | ...
    primary_firm_nfa_id   INTEGER,      -- current sponsoring firm
    registered_categories TEXT,         -- AP | Principal | Branch Manager ...
    disciplinary_actions  INTEGER,      -- count of reportable BASIC matters
    ts                    TEXT DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_nfa_ind_name ON nfa_individual(name);

-- Registration edges: who was at which firm and when. Mirrors bc_registration.
CREATE TABLE IF NOT EXISTS nfa_registration (
    individual_nfa_id INTEGER NOT NULL,
    firm_nfa_id       INTEGER NOT NULL,
    begin_date        TEXT NOT NULL,
    end_date          TEXT,
    current           INTEGER DEFAULT 0,
    PRIMARY KEY (individual_nfa_id, firm_nfa_id, begin_date)
);
CREATE INDEX IF NOT EXISTS idx_nfa_reg_firm ON nfa_registration(firm_nfa_id);
CREATE INDEX IF NOT EXISTS idx_nfa_reg_ind  ON nfa_registration(individual_nfa_id);
