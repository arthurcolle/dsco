#!/usr/bin/env python3
"""Check target record shapes and illustrative cross-record consistency.

Run from any directory:
  uv run --with jsonschema --no-project python docs/operator/contracts/validate.py

No backend is contacted. Passing does not prove authorization, durability,
atomicity, hash correctness, effect truth, or protocol/runtime conformance.
"""
from __future__ import annotations

import copy
import json
import sys
from pathlib import Path

from jsonschema import Draft202012Validator, FormatChecker


HERE = Path(__file__).resolve().parent
EXAMPLES = HERE.parent / "examples"
ID_FIELDS = {
    "WorldContext": "context_id",
    "SourceManifest": "source_manifest_id",
    "EvaluationRecord": "evaluation_id",
    "Scenario": "scenario_id",
    "Changeset": "changeset_id",
    "CommitReceipt": "commit_id",
    "CommandIntent": "command_id",
    "AttemptReceipt": "attempt_id",
    "RunCheckpoint": "checkpoint_id",
    "WatchEvent": "event_id",
}


def load(path: Path):
    def reject_constant(value):
        raise ValueError(f"Non-finite JSON number is forbidden: {value}")

    return json.loads(path.read_text(), parse_constant=reject_constant)


def canonical(value):
    """Fixture equality only, not the protocol's domain canonicalization."""
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def scope(record):
    return record["tenant"], record["realm"]


def record_key(record):
    kind = record["kind"]
    if kind in ID_FIELDS:
        return kind, record.get(ID_FIELDS[kind])
    if kind == "ObjectRef":
        return kind, record.get("tenant"), record.get("realm"), record.get("object_id")
    if kind == "ValueAddress":
        return kind, canonical([record.get("object"), record.get("field"), record.get("args")])
    return kind, canonical(record)


def walk(value):
    if isinstance(value, dict):
        yield value
        for item in value.values():
            yield from walk(item)
    elif isinstance(value, list):
        for item in value:
            yield from walk(item)


def consistency_errors(records):
    errors = []
    index = {}

    def add(code, record, message):
        errors.append({"code": code, "record": list(record_key(record)), "message": message})

    for record in records:
        key = record_key(record)
        if key in index:
            add("duplicate_record", record, "Record identity is duplicated in the fixture bundle.")
        index[key] = record

    def lookup(kind, identifier, owner):
        value = index.get((kind, identifier))
        if value is None:
            add("reference_missing", owner, f"Missing fixture {kind} reference: {identifier}")
        return value

    def baseline(context):
        # Root order is semantic; observation order is not.
        return canonical({
            "scope": scope(context),
            "snapshot": context["snapshot"],
            "roots": context["roots"],
            "source_manifest_id": context["source_manifest_id"],
            "observations": sorted(context["observations"], key=lambda item: item["name"]),
            "workspace_id": context.get("workspace_id"),
            "workspace_head": context.get("workspace_head"),
            "draft_digest": context.get("draft_digest"),
        })

    def check_scope(owner, value, allowed):
        for nested in walk(value):
            if nested.get("kind") == "ObjectRef":
                if scope(nested) not in allowed:
                    add("reference_scope_mismatch", owner, "Object reference is outside the context's explicit root scopes.")
            if nested.get("kind") == "ValueAddress":
                if scope(nested["object"]) not in allowed:
                    add("reference_scope_mismatch", owner, "Value address is outside the context's explicit root scopes.")

    for record in records:
        kind = record["kind"]
        if kind == "SourceManifest":
            modules = {module["name"]: module for module in record["modules"]}
            if len(modules) != len(record["modules"]):
                add("duplicate_module", record, "Module names must be unique within a manifest.")
            for class_pin in record["classes"]:
                module = modules.get(class_pin["module"])
                if not module or class_pin["export"] not in module["exports"]:
                    add("manifest_export_missing", record, "Pinned class must name an export in a pinned module.")

        context_field = {
            "WorldContext": "context_id", "EvaluationRecord": "context_id",
            "Scenario": "baseline_context_id", "Changeset": "base_context_id",
            "CommandIntent": "context_id", "RunCheckpoint": "context_id",
        }.get(kind)
        context = lookup("WorldContext", record[context_field], record) if context_field else None
        if context:
            allowed = {scope(context)} | {scope(root) for root in context["roots"] if root["mode"] == "imported_read_only"}
            check_scope(record, record, allowed)
            if "tenant" in record and scope(record) != scope(context):
                add("reference_scope_mismatch", record, "Record scope differs from its context.")
            if "source_manifest_id" in record and record["source_manifest_id"] != context["source_manifest_id"]:
                add("manifest_context_mismatch", record, "Record source manifest differs from its pinned context.")

        if kind == "WorldContext":
            manifest = lookup("SourceManifest", record["source_manifest_id"], record)
            if manifest and scope(manifest) != scope(record):
                add("reference_scope_mismatch", record, "Manifest must belong to the declared context scope in these fixtures.")
            if any(root["mode"] == "local" and scope(root) != scope(record) for root in record["roots"]):
                add("reference_scope_mismatch", record, "Foreign roots must be explicitly imported read-only.")
            if len({item["name"] for item in record["observations"]}) != len(record["observations"]):
                add("duplicate_observation", record, "Observation bindings must be unique.")
            if record["scenario_id"]:
                scenario = lookup("Scenario", record["scenario_id"], record)
                original = lookup("WorldContext", scenario["baseline_context_id"], record) if scenario else None
                if original and baseline(record) != baseline(original):
                    add("scenario_baseline_mismatch", record, "Scenario context changed pinned data/source/observation baseline.")

        elif kind == "Scenario" and context:
            if context["scenario_id"] is not None:
                add("scenario_baseline_is_scenario", record, "Scenario definitions pin the baseline context; nesting uses parent_scenario_id.")
            addresses = [canonical(override["address"]) for override in record["overrides"]]
            if len(set(addresses)) != len(addresses):
                add("duplicate_override", record, "One scenario scope cannot override the same value address twice.")
            visited = {record["scenario_id"]}
            parent_id = record["parent_scenario_id"]
            while parent_id:
                if parent_id in visited:
                    add("scenario_cycle", record, "Scenario ancestry contains a cycle.")
                    break
                visited.add(parent_id)
                parent = lookup("Scenario", parent_id, record)
                if not parent:
                    break
                parent_context = lookup("WorldContext", parent["baseline_context_id"], record)
                if parent_context and baseline(context) != baseline(parent_context):
                    add("scenario_baseline_mismatch", record, "Parent/child roots, snapshot, manifest and observations must match.")
                parent_id = parent["parent_scenario_id"]

        elif kind == "EvaluationRecord" and context:
            if record["evaluator_profile"] != context["evaluator_profile"]:
                add("evaluator_context_mismatch", record, "Evaluator profile differs from the pinned context.")

        elif kind == "Changeset" and context:
            if record["transaction_domain"] != context["transaction_domain"]:
                add("transaction_domain_mismatch", record, "Changeset must use its baseline transaction domain.")
            if (record["workspace_id"], record["expected_workspace_head"]) != (context.get("workspace_id"), context.get("workspace_head")):
                add("workspace_context_mismatch", record, "Workspace/head must match the baseline context.")
            for change in record["changes"]:
                if scope(change["target"]) != scope(record):
                    add("reference_scope_mismatch", record, "Changeset cannot mutate an imported realm.")
            for evaluation_id in record["evidence_evaluation_ids"]:
                evaluation = lookup("EvaluationRecord", evaluation_id, record)
                if evaluation and (evaluation["status"] != "succeeded" or evaluation.get("completeness") != "complete" or evaluation.get("result", {}).get("state") != "available" or evaluation["context_id"] != record["base_context_id"]):
                    add("evaluation_evidence_invalid", record, "Justification requires an available, complete success against the baseline context.")

        elif kind == "CommitReceipt":
            changeset = lookup("Changeset", record["changeset_id"], record)
            if changeset:
                fields = ("tenant", "realm", "transaction_domain", "idempotency_key", "request_digest")
                if any(record[field] != changeset[field] for field in fields) or record["prior_workspace_head"] != changeset["expected_workspace_head"]:
                    add("commit_changeset_mismatch", record, "Receipt must bind the exact changeset request and prior head.")
                check_scope(record, record["changed_objects"], {scope(changeset)})

        elif kind == "CommandIntent" and context:
            if record["principal_id"] != context["principal_id"]:
                add("command_principal_mismatch", record, "Command principal must match its context; authority is still externally checked.")

        elif kind == "AttemptReceipt":
            command = lookup("CommandIntent", record["command_id"], record)
            if command and any(record.get(field) != command.get(field) for field in ("tenant", "realm", "run_id", "occurrence_id")):
                add("receipt_command_mismatch", record, "Attempt evidence must retain the original command scope/run/occurrence.")
            # Deliberately no comparison with the current run lease: late
            # immutable evidence remains valid after ownership has changed.

        elif kind == "RunCheckpoint":
            generation = record["current_lease"]["generation"]
            transition = record["last_transition"]
            if transition["accepted_generation"] != generation:
                add("stale_checkpoint_transition", record, "This checkpoint transition must be accepted by its recorded current generation.")
            for command_id in record["pending_command_ids"]:
                command = lookup("CommandIntent", command_id, record)
                if command and command.get("run_id") != record["run_id"]:
                    add("run_command_mismatch", record, "Pending command belongs to another run.")
            for receipt_id in record["evidence_receipt_ids"] + transition["receipt_ids"]:
                receipt = lookup("AttemptReceipt", receipt_id, record)
                if receipt and (receipt.get("run_id") != record["run_id"] or scope(receipt) != scope(record)):
                    add("run_receipt_mismatch", record, "Evidence belongs to another run or realm.")
            for receipt_id in transition["receipt_ids"]:
                receipt = lookup("AttemptReceipt", receipt_id, record)
                if receipt and receipt["dispatch_generation"] != generation and not transition["reconciliation_ids"]:
                    add("stale_receipt_transition", record, "An old-owner receipt needs separate reconciliation evidence before advancing current run state.")
                if receipt and receipt["outcome"] == "effect_unknown" and not transition["reconciliation_ids"]:
                    add("unknown_receipt_transition", record, "Unknown effect cannot directly justify a completed transition.")
            # Reconciliation IDs refer to externally retained evidence. This
            # structural suite does not prove that evidence is authoritative.

        elif kind == "WatchEvent" and record["event_type"] == "committed_batch":
            commit = lookup("CommitReceipt", record["commit_id"], record)
            if commit and (scope(record) != scope(commit) or record["transaction_domain"] != commit["transaction_domain"] or record["cursor"] != commit["change_cursor"] or record["changes"] != commit["changed_objects"]):
                add("watch_commit_mismatch", record, "Watch batch must match the referenced committed receipt.")

    return errors


def validate_records(records, validator):
    errors = []
    for position, record in enumerate(records):
        for error in validator.iter_errors(record):
            errors.append({"code": "schema", "record_index": position, "message": error.message})
    if errors:
        return errors
    return consistency_errors(records)


def apply_case(records, case):
    result = copy.deepcopy(records)
    replacement = copy.deepcopy(case["record"])
    key = record_key(replacement)
    found = False
    for index, record in enumerate(result):
        if record_key(record) == key:
            result[index] = replacement
            found = True
            break
    if not found:
        result.append(replacement)
    result.extend(copy.deepcopy(case.get("additional_records", [])))
    return result


def main():
    schema = load(HERE / "records.schema.json")
    Draft202012Validator.check_schema(schema)
    validator = Draft202012Validator(schema, format_checker=FormatChecker())
    valid = load(EXAMPLES / "valid-records.json")
    invalid = load(EXAMPLES / "invalid-records.json")
    failures = []
    errors = validate_records(valid["records"], validator)
    if errors:
        failures.append({"case": "valid_bundle", "errors": errors})
    names = set()
    for case in invalid["cases"]:
        if case["name"] in names:
            failures.append({"case": case["name"], "error": "duplicate fixture name"})
        names.add(case["name"])
        errors = validate_records(apply_case(valid["records"], case), validator)
        if case["expected_error"] not in {error["code"] for error in errors}:
            failures.append({"case": case["name"], "expected": case["expected_error"], "errors": errors})

    # Optional catalog sanity only: its semantics are reviewed separately.
    operation_count = None
    operations_path = HERE / "operations.json"
    if operations_path.exists():
        catalog = load(operations_path)
        entries = catalog.get("operations", []) if isinstance(catalog, dict) else catalog
        if not isinstance(entries, list):
            failures.append({"case": "operations", "error": "operations must be an array"})
        else:
            ids = [entry.get("id", entry.get("operation_id")) for entry in entries if isinstance(entry, dict)]
            if len(ids) != len(entries) or any(not isinstance(identifier, str) or not identifier for identifier in ids) or len(set(ids)) != len(ids):
                failures.append({"case": "operations", "error": "operation IDs must be nonempty, unique strings"})
            operation_count = len(entries)

    print(json.dumps({
        "status": "FAIL" if failures else "PASS",
        "contract_status": "specified_target",
        "record_kinds": len(schema["oneOf"]),
        "valid_records": len(valid["records"]),
        "negative_cases": len(invalid["cases"]),
        "operation_ids_checked": operation_count,
        "checks": ["draft_2020_12_schema", "valid_record_shapes", "expected_negative_rejections", "scenario_baselines", "reference_scopes", "manifest_context_pins", "historical_receipts_vs_current_transitions", "commit_and_watch_references"],
        "not_proven": ["authorization", "hash_correctness", "atomicity", "durability", "effect_truth", "backend_conformance"],
        "failures": failures,
    }, indent=2))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
