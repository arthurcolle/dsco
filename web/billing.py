#!/usr/bin/env python3
"""DSCO commercial billing and invoicing control plane.

This module sits *after* the production prepaid credit ledger. It adds the
postpaid/commercial layer without changing atomic request admission or credit
settlement in web.server:

- versioned/ratified plan terms (trial + proration policy)
- provider identity classification (generic/BYOK keys stay unbillable)
- idempotent monthly invoice generation and reconciliation
- optional Stripe invoice/item/finalization adapter over HTTP
- durable dunning notices, retry schedule, grace and access state
- deterministic dry-run/operator CLI

No network call occurs unless close_period(..., finalize=True) or reconciliation
is explicitly requested with a configured Stripe key.
"""
from __future__ import annotations

import argparse
import calendar
import hashlib
import html
import json
import os
import sqlite3
import sys
import uuid
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from decimal import Decimal, ROUND_HALF_UP
from pathlib import Path
from typing import Any, Iterable, Optional

import httpx

BILLING_SCHEMA_VERSION = 1
SUPPORTED_MANAGED_PROVIDERS = {
    "openai", "anthropic", "openrouter", "groq", "deepseek", "mistral",
    "together", "xai", "perplexity", "cerebras", "cohere", "modal",
}
DEFAULT_RETRY_DAYS = [1, 3, 7]


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def _dt(value: str | None) -> Optional[datetime]:
    if not value:
        return None
    try:
        return datetime.fromisoformat(value.replace("Z", "+00:00")).astimezone(timezone.utc)
    except (ValueError, TypeError):
        return None


def _add_column(conn: sqlite3.Connection, table: str, definition: str) -> None:
    name = definition.split()[0]
    cols = {row[1] for row in conn.execute(f"PRAGMA table_info({table})")}
    if name not in cols:
        conn.execute(f"ALTER TABLE {table} ADD COLUMN {definition}")


def ensure_schema(conn: sqlite3.Connection) -> None:
    """Idempotently install billing schema and migrate plan policy columns."""
    _add_column(conn, "plans", "catalog_version INTEGER NOT NULL DEFAULT 1")
    _add_column(conn, "plans", "terms_status TEXT NOT NULL DEFAULT 'draft'")
    _add_column(conn, "plans", "billing_mode TEXT NOT NULL DEFAULT 'prepaid'")
    _add_column(conn, "plans", "trial_days INTEGER NOT NULL DEFAULT 0")
    _add_column(conn, "plans", "proration_policy TEXT NOT NULL DEFAULT 'none'")
    _add_column(conn, "plans", "grace_period_days INTEGER NOT NULL DEFAULT 7")
    _add_column(conn, "plans", "retry_schedule_json TEXT NOT NULL DEFAULT '[1,3,7]'")
    _add_column(conn, "plans", "ratified_at TEXT")

    conn.executescript(
        """
        CREATE TABLE IF NOT EXISTS billing_accounts (
            person_id TEXT PRIMARY KEY REFERENCES people(id),
            billing_email TEXT NOT NULL DEFAULT '',
            stripe_customer_id TEXT NOT NULL DEFAULT '',
            billing_mode TEXT NOT NULL DEFAULT 'prepaid'
                CHECK (billing_mode IN ('prepaid','postpaid')),
            status TEXT NOT NULL DEFAULT 'active'
                CHECK (status IN ('trialing','active','past_due','suspended','closed')),
            access_state TEXT NOT NULL DEFAULT 'allowed'
                CHECK (access_state IN ('allowed','grace','blocked')),
            trial_ends_at TEXT,
            current_period_start TEXT,
            current_period_end TEXT,
            metadata_json TEXT NOT NULL DEFAULT '{}',
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS billing_invoices (
            id TEXT PRIMARY KEY,
            person_id TEXT NOT NULL REFERENCES people(id),
            plan_id TEXT NOT NULL REFERENCES plans(id),
            period_start TEXT NOT NULL,
            period_end TEXT NOT NULL,
            currency TEXT NOT NULL DEFAULT 'usd',
            status TEXT NOT NULL DEFAULT 'draft'
                CHECK (status IN ('draft','open','paid','past_due','void','uncollectible')),
            subtotal_cents INTEGER NOT NULL DEFAULT 0,
            total_cents INTEGER NOT NULL DEFAULT 0,
            billable_input_tokens INTEGER NOT NULL DEFAULT 0,
            billable_output_tokens INTEGER NOT NULL DEFAULT 0,
            unbillable_request_count INTEGER NOT NULL DEFAULT 0,
            stripe_invoice_id TEXT NOT NULL DEFAULT '',
            stripe_hosted_url TEXT NOT NULL DEFAULT '',
            stripe_pdf_url TEXT NOT NULL DEFAULT '',
            due_at TEXT,
            grace_ends_at TEXT,
            attempt_count INTEGER NOT NULL DEFAULT 0,
            next_retry_at TEXT,
            finalized_at TEXT,
            paid_at TEXT,
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL,
            UNIQUE(person_id, period_start, period_end)
        );

        CREATE TABLE IF NOT EXISTS billing_invoice_items (
            id TEXT PRIMARY KEY,
            invoice_id TEXT NOT NULL REFERENCES billing_invoices(id) ON DELETE CASCADE,
            item_key TEXT NOT NULL,
            kind TEXT NOT NULL,
            description TEXT NOT NULL,
            quantity REAL NOT NULL DEFAULT 1,
            unit_amount_cents INTEGER NOT NULL DEFAULT 0,
            amount_cents INTEGER NOT NULL DEFAULT 0,
            source_ref TEXT NOT NULL DEFAULT '',
            stripe_invoice_item_id TEXT NOT NULL DEFAULT '',
            metadata_json TEXT NOT NULL DEFAULT '{}',
            created_at TEXT NOT NULL,
            UNIQUE(invoice_id, item_key)
        );

        CREATE TABLE IF NOT EXISTS billing_runs (
            id TEXT PRIMARY KEY,
            idempotency_key TEXT NOT NULL UNIQUE,
            period_start TEXT NOT NULL,
            period_end TEXT NOT NULL,
            mode TEXT NOT NULL,
            status TEXT NOT NULL,
            account_count INTEGER NOT NULL DEFAULT 0,
            invoice_count INTEGER NOT NULL DEFAULT 0,
            total_cents INTEGER NOT NULL DEFAULT 0,
            result_json TEXT NOT NULL DEFAULT '{}',
            created_at TEXT NOT NULL,
            completed_at TEXT
        );

        CREATE TABLE IF NOT EXISTS billing_notices (
            id TEXT PRIMARY KEY,
            invoice_id TEXT NOT NULL REFERENCES billing_invoices(id),
            person_id TEXT NOT NULL REFERENCES people(id),
            notice_type TEXT NOT NULL,
            destination TEXT NOT NULL,
            subject TEXT NOT NULL,
            body TEXT NOT NULL,
            status TEXT NOT NULL DEFAULT 'pending'
                CHECK (status IN ('pending','sent','failed','cancelled')),
            idempotency_key TEXT NOT NULL UNIQUE,
            scheduled_at TEXT NOT NULL,
            sent_at TEXT,
            created_at TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS provider_billing_identities (
            identity_key TEXT PRIMARY KEY,
            provider TEXT NOT NULL,
            adapter TEXT NOT NULL,
            billing_class TEXT NOT NULL CHECK (billing_class IN ('billable','unbillable')),
            backend_id TEXT NOT NULL DEFAULT '',
            credential_fingerprint TEXT NOT NULL DEFAULT '',
            first_seen_at TEXT NOT NULL,
            last_seen_at TEXT NOT NULL,
            request_count INTEGER NOT NULL DEFAULT 0,
            metadata_json TEXT NOT NULL DEFAULT '{}'
        );

        CREATE INDEX IF NOT EXISTS idx_billing_invoices_person_period
            ON billing_invoices(person_id, period_start DESC);
        CREATE INDEX IF NOT EXISTS idx_billing_invoices_status_due
            ON billing_invoices(status, due_at);
        CREATE INDEX IF NOT EXISTS idx_billing_notices_status_scheduled
            ON billing_notices(status, scheduled_at);
        """
    )
    conn.execute(
        "INSERT INTO cp_meta(key,value) VALUES('billing_schema_version',?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
        (str(BILLING_SCHEMA_VERSION),),
    )
    # Ratify the built-in catalog explicitly. Existing operator-created plans
    # remain drafts until the operator ratifies them.
    now = now_iso()
    builtins = {
        "starter-byok": ("prepaid", 14, "daily"),
        "team-managed": ("postpaid", 14, "daily"),
        "hybrid-scale": ("postpaid", 30, "daily"),
    }
    for plan_id, (mode, trial, prorate) in builtins.items():
        conn.execute(
            """UPDATE plans SET catalog_version=1, terms_status='ratified',
               billing_mode=?, trial_days=?, proration_policy=?, grace_period_days=7,
               retry_schedule_json='[1,3,7]', ratified_at=COALESCE(ratified_at, ?)
               WHERE id=?""",
            (mode, trial, prorate, now, plan_id),
        )


@dataclass(frozen=True)
class ProviderIdentity:
    key: str
    adapter: str
    billing_class: str
    provider: str
    backend_id: str = ""

    @property
    def billable(self) -> bool:
        return self.billing_class == "billable"


def resolve_provider_identity(*, provider: str, auth_mode: str, backend_id: str = "",
                              metadata: Optional[dict[str, Any]] = None) -> ProviderIdentity:
    """Normalize provider identity without ever treating an opaque key as owner proof.

    Billable identities require a DSCO-managed backend, hosted gateway, or a
    provider OAuth account identifier. Request BYOK and generic/unknown keys are
    deliberately unbillable.
    """
    provider = (provider or "unknown").lower().strip()
    auth_mode = (auth_mode or "unknown").lower().strip()
    backend_id = (backend_id or "").strip()
    metadata = metadata or {}

    account_id = str(metadata.get("provider_account_id") or metadata.get("account_id") or "").strip()
    if auth_mode in {"openai_codex_oauth", "codex_oauth", "subscription_oauth"} and provider == "openai" and account_id:
        return ProviderIdentity(f"openai:codex:{account_id}", "openai_codex_oauth", "billable", provider, backend_id)
    if auth_mode in {"anthropic_oauth", "claude_oauth"} and provider == "anthropic" and account_id:
        return ProviderIdentity(f"anthropic:claude:{account_id}", "anthropic_claude_oauth", "billable", provider, backend_id)
    if auth_mode == "managed_env" and provider in SUPPORTED_MANAGED_PROVIDERS and backend_id:
        adapter = "dsco_hosted" if backend_id.startswith(("modal-", "dsco-")) else f"{provider}_managed"
        return ProviderIdentity(f"{provider}:managed:{backend_id}", adapter, "billable", provider, backend_id)
    if auth_mode == "byok_request":
        return ProviderIdentity(f"{provider}:customer-byok", "customer_byok", "unbillable", provider, backend_id)
    # Generic API keys do not prove account ownership and remain intentionally
    # unbillable even if their provider is known.
    digest = hashlib.sha256(f"{provider}:{backend_id}:{auth_mode}".encode()).hexdigest()[:12]
    return ProviderIdentity(f"{provider}:generic:{digest}", "generic_key", "unbillable", provider, backend_id)


def _record_identity(conn: sqlite3.Connection, identity: ProviderIdentity) -> None:
    now = now_iso()
    conn.execute(
        """INSERT INTO provider_billing_identities
           (identity_key,provider,adapter,billing_class,backend_id,first_seen_at,last_seen_at,request_count)
           VALUES(?,?,?,?,?,?,?,1)
           ON CONFLICT(identity_key) DO UPDATE SET
             last_seen_at=excluded.last_seen_at, request_count=request_count+1,
             adapter=excluded.adapter, billing_class=excluded.billing_class""",
        (identity.key, identity.provider, identity.adapter, identity.billing_class,
         identity.backend_id, now, now),
    )


def period_bounds(period: str | None = None) -> tuple[datetime, datetime]:
    """Return UTC [start,end) for YYYY-MM; default is previous calendar month."""
    now = datetime.now(timezone.utc)
    if period:
        try:
            start = datetime.strptime(period, "%Y-%m").replace(tzinfo=timezone.utc)
        except ValueError as exc:
            raise ValueError("period must be YYYY-MM") from exc
    else:
        first = now.replace(day=1, hour=0, minute=0, second=0, microsecond=0)
        start = (first - timedelta(days=1)).replace(day=1)
    if start.month == 12:
        end = start.replace(year=start.year + 1, month=1)
    else:
        end = start.replace(month=start.month + 1)
    return start, end


def _money_cents(value: Decimal | float | str) -> int:
    return int((Decimal(str(value)) * 100).quantize(Decimal("1"), rounding=ROUND_HALF_UP))


def _active_fraction(person_created: str, trial_ends_at: str | None,
                     start: datetime, end: datetime, policy: str) -> Decimal:
    if policy != "daily":
        if trial_ends_at and (_dt(trial_ends_at) or start) >= end:
            return Decimal("0")
        return Decimal("1")
    active = max(start, _dt(person_created) or start, _dt(trial_ends_at) or start)
    if active >= end:
        return Decimal("0")
    total_seconds = Decimal(str((end - start).total_seconds()))
    return (Decimal(str((end - active).total_seconds())) / total_seconds).quantize(Decimal("0.000001"))


def preview_invoice(conn: sqlite3.Connection, person_id: str, start: datetime,
                    end: datetime) -> dict[str, Any]:
    account = conn.execute(
        """SELECT a.*, p.email, p.name, p.organization, p.plan_id, p.created_at AS person_created,
                  pl.name AS plan_name, pl.price_monthly, pl.included_input_tokens,
                  pl.included_output_tokens, pl.overage_rate_input, pl.overage_rate_output,
                  pl.catalog_version, pl.terms_status, pl.billing_mode AS plan_billing_mode,
                  pl.proration_policy, pl.grace_period_days, pl.retry_schedule_json
           FROM billing_accounts a JOIN people p ON p.id=a.person_id
           JOIN plans pl ON pl.id=p.plan_id WHERE a.person_id=?""",
        (person_id,),
    ).fetchone()
    if not account:
        raise ValueError("billing account not found")
    if account["terms_status"] != "ratified":
        raise ValueError(f"plan {account['plan_id']} terms are not ratified")

    rows = conn.execute(
        """SELECT * FROM request_logs WHERE person_id=? AND status='ok'
           AND created_at>=? AND created_at<? ORDER BY created_at""",
        (person_id, start.isoformat(timespec="seconds"), end.isoformat(timespec="seconds")),
    ).fetchall()
    input_tokens = output_tokens = unbillable = 0
    identity_counts: dict[str, int] = {}
    for row in rows:
        metadata = {}
        try:
            metadata = json.loads(row["metadata_json"] or "{}")
        except (ValueError, TypeError):
            pass
        identity = resolve_provider_identity(
            provider=row["provider"], auth_mode=row["auth_mode"],
            backend_id=row["backend_id"], metadata=metadata,
        )
        _record_identity(conn, identity)
        identity_counts[identity.key] = identity_counts.get(identity.key, 0) + 1
        if identity.billable:
            input_tokens += int(row["input_tokens"] or 0)
            output_tokens += int(row["output_tokens"] or 0)
        else:
            unbillable += 1

    fraction = _active_fraction(account["person_created"], account["trial_ends_at"],
                                start, end, account["proration_policy"])
    included_input = int(Decimal(int(account["included_input_tokens"] or 0)) * fraction)
    included_output = int(Decimal(int(account["included_output_tokens"] or 0)) * fraction)
    over_input = max(0, input_tokens - included_input)
    over_output = max(0, output_tokens - included_output)

    items: list[dict[str, Any]] = []
    base_cents = _money_cents(Decimal(str(account["price_monthly"] or 0)) * fraction)
    if base_cents:
        items.append({"item_key": "plan-base", "kind": "plan_base",
                      "description": f"{account['plan_name']} — {start:%B %Y} ({fraction * 100:.2f}% active)",
                      "quantity": 1, "unit_amount_cents": base_cents, "amount_cents": base_cents,
                      "source_ref": f"plan:{account['plan_id']}:v{account['catalog_version']}"})
    if over_input:
        amount = _money_cents((Decimal(over_input) / Decimal(1_000_000)) * Decimal(str(account["overage_rate_input"] or 0)))
        if amount:
            items.append({"item_key": "overage-input", "kind": "overage_input",
                          "description": "Managed input token overage", "quantity": over_input,
                          "unit_amount_cents": amount, "amount_cents": amount,
                          "source_ref": f"request_logs:{start:%Y-%m}"})
    if over_output:
        amount = _money_cents((Decimal(over_output) / Decimal(1_000_000)) * Decimal(str(account["overage_rate_output"] or 0)))
        if amount:
            items.append({"item_key": "overage-output", "kind": "overage_output",
                          "description": "Managed output token overage", "quantity": over_output,
                          "unit_amount_cents": amount, "amount_cents": amount,
                          "source_ref": f"request_logs:{start:%Y-%m}"})
    total = sum(item["amount_cents"] for item in items)
    return {
        "person_id": person_id, "person_name": account["name"], "email": account["billing_email"] or account["email"],
        "organization": account["organization"], "plan_id": account["plan_id"], "plan_name": account["plan_name"],
        "catalog_version": account["catalog_version"], "period_start": start.isoformat(timespec="seconds"),
        "period_end": end.isoformat(timespec="seconds"), "active_fraction": float(fraction),
        "billable_input_tokens": input_tokens, "billable_output_tokens": output_tokens,
        "unbillable_request_count": unbillable, "provider_identities": identity_counts,
        "items": items, "subtotal_cents": total, "total_cents": total,
        "grace_period_days": int(account["grace_period_days"] or 7),
        "retry_schedule": json.loads(account["retry_schedule_json"] or "[1,3,7]"),
        "stripe_customer_id": account["stripe_customer_id"],
    }


class StripeREST:
    """Minimal idempotent Stripe invoices adapter; no SDK dependency."""
    base_url = "https://api.stripe.com/v1"

    def __init__(self, api_key: str | None = None, *, client: Optional[httpx.Client] = None):
        self.api_key = (api_key or os.getenv("STRIPE_API_KEY", "")).strip()
        self.client = client or httpx.Client(timeout=30.0)

    @property
    def configured(self) -> bool:
        return bool(self.api_key)

    def _request(self, method: str, path: str, *, data: Optional[dict[str, Any]] = None,
                 idempotency_key: str = "") -> dict[str, Any]:
        if not self.api_key:
            raise RuntimeError("STRIPE_API_KEY is not configured")
        headers = {"Authorization": f"Bearer {self.api_key}"}
        if idempotency_key:
            headers["Idempotency-Key"] = idempotency_key[:255]
        response = self.client.request(method, self.base_url + path, data=data, headers=headers)
        if response.status_code >= 400:
            try:
                detail = response.json().get("error", {}).get("message", response.text)
            except ValueError:
                detail = response.text
            raise RuntimeError(f"Stripe HTTP {response.status_code}: {detail}")
        return response.json()

    def create_customer(self, *, email: str, name: str, person_id: str) -> dict[str, Any]:
        return self._request("POST", "/customers", data={"email": email, "name": name,
            "metadata[dsco_person_id]": person_id}, idempotency_key=f"dsco-customer-{person_id}")

    def create_invoice(self, *, customer: str, invoice_id: str, period: str, days_until_due: int) -> dict[str, Any]:
        return self._request("POST", "/invoices", data={"customer": customer,
            "collection_method": "send_invoice", "days_until_due": days_until_due,
            "auto_advance": "false", "metadata[dsco_invoice_id]": invoice_id,
            "metadata[period]": period}, idempotency_key=f"dsco-invoice-{invoice_id}")

    def create_item(self, *, customer: str, stripe_invoice: str, invoice_id: str,
                    item: dict[str, Any]) -> dict[str, Any]:
        return self._request("POST", "/invoiceitems", data={"customer": customer,
            "invoice": stripe_invoice, "currency": "usd", "amount": item["amount_cents"],
            "description": item["description"], "metadata[dsco_item_key]": item["item_key"]},
            idempotency_key=f"dsco-item-{invoice_id}-{item['item_key']}")

    def finalize_invoice(self, stripe_invoice: str, invoice_id: str) -> dict[str, Any]:
        return self._request("POST", f"/invoices/{stripe_invoice}/finalize",
                             idempotency_key=f"dsco-finalize-{invoice_id}")

    def get_invoice(self, stripe_invoice: str) -> dict[str, Any]:
        return self._request("GET", f"/invoices/{stripe_invoice}")


def _persist_preview(conn: sqlite3.Connection, preview: dict[str, Any]) -> str:
    existing = conn.execute(
        "SELECT id,status FROM billing_invoices WHERE person_id=? AND period_start=? AND period_end=?",
        (preview["person_id"], preview["period_start"], preview["period_end"]),
    ).fetchone()
    invoice_id = existing["id"] if existing else str(uuid.uuid4())
    if existing and existing["status"] in {"open", "paid", "void", "uncollectible"}:
        return invoice_id
    now = now_iso()
    conn.execute(
        """INSERT INTO billing_invoices
           (id,person_id,plan_id,period_start,period_end,status,subtotal_cents,total_cents,
            billable_input_tokens,billable_output_tokens,unbillable_request_count,created_at,updated_at)
           VALUES(?,?,?,?,?,'draft',?,?,?,?,?,?,?)
           ON CONFLICT(person_id,period_start,period_end) DO UPDATE SET
             plan_id=excluded.plan_id, subtotal_cents=excluded.subtotal_cents,
             total_cents=excluded.total_cents, billable_input_tokens=excluded.billable_input_tokens,
             billable_output_tokens=excluded.billable_output_tokens,
             unbillable_request_count=excluded.unbillable_request_count, updated_at=excluded.updated_at""",
        (invoice_id, preview["person_id"], preview["plan_id"], preview["period_start"], preview["period_end"],
         preview["subtotal_cents"], preview["total_cents"], preview["billable_input_tokens"],
         preview["billable_output_tokens"], preview["unbillable_request_count"], now, now),
    )
    for item in preview["items"]:
        conn.execute(
            """INSERT INTO billing_invoice_items
               (id,invoice_id,item_key,kind,description,quantity,unit_amount_cents,amount_cents,source_ref,created_at)
               VALUES(?,?,?,?,?,?,?,?,?,?)
               ON CONFLICT(invoice_id,item_key) DO UPDATE SET description=excluded.description,
                 quantity=excluded.quantity,unit_amount_cents=excluded.unit_amount_cents,
                 amount_cents=excluded.amount_cents,source_ref=excluded.source_ref""",
            (str(uuid.uuid4()), invoice_id, item["item_key"], item["kind"], item["description"],
             item["quantity"], item["unit_amount_cents"], item["amount_cents"], item["source_ref"], now),
        )
    return invoice_id


def _finalize_external(conn: sqlite3.Connection, invoice_id: str, preview: dict[str, Any],
                       stripe: StripeREST) -> dict[str, Any]:
    row = conn.execute("SELECT * FROM billing_invoices WHERE id=?", (invoice_id,)).fetchone()
    if row["status"] == "paid":
        return {"id": invoice_id, "status": "paid", "stripe_invoice_id": row["stripe_invoice_id"]}
    customer = preview["stripe_customer_id"]
    if not customer:
        created = stripe.create_customer(email=preview["email"], name=preview["person_name"], person_id=preview["person_id"])
        customer = created["id"]
        conn.execute("UPDATE billing_accounts SET stripe_customer_id=?,updated_at=? WHERE person_id=?",
                     (customer, now_iso(), preview["person_id"]))
    period = preview["period_start"][:7]
    external = stripe.create_invoice(customer=customer, invoice_id=invoice_id, period=period,
                                     days_until_due=max(1, preview["grace_period_days"]))
    stripe_invoice = external["id"]
    for item in preview["items"]:
        item_row = conn.execute("SELECT * FROM billing_invoice_items WHERE invoice_id=? AND item_key=?",
                                (invoice_id, item["item_key"])).fetchone()
        if item_row["stripe_invoice_item_id"]:
            continue
        si = stripe.create_item(customer=customer, stripe_invoice=stripe_invoice,
                                invoice_id=invoice_id, item=item)
        conn.execute("UPDATE billing_invoice_items SET stripe_invoice_item_id=? WHERE id=?",
                     (si["id"], item_row["id"]))
    finalized = stripe.finalize_invoice(stripe_invoice, invoice_id)
    status = finalized.get("status", "open")
    due_at = datetime.fromtimestamp(finalized["due_date"], timezone.utc).isoformat(timespec="seconds") if finalized.get("due_date") else None
    grace = (_dt(due_at) + timedelta(days=preview["grace_period_days"])).isoformat(timespec="seconds") if due_at else None
    conn.execute(
        """UPDATE billing_invoices SET status=?,stripe_invoice_id=?,stripe_hosted_url=?,stripe_pdf_url=?,
           due_at=?,grace_ends_at=?,finalized_at=?,updated_at=? WHERE id=?""",
        (status if status in {"draft","open","paid","void","uncollectible"} else "open", stripe_invoice,
         finalized.get("hosted_invoice_url", ""), finalized.get("invoice_pdf", ""), due_at, grace,
         now_iso(), now_iso(), invoice_id),
    )
    return {"id": invoice_id, "status": status, "stripe_invoice_id": stripe_invoice,
            "hosted_invoice_url": finalized.get("hosted_invoice_url", "")}


def close_period(conn: sqlite3.Connection, *, period: str | None = None, dry_run: bool = True,
                 finalize: bool = False, person_id: str = "", stripe: Optional[StripeREST] = None) -> dict[str, Any]:
    start, end = period_bounds(period)
    if finalize and dry_run:
        raise ValueError("finalize and dry_run are mutually exclusive")
    accounts = conn.execute(
        """SELECT person_id FROM billing_accounts
           WHERE billing_mode='postpaid' AND status!='closed' """ + ("AND person_id=?" if person_id else "") +
        " ORDER BY person_id", ((person_id,) if person_id else ()),
    ).fetchall()
    key = f"close:{start:%Y-%m}:{person_id or 'all'}:{'dry' if dry_run else ('stripe' if finalize else 'local')}"
    run_id = str(uuid.uuid5(uuid.NAMESPACE_URL, "dsco-billing:" + key))
    previews: list[dict[str, Any]] = []
    errors: list[dict[str, str]] = []
    for account in accounts:
        try:
            preview = preview_invoice(conn, account["person_id"], start, end)
            if not dry_run:
                invoice_id = _persist_preview(conn, preview)
                preview["invoice_id"] = invoice_id
                if finalize:
                    preview["external"] = _finalize_external(conn, invoice_id, preview, stripe or StripeREST())
            previews.append(preview)
        except Exception as exc:
            errors.append({"person_id": account["person_id"], "error": str(exc)})
    result = {"run_id": run_id, "period": start.strftime("%Y-%m"), "dry_run": dry_run,
              "finalize": finalize, "account_count": len(accounts), "invoice_count": len(previews),
              "total_cents": sum(p["total_cents"] for p in previews), "invoices": previews, "errors": errors}
    if not dry_run:
        conn.execute(
            """INSERT INTO billing_runs(id,idempotency_key,period_start,period_end,mode,status,
               account_count,invoice_count,total_cents,result_json,created_at,completed_at)
               VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(idempotency_key) DO UPDATE SET
               status=excluded.status,account_count=excluded.account_count,invoice_count=excluded.invoice_count,
               total_cents=excluded.total_cents,result_json=excluded.result_json,completed_at=excluded.completed_at""",
            (run_id, key, start.isoformat(timespec="seconds"), end.isoformat(timespec="seconds"),
             "stripe" if finalize else "local", "partial" if errors else "completed", len(accounts), len(previews),
             result["total_cents"], json.dumps(result, sort_keys=True, default=str), now_iso(), now_iso()),
        )
    return result


def reconcile(conn: sqlite3.Connection, *, invoice_id: str = "", stripe: Optional[StripeREST] = None,
              apply: bool = False) -> dict[str, Any]:
    rows = conn.execute(
        "SELECT * FROM billing_invoices WHERE " + ("id=?" if invoice_id else "status IN ('draft','open','past_due')") +
        " ORDER BY created_at", ((invoice_id,) if invoice_id else ()),
    ).fetchall()
    findings = []
    for row in rows:
        item_total = conn.execute("SELECT COALESCE(SUM(amount_cents),0) FROM billing_invoice_items WHERE invoice_id=?",
                                  (row["id"],)).fetchone()[0]
        finding: dict[str, Any] = {"invoice_id": row["id"], "local_status": row["status"],
            "local_total_cents": row["total_cents"], "item_total_cents": item_total,
            "amount_match": int(row["total_cents"]) == int(item_total)}
        if row["stripe_invoice_id"] and stripe and stripe.configured:
            remote = stripe.get_invoice(row["stripe_invoice_id"])
            finding.update({"stripe_status": remote.get("status"), "stripe_total_cents": remote.get("total"),
                            "stripe_amount_match": int(remote.get("total") or 0) == int(row["total_cents"])})
            if apply and remote.get("status") in {"open", "paid", "void", "uncollectible"}:
                new_status = remote["status"]
                paid_at = now_iso() if new_status == "paid" else row["paid_at"]
                conn.execute("UPDATE billing_invoices SET status=?,paid_at=?,updated_at=? WHERE id=?",
                             (new_status, paid_at, now_iso(), row["id"]))
                if new_status == "paid":
                    conn.execute("UPDATE billing_accounts SET status='active',access_state='allowed',updated_at=? WHERE person_id=?",
                                 (now_iso(), row["person_id"]))
        findings.append(finding)
    return {"checked": len(findings), "apply": apply, "ok": all(f.get("amount_match") and f.get("stripe_amount_match", True) for f in findings),
            "findings": findings}


def run_dunning(conn: sqlite3.Connection, *, at: Optional[datetime] = None, dry_run: bool = True) -> dict[str, Any]:
    at = at or datetime.now(timezone.utc)
    rows = conn.execute(
        """SELECT i.*,p.email,p.name,pl.retry_schedule_json,pl.grace_period_days
           FROM billing_invoices i JOIN people p ON p.id=i.person_id JOIN plans pl ON pl.id=i.plan_id
           WHERE i.status IN ('open','past_due') AND i.due_at IS NOT NULL"""
    ).fetchall()
    actions = []
    for row in rows:
        due = _dt(row["due_at"])
        if not due or at < due:
            continue
        retry_days = json.loads(row["retry_schedule_json"] or "[1,3,7]")
        overdue_days = max(0, (at - due).days)
        attempt = sum(1 for d in retry_days if overdue_days >= int(d))
        notice_type = "payment_overdue" if attempt == 0 else f"payment_retry_{attempt}"
        grace_end = _dt(row["grace_ends_at"]) or due + timedelta(days=int(row["grace_period_days"] or 7))
        access_state = "blocked" if at >= grace_end else "grace"
        action = {"invoice_id": row["id"], "person_id": row["person_id"], "notice_type": notice_type,
                  "attempt": attempt, "access_state": access_state, "grace_ends_at": grace_end.isoformat(timespec="seconds")}
        actions.append(action)
        if dry_run:
            continue
        ikey = f"{row['id']}:{notice_type}"
        conn.execute(
            """INSERT OR IGNORE INTO billing_notices
               (id,invoice_id,person_id,notice_type,destination,subject,body,status,idempotency_key,scheduled_at,created_at)
               VALUES(?,?,?,?,?,?,?,?,?,?,?)""",
            (str(uuid.uuid4()), row["id"], row["person_id"], notice_type, row["email"],
             f"Payment notice for DSCO invoice {row['id'][:8]}",
             f"Invoice {row['id']} is overdue. Grace access ends {grace_end.isoformat(timespec='seconds')}.",
             "pending", ikey, now_iso(), now_iso()),
        )
        next_retry = None
        for day in retry_days:
            candidate = due + timedelta(days=int(day))
            if candidate > at:
                next_retry = candidate.isoformat(timespec="seconds")
                break
        conn.execute("""UPDATE billing_invoices SET status='past_due',attempt_count=?,next_retry_at=?,
                     grace_ends_at=?,updated_at=? WHERE id=?""",
                     (attempt, next_retry, grace_end.isoformat(timespec="seconds"), now_iso(), row["id"]))
        conn.execute("UPDATE billing_accounts SET status=?,access_state=?,updated_at=? WHERE person_id=?",
                     ("suspended" if access_state == "blocked" else "past_due", access_state, now_iso(), row["person_id"]))
    return {"dry_run": dry_run, "actions": actions, "action_count": len(actions)}


def access_error(conn: sqlite3.Connection, person_id: str) -> Optional[dict[str, Any]]:
    row = conn.execute("SELECT status,access_state,trial_ends_at FROM billing_accounts WHERE person_id=?", (person_id,)).fetchone()
    if not row or row["access_state"] != "blocked":
        return None
    return {"error": "billing access suspended", "billing_status": row["status"],
            "access_state": row["access_state"], "code": "billing_suspended"}


def serialize_invoice(conn: sqlite3.Connection, row: sqlite3.Row, *, include_items: bool = True) -> dict[str, Any]:
    data = dict(row)
    data["subtotal_usd"] = round(int(row["subtotal_cents"]) / 100, 2)
    data["total_usd"] = round(int(row["total_cents"]) / 100, 2)
    if include_items:
        data["items"] = [dict(r) for r in conn.execute(
            "SELECT * FROM billing_invoice_items WHERE invoice_id=? ORDER BY created_at,item_key", (row["id"],)).fetchall()]
    return data


def render_invoice_html(conn: sqlite3.Connection, invoice_id: str) -> str:
    row = conn.execute(
        """SELECT i.*,p.name,p.email,p.organization,pl.name AS plan_name
           FROM billing_invoices i JOIN people p ON p.id=i.person_id JOIN plans pl ON pl.id=i.plan_id
           WHERE i.id=?""", (invoice_id,),
    ).fetchone()
    if not row:
        raise ValueError("invoice not found")
    items = conn.execute("SELECT * FROM billing_invoice_items WHERE invoice_id=? ORDER BY created_at", (invoice_id,)).fetchall()
    lines = "".join(f"<tr><td>{html.escape(i['description'])}</td><td>{i['quantity']:,.0f}</td><td>${i['amount_cents']/100:,.2f}</td></tr>" for i in items)
    return f"""<!doctype html><html><head><meta charset='utf-8'><title>DSCO Invoice {invoice_id[:8]}</title>
<style>body{{font:15px system-ui;max-width:800px;margin:48px auto;color:#18202a}}h1{{letter-spacing:.08em}}table{{width:100%;border-collapse:collapse;margin-top:32px}}th,td{{padding:12px;border-bottom:1px solid #ddd;text-align:left}}td:last-child,th:last-child{{text-align:right}}.total{{font-size:22px;font-weight:700;text-align:right;margin-top:24px}}.muted{{color:#667}}</style></head>
<body><h1>DISTRIBUTED SYSTEMS, INC.</h1><h2>Invoice {html.escape(invoice_id)}</h2>
<p><strong>{html.escape(row['name'])}</strong><br>{html.escape(row['organization'] or '')}<br>{html.escape(row['email'])}</p>
<p class='muted'>Period {html.escape(row['period_start'][:10])} – {html.escape(row['period_end'][:10])} · Plan {html.escape(row['plan_name'])} · Status {html.escape(row['status'])}</p>
<table><thead><tr><th>Description</th><th>Quantity</th><th>Amount</th></tr></thead><tbody>{lines}</tbody></table>
<div class='total'>Total ${row['total_cents']/100:,.2f} USD</div></body></html>"""


def _connect(db_path: str) -> sqlite3.Connection:
    conn = sqlite3.connect(db_path, timeout=30)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys=ON")
    ensure_schema(conn)
    return conn


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="DSCO billing close/reconcile operator command")
    parser.add_argument("command", choices=["close", "reconcile", "dunning"])
    parser.add_argument("--db", default=os.getenv("DSCO_CONTROL_PLANE_DB", ".workspace/control_plane/control_plane.db"))
    parser.add_argument("--period", help="YYYY-MM; close defaults to previous month")
    parser.add_argument("--person-id", default="")
    parser.add_argument("--apply", action="store_true", help="persist/finalize changes")
    parser.add_argument("--stripe", action="store_true", help="create/finalize or reconcile Stripe invoices")
    parser.add_argument("--invoice-id", default="")
    args = parser.parse_args(argv)
    conn = _connect(args.db)
    try:
        if args.command == "close":
            result = close_period(conn, period=args.period, dry_run=not args.apply,
                                  finalize=args.apply and args.stripe, person_id=args.person_id,
                                  stripe=StripeREST() if args.stripe else None)
        elif args.command == "reconcile":
            result = reconcile(conn, invoice_id=args.invoice_id,
                               stripe=StripeREST() if args.stripe else None, apply=args.apply)
        else:
            result = run_dunning(conn, dry_run=not args.apply)
        if args.apply:
            conn.commit()
        print(json.dumps(result, indent=2, default=str))
        return 0 if not result.get("errors") else 1
    finally:
        conn.close()


if __name__ == "__main__":
    raise SystemExit(main())
