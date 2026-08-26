import copy
import os

import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from types import SimpleNamespace
from unittest.mock import AsyncMock, patch
from pathlib import Path

from fastapi.testclient import TestClient

from web import server
from web import billing


def ns(**kwargs):
    return SimpleNamespace(**kwargs)


class FakeStream:
    def __init__(self, events):
        self._events = iter(events)

    def __aiter__(self):
        return self

    async def __anext__(self):
        try:
            return next(self._events)
        except StopIteration as exc:
            raise StopAsyncIteration from exc


class FakeMessagesAPI:
    def __init__(self):
        self.calls = []
        self._turn = 0

    async def create(self, **kwargs):
        self.calls.append(copy.deepcopy(kwargs))
        self._turn += 1
        if self._turn == 1:
            return FakeStream([
                ns(type="message_start", message=ns(usage=ns(input_tokens=11, cache_read_input_tokens=0))),
                ns(type="content_block_start", content_block=ns(type="thinking")),
                ns(type="content_block_delta", delta=ns(type="thinking_delta", thinking="Need tool")),
                ns(type="content_block_delta", delta=ns(type="signature_delta", signature="sig-1")),
                ns(type="content_block_stop"),
                ns(type="content_block_start", content_block=ns(type="text")),
                ns(type="content_block_delta", delta=ns(type="text_delta", text="Running tool.")),
                ns(type="content_block_stop"),
                ns(type="content_block_start", content_block=ns(type="tool_use", id="toolu_1", name="bash")),
                ns(type="content_block_delta", delta=ns(type="input_json_delta", partial_json='{"command":"pwd"}')),
                ns(type="content_block_stop"),
                ns(type="message_delta", delta=ns(stop_reason="tool_use"), usage=ns(output_tokens=29)),
            ])
        if self._turn == 2:
            return FakeStream([
                ns(type="message_start", message=ns(usage=ns(input_tokens=7, cache_read_input_tokens=0))),
                ns(type="content_block_start", content_block=ns(type="text")),
                ns(type="content_block_delta", delta=ns(type="text_delta", text="Done.")),
                ns(type="content_block_stop"),
                ns(type="message_delta", delta=ns(stop_reason="end_turn"), usage=ns(output_tokens=5)),
            ])
        raise AssertionError(f"unexpected extra Anthropic turn: {self._turn}")


class FakeAnthropicClient:
    def __init__(self, messages_api):
        self.messages = messages_api


class DummyWebSocket:
    def __init__(self):
        self.events = []

    async def send_json(self, payload):
        self.events.append(payload)


class WebServerTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        server._endpoint_metrics.clear()

    def test_normalize_user_content_accepts_mixed_text_and_images(self):
        normalized = server.normalize_user_content([
            {"type": "text", "text": "Inspect this"},
            {"type": "image", "media_type": "image/png", "data": "aGVsbG8="},
        ])

        self.assertEqual(
            normalized,
            [
                {"type": "text", "text": "Inspect this"},
                {
                    "type": "image",
                    "source": {
                        "type": "base64",
                        "media_type": "image/png",
                        "data": "aGVsbG8=",
                    },
                },
            ],
        )

    def test_to_openai_messages_converts_user_images(self):
        session = server.Session("gpt-4o")
        messages = [
            {
                "role": "user",
                "content": [
                    {"type": "text", "text": "What changed here?"},
                    {
                        "type": "image",
                        "source": {
                            "type": "base64",
                            "media_type": "image/png",
                            "data": "aGVsbG8=",
                        },
                    },
                ],
            }
        ]

        converted = server.to_openai_messages(session, messages)

        self.assertEqual(converted[0]["role"], "system")
        self.assertEqual(converted[1]["role"], "user")
        self.assertEqual(
            converted[1]["content"],
            [
                {"type": "text", "text": "What changed here?"},
                {"type": "image_url", "image_url": {"url": "data:image/png;base64,aGVsbG8="}},
            ],
        )

    def test_assistant_content_for_replay_drops_thinking(self):
        blocks = [
            {"type": "thinking", "thinking": "hidden", "signature": "sig"},
            {"type": "text", "text": "visible"},
            {"type": "tool_use", "id": "toolu_1", "name": "bash", "input": {"command": "pwd"}},
        ]

        replay = server.assistant_content_for_replay(blocks)

        self.assertEqual(
            replay,
            [
                {"type": "text", "text": "visible"},
                {"type": "tool_use", "id": "toolu_1", "name": "bash", "input": {"command": "pwd"}},
            ],
        )

    async def test_anthropic_followup_request_excludes_thinking_blocks(self):
        ws = DummyWebSocket()
        session = server.Session("claude-sonnet-4-6")
        session.messages.append({"role": "user", "content": "Inspect the repo"})

        messages_api = FakeMessagesAPI()

        with patch.object(server, "MODEL_REGISTRY", [
            {
                "alias": "sonnet",
                "model_id": "claude-sonnet-4-6",
                "context_window": 200000,
                "max_output": 16000,
                "input_price": 3.0,
                "output_price": 15.0,
                "cache_read_price": 0.3,
                "cache_write_price": 3.75,
                "supports_thinking": 1,
            }
        ]), patch.object(server, "TOOLS_ANTHROPIC", []), patch.object(
            server.anthropic,
            "AsyncAnthropic",
            return_value=FakeAnthropicClient(messages_api),
        ), patch.object(
            server,
            "execute_tool",
            AsyncMock(return_value="tool output"),
        ):
            await server.agent_loop_anthropic(ws, session)

        self.assertEqual(len(messages_api.calls), 2)

        second_messages = messages_api.calls[1]["messages"]
        self.assertEqual(second_messages[1]["role"], "assistant")
        self.assertEqual(
            [block["type"] for block in second_messages[1]["content"]],
            ["text", "tool_use"],
        )
        self.assertEqual(
            [block["type"] for block in session.messages[1]["content"]],
            ["text", "tool_use"],
        )
        self.assertTrue(any(event["type"] == "thinking_start" for event in ws.events))
        self.assertTrue(any(event["type"] == "thinking_end" for event in ws.events))

    def test_responses_gateway_requires_project_api_key(self):
        client = TestClient(server.app)

        response = client.post(
            "/v1/responses",
            headers={"Authorization": f"Bearer {server.WEB_AUTH_TOKEN}"},
            json={"model": "gpt-4o", "input": "Say hello", "dry_run": True},
        )

        self.assertEqual(response.status_code, 401)
        self.assertEqual(response.json()["error"]["code"], "invalid_api_key")

    def test_hosted_api_key_admission_and_credit_settlement(self):
        client = TestClient(server.app)
        original_db = server.CONTROL_PLANE_DB
        original_ready = server._control_plane_ready

        with tempfile.TemporaryDirectory() as tmpdir:
            try:
                server.CONTROL_PLANE_DB = Path(tmpdir) / "control_plane.db"
                server._control_plane_ready = False
                with server._control_plane_conn() as conn:
                    person = conn.execute(
                        "SELECT id FROM people WHERE auth_policy != 'byok_only' ORDER BY id LIMIT 1"
                    ).fetchone()
                self.assertIsNotNone(person)

                admin_headers = {"Authorization": f"Bearer {server.WEB_AUTH_TOKEN}"}
                project = client.post(
                    "/api/control/projects",
                    headers=admin_headers,
                    json={"person_id": person["id"], "name": "gateway-test", "initial_credits_usd": 5},
                )
                self.assertEqual(project.status_code, 200)
                project_id = project.json()["id"]
                issued = client.post(
                    f"/api/control/projects/{project_id}/keys",
                    headers=admin_headers,
                    json={"name": "test"},
                )
                self.assertEqual(issued.status_code, 200)
                api_key = issued.json()["api_key"]

                gateway = client.post(
                    "/v1/responses",
                    headers={"Authorization": f"Bearer {api_key}"},
                    json={"model": "gpt-4o", "input": "Say hello", "dry_run": True,
                          "api_key": "byok-test"},
                )
                self.assertEqual(gateway.status_code, 200)

                completion = {
                    "model": "gpt-4o",
                    "choices": [{"message": {"role": "assistant", "content": "managed"}}],
                    "usage": {"prompt_tokens": 10, "completion_tokens": 20, "total_tokens": 30},
                }
                with patch.dict(os.environ, {"OPENAI_API_KEY": "test-managed"}), patch.object(
                    server, "model_info",
                    return_value={"model_id": "gpt-4o", "input_price": 1.0, "output_price": 2.0,
                                  "max_output": 1024},
                ), patch.object(server, "_gateway_complete", AsyncMock(return_value=completion)):
                    managed = client.post(
                        "/v1/responses",
                        headers={"Authorization": f"Bearer {api_key}"},
                        json={"model": "gpt-4o", "input": "managed request"},
                    )
                self.assertEqual(managed.status_code, 200)
                self.assertEqual(gateway.json()["object"], "response")

                with server._control_plane_conn() as conn:
                    reservation_id = server._reserve_project_credits(
                        conn, project_id, "settlement-test", server._usd_to_microusd(2)
                    )
                    self.assertIsNotNone(reservation_id)
                    server._settle_project_reservation(
                        conn, reservation_id, server._usd_to_microusd(0.5)
                    )
                    balance = conn.execute(
                        "SELECT credit_balance_microusd FROM api_projects WHERE id = ?",
                        (project_id,),
                    ).fetchone()["credit_balance_microusd"]
                    entries = conn.execute(
                        "SELECT entry_type FROM credit_ledger WHERE project_id = ? ORDER BY created_at",
                        (project_id,),
                    ).fetchall()
                self.assertLess(balance, server._usd_to_microusd(4.5))
                self.assertIn("reserve", [entry["entry_type"] for entry in entries])
                self.assertIn("refund", [entry["entry_type"] for entry in entries])
            finally:
                server.CONTROL_PLANE_DB = original_db
                server._control_plane_ready = original_ready

    def test_dashboard_meta_exposes_limits_and_runbooks(self):
        client = TestClient(server.app, headers={"Authorization": f"Bearer {server.WEB_AUTH_TOKEN}"})

        resp = client.get("/api/dashboard/meta")

        self.assertEqual(resp.status_code, 200)
        data = resp.json()
        self.assertIn("limits", data)
        self.assertIn("runbooks", data)
        self.assertGreaterEqual(data["limits"]["list"], 1)
        self.assertGreaterEqual(len(data["runbooks"]), 1)

    def test_weather_dashboard_enriches_freshness_and_lineage(self):
        client = TestClient(server.app, headers={"Authorization": f"Bearer {server.WEB_AUTH_TOKEN}"})
        now = datetime.now(timezone.utc)

        class FakeRT:
            KALSHI_CITIES = {
                "nyc": ("New York City", "KNYC", "KLGA", 40.7, -73.9, "KXHIGHNY", "KXLOWNY", "NYC", "OKX"),
            }

            @staticmethod
            def dashboard(verbose=False):
                return [{
                    "ck": "nyc",
                    "stats": {
                        "current": 72.0,
                        "obs_max": 80.0,
                        "current_time": now - timedelta(minutes=45),
                        "trend_3h": 1.5,
                    },
                    "models": {"hrrr": 79.0, "nam": 78.0, "gfs": 77.0},
                    "est_high": 79.0,
                    "sigma": 4.0,
                }]

        with patch.object(server, "_lazy_import_weather", return_value=(SimpleNamespace(), SimpleNamespace(), FakeRT)):
            resp = client.get("/api/weather/dashboard?limit=1")

        self.assertEqual(resp.status_code, 200)
        data = resp.json()
        self.assertEqual(data["count"], 1)
        city = data["cities"][0]
        self.assertEqual(city["current_f"], 72.0)
        self.assertEqual(city["obs_max_f"], 80.0)
        self.assertEqual(city["freshness"]["status"], "fresh")
        self.assertIn("source_lineage", city)
        self.assertEqual(city["source_lineage"]["settlement_station"], "KNYC")

    def test_weather_dashboard_export_csv(self):
        client = TestClient(server.app, headers={"Authorization": f"Bearer {server.WEB_AUTH_TOKEN}"})

        class FakeRT:
            KALSHI_CITIES = {
                "nyc": ("New York City", "KNYC", "KLGA", 40.7, -73.9, "KXHIGHNY", "KXLOWNY", "NYC", "OKX"),
            }

            @staticmethod
            def dashboard(verbose=False):
                return [{
                    "ck": "nyc",
                    "stats": {"current": 71.0, "obs_max": 79.0, "current_time": datetime.now(timezone.utc)},
                    "models": {"hrrr": 78.0},
                    "est_high": 78.0,
                    "sigma": 4.0,
                }]

        with patch.object(server, "_lazy_import_weather", return_value=(SimpleNamespace(), SimpleNamespace(), FakeRT)):
            resp = client.get("/api/weather/dashboard/export?format=csv")

        self.assertEqual(resp.status_code, 200)
        self.assertIn("text/csv", resp.headers["content-type"])
        self.assertIn("settlement_station", resp.text)
        self.assertIn("nyc", resp.text)

    def test_trading_status_includes_market_state(self):
        client = TestClient(server.app, headers={"Authorization": f"Bearer {server.WEB_AUTH_TOKEN}"})

        resp = client.get("/api/trading/status")

        self.assertEqual(resp.status_code, 200)
        data = resp.json()
        self.assertIn("market_state", data)
        self.assertIn("no_market_data", data["market_state"])

    def test_files_endpoint_applies_limit_and_offset(self):
        client = TestClient(server.app, headers={"Authorization": f"Bearer {server.WEB_AUTH_TOKEN}"})

        with tempfile.TemporaryDirectory() as tmpdir:
            root = server.WORK_DIR
            try:
                tmp_root = Path(tmpdir).resolve()
                server.WORK_DIR = tmp_root
                (tmp_root / "a.txt").write_text("a")
                (tmp_root / "b.txt").write_text("b")
                (tmp_root / "c.txt").write_text("c")
                resp = client.get("/api/files?path=.&limit=2&offset=1")
            finally:
                server.WORK_DIR = root

        self.assertEqual(resp.status_code, 200)
        data = resp.json()
        self.assertEqual(data["limit"], 2)
        self.assertEqual(data["offset"], 1)
        self.assertLessEqual(len(data["entries"]), 2)

    def test_metrics_endpoint_tracks_requests(self):
        client = TestClient(server.app, headers={"Authorization": f"Bearer {server.WEB_AUTH_TOKEN}"})

        client.get("/health")
        resp = client.get("/api/metrics")

        self.assertEqual(resp.status_code, 200)
        data = resp.json()
        self.assertIn("/health", data["endpoints"])
        self.assertGreaterEqual(data["endpoints"]["/health"]["calls"], 1)


class BillingTests(unittest.TestCase):
    def _fresh_control_plane(self, tmpdir: str):
        server.CONTROL_PLANE_DB = Path(tmpdir) / "control_plane.db"
        server._control_plane_ready = False
        with server._control_plane_conn() as conn:
            person = conn.execute(
                "SELECT id, email FROM people WHERE plan_id = 'team-managed' ORDER BY id LIMIT 1"
            ).fetchone()
        return person

    def test_provider_identity_billability_matrix(self):
        byok = billing.resolve_provider_identity(provider="openai", auth_mode="byok_request", backend_id="")
        self.assertFalse(byok.billable)
        generic = billing.resolve_provider_identity(provider="openai", auth_mode="unknown", backend_id="")
        self.assertFalse(generic.billable)
        managed = billing.resolve_provider_identity(provider="openai", auth_mode="managed_env", backend_id="managed-openai")
        self.assertTrue(managed.billable)
        codex = billing.resolve_provider_identity(
            provider="openai", auth_mode="openai_codex_oauth", backend_id="",
            metadata={"provider_account_id": "acct_123"})
        self.assertTrue(codex.billable)
        self.assertEqual(codex.adapter, "openai_codex_oauth")

    def test_close_period_dry_run_reconcile_and_download(self):
        original_db = server.CONTROL_PLANE_DB
        original_ready = server._control_plane_ready
        with tempfile.TemporaryDirectory() as tmpdir:
            try:
                person = self._fresh_control_plane(tmpdir)
                client = TestClient(server.app, headers={"Authorization": f"Bearer {server.WEB_AUTH_TOKEN}"})

                acct = client.post("/api/billing/accounts",
                                   json={"person_id": person["id"], "billing_mode": "postpaid"})
                self.assertEqual(acct.status_code, 200)

                # Seed billable + unbillable usage last month.
                start, end = billing.period_bounds()
                # Backdate the commercial effective date for this historical-close fixture;
                # enrollment itself correctly starts a current trial by default.
                with server._control_plane_conn() as conn:
                    conn.execute("UPDATE people SET created_at=? WHERE id=?",
                                 ((start - timedelta(days=30)).isoformat(timespec="seconds"), person["id"]))
                    conn.execute("UPDATE billing_accounts SET trial_ends_at=NULL,created_at=? WHERE person_id=?",
                                 (start.isoformat(timespec="seconds"), person["id"]))
                    conn.commit()
                period = start.strftime("%Y-%m")
                ts = (start + timedelta(days=2)).isoformat(timespec="seconds")
                with server._control_plane_conn() as conn:
                    for i in range(4):
                        server._record_request_log(conn, {
                            "id": f"mgd-{i}", "person_id": person["id"], "person_email": person["email"],
                            "plan_id": "team-managed", "model": "gpt-4o", "provider": "openai",
                            "backend_id": "managed-openai", "auth_mode": "managed_env",
                            "route_source": "backend_catalog", "status": "ok",
                            "input_tokens": 2_000_000, "output_tokens": 800_000, "cache_read_tokens": 0,
                            "latency_ms": 1.0, "estimated_cost_usd": 1.0, "metadata": {}, "created_at": ts})
                    for i in range(3):
                        server._record_request_log(conn, {
                            "id": f"byok-{i}", "person_id": person["id"], "person_email": person["email"],
                            "plan_id": "team-managed", "model": "gpt-4o", "provider": "openai",
                            "backend_id": "", "auth_mode": "byok_request",
                            "route_source": "implicit_provider", "status": "ok",
                            "input_tokens": 5_000_000, "output_tokens": 5_000_000, "cache_read_tokens": 0,
                            "latency_ms": 1.0, "estimated_cost_usd": 0.0, "metadata": {}, "created_at": ts})
                    conn.commit()

                dry = client.post("/api/billing/close", json={"period": period})
                self.assertEqual(dry.status_code, 200)
                self.assertTrue(dry.json()["dry_run"])
                self.assertEqual(dry.json()["invoice_count"], 1)
                inv = dry.json()["invoices"][0]
                self.assertEqual(inv["unbillable_request_count"], 3)  # BYOK excluded
                self.assertGreater(inv["total_cents"], 0)
                # Dry-run persists nothing.
                empty = client.get("/api/billing/invoices")
                self.assertEqual(empty.json()["invoices"], [])

                applied = client.post("/api/billing/close", json={"period": period, "apply": True})
                self.assertEqual(applied.status_code, 200)
                self.assertFalse(applied.json()["dry_run"])
                # Idempotent: second apply keeps a single invoice + run.
                client.post("/api/billing/close", json={"period": period, "apply": True})

                listed = client.get("/api/billing/invoices").json()["invoices"]
                self.assertEqual(len(listed), 1)
                invoice_id = listed[0]["id"]

                detail = client.get(f"/api/billing/invoices/{invoice_id}").json()
                self.assertEqual(detail["total_cents"], inv["total_cents"])
                self.assertTrue(any(it["kind"] == "plan_base" for it in detail["items"]))

                # Reconciliation: local totals must match summed items.
                rec = client.post("/api/billing/reconcile", json={"invoice_id": invoice_id})
                self.assertTrue(rec.json()["ok"])

                # Signed customer download works; tampered signature is rejected.
                good = client.get(listed[0]["download_url"])
                self.assertEqual(good.status_code, 200)
                self.assertIn("Total", good.text)
                bad = client.get(f"/billing/invoice/{invoice_id}?sig=deadbeef")
                self.assertEqual(bad.status_code, 403)
            finally:
                server.CONTROL_PLANE_DB = original_db
                server._control_plane_ready = original_ready

    def test_dunning_blocks_access_after_grace(self):
        original_db = server.CONTROL_PLANE_DB
        original_ready = server._control_plane_ready
        with tempfile.TemporaryDirectory() as tmpdir:
            try:
                person = self._fresh_control_plane(tmpdir)
                now = server._now_iso()
                past_due = (datetime.now(timezone.utc) - timedelta(days=30)).isoformat(timespec="seconds")
                with server._control_plane_conn() as conn:
                    conn.execute(
                        """INSERT INTO billing_accounts(person_id,billing_email,billing_mode,status,
                           access_state,created_at,updated_at) VALUES(?,?,?,?,?,?,?)""",
                        (person["id"], person["email"], "postpaid", "active", "allowed", now, now))
                    conn.execute(
                        """INSERT INTO billing_invoices(id,person_id,plan_id,period_start,period_end,status,
                           subtotal_cents,total_cents,due_at,created_at,updated_at)
                           VALUES('past-inv',?,'team-managed',?,?,'open',5000,5000,?,?,?)""",
                        (person["id"], past_due, past_due, past_due, now, now))
                    conn.commit()
                    # Grace already elapsed -> access must be blocked.
                    result = billing.run_dunning(conn, dry_run=False)
                    conn.commit()
                self.assertGreaterEqual(result["action_count"], 1)
                with server._control_plane_conn() as conn:
                    err = billing.access_error(conn, person["id"])
                    notices = conn.execute("SELECT COUNT(*) FROM billing_notices").fetchone()[0]
                self.assertIsNotNone(err)
                self.assertEqual(err["code"], "billing_suspended")
                self.assertGreaterEqual(notices, 1)
            finally:
                server.CONTROL_PLANE_DB = original_db
                server._control_plane_ready = original_ready

    def test_plan_ratify_publishes_terms(self):
        original_db = server.CONTROL_PLANE_DB
        original_ready = server._control_plane_ready
        with tempfile.TemporaryDirectory() as tmpdir:
            try:
                server.CONTROL_PLANE_DB = Path(tmpdir) / "control_plane.db"
                server._control_plane_ready = False
                with server._control_plane_conn() as conn:  # warm up + init schema
                    conn.execute("SELECT 1")
                client = TestClient(server.app, headers={"Authorization": f"Bearer {server.WEB_AUTH_TOKEN}"})
                created = client.post("/api/control/plans", json={"name": "Enterprise Lane", "price_monthly": 999})
                self.assertEqual(created.status_code, 200)
                plan_id = created.json()["id"]
                # New operator plans start as draft.
                plans = {p["id"]: p for p in client.get("/api/control/plans").json()["plans"]}
                self.assertEqual(plans[plan_id]["terms_status"], "draft")
                # Invalid proration is rejected.
                bad = client.post(f"/api/control/plans/{plan_id}/ratify", json={"proration_policy": "weekly"})
                self.assertEqual(bad.status_code, 400)
                # Ratify with explicit trial/proration/retry terms.
                ok = client.post(f"/api/control/plans/{plan_id}/ratify",
                                 json={"billing_mode": "postpaid", "trial_days": 30,
                                       "proration_policy": "daily", "retry_schedule": [1, 5, 10]})
                self.assertEqual(ok.status_code, 200)
                self.assertEqual(ok.json()["terms_status"], "ratified")
                plans = {p["id"]: p for p in client.get("/api/control/plans").json()["plans"]}
                self.assertEqual(plans[plan_id]["terms_status"], "ratified")
                self.assertEqual(plans[plan_id]["trial_days"], 30)
                self.assertEqual(plans[plan_id]["proration_policy"], "daily")
            finally:
                server.CONTROL_PLANE_DB = original_db
                server._control_plane_ready = original_ready

    def test_stripe_finalize_is_idempotent(self):
        import httpx

        seen = []

        def handler(request: "httpx.Request") -> "httpx.Response":
            seen.append((request.method, request.url.path, request.headers.get("Idempotency-Key")))
            path = request.url.path
            if path == "/v1/customers":
                return httpx.Response(200, json={"id": "cus_x"})
            if path == "/v1/invoices" and request.method == "POST":
                return httpx.Response(200, json={"id": "in_x"})
            if path == "/v1/invoiceitems":
                return httpx.Response(200, json={"id": f"ii_{len(seen)}"})
            if path.endswith("/finalize"):
                return httpx.Response(200, json={"id": "in_x", "status": "open",
                    "due_date": 1739923200, "hosted_invoice_url": "https://pay/x", "invoice_pdf": "https://pay/x.pdf"})
            if path.startswith("/v1/invoices/in_x"):
                return httpx.Response(200, json={"id": "in_x", "status": "paid", "total": 4900})
            return httpx.Response(404, json={"error": {"message": "unmocked " + path}})

        client = httpx.Client(transport=httpx.MockTransport(handler))
        stripe = billing.StripeREST(api_key="sk_test_x", client=client)
        self.assertTrue(stripe.configured)
        cust = stripe.create_customer(email="a@x.io", name="A", person_id="p1")
        inv = stripe.create_invoice(customer=cust["id"], invoice_id="loc1", period="2026-02", days_until_due=7)
        item = stripe.create_item(customer=cust["id"], stripe_invoice=inv["id"], invoice_id="loc1",
                                  item={"amount_cents": 4900, "description": "base", "item_key": "plan-base"})
        fin = stripe.finalize_invoice(inv["id"], "loc1")
        remote = stripe.get_invoice(inv["id"])
        self.assertEqual(fin["status"], "open")
        self.assertEqual(remote["status"], "paid")
        # Every mutating POST must carry a deterministic idempotency key.
        posts = [(m, p, k) for (m, p, k) in seen if m == "POST"]
        self.assertTrue(all(k for _, _, k in posts))
        self.assertIn("dsco-finalize-loc1", [k for _, _, k in posts])


if __name__ == "__main__":
    unittest.main()
