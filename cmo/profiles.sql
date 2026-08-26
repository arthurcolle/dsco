-- Dealer-map profiles over the EDGAR deal universe.
-- These views are the queryable substrate a mortgage/ABS trading agent reads:
-- who issues, in what asset class, on what monthly cadence, through which shelf.

-- One row per dealer shelf: deal count, asset-class reach, issuance window, and
-- the monthly distribution-report cadence (10-D) that proxies live deal count.
DROP VIEW IF EXISTS dealer_profile;
CREATE VIEW dealer_profile AS
SELECT
    e.dealer,
    COUNT(DISTINCT e.cik)                                   AS trusts,
    COUNT(DISTINCT e.asset_class)                           AS asset_classes,
    GROUP_CONCAT(DISTINCT e.asset_class)                    AS classes,
    SUM(CASE WHEN f.form='10-D'        THEN 1 ELSE 0 END)   AS monthly_reports,
    SUM(CASE WHEN f.form LIKE '424%'   THEN 1 ELSE 0 END)   AS prospectuses,
    SUM(CASE WHEN f.form='ABS-EE'      THEN 1 ELSE 0 END)   AS asset_exhibits,
    MIN(f.filed_date)                                       AS first_filed,
    MAX(f.filed_date)                                       AS last_filed
FROM entity e JOIN filing f ON f.cik = e.cik
WHERE e.asset_class IN ('CMBS','RMBS','ABS','AGENCY')
GROUP BY e.dealer;

-- One row per issuing entity (trust): its shelf, asset class, filing footprint,
-- and active window — the per-deal record an agent keys trades off.
DROP VIEW IF EXISTS deal_profile;
CREATE VIEW deal_profile AS
SELECT
    e.cik,
    e.name,
    e.dealer,
    e.asset_class,
    COUNT(*)                                               AS filings,
    SUM(CASE WHEN f.form='10-D' THEN 1 ELSE 0 END)         AS monthly_reports,
    e.first_filed,
    e.last_filed
FROM entity e JOIN filing f ON f.cik = e.cik
WHERE e.asset_class IN ('CMBS','RMBS','ABS','AGENCY')
GROUP BY e.cik;

-- Asset-class rollup: structural shape of the market (trust counts + cadence).
DROP VIEW IF EXISTS asset_class_profile;
CREATE VIEW asset_class_profile AS
SELECT
    e.asset_class,
    COUNT(DISTINCT e.cik)                                  AS trusts,
    COUNT(DISTINCT e.dealer)                               AS dealers,
    SUM(CASE WHEN f.form='10-D' THEN 1 ELSE 0 END)         AS monthly_reports,
    MIN(f.filed_date)                                      AS first_filed,
    MAX(f.filed_date)                                      AS last_filed
FROM entity e JOIN filing f ON f.cik = e.cik
WHERE e.asset_class IN ('CMBS','RMBS','ABS','AGENCY')
GROUP BY e.asset_class;
