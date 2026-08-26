#!/usr/bin/env python3
"""Loopback JSON service for the portable Chimera candidate router.

The service is intentionally not an OpenRouter proxy: it never receives an API
key and never performs paid inference.  It turns a request into a ranked model
plan plus the exact ``models``/``provider`` patch a caller can apply to an
OpenRouter request.  Router weights and the catalog are reloaded atomically when
their files change, so the HTTP contract can remain stable across retraining and
catalog churn.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple

try:
    from .ensemble import RouterEnsemble
    from .feedback import FeedbackStore
    from .planner import PLAN_SCHEMA_VERSION, plan_request
    from .route import (
        ROUTE_SCHEMA_VERSION,
        RouterModel,
        _artifact_feature_version,
        _candidate_vector,
        _request_task_vector,
        load_catalog,
        route_request,
    )
except ImportError:  # Direct execution.
    from ensemble import RouterEnsemble
    from feedback import FeedbackStore
    from planner import PLAN_SCHEMA_VERSION, plan_request
    from route import (
        ROUTE_SCHEMA_VERSION,
        RouterModel,
        _artifact_feature_version,
        _candidate_vector,
        _request_task_vector,
        load_catalog,
        route_request,
    )


SERVICE_SCHEMA_VERSION = 2
MAX_REQUEST_BYTES = 4 * 1024 * 1024


def _fingerprint(path: Path) -> Tuple[int, int]:
    stat = path.stat()
    return int(stat.st_mtime_ns), int(stat.st_size)


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class RouterState:
    """Thread-safe, auto-reloading model and catalog state."""

    def __init__(
        self,
        router_path: Path,
        catalog_path: Path,
        quality_router_path: Optional[Path] = None,
        feedback_db: Optional[Path] = None,
    ) -> None:
        self.router_path = Path(router_path).expanduser().resolve()
        self.catalog_path = Path(catalog_path).expanduser().resolve()
        self.quality_router_path = (
            None
            if quality_router_path is None
            else Path(quality_router_path).expanduser().resolve()
        )
        self._lock = threading.RLock()
        self._router: Optional[RouterModel] = None
        self._quality_router: Optional[RouterModel] = None
        self._catalog: List[Dict[str, Any]] = []
        self._router_fingerprint: Optional[Tuple[int, int]] = None
        self._catalog_fingerprint: Optional[Tuple[int, int]] = None
        self._quality_router_fingerprint: Optional[Tuple[int, int]] = None
        self.router_sha256 = ""
        self.catalog_sha256 = ""
        self.quality_router_sha256 = ""
        self.feedback_store = (
            None if feedback_db is None else FeedbackStore(Path(feedback_db))
        )
        self.reload(force=True)

    def reload(self, force: bool = False) -> bool:
        with self._lock:
            router_fingerprint = _fingerprint(self.router_path)
            catalog_fingerprint = _fingerprint(self.catalog_path)
            quality_router_fingerprint = (
                None
                if self.quality_router_path is None
                else _fingerprint(self.quality_router_path)
            )
            changed = force or (
                router_fingerprint != self._router_fingerprint
                or catalog_fingerprint != self._catalog_fingerprint
                or quality_router_fingerprint != self._quality_router_fingerprint
            )
            if not changed:
                return False

            # Fully load and validate replacements before publishing either.
            router = RouterEnsemble.load(str(self.router_path))
            quality_router = (
                None
                if self.quality_router_path is None
                else RouterEnsemble.load(str(self.quality_router_path))
            )
            catalog = load_catalog(str(self.catalog_path))
            if not catalog:
                raise ValueError("catalog is empty")
            # Catalog descriptors change far less often than requests. Cache
            # both versioned feature views at the atomic reload boundary;
            # quality features are recomputed separately with benchmark priors
            # suppressed so this optimization cannot introduce label leakage.
            for descriptor in catalog:
                descriptor["_chimera_model_features_v2"] = _candidate_vector(
                    router, descriptor
                )
                if quality_router is not None:
                    descriptor["_chimera_quality_features_v2"] = _candidate_vector(
                        quality_router,
                        descriptor,
                        include_benchmark_priors=False,
                    )
            router_sha256 = _file_sha256(self.router_path)
            catalog_sha256 = _file_sha256(self.catalog_path)
            quality_router_sha256 = (
                ""
                if self.quality_router_path is None
                else _file_sha256(self.quality_router_path)
            )

            self._router = router
            self._quality_router = quality_router
            self._catalog = catalog
            self._router_fingerprint = router_fingerprint
            self._catalog_fingerprint = catalog_fingerprint
            self._quality_router_fingerprint = quality_router_fingerprint
            self.router_sha256 = router_sha256
            self.catalog_sha256 = catalog_sha256
            self.quality_router_sha256 = quality_router_sha256
            if self.feedback_store is not None:
                self.feedback_store.register_catalog(
                    snapshot_id=catalog_sha256,
                    content_sha256=catalog_sha256,
                    model_count=len(catalog),
                    source_url=str(self.catalog_path),
                )
            return True

    def health(self) -> Dict[str, Any]:
        self.reload()
        with self._lock:
            assert self._router is not None
            return {
                "ok": True,
                "service_schema_version": SERVICE_SCHEMA_VERSION,
                "route_schema_version": ROUTE_SCHEMA_VERSION,
                "plan_schema_version": PLAN_SCHEMA_VERSION,
                "router_sha256": self.router_sha256,
                "quality_router_sha256": self.quality_router_sha256 or None,
                "catalog_sha256": self.catalog_sha256,
                "catalog_models": len(self._catalog),
                "outcomes": list(self._router.outcome_names),
                "task_dim": self._router.task_dim,
                "model_dim": self._router.model_dim,
                "ensemble_members": len(self._router.members),
                "quality_router": self._quality_router is not None,
                "quality_ensemble_members": (
                    0
                    if self._quality_router is None
                    else len(self._quality_router.members)
                ),
                "feedback_enabled": self.feedback_store is not None,
            }

    def route(self, payload: Mapping[str, Any]) -> Dict[str, Any]:
        self.reload()
        wrapped_request = payload.get("request")
        if wrapped_request is not None:
            if not isinstance(wrapped_request, Mapping):
                raise ValueError("request must be a JSON object")
            request = dict(wrapped_request)
        else:
            # Also accept an OpenAI-style request directly. Service-only fields
            # are removed before feature extraction.
            request = {
                key: value
                for key, value in payload.items()
                if key
                not in {
                    "policy",
                    "explicit_model",
                    "top_k",
                    "record_feedback",
                    "policy_id",
                    "logging_propensity",
                    "tenant_hash",
                    "request_id",
                }
            }
        policy = payload.get("policy")
        if policy is not None and not isinstance(policy, Mapping):
            raise ValueError("policy must be a JSON object")
        explicit_model = payload.get("explicit_model")
        request_model = request.get("model")
        if explicit_model is None and request_model is not None:
            requested = str(request_model).strip()
            if requested.casefold() not in {"auto", "chimera/auto", "openrouter/auto"}:
                explicit_model = requested
        top_k = int(payload.get("top_k", 10))
        if top_k < 1:
            raise ValueError("top_k must be positive")

        with self._lock:
            assert self._router is not None
            full_exposure = self.feedback_store is not None and bool(
                payload.get("record_feedback", True)
            )
            decision = route_request(
                self._router,
                request,
                self._catalog,
                quality_router=self._quality_router,
                policy=policy,
                explicit_model=None if explicit_model is None else str(explicit_model),
                top_k=max(top_k, len(self._catalog)) if full_exposure else top_k,
            )
            router_sha256 = self.router_sha256
            catalog_sha256 = self.catalog_sha256
            quality_router_sha256 = self.quality_router_sha256
            request_id = None
            if (
                full_exposure
                and self.feedback_store is not None
                and decision.get("selected_model")
            ):
                task_vector = _request_task_vector(self._router, request)
                task_version = _artifact_feature_version(
                    self._router, "task_feature_version"
                )
                request_id = self.feedback_store.record_decision(
                    request,
                    decision,
                    checkpoint_id=router_sha256,
                    catalog_snapshot_id=catalog_sha256,
                    policy_id=str(payload.get("policy_id") or "chimera-default"),
                    logging_propensity=payload.get("logging_propensity"),
                    tenant_hash=(
                        None
                        if payload.get("tenant_hash") is None
                        else str(payload.get("tenant_hash"))
                    ),
                    request_id=(
                        None
                        if payload.get("request_id") is None
                        else str(payload.get("request_id"))
                    ),
                    task_features=task_vector,
                    task_feature_version=task_version,
                )

        # The feedback sink sees every scored exposure, while the caller gets
        # only the requested top-k surface.
        decision["candidates"] = list(decision.get("candidates") or [])[:top_k]
        decision["fallback_models"] = list(decision.get("fallback_models") or [])[
            : max(top_k - 1, 0)
        ]

        models = []
        if decision.get("selected_model"):
            models.append(decision["selected_model"])
        models.extend(decision.get("fallback_models") or [])
        decision["service_schema_version"] = SERVICE_SCHEMA_VERSION
        decision["router_sha256"] = router_sha256
        decision["catalog_sha256"] = catalog_sha256
        decision["quality_router_sha256"] = quality_router_sha256 or None
        if request_id is not None:
            decision["request_id"] = request_id
        decision["openrouter_request_patch"] = {
            "models": models,
            "provider": {
                "allow_fallbacks": True,
                "require_parameters": True,
            },
        }
        return decision

    def outcome(self, payload: Mapping[str, Any]) -> Dict[str, Any]:
        if self.feedback_store is None:
            raise ValueError("feedback is disabled; start with --feedback-db")
        return self.feedback_store.record_outcome(payload)

    def plan(self, payload: Mapping[str, Any]) -> Dict[str, Any]:
        """Build a declarative plan; never execute any returned stage."""

        self.reload()
        wrapped_request = payload.get("request")
        if wrapped_request is not None:
            if not isinstance(wrapped_request, Mapping):
                raise ValueError("request must be a JSON object")
            request = dict(wrapped_request)
        else:
            request = {
                key: value
                for key, value in payload.items()
                if key
                not in {
                    "policy",
                    "planner",
                    "planning_policy",
                    "explicit_model",
                    "top_k",
                }
            }
        policy = payload.get("policy")
        if policy is not None and not isinstance(policy, Mapping):
            raise ValueError("policy must be a JSON object")
        planning_policy = payload.get("planner", payload.get("planning_policy"))
        if planning_policy is not None and not isinstance(planning_policy, Mapping):
            raise ValueError("planner must be a JSON object")
        explicit_model = payload.get("explicit_model")
        request_model = request.get("model")
        if explicit_model is None and request_model is not None:
            requested = str(request_model).strip()
            if requested.casefold() not in {"auto", "chimera/auto", "openrouter/auto"}:
                explicit_model = requested
        top_k = int(payload.get("top_k", 10))

        with self._lock:
            assert self._router is not None
            decision = plan_request(
                self._router,
                request,
                self._catalog,
                quality_router=self._quality_router,
                policy=policy,
                planning_policy=planning_policy,
                explicit_model=None if explicit_model is None else str(explicit_model),
                top_k=top_k,
            )
            router_sha256 = self.router_sha256
            catalog_sha256 = self.catalog_sha256
            quality_router_sha256 = self.quality_router_sha256

        decision["service_schema_version"] = SERVICE_SCHEMA_VERSION
        decision["router_sha256"] = router_sha256
        decision["catalog_sha256"] = catalog_sha256
        decision["quality_router_sha256"] = quality_router_sha256 or None
        return decision


class RouterHTTPServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(
        self, address: Tuple[str, int], state: RouterState, access_log: bool
    ) -> None:
        self.router_state = state
        self.access_log = access_log
        super().__init__(address, RouterHandler)


class RouterHandler(BaseHTTPRequestHandler):
    server: RouterHTTPServer
    protocol_version = "HTTP/1.1"

    def log_message(self, format_string: str, *args: Any) -> None:
        if self.server.access_log:
            super().log_message(format_string, *args)

    def _send(self, status: int, payload: Mapping[str, Any]) -> None:
        encoded = (
            json.dumps(payload, sort_keys=True, separators=(",", ":")) + "\n"
        ).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(encoded)

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler contract.
        if self.path.rstrip("/") == "/healthz":
            try:
                self._send(200, self.server.router_state.health())
            except Exception as exc:  # Keep errors structured; never echo request text.
                self._send(
                    503, {"ok": False, "error": type(exc).__name__, "detail": str(exc)}
                )
            return
        self._send(404, {"ok": False, "error": "not_found"})

    def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler contract.
        if self.path.rstrip("/") not in {
            "/v1/route",
            "/v1/plan",
            "/v1/outcome",
            "/v1/reload",
        }:
            self._send(404, {"ok": False, "error": "not_found"})
            return
        length_text = self.headers.get("Content-Length")
        try:
            length = int(length_text or "0")
        except ValueError:
            self._send(400, {"ok": False, "error": "invalid_content_length"})
            return
        if length < 0 or length > MAX_REQUEST_BYTES:
            self._send(413, {"ok": False, "error": "request_too_large"})
            return
        try:
            raw = self.rfile.read(length)
            payload = json.loads(raw.decode("utf-8")) if raw else {}
            if not isinstance(payload, Mapping):
                raise ValueError("body must be a JSON object")
            if self.path.rstrip("/") == "/v1/reload":
                changed = self.server.router_state.reload(force=True)
                response = self.server.router_state.health()
                response["reloaded"] = changed
            elif self.path.rstrip("/") == "/v1/plan":
                response = self.server.router_state.plan(payload)
            elif self.path.rstrip("/") == "/v1/outcome":
                response = self.server.router_state.outcome(payload)
            else:
                response = self.server.router_state.route(payload)
            self._send(200, response)
        except (KeyError, ValueError, TypeError, json.JSONDecodeError) as exc:
            self._send(
                400, {"ok": False, "error": type(exc).__name__, "detail": str(exc)}
            )
        except Exception as exc:
            self._send(
                500, {"ok": False, "error": type(exc).__name__, "detail": str(exc)}
            )


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--router", type=Path, required=True, help="trained router .npz"
    )
    parser.add_argument(
        "--catalog", type=Path, required=True, help="pinned/current catalog JSON"
    )
    parser.add_argument(
        "--quality-router",
        type=Path,
        help="optional semantic catalog-quality ensemble .npz",
    )
    parser.add_argument(
        "--feedback-db",
        type=Path,
        help="optional redacted decision/outcome SQLite store",
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--access-log", action="store_true")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_argument_parser().parse_args(argv)
    if not 0 <= args.port <= 65535:
        raise SystemExit("--port must be in [0, 65535]")
    state = RouterState(
        args.router,
        args.catalog,
        quality_router_path=args.quality_router,
        feedback_db=args.feedback_db,
    )
    server = RouterHTTPServer((args.host, args.port), state, args.access_log)
    host, port = server.server_address[:2]
    print(
        json.dumps(
            {
                "ready": True,
                "host": host,
                "port": port,
                **state.health(),
            },
            sort_keys=True,
        ),
        flush=True,
    )
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())


__all__ = ["RouterState", "SERVICE_SCHEMA_VERSION"]
