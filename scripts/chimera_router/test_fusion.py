#!/usr/bin/env python3
"""Deterministic tests for the DuckDB fusion boundary."""

from __future__ import annotations

from contextlib import redirect_stdout
from decimal import Decimal
import io
import json
from pathlib import Path
import sqlite3
import struct
import tempfile
import unittest
import zlib

try:
    from .fusion import (
        DuckDBCLI,
        FusionError,
        StagingArea,
        _export_external_preferences,
        main,
    )
except ImportError:
    from fusion import (  # type: ignore
        DuckDBCLI,
        FusionError,
        StagingArea,
        _export_external_preferences,
        main,
    )


DUCKDB = Path("/opt/homebrew/bin/duckdb")


def _registry_row(source_key: str) -> dict[str, object]:
    return {
        "source_key": source_key,
        "display_name": source_key,
        "source_class": "local_private",
        "adapter": source_key + "_v1",
        "format": "sqlite" if "catalog" not in source_key else "json",
        "grain": "fixture row",
        "stable_ids_json": "[]",
        "source_url": None,
        "license_spdx": None,
        "license_url": None,
        "pii_risk": "low",
        "trust_tier": "local_private",
        "default_enabled": True,
        "refresh_cadence": "test",
        "notes": "test fixture",
    }


def _baseline(path: Path) -> None:
    connection = sqlite3.connect(path)
    connection.executescript(
        """
        CREATE TABLE instances(
          instance_id TEXT PRIMARY KEY,parent_instance_id TEXT,pid INTEGER,
          model TEXT,mode TEXT,started_at TEXT,ended_at TEXT
        );
        CREATE TABLE events(
          id INTEGER PRIMARY KEY,instance_id TEXT,ts TEXT,ts_epoch REAL,
          category TEXT,title TEXT,detail TEXT,metadata_json TEXT,
          input_tokens INTEGER,output_tokens INTEGER,cache_read_tokens INTEGER,
          cache_write_tokens INTEGER,est_cost_usd REAL
        );
        CREATE TABLE trace_spans(
          span_id TEXT PRIMARY KEY,trace_id TEXT,parent_span TEXT,name TEXT,
          start_epoch REAL,end_epoch REAL,status TEXT,metadata_json TEXT
        );
        INSERT INTO instances VALUES(
          'instance-1',NULL,123,'acme/model-one','agent',
          '2026-08-01T00:00:00Z','2026-08-01T00:01:00Z'
        );
        INSERT INTO events VALUES(
          1,'instance-1','2026-08-01T00:00:01Z',1785542401,
          'user','prompt','secret fixture prompt','{}',0,0,0,0,0
        );
        INSERT INTO events VALUES(
          2,'instance-1','2026-08-01T00:00:05Z',1785542405,
          'turn','turn_done','secret response',
          '{"provider":"acme","model":"acme/model-one","billing":"payg"}',
          10,5,2,0,0.002
        );
        INSERT INTO trace_spans VALUES(
          'span-1','trace-1',NULL,'turn',1785542401,1785542405,'ok','{}'
        );
        """
    )
    connection.commit()
    connection.close()


def _evals(path: Path) -> None:
    connection = sqlite3.connect(path)
    connection.executescript(
        """
        CREATE TABLE models(
          id TEXT PRIMARY KEY,name TEXT,provider TEXT,created INTEGER,
          context_length INTEGER,prompt_price REAL,completion_price REAL,last_seen TEXT
        );
        CREATE TABLE evals(
          id INTEGER PRIMARY KEY,model_id TEXT,run_date TEXT,tier INTEGER,
          target_tokens INTEGER,actual_tokens INTEGER,latency_ms REAL,
          http_status INTEGER,ok INTEGER,snippet TEXT,error_msg TEXT
        );
        INSERT INTO models VALUES(
          'acme/model-one','Model One','acme',1780000000,32768,
          0.000001,0.000002,'2026-08-01'
        );
        INSERT INTO evals VALUES(
          1,'acme/model-one','2026-08-01',1,32,32,120,200,1,'private',''
        );
        """
    )
    connection.commit()
    connection.close()


@unittest.skipUnless(DUCKDB.is_file(), "DuckDB CLI is not installed")
class FusionBuildTests(unittest.TestCase):
    def test_tiny_build_redacts_raw_content_and_fuses_model_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            baseline = root / "baseline.db"
            evals = root / "evals.db"
            catalog = root / "catalog.json"
            registry = root / "registry.json"
            warehouse = root / "fusion.duckdb"
            _baseline(baseline)
            _evals(evals)
            catalog.write_text(
                json.dumps(
                    {
                        "data": [
                            {
                                "id": "acme/model-one",
                                "canonical_slug": "acme/model-one",
                                "name": "Model One",
                                "description": "fixture",
                                "context_length": 32768,
                                "architecture": {
                                    "input_modalities": ["text"],
                                    "output_modalities": ["text"],
                                },
                                "pricing": {
                                    "prompt": "0.000001",
                                    "completion": "0.000002",
                                },
                                "benchmarks": {
                                    "artificial_analysis": {"intelligence_index": 70}
                                },
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )
            registry.write_text(
                json.dumps(
                    {
                        "sources": [
                            _registry_row("dsco_baseline"),
                            _registry_row("openrouter_local_evals"),
                            _registry_row("openrouter_catalog"),
                        ]
                    }
                ),
                encoding="utf-8",
            )
            output = io.StringIO()
            with redirect_stdout(output):
                result = main(
                    [
                        "build",
                        "--warehouse",
                        str(warehouse),
                        "--registry",
                        str(registry),
                        "--baseline-db",
                        str(baseline),
                        "--chronicle-db",
                        str(root / "missing-chronicle.db"),
                        "--evals-db",
                        str(evals),
                        "--feedback-db",
                        str(root / "missing-feedback.db"),
                        "--catalog",
                        str(catalog),
                        "--skip-native-model-catalog",
                        "--skip-session-metrics",
                        "--skip-run-wal",
                        "--skip-provider-metadata",
                    ]
                )
            self.assertEqual(result, 0)
            summary = json.loads(output.getvalue())
            self.assertEqual(summary["baseline_events"], 2)
            self.assertEqual(summary["catalog_models"], 1)
            self.assertEqual(summary["benchmark_observations"], 1)
            cli = DuckDBCLI(DUCKDB)
            evidence = cli.query_json(
                warehouse,
                "SELECT model_id,baseline_turn_count,eval_count,intelligence_index "
                "FROM model_evidence",
            )
            self.assertEqual(evidence[0]["model_id"], "acme/model-one")
            self.assertEqual(evidence[0]["baseline_turn_count"], 1)
            self.assertEqual(evidence[0]["eval_count"], 1)
            columns = cli.query_json(
                warehouse,
                "SELECT lower(column_name) AS column_name FROM duckdb_columns() "
                "WHERE table_name IN ('baseline_event','openrouter_eval')",
            )
            names = {row["column_name"] for row in columns}
            self.assertFalse(
                {"detail", "metadata_json", "snippet", "error_msg"} & names
            )
            database_bytes = warehouse.read_bytes()
            self.assertNotIn(b"secret fixture prompt", database_bytes)
            self.assertNotIn(b"secret response", database_bytes)

    def test_native_catalog_preserves_provider_identity_and_redacts_transport_fields(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            catalog = root / "openrouter.json"
            native = root / "native-models.json"
            providers = root / "providers"
            registry = root / "registry.json"
            warehouse = root / "fusion.duckdb"
            providers.mkdir()
            catalog.write_text('{"data":[]}', encoding="utf-8")

            def descriptor(
                provider: str,
                model_id: str,
                *,
                cost: dict[str, float],
                supports_tools: object = True,
            ) -> dict[str, object]:
                value: dict[str, object] = {
                    "id": model_id,
                    "name": model_id,
                    "api": "openai-completions",
                    "provider": provider,
                    "baseUrl": "https://transport-secret.invalid/v1",
                    "headers": {"X-Secret": "native-header-sentinel"},
                    "reasoning": True,
                    "input": ["text", "image"],
                    "cost": cost,
                    "contextWindow": 10000,
                    "maxTokens": 1000,
                    "thinking": {
                        "mode": "effort",
                        "efforts": ["low", "high"],
                        "effortBudgets": {"low": 128, "high": 1024},
                    },
                    "compat": {
                        "supportsDeveloperRole": True,
                        "extraBody": {"secret": "compat-extra-body-sentinel"},
                    },
                }
                if supports_tools is not None:
                    value["supportsTools"] = supports_tools
                return value

            priced = {"input": 2, "output": 4, "cacheRead": 0.2, "cacheWrite": 0.5}
            zero = {"input": 0, "output": 0, "cacheRead": 0, "cacheWrite": 0}
            dynamic = {
                "input": -1000000,
                "output": -1000000,
                "cacheRead": 0,
                "cacheWrite": 0,
            }
            native.write_text(
                json.dumps(
                    {
                        "provider-a": {
                            "Model-X": descriptor(
                                "provider-a",
                                "Model-X",
                                cost=priced,
                                supports_tools=False,
                            ),
                            "model-x": descriptor(
                                "provider-a", "model-x", cost=zero, supports_tools=None
                            ),
                        },
                        "provider-b": {
                            "Model-X": descriptor("provider-b", "Model-X", cost=priced)
                        },
                        "openrouter": {
                            "openrouter/auto": descriptor(
                                "openrouter", "openrouter/auto", cost=dynamic
                            )
                        },
                    }
                ),
                encoding="utf-8",
            )
            (providers / "provider-a.json").write_text(
                json.dumps(
                    {
                        "provider": "Provider-A",
                        "display_name": "Provider A",
                        "confidence": 0.8,
                        "last_reviewed": "2026-08-01",
                        "dsco": {
                            "provider_profile": "native",
                            "risk": "medium",
                            "implemented_features": ["streaming"],
                            "missing_features": [],
                            "tests": ["fixture"],
                        },
                        "wire_apis": [
                            {
                                "modalities": {
                                    "input": ["text", "image"],
                                    "output": ["text"],
                                },
                                "streaming": {"supported": True},
                                "tools": {
                                    "supported": True,
                                    "parallel_tool_calls": False,
                                    "strict_schema": True,
                                },
                            }
                        ],
                        "reasoning": {
                            "supported": True,
                            "field": "reasoning_effort",
                            "efforts": ["low", "high"],
                            "unsupported_values_rejected": True,
                        },
                        "prompt_caching": {
                            "status": "supported",
                            "minimum_cacheable_tokens": 1024,
                            "ttl": "5m",
                            "mechanisms": [],
                        },
                        "streaming": {
                            "request_max_retries": 2,
                            "stream_max_retries": 1,
                            "idle_timeout_ms_recommended": 30000,
                        },
                        "cost_management": {"status": "supported", "levers": []},
                        "usage_accounting": {
                            "cached_tokens": ["cache_read"],
                            "reasoning_tokens": ["reasoning"],
                            "audio_tokens": [],
                            "image_tokens": [],
                        },
                        "api_coverage": {"endpoint_groups": [], "hosted_tools": []},
                        "auth": {"env_keys": ["provider-auth-sentinel"]},
                        "notes": ["provider-note-sentinel"],
                    }
                ),
                encoding="utf-8",
            )
            registry.write_text(
                json.dumps(
                    {
                        "sources": [
                            _registry_row("openrouter_catalog"),
                            _registry_row("oh_my_pi_model_catalog"),
                            _registry_row("dsco_provider_metadata"),
                        ]
                    }
                ),
                encoding="utf-8",
            )
            output = io.StringIO()
            with redirect_stdout(output):
                result = main(
                    [
                        "build",
                        "--warehouse",
                        str(warehouse),
                        "--registry",
                        str(registry),
                        "--baseline-db",
                        str(root / "missing-baseline.db"),
                        "--chronicle-db",
                        str(root / "missing-chronicle.db"),
                        "--evals-db",
                        str(root / "missing-evals.db"),
                        "--feedback-db",
                        str(root / "missing-feedback.db"),
                        "--catalog",
                        str(catalog),
                        "--native-model-catalog",
                        str(native),
                        "--provider-metadata-dir",
                        str(providers),
                        "--skip-session-metrics",
                        "--skip-run-wal",
                    ]
                )
            self.assertEqual(result, 0)
            summary = json.loads(output.getvalue())
            self.assertEqual(summary["native_model_offers"], 4)
            self.assertEqual(summary["native_catalog_providers"], 3)
            self.assertEqual(summary["native_normalized_collision_groups"], 1)
            self.assertEqual(summary["provider_capabilities"], 1)

            cli = DuckDBCLI(DUCKDB)
            exact = cli.query_json(
                warehouse,
                "SELECT model_id,inference_provider_id,pricing_status,"
                "supports_tools_declared,native_tools_usable "
                "FROM native_model_offer_snapshot ORDER BY 1,2",
            )
            self.assertEqual(len(exact), 4)
            self.assertEqual(sum(row["model_id"] == "Model-X" for row in exact), 2)
            self.assertTrue(any(row["model_id"] == "model-x" for row in exact))
            provider_a = next(
                row
                for row in exact
                if row["model_id"] == "Model-X"
                and row["inference_provider_id"] == "provider-a"
            )
            self.assertFalse(provider_a["supports_tools_declared"])
            self.assertFalse(provider_a["native_tools_usable"])
            unspecified = next(row for row in exact if row["model_id"] == "model-x")
            self.assertIsNone(unspecified["supports_tools_declared"])
            self.assertTrue(unspecified["native_tools_usable"])
            self.assertEqual(unspecified["pricing_status"], "zero_unspecified")

            prices = cli.query_json(
                warehouse,
                "SELECT model_id,provider_id,"
                "CAST(prompt_price_usd_token AS VARCHAR) prompt_price,"
                "output_modalities_json,pricing_status "
                "FROM model_provider_offer ORDER BY 1,2",
            )
            quoted = next(
                row
                for row in prices
                if row["model_id"] == "Model-X" and row["provider_id"] == "provider-a"
            )
            self.assertEqual(Decimal(quoted["prompt_price"]), Decimal("0.000002"))
            self.assertIsNone(quoted["output_modalities_json"])
            auto = next(row for row in prices if row["model_id"] == "openrouter/auto")
            self.assertIsNone(auto["prompt_price"])
            self.assertEqual(auto["pricing_status"], "dynamic_unavailable")
            universe = cli.query_json(
                warehouse,
                "SELECT model_id,array_length(inference_providers) provider_count "
                "FROM current_routable_model_universe "
                "WHERE normalized_model_id='model-x' ORDER BY model_id",
            )
            self.assertEqual(
                [row["model_id"] for row in universe], ["Model-X", "model-x"]
            )
            self.assertEqual(universe[0]["provider_count"], 2)
            reviewed = cli.query_json(
                warehouse,
                "SELECT provider_id,CAST(last_reviewed_date AS VARCHAR) reviewed "
                "FROM provider_capability_snapshot",
            )
            self.assertEqual(
                reviewed, [{"provider_id": "provider-a", "reviewed": "2026-08-01"}]
            )

            forbidden = (
                b"native-header-sentinel",
                b"compat-extra-body-sentinel",
                b"provider-auth-sentinel",
                b"provider-note-sentinel",
                b"transport-secret.invalid",
            )
            artifacts = [warehouse]
            artifacts.extend(Path(summary["parquet_snapshot"]).glob("*.parquet"))
            for artifact in artifacts:
                payload = artifact.read_bytes()
                for sentinel in forbidden:
                    self.assertNotIn(sentinel, payload, str(artifact))

    def test_run_wal_is_crc_checked_redacted_and_not_double_counted(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            runs = root / "runs"
            run_id = "run-wal-fixture"
            run_dir = runs / run_id
            run_dir.mkdir(parents=True)
            catalog = root / "catalog.json"
            registry = root / "registry.json"
            warehouse = root / "fusion.duckdb"
            catalog.write_text('{"data":[]}', encoding="utf-8")
            manifest = {
                "schema": "dsco.run-manifest.v1",
                "run_id": run_id,
                "session_id": run_id,
                "installation_id": "fixture",
                "status": "running",
                "started_at": "2026-08-01T00:00:00Z",
                "ended_at": None,
                "updated_at": "2026-08-01T00:00:00Z",
                "schema_version": "1",
                "capture_mode": "fixture",
                "instance_id": None,
                "seq": 6,
                "chronicle_root": str(Path("~/.dsco/chronicle").expanduser()),
                "db_path": str(
                    Path("~/.dsco/chronicle/indexes/chronicle.sqlite").expanduser()
                ),
                "cwd": "/wal-cwd-secret",
            }
            (run_dir / "manifest.json").write_text(
                json.dumps(manifest), encoding="utf-8"
            )

            def outer(
                sequence: int, event_type: str, event_name: str, payload: object
            ) -> dict[str, object]:
                return {
                    "v": 1,
                    "type": event_type,
                    "run_id": run_id,
                    "seq": sequence,
                    "wall_ms": 1785542400000 + sequence,
                    "payload": {
                        "schema": "dsco.agent-event.v1",
                        "event_id": f"event-{sequence}",
                        "event": event_name,
                        "run_id": run_id,
                        "status": "ok",
                        "payload": payload,
                    },
                }

            events = [
                outer(
                    2,
                    "run.started",
                    "run.started",
                    {"run_id": run_id, "cwd": "/wal-cwd-secret"},
                ),
                outer(
                    3,
                    "tool.call",
                    "tool.call",
                    {
                        "tool": "search",
                        "tool_id": "tool-1",
                        "args_bytes": 55,
                        "args": {"query": "wal-args-secret"},
                    },
                ),
                outer(
                    4,
                    "rl.trajectory.event",
                    "tool.call",
                    {
                        "tool": "search",
                        "tool_id": "tool-1",
                        "args_bytes": 55,
                        "args": {"query": "wal-wrapper-secret"},
                    },
                ),
                outer(
                    5,
                    "tool.result",
                    "tool.result",
                    {
                        "tool": "search",
                        "tool_id": "tool-1",
                        "ok": False,
                        "latency_ms": 12.5,
                        "result_bytes": 44,
                        "result_preview": "wal-result-secret",
                    },
                ),
                outer(
                    6,
                    "run.receipt",
                    "run.receipt",
                    {
                        "status": "completed",
                        "duration_ms": 1234,
                        "journal_records": 5,
                        "cost": {
                            "usd_total": 0.0123,
                            "input_tokens": 100,
                            "output_tokens": 20,
                            "cache_read_tokens": 30,
                            "cache_write_tokens": 4,
                            "reasoning_tokens": 5,
                        },
                    },
                ),
            ]
            framed = bytearray()
            first_payload_length = 0
            first_crc = 0
            for index, event in enumerate(events):
                payload = (json.dumps(event, separators=(",", ":")) + "\n").encode()
                crc = zlib.crc32(payload) & 0xFFFFFFFF
                if index == 0:
                    first_payload_length = len(payload)
                    first_crc = crc
                framed.extend(struct.pack("<II", len(payload), crc))
                framed.extend(payload)
            framed.extend(b"\x03\x00\x00")
            (run_dir / "journal.wal").write_bytes(bytes(framed))
            registry.write_text(
                json.dumps(
                    {
                        "sources": [
                            _registry_row("openrouter_catalog"),
                            _registry_row("dsco_run_manifests"),
                            _registry_row("dsco_run_wal"),
                        ]
                    }
                ),
                encoding="utf-8",
            )
            output = io.StringIO()
            with redirect_stdout(output):
                result = main(
                    [
                        "build",
                        "--warehouse",
                        str(warehouse),
                        "--registry",
                        str(registry),
                        "--baseline-db",
                        str(root / "missing-baseline.db"),
                        "--chronicle-db",
                        str(root / "missing-chronicle.db"),
                        "--evals-db",
                        str(root / "missing-evals.db"),
                        "--feedback-db",
                        str(root / "missing-feedback.db"),
                        "--catalog",
                        str(catalog),
                        "--skip-native-model-catalog",
                        "--skip-provider-metadata",
                        "--skip-session-metrics",
                        "--runs-dir",
                        str(runs),
                    ]
                )
            self.assertEqual(result, 0)
            summary = json.loads(output.getvalue())
            self.assertEqual(summary["run_manifests"], 1)
            self.assertEqual(summary["run_wal_events"], 5)
            cli = DuckDBCLI(DUCKDB)
            first = cli.query_json(
                warehouse,
                "SELECT frame_offset,frame_length,frame_crc32,epoch_ms(event_at) event_ms "
                "FROM run_wal_event WHERE sequence_number=2",
            )[0]
            self.assertEqual(first["frame_offset"], 0)
            self.assertEqual(first["frame_length"], first_payload_length)
            self.assertEqual(first["frame_crc32"], first_crc)
            self.assertEqual(first["event_ms"], 1785542400002)
            execution = cli.query_json(
                warehouse,
                "SELECT derived_status,stale_running_at_snapshot,trajectory_events,"
                "tool_calls,tool_failures,input_tokens,output_tokens,"
                "cache_read_tokens,cache_write_tokens,reasoning_tokens,"
                "final_cost_usd,duration_ms FROM run_execution_fact",
            )[0]
            self.assertEqual(execution["derived_status"], "completed")
            self.assertFalse(execution["stale_running_at_snapshot"])
            self.assertEqual(execution["trajectory_events"], 1)
            self.assertEqual(execution["tool_calls"], 1)
            self.assertEqual(execution["tool_failures"], 1)
            self.assertEqual(execution["input_tokens"], 100)
            self.assertEqual(execution["output_tokens"], 20)
            self.assertEqual(execution["cache_read_tokens"], 30)
            self.assertEqual(execution["cache_write_tokens"], 4)
            self.assertEqual(execution["reasoning_tokens"], 5)
            self.assertAlmostEqual(execution["final_cost_usd"], 0.0123)
            self.assertEqual(execution["duration_ms"], 1234)
            provenance = cli.query_json(
                warehouse,
                "SELECT provenance_json FROM source_snapshot "
                "WHERE source_key='dsco_run_wal'",
            )[0]
            self.assertEqual(
                json.loads(provenance["provenance_json"])["corrupt_or_truncated_tails"],
                1,
            )
            forbidden = (
                b"wal-cwd-secret",
                b"wal-args-secret",
                b"wal-wrapper-secret",
                b"wal-result-secret",
            )
            artifacts = [warehouse]
            artifacts.extend(Path(summary["parquet_snapshot"]).glob("*.parquet"))
            for artifact in artifacts:
                payload = artifact.read_bytes()
                for sentinel in forbidden:
                    self.assertNotIn(sentinel, payload, str(artifact))

    def test_incident_and_swarm_metrics_fuse_without_raw_content(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            incidents = root / "incidents"
            swarm = root / "swarm"
            swarm_run = swarm / "4242"
            incidents.mkdir()
            swarm_run.mkdir(parents=True)
            catalog = root / "catalog.json"
            registry = root / "registry.json"
            warehouse = root / "fusion.duckdb"
            catalog.write_text('{"data":[]}', encoding="utf-8")
            (incidents / "incident-1.json").write_text(
                json.dumps(
                    {
                        "event": "supervisor_incident",
                        "ts": 1785542400,
                        "supervisor_pid": 42,
                        "child_pid": 43,
                        "class": "oom_kill",
                        "signal": "SIGKILL",
                        "signal_num": 9,
                        "exit_code": 137,
                        "action": "restart_pending",
                        "restart_count": 2,
                        "next_delay_ms": 500,
                        "uptime_s": 12.5,
                        "peak_rss_mb": 8192,
                        "mem_budget_mb": 8192,
                        "mem_soft_mb": 7000,
                        "mem_pressure": 1,
                        "poll_ms": 500,
                        "preempted": False,
                        "tracer_reaped": True,
                        "resume_after_crash": True,
                        "mem_restart": True,
                        "last_heartbeat_pid": 43,
                        "last_heartbeat_age_s": 2,
                        "last_heartbeat_pid_matches_child": True,
                        "crash_log_present": True,
                        "debugger_backtrace_present": True,
                        "child_cmdline": "incident-command-secret",
                        "paths": {"crash_log": "/incident-path-secret"},
                        "crash_log_excerpt": "incident-crash-secret",
                        "debugger_backtrace_excerpt": "incident-backtrace-secret",
                    }
                ),
                encoding="utf-8",
            )
            task = "compare the same private task"
            for child_id, model, status, output_text in (
                (1, "Vendor/Model-A", "done", "swarm-output-secret-a"),
                (2, "Vendor/Model-B", "error", "swarm-output-secret-b"),
            ):
                (swarm_run / f"child-{child_id}.RESULT.json").write_text(
                    json.dumps(
                        {
                            "id": child_id,
                            "task": task,
                            "model": model,
                            "status": status,
                            "exit_code": 0 if status == "done" else 1,
                            "duration_s": float(child_id),
                            "cost_usd": 0.001 * child_id,
                            "output": output_text,
                        }
                    ),
                    encoding="utf-8",
                )
            degenerate_task = "repeated empty operational probe"
            for child_id in range(3, 23):
                (swarm_run / f"child-{child_id}.RESULT.json").write_text(
                    json.dumps(
                        {
                            "id": child_id,
                            "task": degenerate_task,
                            "model": f"Vendor/Model-{child_id % 2}",
                            "status": "done",
                            "exit_code": 0,
                            "duration_s": 0.01 if child_id < 22 else 0.2,
                            "cost_usd": 0,
                            "output": "",
                        }
                    ),
                    encoding="utf-8",
                )
            registry.write_text(
                json.dumps(
                    {
                        "sources": [
                            _registry_row("openrouter_catalog"),
                            _registry_row("dsco_incident_metrics"),
                            _registry_row("dsco_swarm_child_results"),
                        ]
                    }
                ),
                encoding="utf-8",
            )
            output = io.StringIO()
            with redirect_stdout(output):
                result = main(
                    [
                        "build",
                        "--warehouse",
                        str(warehouse),
                        "--registry",
                        str(registry),
                        "--baseline-db",
                        str(root / "missing-baseline.db"),
                        "--chronicle-db",
                        str(root / "missing-chronicle.db"),
                        "--evals-db",
                        str(root / "missing-evals.db"),
                        "--feedback-db",
                        str(root / "missing-feedback.db"),
                        "--catalog",
                        str(catalog),
                        "--skip-native-model-catalog",
                        "--skip-provider-metadata",
                        "--skip-session-metrics",
                        "--skip-run-wal",
                        "--include-incident-metrics",
                        "--incidents-dir",
                        str(incidents),
                        "--include-swarm-results",
                        "--swarm-results-dir",
                        str(swarm),
                    ]
                )
            self.assertEqual(result, 0)
            summary = json.loads(output.getvalue())
            self.assertEqual(summary["runtime_incidents"], 1)
            self.assertEqual(summary["swarm_child_results"], 22)
            self.assertEqual(summary["swarm_comparison_groups"], 2)
            cli = DuckDBCLI(DUCKDB)
            incident = cli.query_json(
                warehouse,
                "SELECT incident_class,memory_restart,peak_rss_mb "
                "FROM runtime_incident",
            )[0]
            self.assertEqual(incident["incident_class"], "oom_kill")
            self.assertTrue(incident["memory_restart"])
            self.assertEqual(incident["peak_rss_mb"], 8192)
            comparison = cli.query_json(
                warehouse,
                "SELECT task_sha256,model_count,child_results,process_success_count,"
                "output_present_rate,degenerate_group,comparative_operational_usable "
                "FROM swarm_comparison_group ORDER BY child_results",
            )
            normal_comparison = comparison[0]
            degenerate_comparison = comparison[1]
            self.assertEqual(normal_comparison["model_count"], 2)
            self.assertEqual(normal_comparison["child_results"], 2)
            self.assertEqual(normal_comparison["process_success_count"], 1)
            self.assertEqual(normal_comparison["output_present_rate"], 1)
            self.assertFalse(normal_comparison["degenerate_group"])
            self.assertTrue(normal_comparison["comparative_operational_usable"])
            self.assertEqual(degenerate_comparison["child_results"], 20)
            self.assertTrue(degenerate_comparison["degenerate_group"])
            self.assertFalse(degenerate_comparison["comparative_operational_usable"])
            model_ids = cli.query_json(
                warehouse,
                "SELECT requested_model_id,normalized_model_id,output_sha256,"
                "duration_ms,CAST(cost_usd_recorded AS VARCHAR) cost_usd "
                "FROM swarm_child_result ORDER BY child_id",
            )
            self.assertEqual(model_ids[0]["requested_model_id"], "Vendor/Model-A")
            self.assertEqual(model_ids[0]["normalized_model_id"], "vendor/model-a")
            self.assertIsNotNone(model_ids[0]["output_sha256"])
            self.assertEqual(model_ids[0]["duration_ms"], 1000)
            self.assertEqual(Decimal(model_ids[0]["cost_usd"]), Decimal("0.001000"))
            forbidden = (
                b"incident-command-secret",
                b"incident-path-secret",
                b"incident-crash-secret",
                b"incident-backtrace-secret",
                b"swarm-output-secret-a",
                b"swarm-output-secret-b",
                task.encode(),
                degenerate_task.encode(),
            )
            artifacts = [warehouse]
            artifacts.extend(Path(summary["parquet_snapshot"]).glob("*.parquet"))
            for artifact in artifacts:
                payload = artifact.read_bytes()
                for sentinel in forbidden:
                    self.assertNotIn(sentinel, payload, str(artifact))

    def test_external_preference_rejects_raw_text_at_any_depth(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "unsafe.jsonl"
            source.write_text(
                json.dumps(
                    {
                        "source_key": "arena",
                        "model_a": "a/model",
                        "model_b": "b/model",
                        "winner": "a",
                        "metadata": {"prompt": "do not ingest me"},
                    }
                )
                + "\n",
                encoding="utf-8",
            )
            staging = StagingArea(root / "stage")
            staging.root.mkdir()
            with self.assertRaisesRegex(FusionError, "forbidden raw-content field"):
                _export_external_preferences(source, staging)
            staging.close()


if __name__ == "__main__":
    unittest.main()
