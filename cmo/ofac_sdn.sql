-- Treasury OFAC Specially Designated Nationals (SDN) List mirror.
-- Sourced from Treasury OFAC's public sanctions downloads (legacy CSV trio:
-- sdn.csv + sdn_alt.csv + sdn_add.csv). Public under 31 CFR § 501, Executive
-- Orders 12947 / 13224 / 13694, and the SDN List itself — never synthesized.
-- ent_num is OFAC's internal entity number and is the stable join key.

-- Primary SDN entity record. One row per sanctioned person/entity/vessel.
CREATE TABLE IF NOT EXISTS ofac_sdn (
    ent_num     INTEGER PRIMARY KEY,   -- OFAC entity number (the integer we walk)
    sdn_name    TEXT,                  -- name as published (LAST, FIRST or org)
    sdn_type    TEXT,                  -- Individual | Entity | Vessel | Aircraft
    program     TEXT,                  -- sanctions program(s); ; joined when multi
    title       TEXT,
    call_sign   TEXT,                  -- vessels only
    vessel_type TEXT,
    tonnage     TEXT,
    grt         TEXT,
    vessel_flag TEXT,
    vessel_owner TEXT,
    remarks     TEXT,
    ts          TEXT DEFAULT (datetime('now'))
);
CREATE INDEX IF NOT EXISTS idx_ofac_sdn_name    ON ofac_sdn(sdn_name);
CREATE INDEX IF NOT EXISTS idx_ofac_sdn_program ON ofac_sdn(program);
CREATE INDEX IF NOT EXISTS idx_ofac_sdn_type    ON ofac_sdn(sdn_type);

-- Aliases / a.k.a. / former names for an SDN entity. OFAC numbers them per-entity.
CREATE TABLE IF NOT EXISTS ofac_alt (
    ent_num     INTEGER NOT NULL,
    alt_num     INTEGER NOT NULL,
    alt_type    TEXT,                  -- aka | fka | nka
    alt_name    TEXT,
    alt_remarks TEXT,
    PRIMARY KEY (ent_num, alt_num)
);
CREATE INDEX IF NOT EXISTS idx_ofac_alt_name ON ofac_alt(alt_name);

-- Addresses associated with an SDN entity. Per-address numbered by OFAC.
CREATE TABLE IF NOT EXISTS ofac_address (
    ent_num             INTEGER NOT NULL,
    add_num             INTEGER NOT NULL,
    address             TEXT,
    city_state_province TEXT,
    postal_code         TEXT,
    country             TEXT,
    add_remarks         TEXT,
    PRIMARY KEY (ent_num, add_num)
);
CREATE INDEX IF NOT EXISTS idx_ofac_addr_country ON ofac_address(country);
