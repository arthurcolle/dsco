"""
GraphSub custom-domain landing for Modal.

Deploy:
    modal deploy graphsub_domain_app.py

This intentionally lives at repository root so `import modal` resolves to the
Modal SDK, not graphsub-platform/modal/.
"""

import html
import json
import re
import time
from datetime import datetime, timezone
from pathlib import Path

import modal


ROOT = Path(__file__).parent
LANDING_HTML = ROOT / "graphsub-phosphor.html"
FAVICON = ROOT / "favicon.svg"
OG_IMAGE = ROOT / "og-image.png"
APPLE_ICON = ROOT / "apple-icon.png"

app = modal.App("graphsub-com")
DEPLOYMENT_REVISION = "graphsub-cloud-2026-07-03-r23"
SITE_URL = "https://graphsub.com"
BASE_ONTOLOGY_CONCEPTS = 65536
BASE_ONTOLOGY_CONCEPTS_LABEL = "65,536"
BASE_ONTOLOGY_LINKS = 1842176
BASE_ONTOLOGY_LINKS_LABEL = "1,842,176"
BASE_KERNEL_TYPES = 512
BASE_KERNEL_EDGES = 384
BASE_PAGERANK_ROWS = [
    ["OpenOntology", 0.252],
    ["OperationalObjectModel", 0.221],
    ["ObjectTypes", 0.194],
    ["LinkTypes", 0.163],
    ["ActionTypes", 0.131],
    ["PublicKnowledgeGraph", 0.116],
    ["SensorCommandGraph", 0.104],
    ["EvidenceAbstractions", 0.096],
]

OPEN_ONTOLOGY_VERSION = "openontology-2026-07-03-r1"
OPEN_ONTOLOGY_LICENSE = "Apache-2.0 schema; CC-BY-4.0 examples"


def slugify(value: str) -> str:
    cleaned = re.sub(r"([a-z0-9])([A-Z])", r"\1-\2", str(value)).lower()
    cleaned = re.sub(r"[^a-z0-9]+", "-", cleaned).strip("-")
    return cleaned or "item"


def oo_type(name: str, domain: str, description: str, properties: list[str], interfaces: list[str] | None = None, extends: str = "Thing") -> dict:
    return {
        "id": f"oo.object.{slugify(name)}",
        "kind": "object_type",
        "name": name,
        "domain": domain,
        "extends": extends,
        "interfaces": interfaces or ["Provenanced", "Auditable"],
        "properties": properties,
        "description": description,
    }


def oo_link(name: str, source: str, target: str, description: str, cardinality: str = "many_to_many") -> dict:
    return {
        "id": f"oo.link.{slugify(name)}",
        "kind": "link_type",
        "name": name,
        "source": source,
        "target": target,
        "cardinality": cardinality,
        "description": description,
    }


def oo_action(name: str, targets: list[str], inputs: list[str], effects: list[str], description: str) -> dict:
    return {
        "id": f"oo.action.{slugify(name)}",
        "kind": "action_type",
        "name": name,
        "targets": targets,
        "inputs": inputs,
        "effects": effects,
        "description": description,
        "requires_approval": any(effect in {"delete", "merge", "execute", "publish", "escalate"} for effect in effects),
    }


def oo_function(name: str, inputs: list[str], output: str, description: str) -> dict:
    return {
        "id": f"oo.function.{slugify(name)}",
        "kind": "function",
        "name": name,
        "inputs": inputs,
        "output": output,
        "description": description,
    }


OPEN_ONTOLOGY_DOMAINS = [
    {"id": "core", "name": "Core", "description": "Universal identity, time, provenance, and type abstractions."},
    {"id": "party", "name": "Party and Identity", "description": "People, organizations, groups, accounts, roles, and identities."},
    {"id": "place", "name": "Place and Geospatial", "description": "Locations, facilities, boundaries, routes, regions, and geo cells."},
    {"id": "operations", "name": "Operations", "description": "Assets, tasks, processes, orders, resources, policies, and incidents."},
    {"id": "knowledge", "name": "Public Knowledge", "description": "Companies, topics, claims, sources, technologies, people, and sectors."},
    {"id": "finance", "name": "Markets and Finance", "description": "Accounts, trades, books, portfolios, curves, exposures, and scenarios."},
    {"id": "supply_chain", "name": "Supply Chain", "description": "Suppliers, shipments, inventory, lots, facilities, products, and constraints."},
    {"id": "security", "name": "Security and Cyber", "description": "Principals, devices, vulnerabilities, alerts, controls, and investigations."},
    {"id": "sensor", "name": "Sensor and Mission", "description": "Sensors, tracks, detections, platforms, missions, tasking, and command state."},
    {"id": "agent", "name": "Agent Runtime", "description": "Tools, plans, executions, memory, approvals, evals, capsules, and budgets."},
    {"id": "governance", "name": "Governance, Risk, and Compliance", "description": "Policies, controls, obligations, evidence, exceptions, and audit trails."},
    {"id": "data", "name": "Data and Models", "description": "Sources, datasets, transforms, models, features, predictions, and lineage."},
]

OPEN_ONTOLOGY_VALUE_TYPES = [
    "String", "Text", "Boolean", "Integer", "Decimal", "Timestamp", "Date", "Duration", "GeoPoint", "GeoShape",
    "Currency", "Percent", "URI", "URL", "Email", "Phone", "JSON", "Vector", "Embedding", "Enum", "Classification",
]

OPEN_ONTOLOGY_SHARED_PROPERTIES = [
    "id", "canonical_id", "display_name", "description", "status", "created_at", "updated_at", "valid_from", "valid_to",
    "tenant_id", "owner_id", "source_ids", "confidence", "classification", "tags", "acl", "version", "lineage_hash",
    "freshness_at", "quality_score", "embedding", "external_refs", "provenance", "notes",
]

OPEN_ONTOLOGY_INTERFACES = [
    {"id": "oo.interface.provenanced", "name": "Provenanced", "properties": ["source_ids", "confidence", "lineage_hash"]},
    {"id": "oo.interface.temporal", "name": "Temporal", "properties": ["valid_from", "valid_to", "freshness_at"]},
    {"id": "oo.interface.geospatial", "name": "Geospatial", "properties": ["geo_point", "geo_shape", "region"]},
    {"id": "oo.interface.owned", "name": "Owned", "properties": ["owner_id", "tenant_id"]},
    {"id": "oo.interface.auditable", "name": "Auditable", "properties": ["created_at", "updated_at", "version"]},
    {"id": "oo.interface.actionable", "name": "Actionable", "properties": ["status", "priority", "assignee_id"]},
    {"id": "oo.interface.observable", "name": "Observable", "properties": ["observed_at", "sensor_id", "confidence"]},
    {"id": "oo.interface.policy_bound", "name": "PolicyBound", "properties": ["policy_ids", "approval_state", "classification"]},
    {"id": "oo.interface.agent_readable", "name": "AgentReadable", "properties": ["embedding", "summary", "tool_scope"]},
    {"id": "oo.interface.risk_bearing", "name": "RiskBearing", "properties": ["risk_score", "exposure", "mitigations"]},
]

OPEN_ONTOLOGY_OBJECT_TYPES = [
    oo_type("Thing", "core", "Root object for every typed entity in OpenOntology.", OPEN_ONTOLOGY_SHARED_PROPERTIES, ["Provenanced", "Auditable"], "None"),
    oo_type("ObjectType", "core", "A user-facing class of real-world object with properties and links.", ["api_name", "label", "plural_label", "description", "interfaces", "properties", "status"]),
    oo_type("LinkType", "core", "A typed relationship between object types.", ["api_name", "source_type", "target_type", "cardinality", "properties", "status"]),
    oo_type("PropertyType", "core", "A named property on an object, interface, or struct.", ["api_name", "value_type", "required", "indexed", "editable", "description"]),
    oo_type("ActionType", "core", "A typed write or workflow operation over ontology objects.", ["api_name", "target_types", "parameters", "rules", "effects", "requires_approval"]),
    oo_type("Function", "core", "A reusable computation over objects, object sets, models, or external services.", ["api_name", "input_schema", "output_schema", "runtime", "version"]),
    oo_type("Interface", "core", "A reusable property and behavior contract implemented by object types.", ["api_name", "properties", "link_types", "action_constraints"]),
    oo_type("ObjectSet", "core", "A saved, queryable collection of ontology objects.", ["query", "filters", "sort", "owner_id", "materialization"]),
    oo_type("Scenario", "core", "A branchable what-if state over objects, actions, and derived outputs.", ["baseline_id", "assumptions", "changes", "owner_id", "expires_at"]),
    oo_type("PermissionGrant", "core", "Fine-grained access rule for objects, actions, properties, and interfaces.", ["principal_id", "resource_id", "operation", "condition", "expires_at"]),
    oo_type("Person", "party", "A human actor, employee, customer, operator, analyst, author, or approver.", ["full_name", "email", "role", "department", "location_id", "clearance"]),
    oo_type("Organization", "party", "A company, agency, institution, supplier, customer, fund, or internal org.", ["legal_name", "website", "sector", "country", "lei", "parent_org_id"]),
    oo_type("Group", "party", "A team, committee, role group, cohort, or permission group.", ["name", "purpose", "owner_id", "membership_rule"]),
    oo_type("Account", "party", "A customer, system, trading, billing, or service account.", ["account_number", "account_type", "status", "opened_at", "risk_rating"]),
    oo_type("Identity", "party", "A login, device identity, service principal, wallet, or external identifier.", ["namespace", "identifier", "identity_type", "verified_at", "expires_at"]),
    oo_type("Role", "party", "A durable responsibility, job function, entitlement, or approval role.", ["name", "scope", "permissions", "seniority", "required_training"]),
    oo_type("Location", "place", "A physical or logical location with geospatial and administrative context.", ["name", "geo_point", "address", "region", "country", "timezone"], ["Geospatial", "Provenanced", "Auditable"]),
    oo_type("Facility", "place", "A building, factory, data center, warehouse, office, base, or lab.", ["facility_type", "capacity", "operator_id", "location_id", "criticality"], ["Geospatial", "Owned", "RiskBearing"]),
    oo_type("Region", "place", "A jurisdiction, service region, market, coverage area, or operating theater.", ["name", "boundary", "country", "regime", "timezone"], ["Geospatial"]),
    oo_type("Route", "place", "A planned or observed path between locations.", ["origin_id", "destination_id", "mode", "distance", "eta"], ["Geospatial", "Temporal"]),
    oo_type("GeoCell", "place", "A tiled spatial unit for aggregation, indexing, and realtime state.", ["cell_id", "resolution", "centroid", "boundary", "parent_cell_id"], ["Geospatial"]),
    oo_type("Asset", "operations", "A physical, digital, financial, or operational resource with lifecycle state.", ["asset_type", "owner_id", "location_id", "criticality", "lifecycle_state"], ["Owned", "RiskBearing"]),
    oo_type("Device", "operations", "A machine, endpoint, vehicle, sensor host, or controllable equipment.", ["device_type", "serial_number", "firmware", "network_id", "health"]),
    oo_type("SoftwareSystem", "operations", "An application, service, model endpoint, platform, or runtime system.", ["system_type", "repo", "service_level", "owner_id", "environment"]),
    oo_type("Process", "operations", "A repeatable operational workflow, business process, or runbook.", ["name", "process_type", "owner_id", "sla", "state_model"]),
    oo_type("Task", "operations", "A unit of work assigned to a person, agent, team, or system.", ["title", "priority", "status", "assignee_id", "due_at"], ["Actionable", "Temporal"]),
    oo_type("Order", "operations", "A customer, production, procurement, mission, or work order.", ["order_type", "status", "requester_id", "submitted_at", "fulfilled_at"]),
    oo_type("Transaction", "operations", "A state-changing business or system transaction.", ["transaction_type", "amount", "currency", "status", "settled_at"], ["Temporal", "Provenanced"]),
    oo_type("Incident", "operations", "A reliability, security, safety, compliance, or operational incident.", ["severity", "status", "started_at", "resolved_at", "commander_id"], ["Actionable", "RiskBearing"]),
    oo_type("Alert", "operations", "A triggered signal requiring review, triage, automation, or escalation.", ["alert_type", "severity", "status", "triggered_at", "dedupe_key"], ["Actionable", "Observable"]),
    oo_type("Company", "knowledge", "A public or private organization tracked as a market or knowledge entity.", ["legal_name", "sector", "stage", "funding_total", "website"]),
    oo_type("Technology", "knowledge", "A technology area, standard, architecture, product class, or capability.", ["name", "category", "maturity", "aliases", "related_terms"]),
    oo_type("Topic", "knowledge", "A knowledge topic, sector, trend, market, or research area.", ["name", "taxonomy_path", "aliases", "description", "source_count"]),
    oo_type("Claim", "knowledge", "A source-grounded assertion with provenance, confidence, and review state.", ["text", "claim_type", "confidence", "status", "source_ids"], ["Provenanced", "Auditable"]),
    oo_type("Source", "knowledge", "A URL, document, feed, filing, transcript, dataset, or human source.", ["uri", "source_type", "publisher", "published_at", "retrieved_at"], ["Provenanced", "Temporal"]),
    oo_type("Document", "knowledge", "A report, filing, contract, policy, runbook, note, or evidence file.", ["title", "document_type", "uri", "author_id", "published_at"], ["Provenanced", "AgentReadable"]),
    oo_type("Trade", "finance", "A market transaction, execution, allocation, or booking event.", ["trade_id", "instrument_id", "quantity", "price", "book_id", "trade_date"], ["Temporal", "RiskBearing"]),
    oo_type("Instrument", "finance", "A security, derivative, loan, commodity, digital asset, or contract.", ["symbol", "instrument_type", "issuer_id", "currency", "maturity"]),
    oo_type("Portfolio", "finance", "A managed collection of positions, exposures, books, or strategies.", ["name", "strategy", "base_currency", "manager_id", "nav"]),
    oo_type("Book", "finance", "A trading, risk, accounting, or operational book.", ["name", "desk", "owner_id", "currency", "risk_limit"]),
    oo_type("Position", "finance", "A current or historical holding in an instrument.", ["instrument_id", "quantity", "market_value", "as_of", "book_id"], ["Temporal", "RiskBearing"]),
    oo_type("Curve", "finance", "A pricing, risk, rates, credit, volatility, or forward curve.", ["curve_type", "currency", "tenor_set", "as_of", "source_id"], ["Temporal", "Provenanced"]),
    oo_type("Exposure", "finance", "A risk, market, credit, climate, vendor, or operational exposure.", ["exposure_type", "amount", "currency", "scenario_id", "confidence"], ["RiskBearing"]),
    oo_type("Supplier", "supply_chain", "An organization providing goods, services, data, components, or infrastructure.", ["org_id", "tier", "category", "risk_rating", "contract_id"]),
    oo_type("Product", "supply_chain", "A product, service, SKU, component, model, or capability offering.", ["sku", "product_type", "lifecycle_state", "owner_id", "release_version"]),
    oo_type("InventoryLot", "supply_chain", "A batch, lot, serialized inventory pool, or component group.", ["lot_id", "product_id", "quantity", "facility_id", "expiry_at"]),
    oo_type("Shipment", "supply_chain", "A movement of goods, materials, documents, or equipment.", ["shipment_id", "origin_id", "destination_id", "carrier_id", "status"], ["Temporal", "Geospatial"]),
    oo_type("Constraint", "supply_chain", "A capacity, policy, legal, resource, routing, or scheduling constraint.", ["constraint_type", "scope", "severity", "active_from", "active_to"], ["PolicyBound"]),
    oo_type("Principal", "security", "A user, service, API key, device identity, or workload identity.", ["principal_type", "identity_id", "auth_method", "last_seen_at", "risk_score"]),
    oo_type("Vulnerability", "security", "A software, process, control, or configuration weakness.", ["cve", "severity", "affected_system", "published_at", "exploitability"], ["RiskBearing"]),
    oo_type("Control", "security", "A preventive, detective, corrective, or compensating control.", ["control_id", "framework", "owner_id", "frequency", "effectiveness"], ["PolicyBound"]),
    oo_type("Investigation", "security", "A structured review of alerts, incidents, claims, or anomalies.", ["case_id", "status", "lead_id", "opened_at", "closed_at"], ["Actionable", "Auditable"]),
    oo_type("Policy", "governance", "A rule, standard, requirement, operating policy, or regulatory obligation.", ["policy_id", "framework", "scope", "effective_at", "owner_id"], ["PolicyBound"]),
    oo_type("Obligation", "governance", "A requirement imposed by law, contract, policy, or governance process.", ["obligation_type", "source_id", "deadline", "owner_id", "status"], ["Actionable", "PolicyBound"]),
    oo_type("Approval", "governance", "A human or automated authorization decision.", ["approver_id", "target_id", "decision", "reason", "decided_at"], ["Auditable"]),
    oo_type("Evidence", "governance", "An artifact proving a claim, control, decision, output, or event.", ["evidence_type", "source_id", "hash", "collected_at", "review_state"], ["Provenanced", "Auditable"]),
    oo_type("Exception", "governance", "A waiver, violation, unresolved risk, or approved deviation.", ["exception_type", "policy_id", "owner_id", "expires_at", "mitigation"]),
    oo_type("Sensor", "sensor", "A sensing source including camera, radar, feed, detector, API, or human observer.", ["sensor_type", "platform_id", "location_id", "calibration", "health"], ["Observable", "Geospatial"]),
    oo_type("Observation", "sensor", "A raw or normalized observation from a sensor, source, model, or operator.", ["observed_at", "sensor_id", "value", "confidence", "geo_point"], ["Observable", "Temporal"]),
    oo_type("Detection", "sensor", "A candidate object, event, anomaly, or condition detected from observations.", ["detection_type", "confidence", "observed_at", "model_id", "status"], ["Observable"]),
    oo_type("Track", "sensor", "A correlated stream of detections representing a moving or changing object.", ["track_id", "classification", "confidence", "last_seen_at", "trajectory"], ["Observable", "Geospatial"]),
    oo_type("Mission", "sensor", "A goal-directed operational context with tasks, resources, constraints, and approvals.", ["mission_type", "commander_id", "status", "start_at", "end_at"], ["Actionable", "PolicyBound"]),
    oo_type("Platform", "sensor", "A vehicle, drone, satellite, station, robot, device cluster, or host platform.", ["platform_type", "owner_id", "location_id", "capabilities", "health"]),
    oo_type("Dataset", "data", "A governed collection of records, files, objects, features, or source data.", ["dataset_type", "schema_ref", "owner_id", "freshness_at", "quality_score"], ["Provenanced"]),
    oo_type("DataSource", "data", "A system, feed, connector, table, bucket, queue, stream, or document source.", ["source_type", "connection_uri", "owner_id", "refresh_cadence", "classification"]),
    oo_type("Transform", "data", "A pipeline, extraction, normalization, enrichment, or materialization step.", ["runtime", "input_refs", "output_refs", "schedule", "version"], ["Provenanced"]),
    oo_type("Model", "data", "A predictive, embedding, optimization, rules, LLM, or simulation model.", ["model_type", "version", "owner_id", "training_data_id", "eval_score"]),
    oo_type("Prediction", "data", "A model output, score, forecast, classification, or recommendation.", ["model_id", "target_id", "score", "label", "generated_at"], ["Provenanced", "Temporal"]),
    oo_type("Feature", "data", "A reusable model, search, scoring, or analytics feature.", ["feature_type", "entity_type", "value_type", "freshness_at", "owner_id"]),
    oo_type("Agent", "agent", "An autonomous or semi-autonomous worker, assistant, service, or operator process.", ["agent_type", "model", "tool_scope", "autonomy_level", "status"], ["AgentReadable", "PolicyBound"]),
    oo_type("Tool", "agent", "An executable capability exposed to an agent or workflow.", ["tool_type", "endpoint", "input_schema", "output_schema", "risk_level"], ["PolicyBound"]),
    oo_type("Plan", "agent", "A proposed sequence of tasks, actions, tool calls, and approval gates.", ["goal", "steps", "planner_id", "status", "budget"]),
    oo_type("Execution", "agent", "A realized run of a plan, tool call, function, model, or workflow.", ["execution_type", "started_at", "ended_at", "status", "cost_gsu"], ["Temporal", "Auditable"]),
    oo_type("Memory", "agent", "A working, episodic, semantic, or procedural memory item.", ["memory_tier", "content", "importance", "expires_at", "scope"], ["AgentReadable", "Temporal"]),
    oo_type("Eval", "agent", "A test, rubric, judge result, benchmark, or human review outcome.", ["eval_type", "score", "verdict", "target_id", "run_id"], ["Auditable"]),
    oo_type("Capsule", "agent", "A packaged workflow with schema, data contracts, replay traces, and proof outputs.", ["capsule_type", "buyer", "workflow", "status", "roi_model"]),
]

OPEN_ONTOLOGY_LINK_TYPES = [
    oo_link("is_a", "Thing", "ObjectType", "Classifies an object with an object type."),
    oo_link("implements", "ObjectType", "Interface", "Declares that an object type satisfies an interface contract."),
    oo_link("has_property", "ObjectType", "PropertyType", "Attaches a property definition to an object or interface."),
    oo_link("has_action", "ObjectType", "ActionType", "Exposes an action on an object type."),
    oo_link("has_function", "ObjectType", "Function", "Exposes a reusable computation for objects or object sets."),
    oo_link("owned_by", "Thing", "Person", "Identifies an accountable owner."),
    oo_link("member_of", "Person", "Group", "Places a person or identity in a group."),
    oo_link("employed_by", "Person", "Organization", "Connects a person to an employer."),
    oo_link("located_at", "Thing", "Location", "Places an object at a location."),
    oo_link("part_of", "Thing", "Thing", "Represents composition, containment, or membership."),
    oo_link("depends_on", "Thing", "Thing", "Captures operational, data, model, or workflow dependency."),
    oo_link("produces", "Process", "Thing", "Connects a process to its outputs."),
    oo_link("consumes", "Process", "Thing", "Connects a process to its inputs."),
    oo_link("observed_by", "Observation", "Sensor", "Connects an observation to its sensing source."),
    oo_link("derived_from", "Thing", "Source", "Records data, claim, or output provenance."),
    oo_link("supports", "Evidence", "Claim", "Evidence supports a claim."),
    oo_link("contradicts", "Claim", "Claim", "A claim conflicts with another claim."),
    oo_link("governed_by", "Thing", "Policy", "Applies policy or obligation to an object."),
    oo_link("approved_by", "Thing", "Approval", "Associates an object, action, or output with an approval."),
    oo_link("assigned_to", "Task", "Person", "Assigns work to a person."),
    oo_link("assigned_to_agent", "Task", "Agent", "Assigns work to an agent."),
    oo_link("executes", "Execution", "Plan", "An execution realizes a plan."),
    oo_link("used_tool", "Execution", "Tool", "An execution invoked a tool."),
    oo_link("created_memory", "Execution", "Memory", "An execution created or updated memory."),
    oo_link("triggered_by", "Alert", "Observation", "An alert was triggered by observation or detection."),
    oo_link("results_in", "ActionType", "Thing", "An action produces a resulting object or state."),
    oo_link("mitigates", "Control", "Risk", "A control mitigates a risk or exposure."),
    oo_link("exposes", "Thing", "Exposure", "An object has measurable exposure."),
    oo_link("supplies", "Supplier", "Product", "A supplier provides a product or service."),
    oo_link("shipped_to", "Shipment", "Location", "A shipment moves toward a location."),
    oo_link("references", "Document", "Thing", "A document references an object."),
    oo_link("version_of", "Thing", "Thing", "Connects a versioned object to its canonical parent."),
    oo_link("supersedes", "Thing", "Thing", "Marks one object as replacing another."),
    oo_link("connected_to", "Device", "SoftwareSystem", "Connects infrastructure elements."),
    oo_link("communicates_with", "Platform", "Platform", "Represents communication or mesh connectivity."),
    oo_link("classified_as", "Detection", "ObjectType", "Classifies an observed detection."),
    oo_link("tracks", "Track", "Detection", "Associates a correlated track with detections."),
    oo_link("tasked_by", "Mission", "Person", "Connects a mission to a commander or operator."),
    oo_link("funded_by", "Company", "Organization", "Connects a company to investors or funders."),
    oo_link("traded_in", "Trade", "Instrument", "Connects a trade to an instrument."),
    oo_link("held_in", "Position", "Portfolio", "Places a position in a portfolio."),
    oo_link("priced_by", "Instrument", "Curve", "Associates an instrument with a pricing curve."),
]

OPEN_ONTOLOGY_ACTION_TYPES = [
    oo_action("CreateObject", ["ObjectType"], ["properties"], ["create"], "Create a typed ontology object."),
    oo_action("UpdateProperty", ["Thing"], ["property", "value", "reason"], ["update"], "Update a mutable property with audit trail."),
    oo_action("MergeEntities", ["Thing"], ["source_ids", "winner_id", "evidence"], ["merge"], "Merge duplicate entities with provenance."),
    oo_action("SplitEntity", ["Thing"], ["object_id", "split_plan"], ["update"], "Split an over-merged entity."),
    oo_action("ApproveChange", ["Thing"], ["target_id", "decision", "reason"], ["update"], "Approve a proposed ontology or data change."),
    oo_action("RejectChange", ["Thing"], ["target_id", "decision", "reason"], ["update"], "Reject a proposed ontology or data change."),
    oo_action("AssignOwner", ["Thing"], ["owner_id", "scope"], ["update"], "Assign accountability for an object."),
    oo_action("OpenIncident", ["Alert", "Incident"], ["alert_ids", "severity", "commander_id"], ["create", "escalate"], "Open an incident from one or more alerts."),
    oo_action("CloseIncident", ["Incident"], ["root_cause", "resolution", "evidence_ids"], ["update", "publish"], "Close an incident with proof."),
    oo_action("StartInvestigation", ["Alert", "Claim", "Incident"], ["scope", "lead_id"], ["create"], "Start an investigation workflow."),
    oo_action("EscalateAlert", ["Alert"], ["reason", "target_group"], ["escalate"], "Escalate an alert to people, agents, or teams."),
    oo_action("RunModel", ["Model"], ["input_object_set", "parameters"], ["execute"], "Run a model over an object set."),
    oo_action("RunScenario", ["Scenario"], ["baseline_id", "changes"], ["execute"], "Evaluate what-if changes against a baseline."),
    oo_action("PublishDataset", ["Dataset"], ["version", "quality_gate"], ["publish"], "Publish a governed dataset version."),
    oo_action("RefreshSource", ["DataSource", "Source"], ["cursor", "mode"], ["execute"], "Refresh a source or connector."),
    oo_action("AttachEvidence", ["Evidence", "Claim", "Control"], ["evidence_id", "target_id"], ["update"], "Attach evidence to a claim, control, or decision."),
    oo_action("CertifyControl", ["Control"], ["period", "reviewer_id", "evidence_ids"], ["publish"], "Certify a control with review evidence."),
    oo_action("ReplayExecution", ["Execution", "Capsule"], ["execution_id", "snapshot"], ["execute"], "Replay a workflow or capsule execution."),
    oo_action("RecomputeLineage", ["Thing"], ["target_id", "depth"], ["execute"], "Recompute affected downstream objects."),
    oo_action("CreateCapsule", ["Capsule"], ["workflow", "schema", "buyer"], ["create"], "Create a deployable graph workflow capsule."),
    oo_action("RequestHumanApproval", ["Plan", "Execution"], ["approver_role", "reason", "deadline"], ["create"], "Insert a human approval gate."),
]

OPEN_ONTOLOGY_FUNCTIONS = [
    oo_function("resolve_entity", ["ObjectSet"], "ObjectSet", "Deduplicate and canonicalize entity records."),
    oo_function("infer_links", ["ObjectSet", "LinkType"], "ObjectSet", "Infer likely links with confidence and provenance."),
    oo_function("compute_risk_score", ["Thing"], "Decimal", "Compute risk score from exposure, controls, and incidents."),
    oo_function("compute_exposure", ["Thing", "Scenario"], "Exposure", "Compute scenario-specific exposure."),
    oo_function("rank_evidence", ["Claim"], "ObjectSet<Evidence>", "Rank evidence by source quality, recency, and relevance."),
    oo_function("validate_policy", ["Thing", "Policy"], "Eval", "Evaluate an object against policy rules."),
    oo_function("detect_anomaly", ["TimeSeries"], "Alert", "Detect anomalous behavior or metrics."),
    oo_function("summarize_object_set", ["ObjectSet"], "Text", "Generate a source-grounded object set summary."),
    oo_function("forecast_metric", ["TimeSeries", "Scenario"], "Prediction", "Forecast a metric under a scenario."),
    oo_function("calculate_lineage", ["Thing"], "ObjectSet", "Return upstream and downstream lineage."),
    oo_function("compute_pagerank", ["ObjectSet"], "ObjectSet", "Rank central objects in a graph."),
    oo_function("shortest_path", ["Thing", "Thing"], "Path", "Find shortest typed path between objects."),
    oo_function("community_detection", ["ObjectSet"], "ObjectSet", "Detect communities or clusters."),
    oo_function("semantic_search", ["Text", "ObjectSet"], "ObjectSet", "Search objects using embeddings and filters."),
    oo_function("generate_capsule_plan", ["Capsule", "ObjectSet"], "Plan", "Generate a launch plan for a workflow capsule."),
    oo_function("replay_trace", ["Execution"], "Execution", "Replay an execution with frozen inputs."),
    oo_function("reconcile_claims", ["ObjectSet<Claim>"], "ObjectSet<Claim>", "Resolve conflicting claims with provenance."),
    oo_function("route_incident", ["Incident"], "Task", "Assign incident tasks from severity, ownership, and runbooks."),
]

OPEN_ONTOLOGY_OBJECT_SETS = [
    "ActiveIncidents", "HighRiskAssets", "OpenApprovals", "UnresolvedClaims", "LiveTracks", "CriticalSuppliers",
    "Customer360", "PortfolioExposure", "ModelDriftWatch", "PolicyExceptions", "AgentFailures", "FreshSources",
    "MissionObjects", "StaleDatasets", "UnownedCriticalObjects", "RecentExecutiveClaims",
]

OPEN_ONTOLOGY_SCENARIOS = [
    "IncidentResponse", "SanctionsScreening", "SupplyChainShock", "TradingDayReplay", "AgentFailureReplay",
    "SensorFusionTriage", "MarketIntelligenceBrief", "RegulatoryEvidencePack", "AssetMaintenanceWindow",
    "CyberContainment", "MissionPlanning", "ProductLaunch", "ModelRollback", "DataFreshnessRecovery",
]

OPEN_ONTOLOGY_CONCEPT_FAMILIES = [
    "Object", "Property", "Link", "Action", "Function", "Interface", "ObjectSet", "Scenario", "Permission", "Policy",
    "Evidence", "Metric", "State", "Event", "Decision", "Claim", "Source", "Model", "Agent", "Workflow",
]

try:
    CLOUD_STATE = modal.Dict.from_name("graphsub-cloud-state", create_if_missing=True)
except Exception:
    CLOUD_STATE = None

CAPSULES = [
    {
        "slug": "trading-control-graph",
        "name": "Trading Control Graph",
        "buyer": "Head of risk technology; front-office platform lead",
        "wedge": "Typed recomputation graph for trades, curves, models, limits, and downstream outputs.",
        "pilot": "30-day shadow run across one desk or portfolio.",
        "proof": "Replay one trading day, invalidate only affected nodes, and compare blessed outputs against the incumbent stack.",
        "status": "pilot-ready",
        "score": 91,
    },
    {
        "slug": "agent-reliability-graph",
        "name": "Agent Reliability Graph",
        "buyer": "VP AI platform; enterprise automation lead",
        "wedge": "Replay why an agent failed, which tools it used, what memory changed, and which policy gates fired.",
        "pilot": "Run 100 real tasks, cluster failure modes, and reduce repeated failures with memory and approval trace fixes.",
        "proof": "Task replay bundle with input, transcript, tool calls, memory diffs, policy decisions, and eval verdicts.",
        "status": "pilot-ready",
        "score": 94,
    },
    {
        "slug": "regulated-evidence-graph",
        "name": "Regulated Evidence Graph",
        "buyer": "Chief compliance officer; model risk lead; internal audit",
        "wedge": "Generate evidence packs linking conclusions to controls, policy text, source documents, owners, and review history.",
        "pilot": "One audit workflow, one control family, one evidence pack that survives reviewer inspection.",
        "proof": "Reviewer binder with citations, source spans, control mappings, exception trail, and exportable audit log.",
        "status": "available",
        "score": 87,
    },
    {
        "slug": "incident-dependency-twin",
        "name": "Incident Dependency Twin",
        "buyer": "Head of SRE; security operations lead",
        "wedge": "Answer what changed, what is affected, who owns it, and what runbook applies while the incident is still active.",
        "pilot": "Connect one production surface, replay three incidents, and measure time-to-root-cause plus missed dependencies.",
        "proof": "Incident replay timeline with dependency deltas, owner handoff, mitigation steps, and postmortem graph.",
        "status": "pilot-ready",
        "score": 89,
    },
    {
        "slug": "industrial-trace-capsule",
        "name": "Industrial Trace Capsule",
        "buyer": "Quality lead; manufacturing data platform owner",
        "wedge": "Trace a failed batch or test anomaly across materials, procedures, suppliers, instruments, and operator steps.",
        "pilot": "One product line or lab workflow; reconstruct lineage for historical failures and produce a recall-grade trail.",
        "proof": "Lot genealogy graph, anomaly path, certificate map, and replayable exception report.",
        "status": "available",
        "score": 85,
    },
    {
        "slug": "market-intelligence-graph",
        "name": "Market Intelligence Graph",
        "buyer": "Investment research lead; corporate strategy team",
        "wedge": "Convert public information into entities, claims, catalysts, risks, watchlists, and dated provenance.",
        "pilot": "Track one sector for two weeks and produce a daily source-grounded catalyst map with cited deltas.",
        "proof": "Daily intelligence capsule with source links, claim graph, catalyst timeline, contradiction flags, and watchlist changes.",
        "status": "preview",
        "score": 82,
    },
]

COMPETITORS = [
    {
        "slug": "puppygraph",
        "name": "PuppyGraph",
        "posture": "Zero-ETL graph analytics engine",
        "query": "Gremlin, openCypher, graph algorithms",
        "threat": "Strongest answer to the 'why move data into a graph database?' objection.",
        "graphsub_move": "Win on live operational traces, memory, approvals, and proof capsules rather than bulk lakehouse analytics.",
        "sources": ["https://docs.puppygraph.com/"],
        "confidence": "high-docs",
    },
    {
        "slug": "neo4j",
        "name": "Neo4j",
        "posture": "Mature dedicated graph platform",
        "query": "Cypher, GQL-aligned docs, drivers, GraphQL tooling",
        "threat": "Incumbent ecosystem benchmark; buyers already know the category through Neo4j.",
        "graphsub_move": "GraphSub adds agent-native memory, replayable operations, and capsule packaging on top of familiar graph workflows.",
        "sources": ["https://neo4j.com/docs/"],
        "confidence": "high-docs",
    },
    {
        "slug": "memgraph",
        "name": "Memgraph",
        "posture": "Realtime / streaming graph database",
        "query": "Cypher-compatible surface",
        "threat": "Good developer wedge for live graph and GraphRAG demos.",
        "graphsub_move": "Use as the live-graph peer and show why replay, approvals, and memory are a different product surface.",
        "sources": ["https://memgraph.com/docs"],
        "confidence": "high-docs",
    },
    {
        "slug": "tigergraph",
        "name": "TigerGraph",
        "posture": "Enterprise MPP graph analytics platform",
        "query": "GSQL, openCypher, ISO GQL positioning",
        "threat": "Scale-and-enterprise credibility for fraud, AML, entity resolution, supply chain, and recommendations.",
        "graphsub_move": "Win speed-to-capsule, operational proof, and agent control instead of max-scale graph analytics.",
        "sources": ["https://www.tigergraph.com/"],
        "confidence": "high-vendor",
    },
    {
        "slug": "neptune",
        "name": "Amazon Neptune",
        "posture": "Fully managed AWS graph service",
        "query": "Gremlin, openCypher, SPARQL",
        "threat": "Default managed graph infrastructure choice for AWS-heavy buyers.",
        "graphsub_move": "Position above infrastructure as the capsule logic, replay, evidence, and operator-memory layer.",
        "sources": ["https://docs.aws.amazon.com/neptune/latest/userguide/intro.html"],
        "confidence": "high-docs",
    },
    {
        "slug": "dgraph",
        "name": "Dgraph",
        "posture": "Distributed knowledge graph database",
        "query": "DQL, GraphQL API",
        "threat": "Relevant where GraphQL-native app builders want a graph backend.",
        "graphsub_move": "Keep the API posture focused on operational traces, agents, proof artifacts, and capsule workflows.",
        "sources": ["https://docs.hypermode.com/dgraph/overview"],
        "confidence": "high-docs",
    },
    {
        "slug": "janusgraph",
        "name": "JanusGraph",
        "posture": "Open-source distributed graph layer",
        "query": "Gremlin, TinkerPop",
        "threat": "Open-source scale-out assembly option for infrastructure-heavy teams.",
        "graphsub_move": "Be the opinionated product layer: fewer assembly choices, faster pilots, stronger proof artifacts.",
        "sources": ["https://janusgraph.org/"],
        "confidence": "high-site",
    },
    {
        "slug": "kuzu",
        "name": "Kuzu",
        "posture": "Embedded property graph engine",
        "query": "Cypher",
        "threat": "Strong embedded/local graph mental model for applications, notebooks, and developer tools.",
        "graphsub_move": "Learn from the embeddable posture, but sell GraphSub as a live cloud/control system.",
        "sources": ["https://github.com/kuzudb/kuzu"],
        "confidence": "snapshot",
    },
]

BENCHMARK_LANES = [
    {
        "vendor": "GraphSub",
        "lane": "graphsub-performance",
        "latency": "67us lookup; 125us traversal",
        "throughput": "25M ops/s",
        "memory": "2.1GB / 1M nodes",
        "note": "GraphSub product lane.",
    },
    {
        "vendor": "Memgraph",
        "lane": "reference-peer",
        "latency": "~500us lookup; ~800us traversal",
        "throughput": "3.2M ops/s",
        "memory": "10.5GB / 1M nodes",
        "note": "Reference peer lane.",
    },
    {
        "vendor": "Neo4j",
        "lane": "reference-peer",
        "latency": "~11ms lookup; ~20ms traversal",
        "throughput": "150K ops/s",
        "memory": "~15GB / 1M nodes",
        "note": "Reference peer lane.",
    },
    {
        "vendor": "PuppyGraph",
        "lane": "zero-etl-reference",
        "latency": "10-hop queries in seconds",
        "throughput": "petabyte-level scalability positioning",
        "memory": "queries existing warehouse/lake storage",
        "note": "Zero-ETL lakehouse analytics reference.",
    },
]

ROADMAP = {
    "mission": "Deploy graph-native agent workflows with live traces, replayable outputs, and source-linked competitive intelligence.",
    "now": [
        "Choose a capsule and generate a workflow plan.",
        "Compare GraphSub against the graph database market.",
        "Inspect benchmark lanes and source links from live API routes.",
    ],
    "next_30_days": [
        "Connect a real data source and produce a replay bundle.",
        "Package capsule schemas, sample traces, and ROI worksheets.",
        "Publish a reproducible benchmark harness.",
    ],
    "next_60_days": [
        "Ship downloadable capsule manifests and replay traces.",
        "Add a live agent reliability demo with streamed graph events.",
        "Add team workspaces, saved plans, and shared evidence packs.",
    ],
    "next_90_days": [
        "Launch the first hosted capsule workflows.",
        "Publish the benchmark methodology and source bundle.",
        "Turn the landing page into the hosted GraphSub console.",
    ],
}

CLOUD_USER = {
    "id": "usr_arthur",
    "name": "Arthur Colle",
    "email": "arthur@graphsub.com",
    "role": "owner",
    "avatar": "A",
    "created_at": "2026-07-03T00:00:00Z",
}

CLOUD_ORG = {
    "id": "org_dsco",
    "slug": "dsco",
    "name": "Distributed Systems, Inc.",
    "plan": "cloud-founder",
    "billing_status": "active",
    "primary_region": "us-east-1",
    "spend_limit_usd": 5000,
    "included_gsu": 50000,
}

CLOUD_REGIONS = [
    {"id": "us-east-1", "name": "US East", "provider": "modal", "status": "available", "latency_class": "primary"},
    {"id": "us-west-2", "name": "US West", "provider": "modal", "status": "available", "latency_class": "edge"},
    {"id": "eu-west-1", "name": "EU West", "provider": "modal", "status": "preview", "latency_class": "edge"},
]

CLOUD_TEMPLATES = [
    {
        "id": "starter",
        "name": "Starter",
        "cpu": 2,
        "memory_gb": 8,
        "replicas": 1,
        "included_gsu": 50000,
        "price_hourly_usd": 0.28,
        "best_for": "development, eval traces, prototype capsules",
    },
    {
        "id": "pro-4",
        "name": "Pro 4",
        "cpu": 8,
        "memory_gb": 32,
        "replicas": 4,
        "included_gsu": 250000,
        "price_hourly_usd": 1.84,
        "best_for": "production agent memory and replay workflows",
    },
    {
        "id": "scale-16",
        "name": "Scale 16",
        "cpu": 32,
        "memory_gb": 128,
        "replicas": 16,
        "included_gsu": 1000000,
        "price_hourly_usd": 7.2,
        "best_for": "high-throughput capsules and large graph workloads",
    },
]

CLOUD_INSTANCE_SEED = [
    {
        "id": "gsx_prod_agents",
        "name": "prod-agents",
        "org_id": CLOUD_ORG["id"],
        "owner_id": CLOUD_USER["id"],
        "region": "us-east-1",
        "template": "pro-4",
        "status": "running",
        "health": "healthy",
        "version": "v0.8.1",
        "endpoint": "https://prod-agents.graphsub.cloud",
        "created_at": "2026-07-03T00:14:12Z",
        "updated_at": "2026-07-03T04:00:00Z",
        "metrics": {
            "nodes": 824656,
            "edges": 3788131,
            "concepts": BASE_ONTOLOGY_CONCEPTS,
            "lookup_p99_us": 67,
            "traversal_p99_us": 125,
            "ops_per_sec": 24700000,
            "storage_gb": 41.7,
            "gsu_month": 71184,
        },
        "features": ["cypher", "graphql", "vector-search", "replay", "hierarchical-memory", "capsules"],
    },
    {
        "id": "gsx_replay_lab",
        "name": "replay-lab",
        "org_id": CLOUD_ORG["id"],
        "owner_id": CLOUD_USER["id"],
        "region": "us-east-1",
        "template": "starter",
        "status": "running",
        "health": "healthy",
        "version": "v0.8.1",
        "endpoint": "https://replay-lab.graphsub.cloud",
        "created_at": "2026-07-03T01:22:31Z",
        "updated_at": "2026-07-03T04:02:00Z",
        "metrics": {
            "nodes": 126480,
            "edges": 604913,
            "concepts": BASE_ONTOLOGY_CONCEPTS,
            "lookup_p99_us": 74,
            "traversal_p99_us": 151,
            "ops_per_sec": 5900000,
            "storage_gb": 8.4,
            "gsu_month": 12408,
        },
        "features": ["cypher", "replay", "capsules"],
    },
    {
        "id": "gsx_market_intel",
        "name": "market-intel",
        "org_id": CLOUD_ORG["id"],
        "owner_id": CLOUD_USER["id"],
        "region": "us-west-2",
        "template": "starter",
        "status": "sleeping",
        "health": "idle",
        "version": "v0.8.1",
        "endpoint": "https://market-intel.graphsub.cloud",
        "created_at": "2026-07-03T02:08:44Z",
        "updated_at": "2026-07-03T03:35:00Z",
        "metrics": {
            "nodes": 88203,
            "edges": 314622,
            "concepts": BASE_ONTOLOGY_CONCEPTS,
            "lookup_p99_us": 91,
            "traversal_p99_us": 204,
            "ops_per_sec": 0,
            "storage_gb": 5.1,
            "gsu_month": 3921,
        },
        "features": ["cypher", "graphql", "source-ledger"],
    },
]

WIKI_PAGES = [
    {
        "slug": "openontology",
        "title": "GraphSub OpenOntology",
        "description": "GraphSub OpenOntology is an open operational ontology for real-world objects, links, actions, functions, interfaces, scenarios, permissions, provenance, and agent runtime state.",
        "keywords": ["OpenOntology", "operational ontology", "Palantir ontology alternative", "object types", "link types", "action types"],
        "sections": [
            ("Definition", "GraphSub OpenOntology is a public, open schema for modeling real-world operations as object types, link types, shared properties, actions, functions, interfaces, object sets, scenarios, and permissions."),
            ("Design pattern", "The model follows public operational-ontology patterns: objects represent the world, links represent relationships, actions change governed state, functions compute derived views, interfaces provide reusable contracts, and scenarios model what-if branches."),
            ("GraphSub implementation", "The live API exposes OpenOntology at /api/ontology, with paginated concepts, object types, link types, action types, functions, interfaces, object sets, and scenarios. Each GraphSub instance boots with a 65,536-concept OpenOntology lattice."),
        ],
        "faq": [
            ("Is this Palantir's ontology?", "No. It is GraphSub's open ontology, modeled from public ontology concepts rather than copied proprietary schema."),
            ("What does OpenOntology include?", "Domains, value types, shared properties, interfaces, object types, link types, actions, functions, object sets, scenarios, permissions, provenance, and agent runtime concepts."),
        ],
        "related": ["graph-database-for-agents", "operational-proof-graph", "agent-memory-graph"],
    },
    {
        "slug": "graph-database-for-agents",
        "title": "Graph Database for Agent Systems",
        "description": "GraphSub is a graph database and control substrate for AI agents that need persistent memory, tool traces, replayable outputs, and low-latency graph operations.",
        "keywords": ["graph database for agents", "AI agent memory", "agent graph database", "GraphSub"],
        "sections": [
            ("Definition", "A graph database for agent systems stores people, tasks, tools, memories, approvals, events, and outputs as connected state. GraphSub extends that model with agent-native primitives for memory, replay, capability tokens, and operational proof."),
            ("Why it matters", "Autonomous workflows fail when state is trapped in logs, chat transcripts, or isolated tools. A graph substrate gives agents a durable map of what happened, which dependencies changed, and which outputs should be recomputed."),
            ("GraphSub approach", "GraphSub combines sub-millisecond graph operations with hierarchical memory, stigmergic coordination, workflow replay, and capsule packaging for real-world enterprise workflows."),
        ],
        "faq": [
            ("What is an agent graph database?", "It is a graph database optimized for AI agents, tool traces, memory, approvals, and replayable workflow state."),
            ("How is GraphSub different from a traditional graph database?", "Traditional graph databases focus on storage and query. GraphSub adds agent memory, replay, workflow capsules, and proof-oriented APIs."),
        ],
        "related": ["agent-memory-graph", "operational-proof-graph", "graph-capsules"],
    },
    {
        "slug": "agent-memory-graph",
        "title": "Agent Memory Graph",
        "description": "An agent memory graph organizes working, episodic, and semantic memory so autonomous systems can recall, update, and audit context over time.",
        "keywords": ["agent memory", "AI memory graph", "hierarchical memory", "episodic memory"],
        "sections": [
            ("Definition", "An agent memory graph connects short-lived working memory, episodic task history, semantic facts, tool outputs, and user corrections into one queryable structure."),
            ("Core model", "GraphSub models memory as typed nodes and edges with importance, source, timestamp, scope, and replay metadata. That makes memory inspectable rather than hidden inside a prompt window."),
            ("Use cases", "Agent memory graphs are useful for customer support agents, research assistants, compliance workflows, code agents, operations copilots, and any system that must remember why it acted."),
        ],
        "faq": [
            ("Why use a graph for agent memory?", "Graphs preserve relationships between tasks, tools, people, documents, decisions, and outputs."),
            ("Can memory be audited?", "Yes. GraphSub memory can be traced through sources, tool calls, approvals, and replay events."),
        ],
        "related": ["hierarchical-memory", "graph-database-for-agents", "operational-proof-graph"],
    },
    {
        "slug": "operational-proof-graph",
        "title": "Operational Proof Graph",
        "description": "An operational proof graph records inputs, decisions, dependencies, approvals, and outputs so teams can replay and explain complex workflows.",
        "keywords": ["operational proof", "workflow replay", "audit graph", "agent reliability"],
        "sections": [
            ("Definition", "An operational proof graph is a system of record for how an output was produced. It captures the path from source data through tools, decisions, approvals, and final result."),
            ("Enterprise need", "Teams adopting agents need to answer what changed, who approved it, which tools ran, and why an output was accepted. Logs alone rarely provide that causal map."),
            ("GraphSub capsules", "GraphSub packages proof graphs into capsules for trading control, agent reliability, regulated evidence, incident dependencies, industrial traceability, and market intelligence."),
        ],
        "faq": [
            ("What does a proof graph contain?", "Sources, task state, tool calls, memory deltas, approvals, dependencies, replay metadata, and outputs."),
            ("Is this only for compliance?", "No. The same graph improves debugging, reliability, incident response, and product analytics."),
        ],
        "related": ["graph-capsules", "agent-memory-graph", "regulated-evidence-graph"],
    },
    {
        "slug": "graph-capsules",
        "title": "GraphSub Capsules",
        "description": "GraphSub capsules are deployable graph workflows for high-value operational problems such as agent reliability, trading controls, audit evidence, and incident response.",
        "keywords": ["graph capsules", "workflow capsule", "GraphSub capsules", "AI workflow"],
        "sections": [
            ("Definition", "A GraphSub capsule is a packaged graph workflow with schema, data contracts, replay traces, benchmark slice, and ROI worksheet."),
            ("Launch model", "Each capsule starts with a narrow buyer workflow and expands into a control graph as more sources, dependencies, and outputs are connected."),
            ("Available capsules", "Current capsules include Trading Control Graph, Agent Reliability Graph, Regulated Evidence Graph, Incident Dependency Twin, Industrial Trace Capsule, and Market Intelligence Graph."),
        ],
        "faq": [
            ("What is included in a capsule?", "A schema map, source ledger, trace replay, benchmark slice, and launch plan."),
            ("Who buys capsules?", "Risk technology teams, AI platform teams, compliance teams, SRE leaders, quality teams, and research organizations."),
        ],
        "related": ["operational-proof-graph", "agent-reliability-graph", "trading-control-graph"],
    },
    {
        "slug": "graphsub-vs-neo4j",
        "title": "GraphSub vs Neo4j",
        "description": "GraphSub and Neo4j both use graph concepts, but GraphSub is positioned around agent memory, replayable workflows, and operational proof.",
        "keywords": ["GraphSub vs Neo4j", "Neo4j alternative", "agent graph database"],
        "sections": [
            ("Category", "Neo4j is a mature graph database platform with a large Cypher ecosystem. GraphSub targets graph-native agent workflows where memory, replay, and proof are first-class product surfaces."),
            ("Where Neo4j is strong", "Neo4j has broad tooling, education, drivers, Aura, Graph Data Science, and enterprise adoption."),
            ("Where GraphSub is different", "GraphSub emphasizes low-latency graph operations, hierarchical agent memory, workflow replay, source-linked outputs, and deployable capsules."),
        ],
        "faq": [
            ("Is GraphSub a Neo4j replacement?", "GraphSub is not just a storage replacement. It is a control substrate for agent and operational workflows."),
            ("Can GraphSub explain agent behavior?", "Yes. The product model centers on memory, tool traces, approvals, replay, and source-linked outputs."),
        ],
        "related": ["graph-database-for-agents", "graph-database-benchmarks", "graphsub-vs-puppygraph"],
    },
    {
        "slug": "graphsub-vs-puppygraph",
        "title": "GraphSub vs PuppyGraph",
        "description": "PuppyGraph focuses on zero-ETL graph analytics over existing data stores, while GraphSub focuses on live agent workflows, memory, replay, and proof capsules.",
        "keywords": ["GraphSub vs PuppyGraph", "PuppyGraph alternative", "zero ETL graph analytics"],
        "sections": [
            ("Category", "PuppyGraph is a zero-ETL graph analytics engine for querying existing warehouses and lakes as graphs. GraphSub is a graph-native workflow and agent control substrate."),
            ("Where PuppyGraph is strong", "PuppyGraph reduces data movement for lakehouse teams and is compelling when the core job is multi-hop analytics over existing analytical storage."),
            ("Where GraphSub is different", "GraphSub focuses on live operational traces, agent memory, approvals, replayable outputs, and deployable capsules for production workflows."),
        ],
        "faq": [
            ("Is GraphSub zero-ETL?", "GraphSub can connect to existing systems, but its main advantage is the live workflow graph and replay layer."),
            ("Can these products coexist?", "Yes. A zero-ETL analytics engine can serve analytical queries while GraphSub owns operational proof and agent memory."),
        ],
        "related": ["zero-etl-graph-analytics", "graph-database-for-agents", "graphsub-vs-neo4j"],
    },
    {
        "slug": "graph-database-benchmarks",
        "title": "Graph Database Benchmarks",
        "description": "Graph database benchmarks compare lookup latency, traversal latency, throughput, memory footprint, analytics support, and operational workflow fit.",
        "keywords": ["graph database benchmarks", "Neo4j benchmark", "Memgraph benchmark", "GraphSub benchmark"],
        "sections": [
            ("What to measure", "Useful graph database benchmarks include node lookup P99, edge traversal P99, saturated throughput, memory per million nodes, vector search support, and multi-hop query performance."),
            ("GraphSub performance lane", "The GraphSub product map highlights 67us node lookup, 125us traversal, 25M ops/s, and 2.1GB per 1M nodes for the GraphSub lane."),
            ("Beyond speed", "For agent systems, benchmark value also depends on memory, replay, source links, approval traces, and whether the graph can support operational workflows."),
        ],
        "faq": [
            ("What is a fair graph benchmark?", "A fair benchmark publishes hardware, versions, dataset shape, query mix, concurrency, cache state, and P50/P95/P99 latencies."),
            ("Why include workflow fit?", "Agent systems need more than fast graph queries; they need explainable state and replayable outputs."),
        ],
        "related": ["graphsub-vs-neo4j", "graphsub-vs-puppygraph", "agent-memory-graph"],
    },
    {
        "slug": "zero-etl-graph-analytics",
        "title": "Zero-ETL Graph Analytics",
        "description": "Zero-ETL graph analytics lets teams query warehouse or lakehouse data as a graph without first loading it into a dedicated graph database.",
        "keywords": ["zero ETL graph", "graph analytics lakehouse", "PuppyGraph", "warehouse graph"],
        "sections": [
            ("Definition", "Zero-ETL graph analytics maps existing data in warehouses, lakes, or lakehouses into a graph query layer."),
            ("Why teams care", "The approach reduces migration friction and lets data teams run graph queries against governed analytical storage."),
            ("GraphSub context", "GraphSub is complementary when the workflow needs live memory, operational traces, replay, approvals, and capsule outputs rather than only ad hoc graph analytics."),
        ],
        "faq": [
            ("Is zero-ETL the same as operational graph state?", "No. Zero-ETL is usually analytical. Operational graph state changes as workflows run."),
            ("Where does GraphSub fit?", "GraphSub fits where teams need a control graph for live agents, outputs, and proof."),
        ],
        "related": ["graphsub-vs-puppygraph", "graph-database-benchmarks", "graph-database-for-agents"],
    },
    {
        "slug": "graph-rag",
        "title": "GraphRAG and Source-Linked Retrieval",
        "description": "GraphRAG combines graph structure with retrieval-augmented generation so AI systems can answer with entities, relationships, provenance, and source context.",
        "keywords": ["GraphRAG", "graph retrieval augmented generation", "source linked retrieval"],
        "sections": [
            ("Definition", "GraphRAG uses graph relationships to improve retrieval and generation. Instead of retrieving isolated chunks, the system can traverse entities, claims, events, owners, and sources."),
            ("GraphSub role", "GraphSub supports GraphRAG-style workflows by storing memory, source links, tool traces, and output lineage as graph state."),
            ("Enterprise value", "Source-linked retrieval matters when teams need answers that can be inspected, replayed, and connected to operational systems."),
        ],
        "faq": [
            ("How is GraphRAG different from vector search?", "Vector search finds semantically similar content; GraphRAG adds explicit relationships and provenance."),
            ("Does GraphSub include vector search?", "The product surface includes vector and graph-oriented retrieval lanes for agent workflows."),
        ],
        "related": ["agent-memory-graph", "market-intelligence-graph", "regulated-evidence-graph"],
    },
    {
        "slug": "hierarchical-memory",
        "title": "Hierarchical Memory for AI Agents",
        "description": "Hierarchical memory separates working, episodic, and semantic memory so AI agents can maintain context without losing auditability.",
        "keywords": ["hierarchical memory", "working memory AI", "episodic memory AI", "semantic memory AI"],
        "sections": [
            ("Definition", "Hierarchical memory splits context into working memory for immediate tasks, episodic memory for events, and semantic memory for durable concepts."),
            ("Graph representation", "A graph representation lets memory nodes retain links to sources, tasks, users, tools, outputs, and confidence over time."),
            ("GraphSub approach", "GraphSub uses hierarchical memory as one of its native agent primitives, alongside stigmergy, goal hierarchies, prediction models, causal inference, and capability tokens."),
        ],
        "faq": [
            ("Why not keep memory in a prompt?", "Prompt memory is hard to audit, reuse, and invalidate. A graph gives memory structure and provenance."),
            ("Can memory expire?", "Yes. GraphSub models memory with importance, consolidation, scope, and lifecycle controls."),
        ],
        "related": ["agent-memory-graph", "stigmergic-coordination", "graph-database-for-agents"],
    },
    {
        "slug": "stigmergic-coordination",
        "title": "Stigmergic Coordination for Agents",
        "description": "Stigmergic coordination lets agents coordinate through shared traces, signals, and environmental state rather than direct message passing alone.",
        "keywords": ["stigmergic coordination", "agent coordination", "multi-agent systems"],
        "sections": [
            ("Definition", "Stigmergy is coordination through environmental traces. In software, agents can leave weighted signals that other agents read and reinforce."),
            ("GraphSub model", "GraphSub represents coordination signals as graph state with weights, decay, scope, and relationships to tasks, tools, and outcomes."),
            ("Why it matters", "Agent swarms need scalable coordination. Shared graph state can reduce brittle point-to-point messaging and produce inspectable behavior."),
        ],
        "faq": [
            ("Is stigmergy only for swarm robotics?", "No. It is useful anywhere autonomous actors coordinate through shared state."),
            ("How does GraphSub expose it?", "The product surface includes stigmergic coordination primitives and live substrate visualization."),
        ],
        "related": ["hierarchical-memory", "graph-database-for-agents", "agent-reliability-graph"],
    },
    {
        "slug": "agent-reliability-graph",
        "title": "Agent Reliability Graph",
        "description": "An agent reliability graph helps teams understand failures by connecting tasks, tools, memory changes, policy gates, and eval results.",
        "keywords": ["agent reliability", "AI agent evals", "tool trace graph", "agent debugging"],
        "sections": [
            ("Definition", "An agent reliability graph maps every meaningful part of an AI workflow: request, plan, tool calls, memory changes, approvals, errors, evals, and final output."),
            ("Workflow", "Teams can replay failed tasks, cluster failure modes, and fix repeated errors through memory and approval trace improvements."),
            ("GraphSub capsule", "The Agent Reliability Graph capsule packages this workflow for AI platform teams moving from demos to production operations."),
        ],
        "faq": [
            ("What problems does it solve?", "It helps teams debug tool use, repeated failures, missing approvals, memory drift, and eval regressions."),
            ("Who uses it?", "AI platform teams, automation teams, enterprise copilots, and agent infrastructure teams."),
        ],
        "related": ["agent-memory-graph", "operational-proof-graph", "graph-rag"],
    },
    {
        "slug": "trading-control-graph",
        "title": "Trading Control Graph",
        "description": "A trading control graph maps positions, books, curves, scenarios, limits, approvals, and outputs for replayable risk and P&L workflows.",
        "keywords": ["trading control graph", "risk graph", "P&L replay", "finance graph database"],
        "sections": [
            ("Definition", "A trading control graph connects trades, books, portfolios, curves, limits, scenarios, model versions, approvals, and downstream outputs."),
            ("Why it matters", "Financial systems need precise invalidation and replay. When a trade, curve, or model changes, only affected outputs should recompute."),
            ("GraphSub capsule", "GraphSub packages trading control workflows into a capsule for risk technology, front-office platform teams, and audit-aware operations."),
        ],
        "faq": [
            ("What does it replace?", "It complements existing risk systems by adding dependency graphing, replay, lineage, and proof outputs."),
            ("Why graph-based?", "Financial outputs are dependency-heavy. Graphs naturally represent trades, curves, scenarios, owners, and affected reports."),
        ],
        "related": ["graph-capsules", "operational-proof-graph", "graph-database-benchmarks"],
    },
    {
        "slug": "regulated-evidence-graph",
        "title": "Regulated Evidence Graph",
        "description": "A regulated evidence graph links policies, controls, exceptions, reviewers, evidence, and approvals into auditable workflows.",
        "keywords": ["regulated evidence graph", "compliance graph", "audit evidence", "model risk graph"],
        "sections": [
            ("Definition", "A regulated evidence graph is a source-linked map of controls, policies, exceptions, reviewers, approvals, and supporting documents."),
            ("Why teams need it", "Compliance and model risk teams need answers that survive review. The graph shows where conclusions came from and who approved them."),
            ("GraphSub capsule", "The Regulated Evidence Graph capsule turns audit and compliance workflows into replayable evidence packs."),
        ],
        "faq": [
            ("Can it export review packs?", "Yes. The capsule model includes source links, control mappings, exception trails, and exportable logs."),
            ("Does it use AI?", "It can support AI-assisted evidence collection while keeping sources and approval trails explicit."),
        ],
        "related": ["operational-proof-graph", "graph-rag", "graph-capsules"],
    },
    {
        "slug": "market-intelligence-graph",
        "title": "Market Intelligence Graph",
        "description": "A market intelligence graph connects news, filings, transcripts, entities, catalysts, risks, and source links into inspectable research workflows.",
        "keywords": ["market intelligence graph", "research graph", "filings graph", "source linked intelligence"],
        "sections": [
            ("Definition", "A market intelligence graph turns public information into connected entities, claims, risks, catalysts, watchlists, and source links."),
            ("Use cases", "Research teams can track a sector, map catalyst timelines, detect contradictions, and preserve thesis history."),
            ("GraphSub capsule", "The Market Intelligence Graph capsule packages daily intelligence workflows with source links, graph memory, and replayable research outputs."),
        ],
        "faq": [
            ("What sources can it use?", "News, filings, transcripts, prices, people, products, and analyst notes."),
            ("Why graph-based research?", "Research depends on relationships between companies, people, products, claims, events, and prior theses."),
        ],
        "related": ["graph-rag", "agent-memory-graph", "graph-capsules"],
    },
]

WIKI_BY_SLUG = {page["slug"]: page for page in WIKI_PAGES}

CODE_EXAMPLES = [
    {
        "slug": "agent-memory-python",
        "title": "Agent memory and replay",
        "language": "python",
        "description": "Write agent memory, launch a capsule, and inspect affected outputs.",
        "code": """from graphsub import Client

client = Client(base_url="https://graphsub.com")

memory = client.memory.remember(
    tier="working",
    content="limit breach on EUR rates book",
    source="risk-run-2026-07-03",
    importance=0.92,
)

trace = client.capsules.execute(
    capsule="trading-control-graph",
    workload="risk-recompute",
    evidence="source-linked",
)

print(trace.replay_id, trace.affected_outputs)""",
    },
    {
        "slug": "dependency-cypher",
        "title": "Dependency blast-radius query",
        "language": "cypher",
        "description": "Find downstream outputs that need replay after a source changes.",
        "code": """MATCH (s:Source {id: 'SOFR:2026-07-03'})
  -[:FEEDS]->(m:Model)
  -[:PRODUCES]->(o:Output)
WHERE o.status <> 'replayed'
RETURN
  o.id AS output,
  m.version AS model,
  shortestPath((s)-[*..4]->(o)) AS proof_path
ORDER BY o.priority DESC""",
    },
    {
        "slug": "capsule-rest",
        "title": "Capsule planner routes",
        "language": "bash",
        "description": "Use the live Modal routes to build and execute a capsule plan.",
        "code": """curl 'https://graphsub.com/api/plan?capsule=trading-control-graph&target=enterprise-team&workload=risk-recompute' \\
  | jq '.execution_preview | {capsule, buyer, workflow, artifacts}'

curl -X POST 'https://graphsub.com/api/execute?capsule=regulated-evidence-graph&workload=audit-evidence' \\
  | jq '.execution.replay'

curl 'https://graphsub.com/api/examples'""",
    },
    {
        "slug": "graphrag-typescript",
        "title": "Source-linked GraphRAG",
        "language": "typescript",
        "description": "Retrieve graph context with entities, claims, and source links.",
        "code": """const answer = await graphsub.retrieve({
  query: "Which vendor changes affect the risk replay plan?",
  traverse: ["Company", "Claim", "Source", "Output"],
  requireSources: true,
  maxHops: 4,
})

for (const citation of answer.sources) {
  console.log(citation.url, citation.claim_id)
}""",
    },
    {
        "slug": "evidence-yaml",
        "title": "Evidence capsule manifest",
        "language": "yaml",
        "description": "A deployable capsule includes schema, source ledger, replay, benchmark, and ROI artifacts.",
        "code": """capsule: regulated-evidence-graph
buyer: model-risk
workflow: audit-evidence
artifacts:
  - schema_map
  - source_ledger
  - replay_trace
  - benchmark_slice
  - roi_worksheet
controls:
  approvals: required
  sources: source-linked
  replay: deterministic""",
    },
]

WIKI_EXAMPLE_SLUGS = {
    "graph-database-for-agents": "agent-memory-python",
    "agent-memory-graph": "agent-memory-python",
    "hierarchical-memory": "agent-memory-python",
    "operational-proof-graph": "dependency-cypher",
    "graph-capsules": "capsule-rest",
    "graphsub-vs-neo4j": "dependency-cypher",
    "graphsub-vs-puppygraph": "capsule-rest",
    "graph-database-benchmarks": "capsule-rest",
    "zero-etl-graph-analytics": "capsule-rest",
    "graph-rag": "graphrag-typescript",
    "stigmergic-coordination": "agent-memory-python",
    "agent-reliability-graph": "agent-memory-python",
    "trading-control-graph": "dependency-cypher",
    "regulated-evidence-graph": "evidence-yaml",
    "market-intelligence-graph": "graphrag-typescript",
}

CODE_EXAMPLE_BY_SLUG = {example["slug"]: example for example in CODE_EXAMPLES}


def code_example_for_page(page: dict) -> dict:
    return CODE_EXAMPLE_BY_SLUG[WIKI_EXAMPLE_SLUGS.get(page["slug"], "capsule-rest")]


def wiki_url(slug: str = "") -> str:
    return f"{SITE_URL}/wiki/{slug}" if slug else f"{SITE_URL}/wiki"


def json_ld(data: dict) -> str:
    return json.dumps(data, ensure_ascii=False, separators=(",", ":"))


TOKEN_PATTERN = re.compile(
    r"(?P<comment>#.*?$|//.*?$)|"
    r"(?P<string>\"(?:\\.|[^\"])*\"|'(?:\\.|[^'])*')|"
    r"(?P<number>\b\d+(?:\.\d+)?\b)|"
    r"(?P<keyword>\b(?:MATCH|RETURN|WHERE|ORDER|BY|AS|GET|POST|curl|jq|from|import|const|await|for|print|Client|Source|Model|Output|FEEDS|PRODUCES|capsule|buyer|workflow|artifacts|controls|approvals|sources|replay|required|deterministic)\b)",
    re.MULTILINE,
)


def highlight_code(code: str) -> str:
    pieces: list[str] = []
    last = 0
    for match in TOKEN_PATTERN.finditer(code):
        pieces.append(html.escape(code[last : match.start()]))
        cls = next(name for name, value in match.groupdict().items() if value is not None)
        pieces.append(f'<span class="tok-{cls}">{html.escape(match.group(0))}</span>')
        last = match.end()
    pieces.append(html.escape(code[last:]))
    return "".join(pieces)


def render_code_panel(example: dict) -> str:
    return f"""<section class=\"code-section\" id=\"code-example\">
<h2>Code example</h2>
<div class=\"code-card\">
<div class=\"code-head\"><strong>{html.escape(example['title'])}</strong><span>{html.escape(example['language'])}</span></div>
<pre><code>{highlight_code(example['code'])}</code></pre>
<p>{html.escape(example['description'])}</p>
</div>
</section>"""


def render_wiki_shell(title: str, description: str, body: str, canonical: str, schema: dict) -> str:
    return f"""<!doctype html>
<html lang=\"en\">
<head>
<meta charset=\"utf-8\">
<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, viewport-fit=cover\">
<title>{html.escape(title)} | GraphSub Wiki</title>
<meta name=\"description\" content=\"{html.escape(description)}\">
<meta name=\"robots\" content=\"index,follow,max-image-preview:large\">
<link rel=\"canonical\" href=\"{html.escape(canonical)}\">
<meta property=\"og:type\" content=\"article\">
<meta property=\"og:site_name\" content=\"GraphSub\">
<meta property=\"og:title\" content=\"{html.escape(title)}\">
<meta property=\"og:description\" content=\"{html.escape(description)}\">
<meta property=\"og:url\" content=\"{html.escape(canonical)}\">
<meta property=\"og:image\" content=\"{SITE_URL}/og-image.png\">
<meta name=\"twitter:card\" content=\"summary_large_image\">
<link rel=\"icon\" href=\"/favicon.svg\" type=\"image/svg+xml\">
<script type=\"application/ld+json\">{json_ld(schema)}</script>
<style>
:root{{--bg:#0b0803;--panel:rgba(18,12,4,.78);--code:#090602;--text:#f0c36a;--muted:#9b6c1a;--bright:#ffd484;--line:#6f4b11;--ok:#33ff66;--font-sans:-apple-system,BlinkMacSystemFont,"SF Pro Display","Segoe UI",Roboto,Inter,ui-sans-serif,system-ui,sans-serif;--font-mono:"SF Mono","Cascadia Code","JetBrains Mono","Roboto Mono",ui-monospace,Menlo,Consolas,monospace}}
*{{box-sizing:border-box}} body{{margin:0;background:var(--bg);color:var(--text);font:500 16px/1.68 var(--font-sans);font-feature-settings:"liga" 1,"calt" 1,"zero" 1}}
a{{color:var(--bright);text-decoration:none;border-bottom:1px dotted var(--line)}} a:hover{{border-bottom-color:var(--bright)}}
.layout{{display:grid;grid-template-columns:260px minmax(0,900px);gap:34px;max-width:1280px;margin:0 auto;padding:28px 22px 80px}}
.side{{position:sticky;top:0;height:100vh;overflow:auto;border-right:1px solid var(--line);padding:8px 18px 20px 0;font:13px/1.5 var(--font-mono)}}
.brand{{display:block;color:var(--bright);font-weight:700;letter-spacing:.18em;margin-bottom:18px;border:0}}
.side a{{display:block;border:0;color:var(--muted);padding:4px 0}} .side a:hover{{color:var(--bright)}}
.main{{min-width:0}} .crumbs{{font:12px/1.5 var(--font-mono);color:var(--muted);letter-spacing:.08em;margin-bottom:18px}}
h1{{font:800 44px/1.08 var(--font-sans);color:var(--bright);margin:0 0 8px;text-wrap:balance}}
.desc{{font-size:18px;color:#e8b957;margin:0 0 22px;max-width:760px}}
.infobox{{float:right;width:300px;margin:0 0 18px 26px;border:1px solid var(--line);background:var(--panel);font:13px/1.5 var(--font-sans)}}
.infobox h2{{font-size:15px;margin:0;padding:12px 14px;border-bottom:1px solid var(--line);color:var(--bright)}}
.infobox div{{display:grid;grid-template-columns:95px 1fr;gap:10px;padding:8px 14px;border-bottom:1px solid rgba(111,75,17,.55)}} .infobox div:last-child{{border-bottom:0}}
.infobox b{{color:var(--muted);font-weight:500}} .infobox span{{text-align:right}}
h2{{font:800 24px/1.25 var(--font-sans);color:var(--bright);border-bottom:1px solid var(--line);margin:34px 0 10px;padding-bottom:6px}}
p{{margin:0 0 15px}} ul{{padding-left:22px}} li{{margin:4px 0}}
.callout{{border-left:3px solid var(--bright);background:var(--panel);padding:14px 16px;margin:22px 0;color:#efc974}}
.cards{{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px;margin:20px 0}}
.card{{border:1px solid var(--line);background:var(--panel);padding:14px}} .card strong{{display:block;color:var(--bright);font-family:var(--font-sans)}}
.faq dt{{font-weight:700;color:var(--bright);margin-top:14px}} .faq dd{{margin:4px 0 8px 0}}
.code-card{{border:1px solid var(--line);background:var(--panel);margin:14px 0 22px;overflow:hidden}}
.code-head{{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:12px 14px;border-bottom:1px solid var(--line);font-family:var(--font-mono)}}
.code-head strong{{color:var(--bright)}} .code-head span{{color:var(--muted);font-size:12px;letter-spacing:.12em;text-transform:uppercase}}
pre{{margin:0;padding:18px;overflow:auto;background:radial-gradient(circle at 8% 0, rgba(255,212,132,.08), transparent 35%),var(--code);font:13px/1.72 var(--font-mono);white-space:pre}}
.code-card p{{padding:12px 14px 15px;margin:0;color:#e8b957;border-top:1px solid rgba(111,75,17,.55)}}
.tok-comment{{color:var(--muted);font-style:italic}} .tok-keyword{{color:var(--bright);font-weight:700}} .tok-string{{color:#ffe3a3}} .tok-number{{color:var(--ok)}}
.footer{{margin-top:42px;padding-top:18px;border-top:1px solid var(--line);font:12px/1.5 var(--font-mono);color:var(--muted)}}
@media(max-width:820px){{.layout{{display:block;padding:20px 14px 70px}}.side{{position:relative;height:auto;border-right:0;border-bottom:1px solid var(--line);padding:0 0 16px;margin-bottom:22px}}.infobox{{float:none;width:auto;margin:0 0 20px}}h1{{font-size:32px}}.cards{{grid-template-columns:1fr}}pre{{white-space:pre-wrap;overflow-wrap:anywhere;font-size:12px}}.code-head{{display:grid;grid-template-columns:1fr;gap:4px}}}}
</style>
</head>
<body>{body}</body>
</html>"""


def render_wiki_index() -> str:
    item_list = {
        "@context": "https://schema.org",
        "@type": "CollectionPage",
        "name": "GraphSub Wiki",
        "description": "GraphSub encyclopedia pages for agent graph databases, memory graphs, proof graphs, benchmarks, GraphRAG, and competitive comparisons.",
        "url": wiki_url(),
        "mainEntity": {
            "@type": "ItemList",
            "itemListElement": [
                {"@type": "ListItem", "position": i + 1, "url": wiki_url(page["slug"]), "name": page["title"]}
                for i, page in enumerate(WIKI_PAGES)
            ],
        },
    }
    links = "\n".join(
        f"<article class=\"card\"><strong><a href=\"/wiki/{html.escape(page['slug'])}\">{html.escape(page['title'])}</a></strong><p>{html.escape(page['description'])}</p></article>"
        for page in WIKI_PAGES
    )
    side = "\n".join(
        f"<a href=\"/wiki/{html.escape(page['slug'])}\">{html.escape(page['title'])}</a>" for page in WIKI_PAGES
    )
    body = f"""<div class=\"layout\">
<aside class=\"side\"><a class=\"brand\" href=\"/\">GRAPHSUB</a><a href=\"/wiki\">Wiki index</a>{side}<a href=\"/sitemap.xml\">Sitemap</a></aside>
<main class=\"main\">
<div class=\"crumbs\">graphsub.com / wiki</div>
<h1>GraphSub Wiki</h1>
<p class=\"desc\">A crawlable encyclopedia for graph databases built for AI agents, operational proof, memory graphs, GraphRAG, and real-world capsules.</p>
<div class=\"callout\">GraphSub combines low-latency graph operations with agent memory, replayable workflows, source-linked outputs, and deployable capsules.</div>
{render_code_panel(CODE_EXAMPLES[2])}
<section class=\"cards\">{links}</section>
<div class=\"footer\"><a href=\"/\">GraphSub landing</a> · <a href=\"/api/wiki\">Wiki JSON</a> · revision {html.escape(DEPLOYMENT_REVISION)}</div>
</main>
</div>"""
    return render_wiki_shell("GraphSub Wiki", "GraphSub encyclopedia pages for agent graph databases, memory graphs, GraphRAG, benchmarks, and capsules.", body, wiki_url(), item_list)


def render_wiki_page(page: dict) -> str:
    example = code_example_for_page(page)
    article_schema = {
        "@context": "https://schema.org",
        "@type": "TechArticle",
        "headline": page["title"],
        "description": page["description"],
        "url": wiki_url(page["slug"]),
        "dateModified": "2026-07-03",
        "publisher": {"@type": "Organization", "name": "GraphSub", "url": SITE_URL},
        "keywords": page["keywords"],
        "programmingLanguage": example["language"],
        "mainEntity": [
            {"@type": "Question", "name": q, "acceptedAnswer": {"@type": "Answer", "text": a}}
            for q, a in page["faq"]
        ],
    }
    side = "\n".join(
        f"<a href=\"/wiki/{html.escape(other['slug'])}\">{html.escape(other['title'])}</a>" for other in WIKI_PAGES
    )
    sections = "\n".join(
        f"<h2 id=\"{html.escape(heading.lower().replace(' ', '-'))}\">{html.escape(heading)}</h2><p>{html.escape(text)}</p>"
        for heading, text in page["sections"]
    )
    faqs = "\n".join(
        f"<dt>{html.escape(q)}</dt><dd>{html.escape(a)}</dd>" for q, a in page["faq"]
    )
    related = "\n".join(
        f"<li><a href=\"/wiki/{html.escape(slug)}\">{html.escape(WIKI_BY_SLUG[slug]['title'])}</a></li>"
        for slug in page["related"]
        if slug in WIKI_BY_SLUG
    )
    body = f"""<div class=\"layout\">
<aside class=\"side\"><a class=\"brand\" href=\"/\">GRAPHSUB</a><a href=\"/wiki\">Wiki index</a>{side}</aside>
<main class=\"main\">
<div class=\"crumbs\"><a href=\"/\">graphsub.com</a> / <a href=\"/wiki\">wiki</a> / {html.escape(page['slug'])}</div>
<h1>{html.escape(page['title'])}</h1>
<p class=\"desc\">{html.escape(page['description'])}</p>
<aside class=\"infobox\"><h2>GraphSub Wiki</h2><div><b>Topic</b><span>{html.escape(page['title'])}</span></div><div><b>Category</b><span>Agent graph systems</span></div><div><b>Updated</b><span>2026-07-03</span></div><div><b>Product</b><span>GraphSub</span></div></aside>
{sections}
{render_code_panel(example)}
<h2 id=\"faq\">FAQ</h2><dl class=\"faq\">{faqs}</dl>
<h2 id=\"related\">Related pages</h2><ul>{related}</ul>
<div class=\"footer\"><a href=\"/wiki\">Wiki index</a> · <a href=\"/api/wiki\">Wiki JSON</a> · revision {html.escape(DEPLOYMENT_REVISION)}</div>
</main>
</div>"""
    return render_wiki_shell(page["title"], page["description"], body, wiki_url(page["slug"]), article_schema)


def render_not_found(path: str) -> str:
    safe_path = html.escape(path or "/")
    body = f"""<div class=\"layout\">
<aside class=\"side\"><a class=\"brand\" href=\"/\">GRAPHSUB</a><a href=\"/wiki\">Wiki index</a><a href=\"/api\">API manifest</a><a href=\"/llms.txt\">llms.txt</a></aside>
<main class=\"main\">
<div class=\"crumbs\">graphsub.com / missing</div>
<h1>Route not found</h1>
<p class=\"desc\">The requested path <code>{safe_path}</code> is not a published GraphSub page. Use the wiki index, landing page, or API manifest.</p>
<div class=\"callout\"><a href=\"/wiki\">Open the GraphSub Wiki</a> · <a href=\"/\">Return to GraphSub</a> · <a href=\"/api/examples\">Code examples JSON</a></div>
<div class=\"footer\">revision {html.escape(DEPLOYMENT_REVISION)}</div>
</main>
</div>"""
    schema = {
        "@context": "https://schema.org",
        "@type": "WebPage",
        "name": "GraphSub route not found",
        "url": f"{SITE_URL}/{path.lstrip('/')}",
        "isPartOf": {"@type": "WebSite", "name": "GraphSub", "url": SITE_URL},
    }
    return render_wiki_shell("Route not found", "GraphSub route not found page with wiki and API links.", body, f"{SITE_URL}/wiki", schema)


def normalize_wiki_slug(slug: str) -> str:
    cleaned = slug.strip().strip("/")
    if cleaned in {"", "index", "index.html", "wiki", "wiki.html"}:
        return ""
    if cleaned.endswith(".html"):
        cleaned = cleaned[:-5]
    cleaned = re.sub(r"[\s_]+", "-", cleaned.lower())
    cleaned = re.sub(r"[^a-z0-9-]", "", cleaned)
    return cleaned


def capsule_for(slug: str | None) -> dict:
    if slug:
        wanted = slug.lower()
        for capsule in CAPSULES:
            if capsule["slug"] == wanted or wanted in capsule["name"].lower():
                return capsule
    return CAPSULES[1]


def execution_artifact(capsule: dict, target: str = "pilot-buyer", workload: str = "agent-replay") -> dict:
    return {
        "plan_id": f"gsx-{capsule['slug']}-{workload}",
        "status": "ready",
        "capsule": capsule["slug"],
        "target": target,
        "workload": workload,
        "success_metric": "A replayable workflow plan with source links, schema, outputs, and launch steps.",
        "steps": [
            "select one production workflow",
            "connect representative sources",
            "create typed graph schema",
            "run replay and invalidation",
            "ship the capsule workspace",
        ],
        "outputs": [
            "capsule manifest",
            "schema map",
            "trace replay",
            "benchmark slice",
            "ROI worksheet",
            "source ledger",
        ],
    }


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def clone_json(value):
    return json.loads(json.dumps(value))


def openontology_named_items() -> list[dict]:
    items: list[dict] = []
    for domain in OPEN_ONTOLOGY_DOMAINS:
        items.append({"kind": "domain", **domain, "domain_id": domain["id"], "id": f"oo.domain.{domain['id']}"})
    for value_type in OPEN_ONTOLOGY_VALUE_TYPES:
        items.append({"id": f"oo.value.{slugify(value_type)}", "kind": "value_type", "name": value_type})
    for prop in OPEN_ONTOLOGY_SHARED_PROPERTIES:
        items.append({"id": f"oo.property.{slugify(prop)}", "kind": "shared_property", "name": prop})
    items.extend(clone_json(OPEN_ONTOLOGY_INTERFACES))
    items.extend(clone_json(OPEN_ONTOLOGY_OBJECT_TYPES))
    items.extend(clone_json(OPEN_ONTOLOGY_LINK_TYPES))
    items.extend(clone_json(OPEN_ONTOLOGY_ACTION_TYPES))
    items.extend(clone_json(OPEN_ONTOLOGY_FUNCTIONS))
    for object_set in OPEN_ONTOLOGY_OBJECT_SETS:
        items.append({"id": f"oo.object_set.{slugify(object_set)}", "kind": "object_set", "name": object_set})
    for scenario in OPEN_ONTOLOGY_SCENARIOS:
        items.append({"id": f"oo.scenario.{slugify(scenario)}", "kind": "scenario", "name": scenario})
    return items


def openontology_manifest() -> dict:
    return {
        "id": "graphsub-openontology",
        "name": "GraphSub OpenOntology",
        "version": OPEN_ONTOLOGY_VERSION,
        "license": OPEN_ONTOLOGY_LICENSE,
        "revision": DEPLOYMENT_REVISION,
        "posture": "open operational ontology for real-world objects, actions, provenance, and agent runtime state",
        "source_note": "Modeled from public ontology design patterns: object types, link types, properties, actions, functions, interfaces, object sets, scenarios, permissions, provenance, and application workflows. It is not a copy of any proprietary ontology.",
        "declared_concepts": BASE_ONTOLOGY_CONCEPTS,
        "semantic_links": BASE_ONTOLOGY_LINKS,
        "kernel_types": BASE_KERNEL_TYPES,
        "materialized_seed": len(openontology_named_items()),
        "counts": {
            "domains": len(OPEN_ONTOLOGY_DOMAINS),
            "value_types": len(OPEN_ONTOLOGY_VALUE_TYPES),
            "shared_properties": len(OPEN_ONTOLOGY_SHARED_PROPERTIES),
            "interfaces": len(OPEN_ONTOLOGY_INTERFACES),
            "object_types": len(OPEN_ONTOLOGY_OBJECT_TYPES),
            "link_types": len(OPEN_ONTOLOGY_LINK_TYPES),
            "action_types": len(OPEN_ONTOLOGY_ACTION_TYPES),
            "functions": len(OPEN_ONTOLOGY_FUNCTIONS),
            "object_sets": len(OPEN_ONTOLOGY_OBJECT_SETS),
            "scenarios": len(OPEN_ONTOLOGY_SCENARIOS),
        },
        "resources": {
            "manifest": "/api/ontology",
            "concepts": "/api/ontology/concepts?limit=100&offset=0",
            "object_types": "/api/ontology/object-types",
            "link_types": "/api/ontology/link-types",
            "action_types": "/api/ontology/action-types",
            "functions": "/api/ontology/functions",
            "interfaces": "/api/ontology/interfaces",
            "search": "/api/ontology/search?q=asset",
        },
        "relationship_census": {
            "kernel": {"subtype_of": 256, "part_of": 41, "member_of": 38, "precedes": 21, "instance_of": 28},
            "lattice": {"semantic_link": BASE_ONTOLOGY_LINKS, "subtype_of": 60416, "part_of": 22784, "source_of": 8192},
        },
        "pagerank": BASE_PAGERANK_ROWS,
    }


def generated_openontology_concept(index: int) -> dict:
    seed = openontology_named_items()
    if index < len(seed):
        item = clone_json(seed[index])
        item["index"] = index
        item["generated"] = False
        return item
    ordinal = index - len(seed)
    domains = [domain["id"] for domain in OPEN_ONTOLOGY_DOMAINS]
    object_names = [obj["name"] for obj in OPEN_ONTOLOGY_OBJECT_TYPES]
    families = OPEN_ONTOLOGY_CONCEPT_FAMILIES
    states = ["Draft", "Active", "Archived", "Observed", "Approved", "Rejected", "Forecast", "Anomalous", "Linked", "Derived", "Governed", "Actionable"]
    domain = domains[ordinal % len(domains)]
    family = families[(ordinal // len(domains)) % len(families)]
    subject = object_names[(ordinal // (len(domains) * len(families))) % len(object_names)]
    state = states[(ordinal // (len(domains) * len(families) * len(object_names))) % len(states)]
    return {
        "id": f"oo.concept.{index:05d}",
        "index": index,
        "kind": "generated_concept",
        "domain": domain,
        "family": family,
        "name": f"{domain}.{subject}.{family}.{state}",
        "description": f"Generated OpenOntology concept for {domain} {family.lower()} modeling around {subject} in {state.lower()} state.",
        "generated": True,
    }


def openontology_concept_page(limit: int = 100, offset: int = 0, q: str | None = None) -> dict:
    limit = max(1, min(int(limit or 100), 1000))
    offset = max(0, min(int(offset or 0), BASE_ONTOLOGY_CONCEPTS - 1))
    if q:
        needle = q.lower()
        matches = []
        for index in range(BASE_ONTOLOGY_CONCEPTS):
            concept = generated_openontology_concept(index)
            haystack = json.dumps(concept, sort_keys=True).lower()
            if needle in haystack:
                matches.append(concept)
                if len(matches) >= limit:
                    break
        return {
            "revision": DEPLOYMENT_REVISION,
            "ontology": OPEN_ONTOLOGY_VERSION,
            "query": q,
            "count": len(matches),
            "limit": limit,
            "offset": 0,
            "concepts": matches,
        }
    end = min(BASE_ONTOLOGY_CONCEPTS, offset + limit)
    return {
        "revision": DEPLOYMENT_REVISION,
        "ontology": OPEN_ONTOLOGY_VERSION,
        "declared_total": BASE_ONTOLOGY_CONCEPTS,
        "limit": limit,
        "offset": offset,
        "next_offset": end if end < BASE_ONTOLOGY_CONCEPTS else None,
        "concepts": [generated_openontology_concept(index) for index in range(offset, end)],
    }


def openontology_search(q: str, limit: int = 25) -> dict:
    return openontology_concept_page(limit=limit, offset=0, q=q)


def template_for(template_id: str) -> dict:
    for template in CLOUD_TEMPLATES:
        if template["id"] == template_id:
            return template
    return CLOUD_TEMPLATES[0]


def instance_connection(instance: dict) -> dict:
    host = f"{instance['id']}.graphsub.cloud"
    return {
        "graphsub_uri": f"graphsub://{CLOUD_ORG['slug']}:gs_live_demo@{host}:443/{instance['name']}",
        "https": f"{SITE_URL}/api/cloud/instances/{instance['id']}/query",
        "graphql": f"{SITE_URL}/api/cloud/instances/{instance['id']}/graphql",
        "cypher": f"{SITE_URL}/api/cloud/instances/{instance['id']}/query",
        "headers": {"Authorization": "Bearer gs_live_demo"},
    }


def hydrate_instance(instance: dict) -> dict:
    live = clone_json(instance)
    if live["status"] == "running":
        tick = int(time.time()) % 997
        live["metrics"]["nodes"] += tick
        live["metrics"]["edges"] += tick * 5
        live["metrics"]["ops_per_sec"] = max(live["metrics"]["ops_per_sec"], 1200000) + (tick % 29) * 10000
        live["metrics"]["lookup_p99_us"] = max(55, live["metrics"]["lookup_p99_us"] + ((tick % 5) - 2))
    live["connection"] = instance_connection(live)
    return live


def instance_summary(instances: dict[str, dict]) -> dict:
    live = [hydrate_instance(instance) for instance in instances.values()]
    return {
        "count": len(live),
        "running": sum(1 for instance in live if instance["status"] == "running"),
        "sleeping": sum(1 for instance in live if instance["status"] == "sleeping"),
        "total_nodes": sum(instance["metrics"]["nodes"] for instance in live),
        "total_edges": sum(instance["metrics"]["edges"] for instance in live),
        "total_gsu_month": sum(instance["metrics"]["gsu_month"] for instance in live),
        "regions": sorted({instance["region"] for instance in live}),
    }


def create_instance_record(payload: dict, existing: dict[str, dict]) -> dict:
    raw_name = str(payload.get("name") or f"graphsub-{len(existing) + 1}").strip().lower()
    slug = re.sub(r"[^a-z0-9-]+", "-", raw_name).strip("-") or f"graphsub-{len(existing) + 1}"
    instance_id = f"gsx_{slug.replace('-', '_')}"
    suffix = 2
    while instance_id in existing:
        instance_id = f"gsx_{slug.replace('-', '_')}_{suffix}"
        suffix += 1
    template_id = str(payload.get("template") or "starter")
    template = template_for(template_id)
    region = str(payload.get("region") or CLOUD_ORG["primary_region"])
    now = utc_now()
    return {
        "id": instance_id,
        "name": slug,
        "org_id": CLOUD_ORG["id"],
        "owner_id": CLOUD_USER["id"],
        "region": region,
        "template": template["id"],
        "status": "running",
        "health": "provisioning",
        "version": "v0.8.1",
        "endpoint": f"https://{slug}.graphsub.cloud",
        "created_at": now,
        "updated_at": now,
        "metrics": {
            "nodes": BASE_ONTOLOGY_CONCEPTS,
            "edges": BASE_ONTOLOGY_LINKS,
            "concepts": BASE_ONTOLOGY_CONCEPTS,
            "lookup_p99_us": 78,
            "traversal_p99_us": 164,
            "ops_per_sec": 1200000 * max(1, template["replicas"]),
            "storage_gb": 2.4,
            "gsu_month": 0,
        },
        "features": ["cypher", "graphql", "vector-search", "replay", "hierarchical-memory", "capsules"],
    }


def query_instance(instance: dict, query: str) -> dict:
    normalized = re.sub(r"\s+", " ", (query or "").strip()).lower()
    metrics = hydrate_instance(instance)["metrics"]
    if not normalized:
        return {"columns": ["error"], "rows": [["empty query"]], "ok": False}
    compact = normalized.replace(" ", "")
    if "count" in normalized and ("objecttype" in compact or "object type" in normalized):
        return {"columns": ["object_types"], "rows": [[len(OPEN_ONTOLOGY_OBJECT_TYPES)]], "ok": True}
    if "count" in normalized and ("linktype" in compact or "link type" in normalized):
        return {"columns": ["link_types"], "rows": [[len(OPEN_ONTOLOGY_LINK_TYPES)]], "ok": True}
    if "count" in normalized and ("actiontype" in compact or "action type" in normalized):
        return {"columns": ["action_types"], "rows": [[len(OPEN_ONTOLOGY_ACTION_TYPES)]], "ok": True}
    if "openontology" in normalized or "open ontology" in normalized or "ontology" in normalized:
        if "object" in normalized and "type" in normalized and "count" in normalized:
            return {"columns": ["object_types"], "rows": [[len(OPEN_ONTOLOGY_OBJECT_TYPES)]], "ok": True}
        if "link" in normalized and "type" in normalized and "count" in normalized:
            return {"columns": ["link_types"], "rows": [[len(OPEN_ONTOLOGY_LINK_TYPES)]], "ok": True}
        if "action" in normalized and "type" in normalized and "count" in normalized:
            return {"columns": ["action_types"], "rows": [[len(OPEN_ONTOLOGY_ACTION_TYPES)]], "ok": True}
        if "function" in normalized and "count" in normalized:
            return {"columns": ["functions"], "rows": [[len(OPEN_ONTOLOGY_FUNCTIONS)]], "ok": True}
        if "count" in normalized:
            return {"columns": ["concepts"], "rows": [[BASE_ONTOLOGY_CONCEPTS]], "ok": True}
        return {
            "columns": ["ontology", "version", "concepts", "object_types", "link_types", "action_types"],
            "rows": [[
                "GraphSub OpenOntology",
                OPEN_ONTOLOGY_VERSION,
                BASE_ONTOLOGY_CONCEPTS,
                len(OPEN_ONTOLOGY_OBJECT_TYPES),
                len(OPEN_ONTOLOGY_LINK_TYPES),
                len(OPEN_ONTOLOGY_ACTION_TYPES),
            ]],
            "ok": True,
        }
    if "count" in normalized and "concept" in normalized:
        return {"columns": ["concepts"], "rows": [[metrics["concepts"]]], "ok": True}
    if "count" in normalized and "node" in normalized:
        return {"columns": ["nodes"], "rows": [[metrics["nodes"]]], "ok": True}
    if "pagerank" in normalized:
        return {
            "columns": ["node", "score"],
            "rows": BASE_PAGERANK_ROWS,
            "ok": True,
        }
    if "match" in normalized:
        return {
            "columns": ["path", "latency_us", "instance"],
            "rows": [["source->memory->approval->output", metrics["lookup_p99_us"], instance["name"]]],
            "ok": True,
        }
    return {
        "columns": ["accepted", "instance", "latency_us"],
        "rows": [[True, instance["name"], metrics["lookup_p99_us"]]],
        "ok": True,
    }


def render_cloud_landing() -> str:
    return """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>GraphSub Cloud - Hosted graph instances for agent systems</title>
<meta name="description" content="GraphSub Cloud gives every team hosted graph instances for agent memory, replay, operational proof, and real-world capsules.">
<link rel="canonical" href="https://graphsub.com/">
<meta name="robots" content="index,follow,max-image-preview:large">
<style>
:root{color-scheme:dark;--bg:#05070a;--panel:#0b1117;--panel2:#101922;--text:#eff6ff;--muted:#91a4b7;--line:#243244;--accent:#7dd3fc;--green:#34d399;--amber:#fbbf24;--violet:#a78bfa;--shadow:0 24px 80px rgba(0,0,0,.38);--sans:-apple-system,BlinkMacSystemFont,"SF Pro Display","Segoe UI",Inter,Roboto,Arial,sans-serif;--mono:"SF Mono","Cascadia Code","Roboto Mono",Menlo,Consolas,monospace}
*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 20% 0,rgba(125,211,252,.18),transparent 34rem),radial-gradient(circle at 82% 12%,rgba(167,139,250,.16),transparent 30rem),linear-gradient(180deg,#080b10,#05070a 38%,#040506);color:var(--text);font:500 16px/1.6 var(--sans);letter-spacing:0}a{color:inherit;text-decoration:none}code,pre{font-family:var(--mono)}
.wrap{max-width:1180px;margin:0 auto;padding:0 24px}.nav{position:sticky;top:0;z-index:10;background:rgba(5,7,10,.72);backdrop-filter:blur(16px);border-bottom:1px solid rgba(255,255,255,.08)}.nav .wrap{height:68px;display:flex;align-items:center;gap:28px}.brand{font-weight:800;letter-spacing:.12em}.brand span{display:inline-block;width:12px;height:12px;margin-right:9px;background:linear-gradient(135deg,var(--accent),var(--green));transform:rotate(45deg);box-shadow:0 0 22px rgba(125,211,252,.5)}.links{display:flex;gap:20px;color:var(--muted);font-size:14px}.links a:hover{color:var(--text)}.spacer{flex:1}.btn{display:inline-flex;align-items:center;justify-content:center;gap:8px;border:1px solid rgba(255,255,255,.12);background:rgba(255,255,255,.04);color:var(--text);border-radius:10px;padding:10px 14px;font-weight:700}.btn.primary{background:linear-gradient(135deg,var(--accent),#60a5fa);color:#03101b;border:0;box-shadow:0 18px 50px rgba(96,165,250,.28)}
.hero{padding:86px 0 48px}.hero-grid{display:grid;grid-template-columns:minmax(0,1.02fr) minmax(360px,.98fr);gap:34px;align-items:center}.eyebrow{display:inline-flex;color:var(--accent);border:1px solid rgba(125,211,252,.24);background:rgba(125,211,252,.08);border-radius:999px;padding:6px 10px;font:700 12px/1 var(--mono);margin-bottom:18px}.h1{font-size:64px;line-height:.98;margin:0 0 18px;letter-spacing:-.03em}.lead{font-size:20px;color:#c7d7e6;max-width:680px;margin:0 0 26px}.actions{display:flex;gap:12px;flex-wrap:wrap}.metrics{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;margin-top:34px}.metric{border:1px solid rgba(255,255,255,.1);background:rgba(255,255,255,.04);border-radius:14px;padding:16px}.metric b{display:block;font-size:28px;letter-spacing:-.02em}.metric span{color:var(--muted);font-size:13px}.product{border:1px solid rgba(255,255,255,.12);background:linear-gradient(180deg,rgba(255,255,255,.08),rgba(255,255,255,.035));border-radius:20px;box-shadow:var(--shadow);overflow:hidden}.chrome{height:44px;display:flex;align-items:center;gap:8px;border-bottom:1px solid rgba(255,255,255,.08);padding:0 16px}.dot{width:10px;height:10px;border-radius:50%;background:#ef4444}.dot:nth-child(2){background:#f59e0b}.dot:nth-child(3){background:#22c55e}.screen{padding:20px}.topline{display:flex;justify-content:space-between;color:var(--muted);font:12px var(--mono);margin-bottom:16px}.instances{display:grid;gap:10px}.inst{display:grid;grid-template-columns:1fr auto;gap:8px;border:1px solid rgba(255,255,255,.1);background:#081018;border-radius:12px;padding:14px}.inst b{display:block}.inst span{color:var(--muted);font:12px var(--mono)}.pill{border-radius:999px;padding:4px 8px;background:rgba(52,211,153,.12);color:var(--green);font:700 12px var(--mono)}.code{margin-top:14px;background:#03070b;border:1px solid rgba(255,255,255,.08);border-radius:12px;padding:16px;color:#d6e8f8;font:13px/1.7 var(--mono);overflow:auto}
.section{padding:54px 0}.section h2{font-size:34px;letter-spacing:-.02em;margin:0 0 10px}.sub{color:var(--muted);margin:0 0 22px}.cards{display:grid;grid-template-columns:repeat(3,1fr);gap:14px}.card{border:1px solid rgba(255,255,255,.1);background:rgba(255,255,255,.04);border-radius:16px;padding:20px}.card h3{margin:0 0 8px}.card p{color:var(--muted);margin:0}.table{border:1px solid rgba(255,255,255,.1);border-radius:16px;overflow:hidden}.row{display:grid;grid-template-columns:1fr 1fr 1.3fr;border-bottom:1px solid rgba(255,255,255,.08)}.row:last-child{border-bottom:0}.row div{padding:14px}.head{background:rgba(255,255,255,.05);color:#cbd5e1;font:700 12px var(--mono);text-transform:uppercase}.foot{padding:50px 0 70px;color:var(--muted);border-top:1px solid rgba(255,255,255,.08);margin-top:50px}
@media(max-width:860px){.links{display:none}.hero{padding:48px 0 30px}.hero-grid{grid-template-columns:1fr}.h1{font-size:42px}.lead{font-size:18px}.metrics,.cards{grid-template-columns:1fr}.row{grid-template-columns:1fr}.nav .wrap{height:62px}.product{border-radius:16px}}
</style>
</head>
<body>
<nav class="nav"><div class="wrap"><a class="brand" href="/"><span></span>GRAPHSUB</a><div class="links"><a href="/console">Console</a><a href="/api/cloud">API</a><a href="/wiki">Wiki</a><a href="/docs">Docs</a></div><div class="spacer"></div><a class="btn" href="/api/cloud">API</a><a class="btn primary" href="/console">Open console</a></div></nav>
<main>
<section class="hero"><div class="wrap hero-grid"><div><div class="eyebrow">GraphSub Cloud control plane</div><h1 class="h1">Hosted graph instances for agent systems.</h1><p class="lead">Create GraphSub instances per user and org, connect agents through Cypher/GraphQL/REST, and keep memory, replay, usage, and operational proof in one cloud surface.</p><div class="actions"><a class="btn primary" href="/console">Launch an instance</a><a class="btn" href="/api/cloud/instances">View instance API</a></div><div class="metrics"><div class="metric"><b>3</b><span>live instances</span></div><div class="metric"><b>65,536</b><span>base concepts per instance</span></div><div class="metric"><b>67us</b><span>P99 lookup lane</span></div></div></div><div class="product"><div class="chrome"><i class="dot"></i><i class="dot"></i><i class="dot"></i></div><div class="screen"><div class="topline"><span>org/dsco</span><span>us-east-1</span></div><div class="instances"><div class="inst"><div><b>prod-agents</b><span>pro-4 · 824k nodes · 24.7M ops/s</span></div><em class="pill">running</em></div><div class="inst"><div><b>replay-lab</b><span>starter · capsule replay workspace</span></div><em class="pill">running</em></div><div class="inst"><div><b>market-intel</b><span>starter · source-linked research graph</span></div><em class="pill">sleeping</em></div></div><pre class="code">curl https://graphsub.com/api/cloud/instances
curl -X POST https://graphsub.com/api/cloud/instances \\
  -d '{"name":"risk-prod","template":"pro-4"}'</pre></div></div></div></section>
<section class="section"><div class="wrap"><h2>What ships in the backend</h2><p class="sub">A cloud product surface, not only a marketing page.</p><div class="cards"><article class="card"><h3>Users and orgs</h3><p>Session, current user, org membership, plan, spend limits, and billing state are explicit API resources.</p></article><article class="card"><h3>GraphSub instances</h3><p>List, create, inspect, start, stop, restart, scale, connect, and query hosted instances by ID.</p></article><article class="card"><h3>Usage and events</h3><p>Per-org GSU usage, node/edge counts, audit log, connection strings, and SSE-style instance events.</p></article></div></div></section>
<section class="section"><div class="wrap"><h2>Competitive posture</h2><p class="sub">GraphSub Cloud is the control plane above graph storage: memory, replay, agents, and real-world capsules.</p><div class="table"><div class="row head"><div>Surface</div><div>Typical graph DB</div><div>GraphSub Cloud</div></div><div class="row"><div>Provisioning</div><div>Database first</div><div>User/org instances with cloud lifecycle APIs</div></div><div class="row"><div>Agent workloads</div><div>App code owns traces</div><div>Memory, replay, approvals, and proof are native resources</div></div><div class="row"><div>Go-to-market</div><div>Platform sale</div><div>Hosted capsules that expand into platform usage</div></div></div></div></section>
</main><footer class="wrap foot">revision __REVISION__ · <a href="/console">console</a> · <a href="/api/cloud">cloud api</a> · <a href="/wiki">wiki</a></footer>
</body></html>""".replace("__REVISION__", html.escape(DEPLOYMENT_REVISION))


def render_cloud_console(session: dict, instances: list[dict]) -> str:
    boot = html.escape(json.dumps({"session": session, "instances": instances, "revision": DEPLOYMENT_REVISION}), quote=False)
    return """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>GraphSub Cloud Console</title>
<meta name="robots" content="noindex,nofollow">
<style>
:root{color-scheme:dark;--bg:#06090d;--panel:#0d131b;--panel2:#111b25;--text:#eef6ff;--muted:#8da2b5;--line:#253447;--blue:#7dd3fc;--green:#34d399;--red:#fb7185;--amber:#fbbf24;--sans:-apple-system,BlinkMacSystemFont,"SF Pro Display","Segoe UI",Inter,Roboto,Arial,sans-serif;--mono:"SF Mono","Cascadia Code","Roboto Mono",Menlo,Consolas,monospace}*{box-sizing:border-box}body{margin:0;background:linear-gradient(180deg,#08111a,#06090d);color:var(--text);font:500 14px/1.55 var(--sans)}button,input,select,textarea{font:inherit}button{cursor:pointer}.app{display:grid;grid-template-columns:260px minmax(0,1fr);min-height:100dvh}.side{border-right:1px solid var(--line);background:#070b10;padding:22px}.brand{font-weight:850;letter-spacing:.12em;margin-bottom:24px}.nav a{display:block;color:var(--muted);padding:8px 0;text-decoration:none}.nav a.active,.nav a:hover{color:var(--text)}.main{min-width:0}.top{height:66px;border-bottom:1px solid var(--line);display:flex;align-items:center;gap:14px;padding:0 24px;background:rgba(13,19,27,.72);backdrop-filter:blur(16px);position:sticky;top:0;z-index:3}.top h1{font-size:18px;margin:0}.grow{flex:1}.user{color:var(--muted);font:12px var(--mono)}.content{padding:24px;display:grid;gap:18px}.grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:14px}.card{border:1px solid var(--line);background:var(--panel);border-radius:14px;padding:16px}.card b{font-size:24px;display:block}.card span{color:var(--muted)}.panel{border:1px solid var(--line);background:var(--panel);border-radius:16px;overflow:hidden}.panel-head{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:16px 18px;border-bottom:1px solid var(--line)}.panel-head h2{font-size:16px;margin:0}.btn{border:1px solid var(--line);border-radius:10px;padding:8px 11px;background:#0a1118;color:var(--text)}.btn.primary{background:linear-gradient(135deg,var(--blue),#60a5fa);color:#04111d;border:0;font-weight:800}.instances{display:grid}.instance{display:grid;grid-template-columns:1.1fr .8fr .8fr .8fr auto;gap:12px;align-items:center;padding:14px 18px;border-bottom:1px solid rgba(255,255,255,.06)}.instance:last-child{border-bottom:0}.muted{color:var(--muted)}.mono{font-family:var(--mono)}.status{display:inline-flex;border-radius:999px;padding:4px 8px;background:rgba(52,211,153,.12);color:var(--green);font:700 12px var(--mono)}.status.sleeping{background:rgba(251,191,36,.12);color:var(--amber)}.actions{display:flex;gap:8px}.split{display:grid;grid-template-columns:1fr 1fr;gap:18px}.code{background:#04080d;border:1px solid var(--line);border-radius:12px;padding:14px;min-height:184px;color:#d8eafd;font:12px/1.65 var(--mono);white-space:pre-wrap;overflow:auto}.form{display:grid;grid-template-columns:1fr 150px 150px auto;gap:10px}.form input,.form select{background:#080d13;color:var(--text);border:1px solid var(--line);border-radius:10px;padding:10px}.events{max-height:250px;overflow:auto}.event{display:grid;grid-template-columns:120px 1fr;gap:12px;border-bottom:1px solid rgba(255,255,255,.06);padding:10px 14px}.event:last-child{border-bottom:0}@media(max-width:980px){.app{grid-template-columns:1fr}.side{display:none}.grid{grid-template-columns:1fr 1fr}.instance{grid-template-columns:1fr}.split{grid-template-columns:1fr}.form{grid-template-columns:1fr}.content{padding:16px}}@media(max-width:560px){.grid{grid-template-columns:1fr}.top{padding:0 16px}.user{display:none}}
</style>
</head>
<body>
<div class="app"><aside class="side"><div class="brand">GRAPHSUB CLOUD</div><nav class="nav"><a class="active" href="/console">Instances</a><a href="/api/cloud/session">Session API</a><a href="/api/cloud/usage">Usage</a><a href="/docs">OpenAPI</a><a href="/">Public site</a></nav></aside><main class="main"><header class="top"><h1>Cloud console</h1><span class="muted mono" id="rev"></span><div class="grow"></div><span class="user" id="user"></span></header><section class="content"><div class="grid"><div class="card"><b id="metricInstances">-</b><span>instances</span></div><div class="card"><b id="metricRunning">-</b><span>running</span></div><div class="card"><b id="metricNodes">-</b><span>nodes</span></div><div class="card"><b id="metricGsu">-</b><span>GSU this month</span></div></div><div class="panel"><div class="panel-head"><h2>GraphSub instances</h2><form class="form" id="createForm"><input name="name" placeholder="instance name" value="risk-prod"><select name="template"><option value="starter">Starter</option><option value="pro-4">Pro 4</option><option value="scale-16">Scale 16</option></select><select name="region"><option>us-east-1</option><option>us-west-2</option><option>eu-west-1</option></select><button class="btn primary">Create</button></form></div><div class="instances" id="instances"></div></div><div class="split"><div class="panel"><div class="panel-head"><h2>Connection</h2><button class="btn" id="refresh">Refresh</button></div><pre class="code" id="connection"></pre></div><div class="panel"><div class="panel-head"><h2>Query result</h2><button class="btn" id="runQuery">Run concept count</button></div><pre class="code" id="query"></pre></div></div><div class="panel"><div class="panel-head"><h2>Audit log</h2><span class="muted mono">/api/cloud/audit-log</span></div><div class="events" id="events"></div></div></section></main></div>
<script id="boot" type="application/json">__BOOT__</script>
<script>
const boot = JSON.parse(document.getElementById('boot').textContent);
let state = boot;
const fmt = n => Number(n || 0).toLocaleString();
const $ = s => document.querySelector(s);
let selected = null;
function paint(){
  $('#rev').textContent = state.revision;
  $('#user').textContent = state.session.user.email + ' / ' + state.session.org.slug;
  const list = state.instances || [];
  selected = selected || (list[0] && list[0].id);
  $('#metricInstances').textContent = list.length;
  $('#metricRunning').textContent = list.filter(i=>i.status==='running').length;
  $('#metricNodes').textContent = fmt(list.reduce((a,i)=>a+i.metrics.nodes,0));
  $('#metricGsu').textContent = fmt(list.reduce((a,i)=>a+i.metrics.gsu_month,0));
  $('#instances').innerHTML = list.map(i => `<div class="instance"><div><b>${i.name}</b><div class="muted mono">${i.id} · ${i.endpoint}</div></div><div><span class="status ${i.status}">${i.status}</span></div><div><b>${fmt(i.metrics.nodes)}</b><div class="muted">nodes</div></div><div><b>${fmt(i.metrics.ops_per_sec)}</b><div class="muted">ops/s</div></div><div class="actions"><button class="btn" data-pick="${i.id}">Open</button><button class="btn" data-start="${i.id}">Start</button><button class="btn" data-stop="${i.id}">Stop</button></div></div>`).join('');
  for (const b of document.querySelectorAll('[data-pick]')) b.onclick = () => { selected = b.dataset.pick; showConnection(); };
  for (const b of document.querySelectorAll('[data-start]')) b.onclick = () => action(b.dataset.start, 'start');
  for (const b of document.querySelectorAll('[data-stop]')) b.onclick = () => action(b.dataset.stop, 'stop');
  showConnection();
}
async function reload(){
  const [session, instances] = await Promise.all([fetch('/api/cloud/session').then(r=>r.json()), fetch('/api/cloud/instances').then(r=>r.json())]);
  state.session = session.session; state.instances = instances.instances; paint();
  const audit = await fetch('/api/cloud/audit-log').then(r=>r.json());
  $('#events').innerHTML = audit.events.map(e => `<div class="event"><span class="mono muted">${e.at}</span><span>${e.action} <span class="muted">${e.target}</span></span></div>`).join('');
}
function current(){ return state.instances.find(i=>i.id===selected) || state.instances[0]; }
async function showConnection(){
  const inst = current(); if(!inst){ $('#connection').textContent = 'No instance'; return; }
  const data = await fetch(`/api/cloud/instances/${inst.id}/connect`).then(r=>r.json());
  $('#connection').textContent = JSON.stringify(data.connection, null, 2);
}
async function action(id, op){ await fetch(`/api/cloud/instances/${id}/${op}`, {method:'POST'}); await reload(); }
$('#refresh').onclick = reload;
$('#runQuery').onclick = async () => {
  const inst = current();
  const r = await fetch(`/api/cloud/instances/${inst.id}/query`, {method:'POST', headers:{'content-type':'application/json'}, body:JSON.stringify({query:'MATCH (n:Concept) RETURN count(n) AS concepts'})}).then(r=>r.json());
  $('#query').textContent = JSON.stringify(r, null, 2);
};
$('#createForm').onsubmit = async e => {
  e.preventDefault();
  const body = Object.fromEntries(new FormData(e.target).entries());
  const created = await fetch('/api/cloud/instances', {method:'POST', headers:{'content-type':'application/json'}, body:JSON.stringify(body)}).then(r=>r.json());
  selected = created.instance.id;
  await reload();
};
paint(); reload();
</script>
</body></html>""".replace("__BOOT__", boot)


def render_cloud_landing_v2(session: dict, instances: list[dict]) -> str:
    summary = {
        "instances": len(instances),
        "running": sum(1 for item in instances if item["status"] == "running"),
        "nodes": sum(item["metrics"]["nodes"] for item in instances),
        "gsu": sum(item["metrics"]["gsu_month"] for item in instances),
    }
    instance_rows = "\n".join(
        f"""<article class=\"instance-card\">
  <div><strong>{html.escape(item['name'])}</strong><span>{html.escape(item['template'])} / {html.escape(item['region'])}</span></div>
  <em class=\"state {html.escape(item['status'])}\">{html.escape(item['status'])}</em>
  <dl><div><dt>nodes</dt><dd>{item['metrics']['nodes']:,}</dd></div><div><dt>p99</dt><dd>{item['metrics']['lookup_p99_us']}us</dd></div><div><dt>ops/s</dt><dd>{item['metrics']['ops_per_sec']:,}</dd></div></dl>
</article>"""
        for item in instances[:4]
    )
    return f"""<!doctype html>
<html lang=\"en\">
<head>
<meta charset=\"utf-8\">
<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, viewport-fit=cover\">
<title>GraphSub Cloud - Managed graph infrastructure for agent systems</title>
<meta name=\"description\" content=\"GraphSub Cloud provisions hosted graph instances for agent memory, replay, operational proof, GraphRAG, and real-world capsules.\">
<link rel=\"canonical\" href=\"https://graphsub.com/\">
<meta name=\"robots\" content=\"index,follow,max-image-preview:large\">
<style>
:root{{--bg:#f7f8fb;--surface:#fff;--ink:#0b1220;--muted:#5e6b7a;--line:#dfe5ee;--soft:#eef3f8;--brand:#0f5fff;--brand2:#05a3ff;--ok:#058a55;--warn:#a16207;--shadow:0 24px 70px rgba(15,27,45,.10);--sans:-apple-system,BlinkMacSystemFont,\"SF Pro Display\",\"Segoe UI\",Inter,Roboto,Arial,sans-serif;--mono:\"SF Mono\",\"Cascadia Code\",\"Roboto Mono\",Menlo,Consolas,monospace}}
*{{box-sizing:border-box}}body{{margin:0;background:linear-gradient(180deg,#fff,var(--bg));color:var(--ink);font:500 16px/1.55 var(--sans)}}a{{color:inherit;text-decoration:none}}.wrap{{max-width:1180px;margin:auto;padding:0 24px}}.nav{{position:sticky;top:0;z-index:20;background:rgba(255,255,255,.86);backdrop-filter:blur(18px);border-bottom:1px solid var(--line)}}.navin{{height:68px;display:flex;align-items:center;gap:26px}}.brand{{display:flex;align-items:center;gap:10px;font-weight:850;letter-spacing:-.02em}}.mark{{width:24px;height:24px;border-radius:7px;background:linear-gradient(135deg,var(--brand),var(--brand2));box-shadow:0 10px 30px rgba(15,95,255,.22)}}.links{{display:flex;gap:20px;color:var(--muted);font-size:14px}}.links a:hover{{color:var(--ink)}}.grow{{flex:1}}.btn{{display:inline-flex;align-items:center;justify-content:center;border:1px solid var(--line);background:#fff;border-radius:10px;padding:10px 14px;font-weight:700;box-shadow:0 1px 0 rgba(12,20,30,.04)}}.btn.primary{{background:var(--ink);color:#fff;border-color:var(--ink)}}.hero{{padding:84px 0 54px}}.hero-grid{{display:grid;grid-template-columns:minmax(0,1fr) minmax(420px,.88fr);gap:44px;align-items:center}}.eyebrow{{display:inline-flex;margin-bottom:16px;border:1px solid #b8d7ff;background:#eef6ff;color:#0b63ce;border-radius:999px;padding:6px 10px;font:750 12px/1 var(--mono)}}h1{{font-size:70px;line-height:.96;letter-spacing:-.055em;margin:0 0 18px;max-width:760px}}.lead{{font-size:21px;line-height:1.52;color:#405064;margin:0 0 28px;max-width:700px}}.actions{{display:flex;gap:12px;flex-wrap:wrap}}.proof{{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin-top:34px}}.proof div{{background:var(--surface);border:1px solid var(--line);border-radius:14px;padding:15px;box-shadow:0 8px 24px rgba(15,27,45,.04)}}.proof b{{display:block;font-size:26px;letter-spacing:-.03em}}.proof span{{color:var(--muted);font-size:13px}}.console{{background:#0b111b;color:#eaf2ff;border-radius:22px;box-shadow:var(--shadow);overflow:hidden;border:1px solid #182538}}.console-top{{height:48px;display:flex;align-items:center;gap:8px;padding:0 18px;border-bottom:1px solid #1e2d42;color:#8fa2b8;font:12px var(--mono)}}.dot{{width:10px;height:10px;border-radius:50%;background:#ff5f57}}.dot:nth-child(2){{background:#ffbd2e}}.dot:nth-child(3){{background:#28c840}}.console-body{{padding:18px;display:grid;gap:12px}}.instance-card{{background:#111a27;border:1px solid #243348;border-radius:14px;padding:14px;display:grid;grid-template-columns:1fr auto;gap:12px}}.instance-card strong{{display:block;color:#fff}}.instance-card span{{color:#93a4b8;font-size:13px}}.state{{font:760 12px var(--mono);border-radius:999px;padding:5px 9px;background:rgba(5,138,85,.14);color:#5ee0a8;height:max-content}}.state.sleeping{{background:rgba(251,191,36,.14);color:#f6c967}}dl{{grid-column:1/-1;display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin:0}}dt{{color:#7f91a7;font-size:11px;text-transform:uppercase}}dd{{margin:0;color:#fff;font:700 13px var(--mono)}}.code{{margin:0;background:#050910;border:1px solid #233247;border-radius:14px;padding:14px;color:#dbeafe;font:12.5px/1.7 var(--mono);overflow:auto}}.section{{padding:62px 0}}.section h2{{font-size:38px;letter-spacing:-.035em;margin:0 0 10px}}.sub{{color:var(--muted);margin:0 0 24px;font-size:18px}}.grid3{{display:grid;grid-template-columns:repeat(3,1fr);gap:16px}}.card{{background:#fff;border:1px solid var(--line);border-radius:18px;padding:22px;box-shadow:0 10px 28px rgba(15,27,45,.04)}}.card h3{{margin:0 0 8px;font-size:19px}}.card p{{margin:0;color:var(--muted)}}.api{{display:grid;grid-template-columns:.9fr 1.1fr;gap:18px;align-items:start}}.endpoint{{display:grid;grid-template-columns:90px 1fr;border-bottom:1px solid var(--line);padding:13px 0;font:14px var(--mono)}}.method{{color:var(--brand);font-weight:800}}.foot{{border-top:1px solid var(--line);margin-top:40px;padding:34px 0 54px;color:var(--muted);font-size:14px}}
@media(max-width:920px){{.links{{display:none}}.hero{{padding:52px 0 36px}}.hero-grid,.api{{grid-template-columns:1fr}}h1{{font-size:44px}}.lead{{font-size:18px}}.proof,.grid3{{grid-template-columns:1fr}}.console{{border-radius:16px}}}}
</style>
</head>
<body>
<nav class=\"nav\"><div class=\"wrap navin\"><a class=\"brand\" href=\"/\"><span class=\"mark\"></span>GraphSub Cloud</a><div class=\"links\"><a href=\"/console\">Console</a><a href=\"/api/cloud\">API</a><a href=\"/wiki\">Wiki</a><a href=\"/docs\">Docs</a></div><div class=\"grow\"></div><a class=\"btn\" href=\"/api/cloud\">API</a><a class=\"btn primary\" href=\"/console\">Open console</a></div></nav>
<main>
<section class=\"hero\"><div class=\"wrap hero-grid\"><div><span class=\"eyebrow\">Modal-backed cloud control plane</span><h1>Managed graph instances for agent systems.</h1><p class=\"lead\">GraphSub Cloud gives each user and organization provisioned graph instances with query endpoints, lifecycle controls, usage metering, audit events, and the primitives agents need: memory, replay, proof, and capsules.</p><div class=\"actions\"><a class=\"btn primary\" href=\"/console\">Launch instance</a><a class=\"btn\" href=\"/api/cloud/instances\">Inspect API</a><a class=\"btn\" href=\"/wiki/graph-database-for-agents\">Read the model</a></div><div class=\"proof\"><div><b>{summary['instances']}</b><span>instances</span></div><div><b>{summary['running']}</b><span>running</span></div><div><b>{summary['nodes']:,}</b><span>nodes</span></div><div><b>{summary['gsu']:,}</b><span>GSU used</span></div></div></div><aside class=\"console\"><div class=\"console-top\"><i class=\"dot\"></i><i class=\"dot\"></i><i class=\"dot\"></i><span>org/{html.escape(session['org']['slug'])} / {html.escape(session['org']['primary_region'])}</span></div><div class=\"console-body\">{instance_rows}<pre class=\"code\">POST /api/cloud/instances
{{\"name\":\"risk-prod\",\"template\":\"pro-4\",\"region\":\"us-east-1\"}}

POST /api/cloud/instances/gsx_prod_agents/query
{{\"query\":\"MATCH (n:Concept) RETURN count(n)\"}}</pre></div></aside></div></section>
<section class=\"section\"><div class=\"wrap\"><h2>Cloud primitives, not a demo skin.</h2><p class=\"sub\">The public site now maps directly to live backend resources.</p><div class=\"grid3\"><article class=\"card\"><h3>User and org model</h3><p>Session, current user, organization, membership, plan, spend limit, regions, templates, and billing state are first-class JSON resources.</p></article><article class=\"card\"><h3>Instance lifecycle</h3><p>Provision, inspect, start, stop, restart, scale, connect, query, and stream instance events through `/api/cloud/instances`.</p></article><article class=\"card\"><h3>Agent substrate</h3><p>Every instance boots with the 65,536-concept global ontology plus memory, replay, source ledger, vector search, and capsule surfaces.</p></article></div></div></section>
<section class=\"section\"><div class=\"wrap api\"><div><h2>API-first by default.</h2><p class=\"sub\">The console is only one client. The control plane is usable from curl, SDKs, agents, or your own product.</p></div><div class=\"card\"><div class=\"endpoint\"><span class=\"method\">GET</span><span>/api/cloud/session</span></div><div class=\"endpoint\"><span class=\"method\">GET</span><span>/api/cloud/instances</span></div><div class=\"endpoint\"><span class=\"method\">POST</span><span>/api/cloud/instances</span></div><div class=\"endpoint\"><span class=\"method\">POST</span><span>/api/cloud/instances/{{id}}/query</span></div><div class=\"endpoint\"><span class=\"method\">GET</span><span>/api/cloud/usage</span></div></div></div></section>
</main><footer class=\"wrap foot\">{html.escape(DEPLOYMENT_REVISION)} / <a href=\"/console\">console</a> / <a href=\"/api/cloud\">cloud api</a> / <a href=\"/wiki\">wiki</a></footer>
</body></html>"""


def render_cloud_console_v2(session: dict, instances: list[dict]) -> str:
    boot = html.escape(json.dumps({"session": session, "instances": instances, "revision": DEPLOYMENT_REVISION}), quote=False)
    return f"""<!doctype html>
<html lang=\"en\">
<head>
<meta charset=\"utf-8\">
<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, viewport-fit=cover\">
<title>GraphSub Cloud Console</title>
<meta name=\"robots\" content=\"noindex,nofollow\">
<style>
:root{{--bg:#f5f7fb;--surface:#fff;--ink:#0b1220;--muted:#64748b;--line:#dbe3ee;--blue:#0f5fff;--ok:#07845c;--warn:#9a6700;--shadow:0 18px 48px rgba(15,27,45,.08);--sans:-apple-system,BlinkMacSystemFont,\"SF Pro Display\",\"Segoe UI\",Inter,Roboto,Arial,sans-serif;--mono:\"SF Mono\",\"Cascadia Code\",\"Roboto Mono\",Menlo,Consolas,monospace}}
*{{box-sizing:border-box}}body{{margin:0;background:var(--bg);color:var(--ink);font:500 14px/1.5 var(--sans)}}button,input,select{{font:inherit}}button{{cursor:pointer}}a{{color:inherit;text-decoration:none}}.shell{{display:grid;grid-template-columns:248px minmax(0,1fr);min-height:100dvh}}.side{{background:#fff;border-right:1px solid var(--line);padding:22px 18px}}.brand{{font-weight:850;font-size:18px;margin-bottom:28px}}.brand span{{display:inline-block;width:20px;height:20px;border-radius:6px;background:linear-gradient(135deg,var(--blue),#05a3ff);vertical-align:-4px;margin-right:8px}}.nav a{{display:flex;justify-content:space-between;color:var(--muted);padding:9px 10px;border-radius:9px}}.nav a.active,.nav a:hover{{background:#eef4ff;color:#0c4ec0}}.main{{min-width:0}}.top{{height:64px;background:rgba(255,255,255,.86);backdrop-filter:blur(14px);border-bottom:1px solid var(--line);display:flex;align-items:center;gap:16px;padding:0 24px;position:sticky;top:0;z-index:5}}.top h1{{font-size:17px;margin:0}}.grow{{flex:1}}.account{{color:var(--muted);font:12px var(--mono)}}.content{{padding:22px;display:grid;gap:18px}}.kpis{{display:grid;grid-template-columns:repeat(4,1fr);gap:14px}}.kpi,.panel{{background:#fff;border:1px solid var(--line);border-radius:16px;box-shadow:0 8px 28px rgba(15,27,45,.035)}}.kpi{{padding:16px}}.kpi b{{font-size:28px;letter-spacing:-.03em;display:block}}.kpi span{{color:var(--muted)}}.panel-head{{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:16px 18px;border-bottom:1px solid var(--line)}}.panel-head h2{{font-size:16px;margin:0}}.btn{{border:1px solid var(--line);background:#fff;border-radius:10px;padding:8px 11px;font-weight:700}}.btn.primary{{background:var(--ink);color:#fff;border-color:var(--ink)}}.form{{display:grid;grid-template-columns:1fr 140px 140px auto;gap:9px}}.form input,.form select{{border:1px solid var(--line);border-radius:10px;padding:9px 10px;background:#fff}}.table{{display:grid}}.row{{display:grid;grid-template-columns:1.25fr .8fr .7fr .7fr .9fr auto;gap:12px;align-items:center;padding:13px 18px;border-bottom:1px solid #edf1f6}}.row:last-child{{border-bottom:0}}.head{{color:var(--muted);font:750 11px var(--mono);text-transform:uppercase;background:#fbfcfe}}.name b{{display:block}}.name span,.muted{{color:var(--muted)}}.mono{{font-family:var(--mono)}}.badge{{display:inline-flex;border-radius:999px;padding:4px 8px;background:#e9f9f2;color:var(--ok);font:750 12px var(--mono)}}.badge.sleeping{{background:#fff7df;color:var(--warn)}}.actions{{display:flex;gap:7px;flex-wrap:wrap}}.split{{display:grid;grid-template-columns:1fr 1fr;gap:18px}}pre{{margin:0;min-height:190px;background:#07111f;color:#dcecff;border-radius:0 0 16px 16px;padding:16px;font:12.5px/1.65 var(--mono);overflow:auto}}.events{{max-height:280px;overflow:auto}}.event{{display:grid;grid-template-columns:124px 1fr 1fr;padding:10px 18px;border-bottom:1px solid #edf1f6}}.event:last-child{{border-bottom:0}}@media(max-width:1040px){{.shell{{grid-template-columns:1fr}}.side{{display:none}}.kpis,.split{{grid-template-columns:1fr 1fr}}.row{{grid-template-columns:1fr}}.form{{grid-template-columns:1fr}}}}@media(max-width:640px){{.kpis,.split{{grid-template-columns:1fr}}.content{{padding:14px}}.account{{display:none}}}}
</style>
</head>
<body>
<div class=\"shell\"><aside class=\"side\"><div class=\"brand\"><span></span>GraphSub</div><nav class=\"nav\"><a class=\"active\" href=\"/console\">Instances</a><a href=\"/api/cloud/session\">Session</a><a href=\"/api/cloud/usage\">Usage</a><a href=\"/api/cloud/audit-log\">Audit</a><a href=\"/docs\">OpenAPI</a><a href=\"/\">Public site</a></nav></aside><main class=\"main\"><header class=\"top\"><h1>Cloud console</h1><span class=\"muted mono\" id=\"rev\"></span><div class=\"grow\"></div><span class=\"account\" id=\"account\"></span></header><section class=\"content\"><div class=\"kpis\"><div class=\"kpi\"><b id=\"kInstances\">-</b><span>instances</span></div><div class=\"kpi\"><b id=\"kRunning\">-</b><span>running</span></div><div class=\"kpi\"><b id=\"kNodes\">-</b><span>nodes</span></div><div class=\"kpi\"><b id=\"kGsu\">-</b><span>GSU month</span></div></div><section class=\"panel\"><div class=\"panel-head\"><h2>Instances</h2><form class=\"form\" id=\"create\"><input name=\"name\" value=\"risk-prod\" aria-label=\"Instance name\"><select name=\"template\"><option value=\"starter\">Starter</option><option value=\"pro-4\">Pro 4</option><option value=\"scale-16\">Scale 16</option></select><select name=\"region\"><option>us-east-1</option><option>us-west-2</option><option>eu-west-1</option></select><button class=\"btn primary\">Create</button></form></div><div class=\"table\" id=\"table\"></div></section><div class=\"split\"><section class=\"panel\"><div class=\"panel-head\"><h2>Connection</h2><button class=\"btn\" id=\"refresh\">Refresh</button></div><pre id=\"connection\"></pre></section><section class=\"panel\"><div class=\"panel-head\"><h2>Query</h2><button class=\"btn primary\" id=\"query\">Run count</button></div><pre id=\"result\"></pre></section></div><section class=\"panel\"><div class=\"panel-head\"><h2>Audit log</h2><span class=\"muted mono\">Modal Dict backed</span></div><div class=\"events\" id=\"events\"></div></section></section></main></div>
<script id=\"boot\" type=\"application/json\">{boot}</script>
<script>
const $ = s => document.querySelector(s);
const fmt = n => Number(n || 0).toLocaleString();
let state = JSON.parse($('#boot').textContent);
let selected = state.instances[0]?.id;
function current(){{return state.instances.find(i => i.id === selected) || state.instances[0];}}
function paint(){{
  $('#rev').textContent = state.revision;
  $('#account').textContent = `${{state.session.user.email}} / ${{state.session.org.slug}}`;
  const list = state.instances || [];
  $('#kInstances').textContent = list.length;
  $('#kRunning').textContent = list.filter(i => i.status === 'running').length;
  $('#kNodes').textContent = fmt(list.reduce((a,i)=>a+i.metrics.nodes,0));
  $('#kGsu').textContent = fmt(list.reduce((a,i)=>a+i.metrics.gsu_month,0));
  $('#table').innerHTML = `<div class=\"row head\"><div>Name</div><div>Status</div><div>Nodes</div><div>P99</div><div>Region</div><div></div></div>` + list.map(i => `<div class=\"row\"><div class=\"name\"><b>${{i.name}}</b><span>${{i.id}}</span></div><div><span class=\"badge ${{i.status}}\">${{i.status}}</span></div><div class=\"mono\">${{fmt(i.metrics.nodes)}}</div><div class=\"mono\">${{i.metrics.lookup_p99_us}}us</div><div>${{i.region}}</div><div class=\"actions\"><button class=\"btn\" data-open=\"${{i.id}}\">Open</button><button class=\"btn\" data-start=\"${{i.id}}\">Start</button><button class=\"btn\" data-stop=\"${{i.id}}\">Stop</button></div></div>`).join('');
  document.querySelectorAll('[data-open]').forEach(b => b.onclick = () => {{selected = b.dataset.open; showConnection();}});
  document.querySelectorAll('[data-start]').forEach(b => b.onclick = () => action(b.dataset.start, 'start'));
  document.querySelectorAll('[data-stop]').forEach(b => b.onclick = () => action(b.dataset.stop, 'stop'));
  showConnection();
}}
async function reload(){{
  const [session, instances, audit] = await Promise.all([fetch('/api/cloud/session').then(r=>r.json()), fetch('/api/cloud/instances').then(r=>r.json()), fetch('/api/cloud/audit-log').then(r=>r.json())]);
  state.session = session.session; state.instances = instances.instances; state.revision = instances.revision;
  $('#events').innerHTML = audit.events.map(e => `<div class=\"event\"><span class=\"mono muted\">${{e.at}}</span><span>${{e.action}}</span><span class=\"muted\">${{e.target}}</span></div>`).join('');
  paint();
}}
async function showConnection(){{
  const inst = current(); if (!inst) return;
  const data = await fetch(`/api/cloud/instances/${{inst.id}}/connect`).then(r=>r.json());
  $('#connection').textContent = JSON.stringify(data.connection, null, 2);
}}
async function action(id, op){{await fetch(`/api/cloud/instances/${{id}}/${{op}}`, {{method:'POST'}}); await reload();}}
$('#refresh').onclick = reload;
$('#query').onclick = async () => {{
  const inst = current();
  const data = await fetch(`/api/cloud/instances/${{inst.id}}/query`, {{method:'POST', headers:{{'content-type':'application/json'}}, body:JSON.stringify({{query:'MATCH (n:Concept) RETURN count(n) AS concepts'}})}}).then(r=>r.json());
  $('#result').textContent = JSON.stringify(data, null, 2);
}};
$('#create').onsubmit = async e => {{
  e.preventDefault();
  const body = Object.fromEntries(new FormData(e.target).entries());
  const data = await fetch('/api/cloud/instances', {{method:'POST', headers:{{'content-type':'application/json'}}, body:JSON.stringify(body)}}).then(r=>r.json());
  selected = data.instance.id;
  await reload();
}};
paint(); reload();
</script>
</body></html>"""


def render_cloud_landing_v3(session: dict, instances: list[dict]) -> str:
    summary = {
        "instances": len(instances),
        "running": sum(1 for item in instances if item["status"] == "running"),
        "nodes": sum(item["metrics"]["nodes"] for item in instances),
        "edges": sum(item["metrics"]["edges"] for item in instances),
        "gsu": sum(item["metrics"]["gsu_month"] for item in instances),
    }
    featured = instances[:4]
    instance_rows = "\n".join(
        f"""<article class="runtime-card">
  <div class="runtime-head"><div><strong>{html.escape(item['name'])}</strong><span>{html.escape(item['id'])}</span></div><em class="state {html.escape(item['status'])}">{html.escape(item['status'])}</em></div>
  <div class="runtime-meta">{html.escape(item['template'])} / {html.escape(item['region'])} / {html.escape(item['version'])}</div>
  <dl><div><dt>nodes</dt><dd>{item['metrics']['nodes']:,}</dd></div><div><dt>p99</dt><dd>{item['metrics']['lookup_p99_us']}us</dd></div><div><dt>ops/s</dt><dd>{item['metrics']['ops_per_sec']:,}</dd></div></dl>
</article>"""
        for item in featured
    )
    workload_cards = "\n".join(
        f"""<article class="workload"><span>0{idx}</span><h3>{html.escape(capsule['name'])}</h3><p>{html.escape(capsule['wedge'])}</p><b>{html.escape(capsule['status'])}</b></article>"""
        for idx, capsule in enumerate(CAPSULES[:4], 1)
    )
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>GraphSub Cloud - Graph runtime cloud for agent memory and proof</title>
<meta name="description" content="GraphSub Cloud provisions hosted graph runtimes for agent memory, replay, operational proof, GraphRAG, and real-world capsules.">
<link rel="canonical" href="https://graphsub.com/">
<meta name="robots" content="index,follow,max-image-preview:large">
<style>
:root{{--bg:#fbfcff;--ink:#07111f;--muted:#607086;--line:#dfe7f2;--panel:#fff;--soft:#f2f6fb;--blue:#145cff;--cyan:#00a6ff;--green:#078b5d;--amber:#a46400;--purple:#6d5dfc;--shadow:0 28px 80px rgba(17,31,52,.12);--sans:-apple-system,BlinkMacSystemFont,"SF Pro Display","Segoe UI",Inter,Roboto,Arial,sans-serif;--mono:"SF Mono","Cascadia Code","Roboto Mono",Menlo,Consolas,monospace}}
*{{box-sizing:border-box}}body{{margin:0;background:linear-gradient(180deg,#fff 0,#f8fbff 45%,#f3f7fc 100%);color:var(--ink);font:500 16px/1.55 var(--sans)}}a{{color:inherit;text-decoration:none}}.wrap{{max-width:1200px;margin:0 auto;padding:0 24px}}.nav{{position:sticky;top:0;z-index:20;background:rgba(255,255,255,.88);border-bottom:1px solid var(--line);backdrop-filter:blur(18px)}}.navin{{height:70px;display:flex;align-items:center;gap:28px}}.brand{{display:flex;align-items:center;gap:10px;font-weight:850;font-size:17px;letter-spacing:-.02em}}.mark{{width:26px;height:26px;border-radius:8px;background:conic-gradient(from 180deg,var(--cyan),var(--blue),var(--purple),var(--cyan));box-shadow:0 12px 32px rgba(20,92,255,.25)}}.links{{display:flex;gap:22px;color:var(--muted);font-weight:650;font-size:14px}}.links a:hover{{color:var(--ink)}}.grow{{flex:1}}.btn{{display:inline-flex;align-items:center;justify-content:center;border:1px solid var(--line);background:#fff;border-radius:11px;padding:10px 14px;font-weight:760;box-shadow:0 1px 0 rgba(17,31,52,.04)}}.btn.primary{{background:var(--ink);color:#fff;border-color:var(--ink)}}.hero{{padding:58px 0 42px}}.hero-grid{{display:grid;grid-template-columns:minmax(0,1.02fr) minmax(500px,.98fr);gap:34px;align-items:center}}.eyebrow{{display:inline-flex;align-items:center;gap:8px;border:1px solid #bed7ff;background:#eff6ff;color:#0c59d1;border-radius:999px;padding:7px 11px;font:800 12px/1 var(--mono);margin-bottom:18px}}.eyebrow:before{{content:"";width:7px;height:7px;border-radius:50%;background:var(--green);box-shadow:0 0 0 4px rgba(7,139,93,.12)}}h1{{font-size:68px;line-height:.94;letter-spacing:-.06em;margin:0 0 20px;max-width:790px}}.lead{{font-size:21px;line-height:1.55;color:#40516a;margin:0 0 26px;max-width:720px}}.actions{{display:flex;flex-wrap:wrap;gap:12px}}.proof{{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin-top:32px}}.proof div{{background:rgba(255,255,255,.86);border:1px solid var(--line);border-radius:15px;padding:15px;box-shadow:0 10px 26px rgba(17,31,52,.05)}}.proof b{{display:block;font-size:27px;line-height:1;letter-spacing:-.035em}}.proof span{{display:block;color:var(--muted);font-size:13px;margin-top:7px}}.product{{position:relative;background:#07111f;border-radius:24px;color:#dbeafe;box-shadow:var(--shadow);overflow:hidden;border:1px solid #17253a}}.product:before{{content:"";position:absolute;inset:0;background:radial-gradient(circle at 12% 0,rgba(20,92,255,.34),transparent 18rem),radial-gradient(circle at 92% 0,rgba(0,166,255,.22),transparent 20rem);pointer-events:none}}.product-top{{position:relative;height:52px;border-bottom:1px solid #1b2a40;display:flex;align-items:center;gap:10px;padding:0 18px;font:12px var(--mono);color:#8fa4bc}}.lights i{{display:inline-block;width:10px;height:10px;border-radius:50%;background:#ff5f57;margin-right:6px}}.lights i:nth-child(2){{background:#ffbd2e}}.lights i:nth-child(3){{background:#28c840}}.product-body{{position:relative;display:grid;grid-template-columns:1.05fr .95fr;gap:14px;padding:18px}}.runtime-stack{{display:grid;gap:10px}}.runtime-card{{background:rgba(17,28,43,.92);border:1px solid #263953;border-radius:15px;padding:14px}}.runtime-head{{display:flex;justify-content:space-between;gap:12px}}.runtime-head strong{{display:block;color:#fff;font-size:17px}}.runtime-head span,.runtime-meta{{color:#91a4bb;font:12px var(--mono)}}.state{{height:max-content;border-radius:999px;padding:5px 9px;background:rgba(7,139,93,.18);color:#59d9a7;font:800 12px var(--mono)}}.state.sleeping{{background:rgba(164,100,0,.2);color:#f7c873}}dl{{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin:12px 0 0}}dt{{font:800 10px var(--mono);color:#788ba3;text-transform:uppercase}}dd{{margin:2px 0 0;color:#fff;font:800 13px var(--mono)}}.control{{display:grid;gap:12px}}.mini-panel{{background:#0b1625;border:1px solid #263953;border-radius:15px;padding:14px}}.mini-panel h3{{margin:0 0 10px;color:#fff;font-size:14px}}.bars{{display:grid;gap:8px}}.bar{{display:grid;grid-template-columns:88px 1fr 44px;gap:8px;align-items:center;color:#8fa4bc;font:12px var(--mono)}}.track{{height:8px;border-radius:999px;background:#17243a;overflow:hidden}}.fill{{height:100%;background:linear-gradient(90deg,var(--blue),var(--cyan));border-radius:999px}}.code{{margin:0;background:#030811;border:1px solid #253852;border-radius:15px;padding:14px;color:#dbeafe;font:12px/1.65 var(--mono);min-height:164px;overflow:auto}}.section{{padding:54px 0}}.section h2{{font-size:40px;line-height:1;letter-spacing:-.045em;margin:0 0 10px}}.sub{{color:var(--muted);font-size:18px;margin:0 0 24px;max-width:760px}}.tiles{{display:grid;grid-template-columns:repeat(4,1fr);gap:14px}}.tile,.workload,.api-card{{background:#fff;border:1px solid var(--line);border-radius:18px;padding:20px;box-shadow:0 12px 28px rgba(17,31,52,.045)}}.tile h3,.workload h3{{margin:0 0 8px;font-size:18px}}.tile p,.workload p{{margin:0;color:var(--muted)}}.workloads{{display:grid;grid-template-columns:repeat(4,1fr);gap:14px}}.workload span{{display:block;color:var(--blue);font:850 12px var(--mono);margin-bottom:12px}}.workload b{{display:inline-flex;margin-top:16px;color:var(--green);font:800 12px var(--mono)}}.api-grid{{display:grid;grid-template-columns:.85fr 1.15fr;gap:20px;align-items:start}}.endpoint{{display:grid;grid-template-columns:82px 1fr;border-bottom:1px solid var(--line);padding:13px 0;font:13.5px var(--mono)}}.endpoint:last-child{{border-bottom:0}}.method{{color:var(--blue);font-weight:900}}.footer{{border-top:1px solid var(--line);margin-top:40px;padding:34px 0 56px;color:var(--muted);font-size:14px}}
@media(max-width:980px){{.links{{display:none}}.hero{{padding:44px 0 32px}}.hero-grid,.product-body,.api-grid{{grid-template-columns:1fr}}h1{{font-size:45px}}.lead{{font-size:18px}}.proof,.tiles,.workloads{{grid-template-columns:1fr 1fr}}}}
@media(max-width:620px){{.navin{{height:62px}}.btn:not(.primary){{display:none}}.proof,.tiles,.workloads{{grid-template-columns:1fr}}h1{{font-size:38px}}.product{{border-radius:18px}}}}
</style>
</head>
<body>
<nav class="nav"><div class="wrap navin"><a class="brand" href="/"><span class="mark"></span>GraphSub Cloud</a><div class="links"><a href="/console">Console</a><a href="/api/cloud">API</a><a href="/wiki">Wiki</a><a href="/docs">Docs</a></div><div class="grow"></div><a class="btn" href="/api/cloud">API</a><a class="btn primary" href="/console">Open console</a></div></nav>
<main>
<section class="hero"><div class="wrap hero-grid"><div><span class="eyebrow">Graph runtime cloud / {html.escape(session['org']['slug'])}</span><h1>Infrastructure for agents that need memory and proof.</h1><p class="lead">Provision GraphSub runtimes per user, organization, and workload. Each instance exposes query APIs, lifecycle controls, usage metering, audit logs, replay state, and a 65,536-concept global realtime ontology.</p><div class="actions"><a class="btn primary" href="/console">Open cloud console</a><a class="btn" href="/api/cloud/instances">List instances</a><a class="btn" href="/docs">Read API docs</a></div><div class="proof"><div><b>{summary['instances']}</b><span>instances</span></div><div><b>{summary['running']}</b><span>running</span></div><div><b>{summary['nodes']:,}</b><span>nodes</span></div><div><b>{summary['gsu']:,}</b><span>GSU month</span></div></div></div><aside class="product"><div class="product-top"><span class="lights"><i></i><i></i><i></i></span><span>control-plane / modal / {html.escape(session['org']['primary_region'])}</span></div><div class="product-body"><div class="runtime-stack">{instance_rows}</div><div class="control"><div class="mini-panel"><h3>Runtime mix</h3><div class="bars"><div class="bar"><span>memory</span><span class="track"><i class="fill" style="width:86%"></i></span><b>86%</b></div><div class="bar"><span>replay</span><span class="track"><i class="fill" style="width:64%"></i></span><b>64%</b></div><div class="bar"><span>query</span><span class="track"><i class="fill" style="width:91%"></i></span><b>91%</b></div></div></div><pre class="code">from graphsub import Client

gs = Client.cloud("org/{html.escape(session['org']['slug'])}")
inst = gs.instances.create(
    name="risk-prod",
    template="pro-4",
)

inst.query("MATCH (n:Concept) RETURN count(n)")</pre></div></div></aside></div></section>
<section class="section"><div class="wrap"><h2>One control plane. Many graph runtimes.</h2><p class="sub">GraphSub is a cloud pure play: users belong to organizations, organizations own instances, and instances serve agents, apps, capsules, and operators.</p><div class="tiles"><article class="tile"><h3>Provision</h3><p>Create isolated GraphSub runtimes with templates, regions, lifecycle controls, and connection strings.</p></article><article class="tile"><h3>Operate</h3><p>Start, stop, restart, scale, query, and stream instance events through the control-plane API.</p></article><article class="tile"><h3>Remember</h3><p>Store working, episodic, semantic, source, and proof memory as graph state instead of hidden prompt context.</p></article><article class="tile"><h3>Prove</h3><p>Replay outputs through sources, tools, approvals, dependencies, and graph changes.</p></article></div></div></section>
<section class="section"><div class="wrap"><h2>Capsules that make the platform concrete.</h2><p class="sub">Start with a buyer workflow, then expand into a durable graph substrate.</p><div class="workloads">{workload_cards}</div></div></section>
<section class="section"><div class="wrap api-grid"><div><h2>API-first, console-assisted.</h2><p class="sub">The console is just a client over the same routes that agents and applications use.</p></div><div class="api-card"><div class="endpoint"><span class="method">GET</span><span>/api/cloud/session</span></div><div class="endpoint"><span class="method">GET</span><span>/api/cloud/instances</span></div><div class="endpoint"><span class="method">POST</span><span>/api/cloud/instances</span></div><div class="endpoint"><span class="method">POST</span><span>/api/cloud/instances/{{id}}/query</span></div><div class="endpoint"><span class="method">GET</span><span>/api/cloud/instances/{{id}}/events</span></div></div></div></section>
</main><footer class="wrap footer">{html.escape(DEPLOYMENT_REVISION)} / Modal-backed state / <a href="/console">console</a> / <a href="/api/cloud">cloud api</a> / <a href="/wiki">wiki</a></footer>
</body></html>"""


def render_cloud_console_v3(session: dict, instances: list[dict]) -> str:
    boot = html.escape(json.dumps({"session": session, "instances": instances, "revision": DEPLOYMENT_REVISION}), quote=False)
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<title>GraphSub Cloud Console</title>
<meta name="robots" content="noindex,nofollow">
<style>
:root{{--bg:#f6f8fb;--ink:#07111f;--muted:#64748b;--line:#dde6f1;--panel:#fff;--soft:#f1f5fa;--blue:#145cff;--green:#078b5d;--amber:#a46400;--red:#b4233c;--shadow:0 18px 46px rgba(17,31,52,.07);--sans:-apple-system,BlinkMacSystemFont,"SF Pro Display","Segoe UI",Inter,Roboto,Arial,sans-serif;--mono:"SF Mono","Cascadia Code","Roboto Mono",Menlo,Consolas,monospace}}
*{{box-sizing:border-box}}body{{margin:0;background:var(--bg);color:var(--ink);font:500 14px/1.5 var(--sans)}}a{{color:inherit;text-decoration:none}}button,input,select{{font:inherit}}button{{cursor:pointer}}.shell{{display:grid;grid-template-columns:264px minmax(0,1fr);min-height:100dvh}}.side{{background:#fff;border-right:1px solid var(--line);padding:22px 18px;display:flex;flex-direction:column;gap:20px}}.brand{{display:flex;align-items:center;gap:10px;font-weight:850;font-size:18px}}.mark{{width:24px;height:24px;border-radius:8px;background:linear-gradient(135deg,#145cff,#00a6ff)}}.org{{border:1px solid var(--line);border-radius:14px;padding:12px;background:var(--soft)}}.org b{{display:block}}.org span{{color:var(--muted);font:12px var(--mono)}}.nav a{{display:block;padding:9px 10px;border-radius:10px;color:var(--muted);font-weight:700}}.nav a.active,.nav a:hover{{background:#eef5ff;color:#0e55cf}}.main{{min-width:0}}.top{{height:66px;background:rgba(255,255,255,.9);backdrop-filter:blur(16px);border-bottom:1px solid var(--line);display:flex;align-items:center;gap:14px;padding:0 24px;position:sticky;top:0;z-index:4}}.top h1{{font-size:18px;margin:0}}.grow{{flex:1}}.search{{min-width:320px;border:1px solid var(--line);background:#fff;border-radius:999px;padding:9px 14px;color:var(--muted)}}.account{{color:var(--muted);font:12px var(--mono)}}.content{{padding:22px;display:grid;gap:18px}}.kpis{{display:grid;grid-template-columns:repeat(4,1fr);gap:14px}}.kpi,.panel{{background:#fff;border:1px solid var(--line);border-radius:18px;box-shadow:var(--shadow)}}.kpi{{padding:17px}}.kpi b{{display:block;font-size:28px;letter-spacing:-.035em}}.kpi span{{color:var(--muted)}}.layout{{display:grid;grid-template-columns:minmax(0,1.25fr) minmax(360px,.75fr);gap:18px;align-items:start}}.panel-head{{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:16px 18px;border-bottom:1px solid var(--line)}}.panel-head h2{{font-size:16px;margin:0}}.btn{{border:1px solid var(--line);background:#fff;border-radius:10px;padding:8px 11px;font-weight:760}}.btn.primary{{background:var(--ink);color:#fff;border-color:var(--ink)}}.form{{display:grid;grid-template-columns:1fr 130px 130px auto;gap:8px}}.form input,.form select{{border:1px solid var(--line);border-radius:10px;padding:9px 10px;background:#fff}}.table{{display:grid}}.row{{display:grid;grid-template-columns:1.25fr .72fr .72fr .65fr .8fr auto;gap:12px;align-items:center;padding:13px 18px;border-bottom:1px solid #edf2f7}}.row:last-child{{border-bottom:0}}.head{{background:#fbfdff;color:var(--muted);font:850 11px var(--mono);text-transform:uppercase}}.name b{{display:block}}.name span,.muted{{color:var(--muted)}}.mono{{font-family:var(--mono)}}.badge{{display:inline-flex;border-radius:999px;padding:4px 8px;background:#e9f9f2;color:var(--green);font:850 12px var(--mono)}}.badge.sleeping{{background:#fff5da;color:var(--amber)}}.actions{{display:flex;gap:7px;flex-wrap:wrap}}pre{{margin:0;background:#07111f;color:#dbeafe;border-radius:0 0 18px 18px;padding:16px;font:12.5px/1.65 var(--mono);min-height:188px;overflow:auto}}.side-stack{{display:grid;gap:18px}}.detail{{display:grid;grid-template-columns:1fr 1fr;gap:10px;padding:16px 18px}}.detail div{{border:1px solid var(--line);background:var(--soft);border-radius:12px;padding:12px}}.detail b{{display:block;font-size:18px}}.detail span{{color:var(--muted);font-size:12px}}.events{{max-height:280px;overflow:auto}}.event{{display:grid;grid-template-columns:96px 1fr;padding:10px 18px;border-bottom:1px solid #edf2f7}}.event:last-child{{border-bottom:0}}@media(max-width:1100px){{.shell{{grid-template-columns:1fr}}.side{{display:none}}.layout{{grid-template-columns:1fr}}.kpis{{grid-template-columns:1fr 1fr}}.row{{grid-template-columns:1fr}}.form{{grid-template-columns:1fr}}.search{{display:none}}}}@media(max-width:620px){{.kpis{{grid-template-columns:1fr}}.content{{padding:14px}}.account{{display:none}}}}
</style>
</head>
<body>
<div class="shell"><aside class="side"><div class="brand"><span class="mark"></span>GraphSub</div><div class="org"><b>{html.escape(session['org']['name'])}</b><span>{html.escape(session['org']['plan'])} / {html.escape(session['org']['primary_region'])}</span></div><nav class="nav"><a class="active" href="/console">Instances</a><a href="/api/cloud/session">Session</a><a href="/api/cloud/usage">Usage</a><a href="/api/cloud/audit-log">Audit log</a><a href="/docs">OpenAPI</a><a href="/">Public site</a></nav></aside><main class="main"><header class="top"><h1>Instances</h1><input class="search" value="org:{html.escape(session['org']['slug'])} status:any" readonly><div class="grow"></div><span class="account">{html.escape(session['user']['email'])}</span></header><section class="content"><div class="kpis"><div class="kpi"><b id="kInstances">-</b><span>instances</span></div><div class="kpi"><b id="kRunning">-</b><span>running</span></div><div class="kpi"><b id="kNodes">-</b><span>nodes</span></div><div class="kpi"><b id="kGsu">-</b><span>GSU month</span></div></div><div class="layout"><section class="panel"><div class="panel-head"><h2>Runtime inventory</h2><form class="form" id="create"><input name="name" value="risk-prod" aria-label="Instance name"><select name="template"><option value="starter">Starter</option><option value="pro-4">Pro 4</option><option value="scale-16">Scale 16</option></select><select name="region"><option>us-east-1</option><option>us-west-2</option><option>eu-west-1</option></select><button class="btn primary">Create</button></form></div><div class="table" id="table"></div></section><aside class="side-stack"><section class="panel"><div class="panel-head"><h2>Selected runtime</h2><button class="btn" id="refresh">Refresh</button></div><div class="detail" id="detail"></div><pre id="connection"></pre></section><section class="panel"><div class="panel-head"><h2>Query</h2><button class="btn primary" id="query">Run concept count</button></div><pre id="result"></pre></section><section class="panel"><div class="panel-head"><h2>Audit</h2><span class="muted mono">persistent state</span></div><div class="events" id="events"></div></section></aside></div></section></main></div>
<script id="boot" type="application/json">{boot}</script>
<script>
const $ = s => document.querySelector(s);
const fmt = n => Number(n || 0).toLocaleString();
let state = JSON.parse($('#boot').textContent);
let selected = state.instances[0]?.id;
function current(){{return state.instances.find(i => i.id === selected) || state.instances[0];}}
function paint(){{
  const list = state.instances || [];
  $('#kInstances').textContent = list.length;
  $('#kRunning').textContent = list.filter(i => i.status === 'running').length;
  $('#kNodes').textContent = fmt(list.reduce((a,i)=>a+i.metrics.nodes,0));
  $('#kGsu').textContent = fmt(list.reduce((a,i)=>a+i.metrics.gsu_month,0));
  $('#table').innerHTML = `<div class="row head"><div>Name</div><div>Status</div><div>Nodes</div><div>P99</div><div>Region</div><div></div></div>` + list.map(i => `<div class="row"><div class="name"><b>${{i.name}}</b><span>${{i.id}}</span></div><div><span class="badge ${{i.status}}">${{i.status}}</span></div><div class="mono">${{fmt(i.metrics.nodes)}}</div><div class="mono">${{i.metrics.lookup_p99_us}}us</div><div>${{i.region}}</div><div class="actions"><button class="btn" data-open="${{i.id}}">Open</button><button class="btn" data-start="${{i.id}}">Start</button><button class="btn" data-stop="${{i.id}}">Stop</button></div></div>`).join('');
  document.querySelectorAll('[data-open]').forEach(b => b.onclick = () => {{selected = b.dataset.open; showConnection();}});
  document.querySelectorAll('[data-start]').forEach(b => b.onclick = () => action(b.dataset.start, 'start'));
  document.querySelectorAll('[data-stop]').forEach(b => b.onclick = () => action(b.dataset.stop, 'stop'));
  showConnection();
}}
async function reload(){{
  const [session, instances, audit] = await Promise.all([fetch('/api/cloud/session').then(r=>r.json()), fetch('/api/cloud/instances').then(r=>r.json()), fetch('/api/cloud/audit-log').then(r=>r.json())]);
  state.session = session.session; state.instances = instances.instances; state.revision = instances.revision;
  $('#events').innerHTML = audit.events.slice(0,8).map(e => `<div class="event"><span class="mono muted">${{e.at}}</span><span>${{e.action}} <span class="muted">${{e.target}}</span></span></div>`).join('');
  paint();
}}
async function showConnection(){{
  const inst = current(); if (!inst) return;
  $('#detail').innerHTML = `<div><b>${{fmt(inst.metrics.edges)}}</b><span>edges</span></div><div><b>${{inst.metrics.concepts.toLocaleString()}}</b><span>concepts</span></div><div><b>${{inst.metrics.storage_gb}}GB</b><span>storage</span></div><div><b>${{inst.template}}</b><span>template</span></div>`;
  const data = await fetch(`/api/cloud/instances/${{inst.id}}/connect`).then(r=>r.json());
  $('#connection').textContent = JSON.stringify(data.connection, null, 2);
}}
async function action(id, op){{await fetch(`/api/cloud/instances/${{id}}/${{op}}`, {{method:'POST'}}); await reload();}}
$('#refresh').onclick = reload;
$('#query').onclick = async () => {{
  const inst = current();
  const data = await fetch(`/api/cloud/instances/${{inst.id}}/query`, {{method:'POST', headers:{{'content-type':'application/json'}}, body:JSON.stringify({{query:'MATCH (n:Concept) RETURN count(n) AS concepts'}})}}).then(r=>r.json());
  $('#result').textContent = JSON.stringify(data, null, 2);
}};
$('#create').onsubmit = async e => {{
  e.preventDefault();
  const body = Object.fromEntries(new FormData(e.target).entries());
  const data = await fetch('/api/cloud/instances', {{method:'POST', headers:{{'content-type':'application/json'}}, body:JSON.stringify(body)}}).then(r=>r.json());
  selected = data.instance.id;
  await reload();
}};
paint(); reload();
</script>
</body></html>"""

image = (
    modal.Image.debian_slim(python_version="3.12")
    .pip_install("fastapi")
    .add_local_file(str(LANDING_HTML), "/assets/graphsub-phosphor.html")
    .add_local_file(str(FAVICON), "/assets/favicon.svg")
    .add_local_file(str(OG_IMAGE), "/assets/og-image.png")
    .add_local_file(str(APPLE_ICON), "/assets/apple-icon.png")
)


@app.function(image=image, scaledown_window=300, timeout=60)
@modal.asgi_app(custom_domains=["graphsub.com"])
def site():
    import asyncio
    from pathlib import Path

    from fastapi import FastAPI, Request, Response
    from fastapi.responses import HTMLResponse, JSONResponse, PlainTextResponse, RedirectResponse, StreamingResponse

    web = FastAPI(title="GraphSub", version="1.0.0", docs_url="/docs", redoc_url=None)

    security_headers = {
        "Strict-Transport-Security": "max-age=31536000; includeSubDomains; preload",
        "X-Content-Type-Options": "nosniff",
        "Referrer-Policy": "strict-origin-when-cross-origin",
        "Permissions-Policy": "camera=(), microphone=(), geolocation=()",
    }

    asset_cache_headers = {
        **security_headers,
        "Cache-Control": "public, max-age=86400",
    }

    data_cache_headers = {
        **security_headers,
        "Cache-Control": "public, max-age=60",
    }

    cloud_headers = {
        **security_headers,
        "Cache-Control": "no-store",
    }

    html_cache_headers = {
        **security_headers,
        "Cache-Control": "public, max-age=30",
    }

    landing_html = Path("/assets/graphsub-phosphor.html").read_text(encoding="utf-8")
    fallback_state = {"state": None}

    def initial_cloud_state() -> dict:
        return {
            "ontology": {
                "concepts": BASE_ONTOLOGY_CONCEPTS,
                "semantic_links": BASE_ONTOLOGY_LINKS,
                "kernel_types": BASE_KERNEL_TYPES,
                "kernel_edges": BASE_KERNEL_EDGES,
                "posture": "global realtime ontology",
            },
            "instances": {instance["id"]: clone_json(instance) for instance in CLOUD_INSTANCE_SEED},
            "audit_events": [
                {"at": "04:00:00Z", "actor": CLOUD_USER["email"], "action": "instance.seeded", "target": "prod-agents"},
                {"at": "04:02:00Z", "actor": CLOUD_USER["email"], "action": "instance.seeded", "target": "replay-lab"},
                {"at": "04:05:00Z", "actor": CLOUD_USER["email"], "action": "console.deployed", "target": "graphsub.com"},
            ],
        }

    def migrate_cloud_state(state: dict) -> tuple[dict, bool]:
        changed = False
        ontology = state.setdefault("ontology", {})
        expected = {
            "concepts": BASE_ONTOLOGY_CONCEPTS,
            "semantic_links": BASE_ONTOLOGY_LINKS,
            "kernel_types": BASE_KERNEL_TYPES,
            "kernel_edges": BASE_KERNEL_EDGES,
            "posture": "global realtime ontology",
        }
        for key, value in expected.items():
            if ontology.get(key) != value:
                ontology[key] = value
                changed = True
        for instance in state.get("instances", {}).values():
            metrics = instance.setdefault("metrics", {})
            if int(metrics.get("concepts") or 0) < BASE_ONTOLOGY_CONCEPTS:
                metrics["concepts"] = BASE_ONTOLOGY_CONCEPTS
                changed = True
            if int(metrics.get("nodes") or 0) < BASE_ONTOLOGY_CONCEPTS:
                metrics["nodes"] = BASE_ONTOLOGY_CONCEPTS
                changed = True
            if int(metrics.get("edges") or 0) < BASE_ONTOLOGY_LINKS:
                metrics["edges"] = BASE_ONTOLOGY_LINKS
                changed = True
            features = instance.setdefault("features", [])
            for feature in ("global-realtime-ontology", "source-provenance", "operational-objects"):
                if feature not in features:
                    features.append(feature)
                    changed = True
        return state, changed

    def load_cloud_state() -> dict:
        if CLOUD_STATE is None:
            if fallback_state["state"] is None:
                fallback_state["state"] = initial_cloud_state()
            state, changed = migrate_cloud_state(clone_json(fallback_state["state"]))
            if changed:
                fallback_state["state"] = clone_json(state)
            return state
        raw = CLOUD_STATE.get("state_json")
        if not raw:
            state = initial_cloud_state()
            CLOUD_STATE["state_json"] = json.dumps(state)
            return state
        state, changed = migrate_cloud_state(json.loads(raw))
        if changed:
            CLOUD_STATE["state_json"] = json.dumps(state)
        return state

    def save_cloud_state(state: dict) -> None:
        if CLOUD_STATE is None:
            fallback_state["state"] = clone_json(state)
            return
        CLOUD_STATE["state_json"] = json.dumps(state)

    def with_headers(resp: Response, headers: dict[str, str] | None = None) -> Response:
        for key, value in (headers or security_headers).items():
            resp.headers[key] = value
        return resp

    def session_object() -> dict:
        return {
            "user": CLOUD_USER,
            "org": CLOUD_ORG,
            "membership": {"role": "owner", "permissions": ["instances:*", "billing:read", "usage:read", "query:*"]},
            "revision": DEPLOYMENT_REVISION,
        }

    def instance_list(instances: dict[str, dict] | None = None) -> list[dict]:
        if instances is None:
            instances = load_cloud_state()["instances"]
        return [hydrate_instance(instance) for instance in instances.values()]

    def not_found_instance(instance_id: str) -> JSONResponse:
        return JSONResponse(
            {"revision": DEPLOYMENT_REVISION, "error": "instance_not_found", "instance_id": instance_id},
            status_code=404,
        )

    def log_event(state: dict, action: str, target: str) -> None:
        events = state.setdefault("audit_events", [])
        events.insert(0, {"at": utc_now()[11:], "actor": CLOUD_USER["email"], "action": action, "target": target})
        del events[80:]

    @web.get("/", response_class=HTMLResponse)
    @web.head("/", response_class=HTMLResponse)
    async def root():
        response = HTMLResponse(landing_html)
        response.headers["X-GraphSub-Revision"] = DEPLOYMENT_REVISION
        return with_headers(response, html_cache_headers)

    @web.get("/terminal", response_class=HTMLResponse)
    @web.head("/terminal", response_class=HTMLResponse)
    @web.get("/tui", response_class=HTMLResponse)
    @web.head("/tui", response_class=HTMLResponse)
    async def terminal_landing():
        response = HTMLResponse(landing_html)
        response.headers["X-GraphSub-Revision"] = DEPLOYMENT_REVISION
        return with_headers(response, html_cache_headers)

    @web.get("/cloud", response_class=HTMLResponse)
    @web.head("/cloud", response_class=HTMLResponse)
    async def cloud_landing():
        state = load_cloud_state()
        response = HTMLResponse(render_cloud_landing_v3(session_object(), instance_list(state["instances"])))
        response.headers["X-GraphSub-Revision"] = DEPLOYMENT_REVISION
        return with_headers(response, html_cache_headers)

    @web.get("/console", response_class=HTMLResponse)
    @web.head("/console", response_class=HTMLResponse)
    @web.get("/dashboard", response_class=HTMLResponse)
    @web.head("/dashboard", response_class=HTMLResponse)
    @web.get("/app", response_class=HTMLResponse)
    @web.head("/app", response_class=HTMLResponse)
    async def console():
        state = load_cloud_state()
        response = HTMLResponse(render_cloud_console_v3(session_object(), instance_list(state["instances"])))
        response.headers["X-GraphSub-Revision"] = DEPLOYMENT_REVISION
        return with_headers(response, cloud_headers)

    @web.get("/health")
    @web.head("/health")
    async def health():
        return with_headers(
            JSONResponse({"status": "healthy", "service": "graphsub-com", "domain": "graphsub.com"})
        )

    @web.get("/status")
    @web.head("/status")
    async def status():
        state = load_cloud_state()
        instances = state["instances"]
        return with_headers(
            JSONResponse(
                {
                    "status": "operational",
                    "service": "graphsub-com",
                    "domain": "graphsub.com",
                    "deployment": "modal",
                    "cloud": {
                        "users": 1,
                        "orgs": 1,
                        "instances": len(instances),
                        "running_instances": sum(1 for i in instances.values() if i["status"] == "running"),
                    },
                }
            )
        )

    @web.get("/api")
    async def api_info():
        return with_headers(
            JSONResponse(
                {
                    "service": "graphsub",
                    "api": "https://arthurcolle--graphsub-api.modal.run",
                    "docs": "/docs",
                    "health": "/health",
                    "revision": DEPLOYMENT_REVISION,
                    "cloud_control_plane": {
                        "console": "/console",
                        "session": "/api/cloud/session",
                        "users": "/api/cloud/users",
                        "current_user": "/api/cloud/users/me",
                        "org": "/api/cloud/orgs/current",
                        "instances": "/api/cloud/instances",
                        "usage": "/api/cloud/usage",
                        "billing": "/api/cloud/billing",
                        "audit_log": "/api/cloud/audit-log",
                        "regions": "/api/cloud/regions",
                        "templates": "/api/cloud/templates",
                        "ontology": "/api/ontology",
                    },
                    "product_console": {
                        "plan": "/api/plan",
                        "competitors": "/api/competitors",
                        "capsules": "/api/capsules",
                        "bench": "/api/bench",
                        "evidence": "/api/evidence",
                        "execute": "/api/execute",
                        "wiki": "/api/wiki",
                        "examples": "/api/examples",
                    },
                }
            ),
            data_cache_headers,
        )

    @web.get("/api/cloud")
    async def cloud_api():
        state = load_cloud_state()
        return with_headers(
            JSONResponse(
                {
                    "revision": DEPLOYMENT_REVISION,
                    "service": "graphsub-cloud",
                    "model": "users own orgs; orgs own hosted GraphSub instances",
                    "console": "/console",
                    "resources": {
                        "session": "/api/cloud/session",
                        "users": "/api/cloud/users",
                        "current_user": "/api/cloud/users/me",
                        "org": "/api/cloud/orgs/current",
                        "instances": "/api/cloud/instances",
                        "instance": "/api/cloud/instances/{instance_id}",
                        "connect": "/api/cloud/instances/{instance_id}/connect",
                        "query": "/api/cloud/instances/{instance_id}/query",
                        "events": "/api/cloud/instances/{instance_id}/events",
                        "usage": "/api/cloud/usage",
                        "billing": "/api/cloud/billing",
                        "audit_log": "/api/cloud/audit-log",
                        "ontology": "/api/ontology",
                        "ontology_concepts": "/api/ontology/concepts",
                    },
                    "summary": instance_summary(state["instances"]),
                }
            ),
            cloud_headers,
        )

    @web.get("/api/cloud/session")
    async def cloud_session():
        state = load_cloud_state()
        return with_headers(JSONResponse({"session": session_object(), "summary": instance_summary(state["instances"])}), cloud_headers)

    @web.get("/api/cloud/users")
    async def cloud_users():
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "count": 1, "users": [CLOUD_USER]}), cloud_headers)

    @web.get("/api/cloud/users/me")
    async def cloud_user_me():
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "user": CLOUD_USER, "org": CLOUD_ORG}), cloud_headers)

    @web.get("/api/cloud/orgs/current")
    async def cloud_org_current():
        state = load_cloud_state()
        return with_headers(
            JSONResponse({"revision": DEPLOYMENT_REVISION, "org": CLOUD_ORG, "summary": instance_summary(state["instances"])}),
            cloud_headers,
        )

    @web.get("/api/cloud/regions")
    async def cloud_regions():
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "regions": CLOUD_REGIONS}), cloud_headers)

    @web.get("/api/cloud/templates")
    async def cloud_templates():
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "templates": CLOUD_TEMPLATES}), cloud_headers)

    @web.get("/api/ontology")
    @web.get("/api/openontology")
    async def ontology_manifest_api():
        return with_headers(JSONResponse(openontology_manifest()), cloud_headers)

    @web.get("/api/ontology/concepts")
    async def ontology_concepts_api(limit: int = 100, offset: int = 0, q: str | None = None):
        return with_headers(JSONResponse(openontology_concept_page(limit=limit, offset=offset, q=q)), cloud_headers)

    @web.get("/api/ontology/search")
    async def ontology_search_api(q: str = "", limit: int = 25):
        return with_headers(JSONResponse(openontology_search(q or "", limit=limit)), cloud_headers)

    @web.get("/api/ontology/domains")
    async def ontology_domains_api():
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "count": len(OPEN_ONTOLOGY_DOMAINS), "domains": OPEN_ONTOLOGY_DOMAINS}), cloud_headers)

    @web.get("/api/ontology/value-types")
    async def ontology_value_types_api():
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "count": len(OPEN_ONTOLOGY_VALUE_TYPES), "value_types": OPEN_ONTOLOGY_VALUE_TYPES}), cloud_headers)

    @web.get("/api/ontology/shared-properties")
    async def ontology_shared_properties_api():
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "count": len(OPEN_ONTOLOGY_SHARED_PROPERTIES), "shared_properties": OPEN_ONTOLOGY_SHARED_PROPERTIES}), cloud_headers)

    @web.get("/api/ontology/interfaces")
    async def ontology_interfaces_api():
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "count": len(OPEN_ONTOLOGY_INTERFACES), "interfaces": OPEN_ONTOLOGY_INTERFACES}), cloud_headers)

    @web.get("/api/ontology/object-types")
    async def ontology_object_types_api():
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "count": len(OPEN_ONTOLOGY_OBJECT_TYPES), "object_types": OPEN_ONTOLOGY_OBJECT_TYPES}), cloud_headers)

    @web.get("/api/ontology/link-types")
    async def ontology_link_types_api():
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "count": len(OPEN_ONTOLOGY_LINK_TYPES), "link_types": OPEN_ONTOLOGY_LINK_TYPES}), cloud_headers)

    @web.get("/api/ontology/action-types")
    async def ontology_action_types_api():
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "count": len(OPEN_ONTOLOGY_ACTION_TYPES), "action_types": OPEN_ONTOLOGY_ACTION_TYPES}), cloud_headers)

    @web.get("/api/ontology/functions")
    async def ontology_functions_api():
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "count": len(OPEN_ONTOLOGY_FUNCTIONS), "functions": OPEN_ONTOLOGY_FUNCTIONS}), cloud_headers)

    @web.get("/api/ontology/object-sets")
    async def ontology_object_sets_api():
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "count": len(OPEN_ONTOLOGY_OBJECT_SETS), "object_sets": OPEN_ONTOLOGY_OBJECT_SETS}), cloud_headers)

    @web.get("/api/ontology/scenarios")
    async def ontology_scenarios_api():
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "count": len(OPEN_ONTOLOGY_SCENARIOS), "scenarios": OPEN_ONTOLOGY_SCENARIOS}), cloud_headers)

    @web.get("/api/cloud/instances")
    async def cloud_instances():
        state = load_cloud_state()
        instances = state["instances"]
        return with_headers(
            JSONResponse(
                {
                    "revision": DEPLOYMENT_REVISION,
                    "org": CLOUD_ORG["id"],
                    "count": len(instances),
                    "instances": instance_list(instances),
                }
            ),
            cloud_headers,
        )

    @web.post("/api/cloud/instances")
    async def cloud_create_instance(request: Request):
        state = load_cloud_state()
        instances = state["instances"]
        try:
            payload = await request.json()
        except Exception:
            payload = {}
        instance = create_instance_record(payload if isinstance(payload, dict) else {}, instances)
        instances[instance["id"]] = instance
        log_event(state, "instance.created", instance["name"])
        save_cloud_state(state)
        return with_headers(
            JSONResponse(
                {
                    "revision": DEPLOYMENT_REVISION,
                    "instance": hydrate_instance(instance),
                    "message": "GraphSub instance provisioned on Modal control plane.",
                },
                status_code=201,
            ),
            cloud_headers,
        )

    @web.get("/api/cloud/instances/{instance_id}")
    async def cloud_instance(instance_id: str):
        state = load_cloud_state()
        instance = state["instances"].get(instance_id)
        if not instance:
            return with_headers(not_found_instance(instance_id), cloud_headers)
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "instance": hydrate_instance(instance)}), cloud_headers)

    @web.get("/api/cloud/instances/{instance_id}/connect")
    async def cloud_instance_connect(instance_id: str):
        state = load_cloud_state()
        instance = state["instances"].get(instance_id)
        if not instance:
            return with_headers(not_found_instance(instance_id), cloud_headers)
        return with_headers(
            JSONResponse({"revision": DEPLOYMENT_REVISION, "instance_id": instance_id, "connection": instance_connection(instance)}),
            cloud_headers,
        )

    @web.post("/api/cloud/instances/{instance_id}/start")
    async def cloud_instance_start(instance_id: str):
        state = load_cloud_state()
        instance = state["instances"].get(instance_id)
        if not instance:
            return with_headers(not_found_instance(instance_id), cloud_headers)
        instance["status"] = "running"
        instance["health"] = "healthy"
        instance["updated_at"] = utc_now()
        log_event(state, "instance.started", instance["name"])
        save_cloud_state(state)
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "instance": hydrate_instance(instance)}), cloud_headers)

    @web.post("/api/cloud/instances/{instance_id}/stop")
    async def cloud_instance_stop(instance_id: str):
        state = load_cloud_state()
        instance = state["instances"].get(instance_id)
        if not instance:
            return with_headers(not_found_instance(instance_id), cloud_headers)
        instance["status"] = "sleeping"
        instance["health"] = "idle"
        instance["metrics"]["ops_per_sec"] = 0
        instance["updated_at"] = utc_now()
        log_event(state, "instance.stopped", instance["name"])
        save_cloud_state(state)
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "instance": hydrate_instance(instance)}), cloud_headers)

    @web.post("/api/cloud/instances/{instance_id}/restart")
    async def cloud_instance_restart(instance_id: str):
        state = load_cloud_state()
        instance = state["instances"].get(instance_id)
        if not instance:
            return with_headers(not_found_instance(instance_id), cloud_headers)
        instance["status"] = "running"
        instance["health"] = "restarting"
        instance["updated_at"] = utc_now()
        log_event(state, "instance.restarted", instance["name"])
        save_cloud_state(state)
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "instance": hydrate_instance(instance)}), cloud_headers)

    @web.post("/api/cloud/instances/{instance_id}/scale")
    async def cloud_instance_scale(instance_id: str, request: Request):
        state = load_cloud_state()
        instance = state["instances"].get(instance_id)
        if not instance:
            return with_headers(not_found_instance(instance_id), cloud_headers)
        try:
            payload = await request.json()
        except Exception:
            payload = {}
        template_id = str((payload or {}).get("template") or instance["template"])
        template = template_for(template_id)
        instance["template"] = template["id"]
        instance["metrics"]["ops_per_sec"] = max(instance["metrics"]["ops_per_sec"], 1200000 * template["replicas"])
        instance["updated_at"] = utc_now()
        log_event(state, "instance.scaled", f"{instance['name']}->{template['id']}")
        save_cloud_state(state)
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "instance": hydrate_instance(instance)}), cloud_headers)

    @web.post("/api/cloud/instances/{instance_id}/query")
    async def cloud_instance_query(instance_id: str, request: Request):
        state = load_cloud_state()
        instance = state["instances"].get(instance_id)
        if not instance:
            return with_headers(not_found_instance(instance_id), cloud_headers)
        try:
            payload = await request.json()
        except Exception:
            payload = {}
        query = str((payload or {}).get("query") or "MATCH (n:Concept) RETURN count(n) AS concepts")
        result = query_instance(instance, query)
        instance["metrics"]["gsu_month"] += 1
        log_event(state, "instance.query", instance["name"])
        save_cloud_state(state)
        return with_headers(
            JSONResponse(
                {
                    "revision": DEPLOYMENT_REVISION,
                    "instance_id": instance_id,
                    "query": query,
                    "result": result,
                    "usage": {"gsu": 1, "billable": True},
                }
            ),
            cloud_headers,
        )

    @web.get("/api/cloud/instances/{instance_id}/events")
    async def cloud_instance_events(instance_id: str):
        state = load_cloud_state()
        instance = state["instances"].get(instance_id)
        if not instance:
            return with_headers(not_found_instance(instance_id), cloud_headers)

        async def event_stream():
            for n in range(12):
                live = hydrate_instance(instance)
                event = {
                    "revision": DEPLOYMENT_REVISION,
                    "instance_id": instance_id,
                    "sequence": n,
                    "status": live["status"],
                    "health": live["health"],
                    "metrics": live["metrics"],
                    "at": utc_now(),
                }
                yield f"event: instance.update\ndata: {json.dumps(event)}\n\n"
                await asyncio.sleep(0.5)

        response = StreamingResponse(event_stream(), media_type="text/event-stream")
        return with_headers(response, cloud_headers)

    @web.get("/api/cloud/usage")
    async def cloud_usage():
        state = load_cloud_state()
        summary = instance_summary(state["instances"])
        return with_headers(
            JSONResponse(
                {
                    "revision": DEPLOYMENT_REVISION,
                    "org": CLOUD_ORG["id"],
                    "included_gsu": CLOUD_ORG["included_gsu"],
                    "used_gsu_month": summary["total_gsu_month"],
                    "overage_gsu": max(0, summary["total_gsu_month"] - CLOUD_ORG["included_gsu"]),
                    "estimated_cost_usd": round(max(0, summary["total_gsu_month"] - CLOUD_ORG["included_gsu"]) * 0.0001, 2),
                    "summary": summary,
                }
            ),
            cloud_headers,
        )

    @web.get("/api/cloud/billing")
    async def cloud_billing():
        return with_headers(
            JSONResponse(
                {
                    "revision": DEPLOYMENT_REVISION,
                    "org": CLOUD_ORG["id"],
                    "plan": CLOUD_ORG["plan"],
                    "billing_status": CLOUD_ORG["billing_status"],
                    "spend_limit_usd": CLOUD_ORG["spend_limit_usd"],
                    "unit_prices": {"gsu_usd": 0.0001, "storage_gb_month_usd": 0.18},
                }
            ),
            cloud_headers,
        )

    @web.get("/api/cloud/audit-log")
    async def cloud_audit_log():
        state = load_cloud_state()
        return with_headers(JSONResponse({"revision": DEPLOYMENT_REVISION, "events": state.get("audit_events", [])[:50]}), cloud_headers)

    @web.get("/wiki", response_class=HTMLResponse)
    @web.head("/wiki", response_class=HTMLResponse)
    @web.get("/wiki/", response_class=HTMLResponse)
    @web.head("/wiki/", response_class=HTMLResponse)
    @web.get("/wiki.html", response_class=HTMLResponse)
    @web.head("/wiki.html", response_class=HTMLResponse)
    @web.get("/wiki/index.html", response_class=HTMLResponse)
    @web.head("/wiki/index.html", response_class=HTMLResponse)
    async def wiki_index():
        response = HTMLResponse(render_wiki_index())
        response.headers["X-GraphSub-Revision"] = DEPLOYMENT_REVISION
        return with_headers(response, html_cache_headers)

    @web.get("/wiki/{slug}", response_class=HTMLResponse)
    @web.head("/wiki/{slug}", response_class=HTMLResponse)
    @web.get("/wiki/{slug}/", response_class=HTMLResponse)
    @web.head("/wiki/{slug}/", response_class=HTMLResponse)
    async def wiki_page(slug: str):
        normalized = normalize_wiki_slug(slug)
        if not normalized:
            return await wiki_index()
        page = WIKI_BY_SLUG.get(normalized)
        if not page:
            return with_headers(RedirectResponse("/wiki", status_code=302), html_cache_headers)
        response = HTMLResponse(render_wiki_page(page))
        response.headers["X-GraphSub-Revision"] = DEPLOYMENT_REVISION
        return with_headers(response, html_cache_headers)

    @web.get("/wiki/{slug:path}", response_class=HTMLResponse)
    @web.head("/wiki/{slug:path}", response_class=HTMLResponse)
    async def wiki_path_fallback(slug: str):
        normalized = normalize_wiki_slug(slug)
        if not normalized:
            return await wiki_index()
        page = WIKI_BY_SLUG.get(normalized)
        if page:
            response = HTMLResponse(render_wiki_page(page))
            response.headers["X-GraphSub-Revision"] = DEPLOYMENT_REVISION
            return with_headers(response, html_cache_headers)
        return with_headers(RedirectResponse("/wiki", status_code=302), html_cache_headers)

    @web.get("/api/wiki")
    async def wiki_api():
        return with_headers(
            JSONResponse(
                {
                    "revision": DEPLOYMENT_REVISION,
                    "count": len(WIKI_PAGES),
                    "pages": [
                        {
                            "slug": page["slug"],
                            "title": page["title"],
                            "description": page["description"],
                            "url": wiki_url(page["slug"]),
                            "keywords": page["keywords"],
                        }
                        for page in WIKI_PAGES
                    ],
                }
            ),
            data_cache_headers,
        )

    @web.get("/api/examples")
    async def examples_api():
        return with_headers(
            JSONResponse(
                {
                    "revision": DEPLOYMENT_REVISION,
                    "count": len(CODE_EXAMPLES),
                    "examples": CODE_EXAMPLES,
                }
            ),
            data_cache_headers,
        )

    @web.get("/api/plan")
    async def product_plan(capsule: str | None = None, target: str = "pilot-buyer", workload: str = "agent-replay"):
        selected = capsule_for(capsule)
        return with_headers(
            JSONResponse(
                {
                    "revision": DEPLOYMENT_REVISION,
                    "roadmap": ROADMAP,
                    "selected_capsule": selected,
                    "execution_preview": execution_artifact(selected, target, workload),
                }
            ),
            data_cache_headers,
        )

    @web.get("/api/competitors")
    async def competitors():
        return with_headers(
            JSONResponse(
                {
                    "revision": DEPLOYMENT_REVISION,
                    "count": len(COMPETITORS),
                    "competitors": COMPETITORS,
                    "summary": "GraphSub performance, peer reference lanes, and zero-ETL market context.",
                }
            ),
            data_cache_headers,
        )

    @web.get("/api/capsules")
    async def capsules():
        return with_headers(
            JSONResponse({"revision": DEPLOYMENT_REVISION, "count": len(CAPSULES), "capsules": CAPSULES}),
            data_cache_headers,
        )

    @web.get("/api/bench")
    async def bench():
        return with_headers(
            JSONResponse(
                {
                    "revision": DEPLOYMENT_REVISION,
                    "summary": "Performance lanes for the live product console.",
                    "lanes": BENCHMARK_LANES,
                }
            ),
            data_cache_headers,
        )

    @web.get("/api/evidence")
    async def evidence():
        return with_headers(
            JSONResponse(
                {
                    "revision": DEPLOYMENT_REVISION,
                    "sources": sorted(
                        {
                            source
                            for competitor in COMPETITORS
                            for source in competitor["sources"]
                        }
                    ),
                    "source_types": ["official docs", "vendor pages", "public reports", "GraphSub product data"],
                    "source_policy": "Source links are included for competitor context and product comparisons.",
                }
            ),
            data_cache_headers,
        )

    @web.get("/api/execute")
    @web.post("/api/execute")
    async def execute(capsule: str | None = None, target: str = "pilot-buyer", workload: str = "agent-replay"):
        selected = capsule_for(capsule)
        return with_headers(
            JSONResponse(
                {
                    "revision": DEPLOYMENT_REVISION,
                    "execution": execution_artifact(selected, target, workload),
                    "note": "Plan generated. Connect sources to launch the capsule workflow.",
                }
            ),
            data_cache_headers,
        )

    @web.get("/robots.txt", response_class=PlainTextResponse)
    async def robots():
        return with_headers(
            PlainTextResponse(
                "User-agent: *\n"
                "Allow: /\n"
                "Sitemap: https://graphsub.com/sitemap.xml\n"
                "Sitemap: https://graphsub.com/wiki-sitemap.xml\n"
            )
        )

    @web.get("/sitemap.xml")
    async def sitemap():
        wiki_urls = "\n".join(
            f"  <url><loc>{wiki_url(page['slug'])}</loc><lastmod>2026-07-03</lastmod><changefreq>weekly</changefreq><priority>0.75</priority></url>"
            for page in WIKI_PAGES
        )
        xml = f"""<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">
  <url><loc>https://graphsub.com/</loc><lastmod>2026-07-03</lastmod><changefreq>daily</changefreq><priority>1.0</priority></url>
  <url><loc>https://graphsub.com/wiki</loc><lastmod>2026-07-03</lastmod><changefreq>weekly</changefreq><priority>0.9</priority></url>
{wiki_urls}
  <url><loc>https://graphsub.com/api</loc><lastmod>2026-07-03</lastmod><changefreq>weekly</changefreq><priority>0.5</priority></url>
</urlset>
"""
        return with_headers(Response(xml, media_type="application/xml"), asset_cache_headers)

    @web.get("/wiki-sitemap.xml")
    async def wiki_sitemap():
        wiki_urls = "\n".join(
            f"  <url><loc>{wiki_url(page['slug'])}</loc><lastmod>2026-07-03</lastmod><changefreq>weekly</changefreq><priority>0.8</priority></url>"
            for page in WIKI_PAGES
        )
        xml = f"""<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">
  <url><loc>https://graphsub.com/wiki</loc><lastmod>2026-07-03</lastmod><changefreq>weekly</changefreq><priority>0.9</priority></url>
{wiki_urls}
</urlset>
"""
        return with_headers(Response(xml, media_type="application/xml"), asset_cache_headers)

    @web.get("/llms.txt", response_class=PlainTextResponse)
    async def llms_txt():
        pages = "\n".join(f"- {page['title']}: {wiki_url(page['slug'])}" for page in WIKI_PAGES)
        text = f"""# GraphSub

GraphSub is a graph database and control substrate for AI agents, memory graphs, replayable workflows, and real-world graph capsules.

## Primary URLs
- Landing page: https://graphsub.com/
- Wiki index: https://graphsub.com/wiki
- API manifest: https://graphsub.com/api
- Wiki JSON: https://graphsub.com/api/wiki
- Code examples JSON: https://graphsub.com/api/examples

## Wiki pages
{pages}
"""
        return with_headers(PlainTextResponse(text), asset_cache_headers)

    @web.get("/favicon.svg")
    @web.head("/favicon.svg")
    async def favicon_svg():
        return with_headers(
            Response(Path("/assets/favicon.svg").read_bytes(), media_type="image/svg+xml"),
            asset_cache_headers,
        )

    @web.get("/favicon.ico")
    @web.head("/favicon.ico")
    async def favicon_ico():
        return await favicon_svg()

    @web.get("/og-image.png")
    @web.head("/og-image.png")
    async def og_image():
        return with_headers(
            Response(Path("/assets/og-image.png").read_bytes(), media_type="image/png"),
            asset_cache_headers,
        )

    @web.get("/apple-icon.png")
    @web.head("/apple-icon.png")
    async def apple_icon():
        return with_headers(
            Response(Path("/assets/apple-icon.png").read_bytes(), media_type="image/png"),
            asset_cache_headers,
        )

    @web.get("/signup")
    async def signup_redirect():
        return RedirectResponse("/#pricing", status_code=302)

    @web.get("/api-docs")
    async def api_docs_redirect():
        return RedirectResponse("/docs", status_code=302)

    @web.get("/encyclopedia")
    async def encyclopedia_redirect():
        return RedirectResponse("/wiki", status_code=302)

    @web.get("/{path:path}", response_class=HTMLResponse)
    @web.head("/{path:path}", response_class=HTMLResponse)
    async def browser_fallback(path: str):
        normalized = path.strip().strip("/").lower()
        if normalized.startswith(("wiki", "wikis", "encyclopedia")):
            return with_headers(RedirectResponse("/wiki", status_code=302), html_cache_headers)
        response = HTMLResponse(render_not_found("/" + normalized if normalized else "/"), status_code=404)
        response.headers["X-GraphSub-Revision"] = DEPLOYMENT_REVISION
        return with_headers(response, html_cache_headers)

    return web
