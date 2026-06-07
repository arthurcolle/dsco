import copy
import unittest
from types import SimpleNamespace
from unittest.mock import AsyncMock, patch

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


if __name__ == "__main__":
    unittest.main()
