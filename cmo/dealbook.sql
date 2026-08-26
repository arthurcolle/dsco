-- deal_book: one row per securitization, stitching the collateral tape, the bond
-- stack, and the lead underwriter together via the canonical deal_key. This is
-- the top-level object an agent reasons over: "what's in the pool, what bonds it
-- backs, who placed them." Built from deal_identity (the cross-form join key).

DROP VIEW IF EXISTS deal_book;
CREATE VIEW deal_book AS
WITH coll AS (              -- collateral side: pool economics keyed by deal
    SELECT i.deal_key,
           MAX(p.asset_type)               AS asset_type,
           SUM(p.n_loans)                  AS n_loans,
           ROUND(SUM(p.cur_balance)/1e6,1) AS cur_mm,
           ROUND(SUM(p.wac*p.cur_balance)/NULLIF(SUM(p.cur_balance),0)*100,2) AS wac_pct,
           ROUND(SUM(p.wa_fico*p.cur_balance)/NULLIF(SUM(p.cur_balance),0),0) AS wa_fico
    FROM collateral_pool p JOIN deal_identity i ON i.accession=p.accession
    GROUP BY i.deal_key
),
bond AS (                  -- bond side: tranche count + notional keyed by deal
    SELECT i.deal_key,
           COUNT(*)                        AS n_tranches,
           ROUND(SUM(t.orig_balance)/1e6,1) AS tranche_mm
    FROM tranche t JOIN deal_identity i ON i.accession=t.accession
    GROUP BY i.deal_key
),
lead AS (                  -- the lead underwriter (first found on a deal's cover)
    SELECT i.deal_key, MIN(u.name) AS lead
    FROM underwriter u JOIN deal_identity i ON i.accession=u.accession
    WHERE u.role='lead'
    GROUP BY i.deal_key
),
keys AS (SELECT DISTINCT deal_key FROM deal_identity WHERE deal_key IS NOT NULL)
SELECT k.deal_key,
       c.asset_type, c.n_loans, c.cur_mm, c.wac_pct, c.wa_fico,
       b.n_tranches, b.tranche_mm,
       l.lead
FROM keys k
LEFT JOIN coll c ON c.deal_key=k.deal_key
LEFT JOIN bond b ON b.deal_key=k.deal_key
LEFT JOIN lead l ON l.deal_key=k.deal_key;
