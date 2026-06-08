-- Agent-ready views over deal contents: the bond stack with computed credit
-- support, and the underwriter league table. These are what a structuring/trading
-- agent reads to size subordination and pick a syndicate.

-- Tranche stack with subordination. Cover order (rowid) is seniority: classes
-- listed later are more junior, so credit support for a class = the share of the
-- deal sitting below it (its loss-absorbing cushion).
DROP VIEW IF EXISTS tranche_stack;
CREATE VIEW tranche_stack AS
WITH deal AS (
    SELECT accession, SUM(orig_balance) AS deal_total
    FROM tranche GROUP BY accession
)
SELECT
    t.accession,
    t.deal_name,
    t.class_name,
    t.orig_balance,
    t.coupon,
    t.coupon_type,
    ROUND(t.orig_balance / d.deal_total, 4)                AS pct_of_deal,
    ROUND(COALESCE(SUM(t.orig_balance) OVER (
        PARTITION BY t.accession ORDER BY t.rowid
        ROWS BETWEEN 1 FOLLOWING AND UNBOUNDED FOLLOWING), 0)
        / d.deal_total, 4)                                 AS credit_support
FROM tranche t JOIN deal d ON d.accession = t.accession;

-- Underwriter league table: deal count + notional placed, split lead/co-manager.
DROP VIEW IF EXISTS underwriter_league;
CREATE VIEW underwriter_league AS
SELECT
    u.name,
    COUNT(DISTINCT u.accession)                                    AS deals,
    SUM(CASE WHEN u.role='lead' THEN 1 ELSE 0 END)                 AS lead_roles,
    SUM(CASE WHEN u.role='co-manager' THEN 1 ELSE 0 END)           AS co_roles,
    ROUND(SUM(dt.deal_total) / 1e6, 0)                             AS notional_touched_mm
FROM underwriter u
JOIN (SELECT accession, SUM(orig_balance) AS deal_total FROM tranche GROUP BY accession) dt
     ON dt.accession = u.accession
GROUP BY u.name;

-- Collateral screen: pools ranked for an agent deciding what to bid, with the
-- credit/quality signals a buyer cares about.
DROP VIEW IF EXISTS collateral_screen;
CREATE VIEW collateral_screen AS
SELECT
    deal_name,
    asset_type,
    n_loans,
    ROUND(cur_balance / 1e6, 1)   AS cur_mm,
    ROUND(wac * 100, 2)           AS wac_pct,
    wa_fico,
    wam                           AS wam_mo,
    ROUND(pct_new * 100, 0)       AS pct_new,
    ROUND(pct_delinq * 100, 1)    AS delinq_pct,
    top_state,
    ROUND(top_state_pct * 100, 0) AS top_state_pct,
    CASE WHEN wa_fico >= 720 THEN 'PRIME'
         WHEN wa_fico >= 660 THEN 'NEAR-PRIME'
         ELSE 'SUBPRIME' END      AS credit_tier
FROM collateral_pool;
