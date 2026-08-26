# DSCO Commercial Billing & Invoicing

This is the **postpaid/commercial layer** that sits on top of the production
prepaid credit ledger. Prepaid credit reservation and settlement
(`_reserve_project_credits` / `_settle_project_reservation`) are unchanged and
remain the atomic admission path for `/v1/responses`. The billing subsystem adds
monthly invoicing, Stripe integration, dunning, and access policy on top of the
already-metered `request_logs`.

Module: `web/billing.py`. Schema is installed idempotently by
`billing.ensure_schema(conn)` from the control-plane initializer.

## What each piece solves

| Concern | Mechanism |
|---|---|
| External Stripe invoice + items + finalize | `StripeREST` (httpx, no SDK dep), idempotency-keyed |
| Durable month-close scheduler / operator command | `close_period()` + `python -m web.billing close` with `--dry-run`/`--apply`/`--stripe` and idempotent `billing_runs` |
| Reconciliation | `reconcile()` + `python -m web.billing reconcile` (local item-sum vs invoice total, and Stripe remote status when `--stripe`) |
| Customer invoice UI/download | `/api/billing/invoices`, `/api/billing/invoices/{id}`, signed public `/billing/invoice/{id}?sig=…`, Billing band in `management.html` |
| Notices / retry / grace / access | `run_dunning()` writes `billing_notices`, sets `next_retry_at`, `grace_ends_at`, and account `access_state` (`allowed`→`grace`→`blocked`) |
| Provider identity adapters | `resolve_provider_identity()` — managed/OAuth are billable; **generic and BYOK keys are intentionally unbillable** |
| Ratified plan catalog + trial/proration | `plans.terms_status='ratified'`, `trial_days`, `proration_policy` (built-in catalog auto-ratified; operator plans stay `draft` until ratified) |

## Billability rule (why generic keys don't bill)

An opaque API key never proves account ownership, so it can't be billed.
`resolve_provider_identity` classifies:

- **billable** — DSCO-managed backend (`managed_env` + known backend), DSCO-hosted
  gateway (`modal-*`/`dsco-*`), OpenAI Codex OAuth or Anthropic Claude OAuth with a
  provider account id.
- **unbillable** — customer BYOK (`byok_request`) and any generic/unknown key.

Only billable usage contributes to overage; unbillable requests are counted and
surfaced on the invoice as `unbillable_request_count`.

## Operator commands

```bash
# Preview previous month for all postpaid accounts (no writes, no network):
python -m web.billing close

# Persist local draft invoices (idempotent; safe to re-run):
python -m web.billing close --period 2026-02 --apply

# Create + finalize external Stripe invoices (requires STRIPE_API_KEY):
python -m web.billing close --period 2026-02 --apply --stripe

# Reconcile local vs Stripe and mark paid/void:
python -m web.billing reconcile --apply --stripe

# Run dunning: schedule notices, advance grace, block access past grace:
python -m web.billing dunning --apply
```

`--db` defaults to `$DSCO_CONTROL_PLANE_DB` or
`.workspace/control_plane/control_plane.db`.

## HTTP surface (admin-token gated, except signed downloads)

- `GET  /api/billing/accounts`
- `POST /api/billing/accounts` — enroll a person (`billing_mode`, starts trial)
- `GET  /api/billing/invoices?person_id=&status=&limit=`
- `GET  /api/billing/invoices/{id}`
- `GET  /billing/invoice/{id}?sig=…` — public, HMAC-signed customer download
- `POST /api/billing/close` — `{period, apply, stripe, person_id}`
- `POST /api/billing/reconcile` — `{invoice_id, apply, stripe}`
- `POST /api/billing/dunning` — `{apply}`

## Safety properties

- **No surprise network**: Stripe is only contacted when `--stripe`/`stripe:true`
  is explicitly passed *and* `STRIPE_API_KEY` is set.
- **Idempotent**: month-close keys on `(person, period)` for invoices and a
  deterministic run key for `billing_runs`; every Stripe write carries a
  deterministic `Idempotency-Key`.
- **Dry-run first**: `close`/`dunning` default to dry-run; nothing is written or
  charged until `--apply`.
- **Additive**: billing schema failure never blocks the control plane; the
  prepaid admission path is untouched.

Tests: `tests/test_web_server.py::BillingTests` (identity matrix, dry-run/close/
reconcile/download, dunning→access-block). Stripe adapter is covered via
`httpx.MockTransport` in the module smoke path.
