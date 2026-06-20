import copy
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from types import SimpleNamespace
from unittest.mock import AsyncMock, patch
from pathlib import Path

from fastapi.testclient import TestClient

from web import server


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

    def test_dashboard_meta_exposes_limits_and_runbooks(self):
        client = TestClient(server.app)

        resp = client.get("/api/dashboard/meta")

        self.assertEqual(resp.status_code, 200)
        data = resp.json()
        self.assertIn("limits", data)
        self.assertIn("runbooks", data)
        self.assertGreaterEqual(data["limits"]["list"], 1)
        self.assertGreaterEqual(len(data["runbooks"]), 1)

    def test_weather_dashboard_enriches_freshness_and_lineage(self):
        client = TestClient(server.app)
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
        client = TestClient(server.app)

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
        client = TestClient(server.app)

        resp = client.get("/api/trading/status")

        self.assertEqual(resp.status_code, 200)
        data = resp.json()
        self.assertIn("market_state", data)
        self.assertIn("no_market_data", data["market_state"])

    def test_files_endpoint_applies_limit_and_offset(self):
        client = TestClient(server.app)

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
        client = TestClient(server.app)

        client.get("/health")
        resp = client.get("/api/metrics")

        self.assertEqual(resp.status_code, 200)
        data = resp.json()
        self.assertIn("/health", data["endpoints"])
        self.assertGreaterEqual(data["endpoints"]["/health"]["calls"], 1)


if __name__ == "__main__":
    unittest.main()
