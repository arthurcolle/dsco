#!/usr/bin/env python3
"""
Generate an extended governed consumer profile ontology.

Base ontology: 10,000 human-auditable facets from generate_ontology.py.
Extended ontology: 50,000 total facets = base 10,000 + 40,000 additional governed facets.

The extension deliberately uses deeper semantic paths rather than S4/prohibited
sensitive profiling. It expands content, commerce, professional, entertainment,
lifestyle, geo, social, technical, communication, life-planning, privacy, governance,
and values facets with purpose-limited metadata.
"""
from __future__ import annotations

import csv
import hashlib
import json
import sys
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

SCRIPT_DIR = Path(__file__).resolve().parent
ROOT = SCRIPT_DIR.parents[1]
sys.path.insert(0, str(SCRIPT_DIR))

import generate_ontology as base  # noqa: E402

OUT_DATA = ROOT / "data" / "consumer_profile_ontology"
OUT_DOCS = ROOT / "docs" / "consumer_profile_ontology"
VERSION = "2026.06.extended50k"
GENERATED_AT = datetime.now(timezone.utc).replace(microsecond=0).isoformat()
TARGET_TOTAL = 50_000
TARGET_DELTA = 40_000

PROHIBITED_SUBSTRINGS = set(base.PROHIBITED_PATTERNS) | {
    "pregnancy", "abortion", "clinic_visit", "hospital_visit", "religious_identity",
    "political_identity", "party_affiliation", "sexuality", "ethnicity", "racial",
    "diagnosis", "addiction", "bankruptcy", "eviction", "immigration", "criminal",
    "disability_inference", "depression", "anxiety_disorder", "infertility",
}


def slug(s: str) -> str:
    return base.slug(s)


def flatten_content_topics() -> List[str]:
    topics: List[str] = []
    for subdomain in base.TOPICS["content"].values():
        topics.extend(subdomain)
    # Remove intentionally academic-but-sensitive-adjacent terms from expansion.
    return sorted({slug(t) for t in topics if "belief" not in t and "political" not in t})


def commerce_categories() -> List[str]:
    departments = {
        "apparel": ["casualwear", "workwear", "outerwear", "activewear", "athleisure", "formalwear", "socks", "underlayers", "rainwear", "seasonal_clothing"],
        "footwear": ["running_shoes", "trail_shoes", "walking_shoes", "boots", "sandals", "sneakers", "dress_shoes", "cycling_shoes", "hiking_boots", "kids_shoes"],
        "electronics": ["smartphone", "laptop", "desktop", "monitor", "tablet", "camera", "headphones", "speakers", "router", "storage_drive", "printer", "wearable", "keyboard", "mouse", "charger"],
        "home": ["sofa", "desk", "chair", "mattress", "bedding", "lighting", "rug", "storage", "kitchenware", "cookware", "vacuum", "air_purifier", "tool_set", "garden_tool", "decor"],
        "grocery": ["coffee", "tea", "snacks", "pantry_staples", "breakfast", "meal_kits", "beverages", "baking", "pet_food", "household_supplies"],
        "beauty": ["skincare_general", "haircare", "grooming", "fragrance", "makeup", "nail_care", "sun_care", "bath_body", "oral_care", "personal_care"],
        "sports_outdoor": ["running_gear", "cycling_gear", "camping_gear", "hiking_gear", "ski_gear", "fitness_equipment", "yoga_gear", "team_sports_gear", "fishing_gear", "outdoor_apparel"],
        "travel": ["luggage", "backpack", "hotel", "flight", "car_rental", "travel_accessory", "travel_app", "tour", "rail_ticket", "theme_park_ticket"],
        "digital": ["productivity_app", "cloud_storage", "streaming_video", "streaming_music", "learning_app", "creative_software", "developer_tool", "security_software", "finance_app", "gaming_subscription"],
        "auto": ["tires", "car_care", "ev_charger", "bike_rack", "dash_cam", "floor_mats", "maintenance_tool", "car_audio", "child_seat_explicit", "roadside_kit"],
    }
    cats = []
    for dept, items in departments.items():
        cats.extend(f"{dept}_{item}" for item in items)
    return cats


def work_skills() -> List[str]:
    areas = {
        "engineering": ["software_architecture", "api_design", "distributed_systems", "databases", "devops", "sre", "security_engineering", "mobile_development", "frontend_development", "backend_development", "testing", "observability", "data_engineering", "ml_engineering", "cloud_architecture"],
        "data": ["analytics", "statistics", "business_intelligence", "data_visualization", "experimentation", "forecasting", "etl", "sql", "python", "spreadsheet_modeling", "dashboarding", "data_quality", "privacy_analytics", "causal_inference", "reporting"],
        "business": ["strategy", "operations", "pricing", "sales", "marketing", "customer_success", "finance", "accounting", "legal_ops", "procurement", "people_ops", "project_management", "program_management", "vendor_management", "partnerships"],
        "creative": ["writing", "copywriting", "graphic_design", "product_design", "video_editing", "audio_production", "presentation_design", "brand_strategy", "photography", "motion_design", "content_strategy", "community_management", "social_media", "technical_writing", "instructional_design"],
        "leadership": ["hiring", "performance_management", "team_building", "negotiation", "conflict_resolution", "planning", "budgeting", "decision_making", "coaching", "meeting_facilitation", "stakeholder_management", "change_management", "risk_management", "exec_communication", "board_reporting"],
    }
    out = []
    for group, skills in areas.items():
        out.extend(f"{group}_{skill}" for skill in skills)
    return out


def entertainment_topics() -> List[str]:
    groups = {
        "music": ["pop", "hip_hop", "electronic", "jazz", "classical", "rock", "indie", "folk", "country", "latin", "r_and_b", "ambient", "metal", "punk", "soundtracks", "live_music"],
        "film": ["action", "comedy", "documentary", "independent", "horror", "science_fiction", "animation", "drama", "thriller", "fantasy", "biographical", "classic_cinema", "short_films", "film_reviews"],
        "tv": ["drama_series", "comedy_series", "documentary_series", "anime", "reality", "competition", "streaming_originals", "limited_series", "family_series", "educational_series"],
        "gaming": ["rpg", "fps", "strategy", "simulation", "sports_games", "racing_games", "puzzle_games", "mobile_games", "indie_games", "esports", "retro_games", "sandbox_games", "cozy_games", "tabletop_games"],
        "books": ["fiction", "nonfiction", "mystery", "science_fiction", "fantasy", "history_books", "business_books", "science_books", "memoir", "audiobooks", "ebooks", "book_clubs"],
        "events": ["concerts", "comedy_shows", "film_festivals", "book_events", "gaming_events", "fan_conventions", "theater", "dance_performance", "local_events", "family_events"],
    }
    out = []
    for group, topics in groups.items():
        out.extend(f"{group}_{topic}" for topic in topics)
    return out


def lifestyle_topics() -> List[str]:
    groups = {
        "fitness": ["running", "cycling", "strength_training", "yoga", "pilates", "hiking", "swimming", "walking", "home_workout", "gym", "team_sports", "mobility_training", "dance_fitness", "climbing", "skiing"],
        "nutrition": ["home_cooking", "meal_prep", "baking", "restaurant_discovery", "coffee", "tea", "family_meals", "quick_meals", "budget_meals", "special_diet_explicit", "food_learning", "kitchen_tools"],
        "routines": ["morning_routine", "evening_routine", "weekend_projects", "habit_tracking", "journaling", "calendar_planning", "decluttering", "home_maintenance", "family_routine", "travel_routine"],
        "outdoors": ["camping", "fishing", "beach", "mountains", "parks", "birdwatching", "gardening", "nature_photography", "road_trips", "picnics", "snow_activities", "water_activities"],
        "comfort": ["low_motion_preference", "quiet_environment", "large_text_preference", "captions_preference", "ergonomic_products", "simple_interface", "home_comfort", "weather_comfort", "travel_comfort", "low_stimulation_content"],
    }
    out = []
    for group, topics in groups.items():
        out.extend(f"{group}_{topic}" for topic in topics)
    return out


def geo_topics() -> List[str]:
    return [
        "coarse_city_context", "metro_region_context", "suburban_context", "rural_context", "college_town_context",
        "tourist_area_context", "walkable_area", "bikeable_area", "transit_access", "car_dependent_area",
        "coastal_context", "mountain_context", "desert_context", "cold_winter_context", "hot_summer_context",
        "rainy_season_context", "snow_season_context", "restaurant_density", "park_access", "shopping_access",
        "airport_access", "train_station_access", "commute_pattern", "weekend_radius", "domestic_travel_pattern",
        "international_travel_pattern", "local_event_context", "seasonal_event_context", "tourism_planning_context", "coarse_location_control",
    ]


def social_topics() -> List[str]:
    return [
        "close_friend_cluster", "family_cluster_explicit", "work_cluster", "school_cluster", "local_cluster", "creator_cluster",
        "interest_cluster", "group_admin_role", "group_helper_role", "event_organizer_role", "marketplace_seller_role",
        "commenting_pattern", "sharing_pattern", "saving_pattern", "private_reply_pattern", "invitation_pattern",
        "social_proof_response", "expert_node_role", "local_connector_role", "early_adopter_pattern", "community_lurker_role",
        "blocking_controls", "comment_filter_controls", "message_limit_controls", "unknown_contact_controls", "safety_review_controls",
    ]


def tech_topics() -> List[str]:
    return [
        "mobile_device", "desktop_device", "tablet_device", "multi_device", "ios_platform", "android_platform", "macos_platform", "windows_platform", "linux_platform",
        "chrome_browser", "safari_browser", "firefox_browser", "edge_browser", "wifi_network", "cellular_network", "low_bandwidth_context", "high_latency_context",
        "offline_usage", "data_saver", "battery_saver", "storage_constraint", "camera_capability", "microphone_capability", "video_capability", "audio_capability",
        "push_channel", "email_channel", "sms_channel", "in_app_channel", "mfa_state", "account_recovery_state", "trusted_device_state", "suspicious_activity_state",
    ]


def communication_topics() -> List[str]:
    return [
        "short_text", "longform_text", "bullet_points", "tables", "examples", "step_by_step", "video", "audio", "visual_summary", "interactive",
        "formal_tone", "casual_tone", "direct_tone", "encouraging_tone", "technical_tone", "plain_language", "high_density", "low_density", "summary_first", "detail_first",
        "captions", "transcripts", "large_text", "low_motion", "screen_reader", "translation", "bilingual_content", "locale_formatting", "timezone_context", "currency_context",
    ]


def life_topics() -> List[str]:
    return [
        "relocation_planning", "local_move_planning", "long_distance_move_planning", "home_search", "rental_search", "home_project", "renovation_project",
        "college_planning", "online_course_planning", "certification_planning", "career_change_research", "resume_update", "interview_prep", "skill_upgrade",
        "travel_planning", "event_planning", "birthday_planning", "holiday_hosting", "gift_planning", "budgeting_learning", "tax_prep", "retirement_education",
        "seasonal_shopping", "summer_planning", "winter_planning", "back_to_school_explicit", "moving_supplies", "storage_service", "furniture_research",
    ]


def privacy_topics() -> List[str]:
    return [
        "global_personalization", "ads_personalization", "feed_personalization", "recommendation_personalization", "email_personalization", "push_personalization",
        "first_party_source", "partner_data_source", "precise_location_source", "cross_device_source", "off_platform_source", "model_inference_source",
        "alcohol_suppression", "gambling_suppression", "weight_loss_suppression", "dating_suppression", "financial_product_suppression", "sensitive_category_suppression",
        "profile_view", "facet_explanation", "interest_delete", "demographic_correction", "data_download", "data_deletion", "appeal", "policy_review",
    ]


def governance_topics() -> List[str]:
    return [
        "identity_resolution", "household_linking", "multi_account_linking", "profile_completeness", "profile_freshness", "source_quality", "evidence_diversity",
        "consent_lineage", "activation_audit", "policy_decision", "proxy_risk", "confidence_calibration", "staleness_detection", "contradiction_detection",
        "experiment_holdout", "shadow_mode", "rollout_state", "rollback_state", "explanation_coverage", "user_correction_rate",
    ]


def values_topics() -> List[str]:
    return [
        "animal_welfare", "environment", "education_support", "disaster_relief", "local_community", "arts_culture_support", "volunteering", "fundraiser_participation",
        "sustainable_products", "recycling", "energy_efficiency", "secondhand_shopping", "repair_reuse", "local_business", "fair_trade", "community_alerts",
        "nonpartisan_voter_info", "public_meeting_info", "civic_services_info", "neighborhood_support",
    ]


EXPANSION_SPECS = [
    ("content", "deep_topics", "Deeper content interest and knowledge facets", 9000, flatten_content_topics(),
     ["beginner", "intermediate", "advanced", "professional", "academic", "practical", "tooling", "project", "community", "local", "longform", "video", "audio", "newsletter", "event"],
     ["affinity", "recency", "expertise", "learning_intent", "creator_role", "share_propensity", "save_propensity", "format_fit", "depth_preference", "novelty_preference"]),
    ("commerce", "product_microcategory", "Deeper commerce and product preference facets", 6500, commerce_categories(),
     ["research", "comparison", "deal", "premium", "budget", "gift", "replacement", "upgrade", "subscription", "local_store", "online_store", "seasonal", "review_reading"],
     ["affinity", "purchase_intent", "purchase_window_0_7d", "purchase_window_8_30d", "price_sensitivity", "brand_loyalty", "channel_preference", "purchase_history_strength"]),
    ("work", "skill_context", "Deeper professional skill and work-context facets", 4500, work_skills(),
     ["beginner", "intermediate", "advanced", "managerial", "hands_on", "tooling", "certification", "portfolio", "hiring", "content_creation", "conference", "remote"],
     ["affinity", "expertise", "learning_intent", "tool_interest", "creator_role", "recency", "confidence", "community_interest"]),
    ("entertainment", "fandom_microcontext", "Deeper entertainment and fandom facets", 3500, entertainment_topics(),
     ["discovery", "deep_fandom", "casual", "live_event", "streaming", "collecting", "social", "review", "creator_following", "family_friendly"],
     ["affinity", "recency", "fandom_strength", "event_intent", "purchase_intent", "format_preference", "social_sharing"]),
    ("lifestyle", "routine_microcontext", "Deeper lifestyle, fitness, and routine facets", 3500, lifestyle_topics(),
     ["beginner", "habit", "gear", "community", "local", "travel", "seasonal", "family", "low_cost", "premium", "content", "product"],
     ["affinity", "content_interest", "routine_fit", "product_interest", "goal_explicit", "recency", "confidence"]),
    ("geo", "coarse_context_expanded", "Expanded coarse geography and mobility context", 2500, geo_topics(),
     ["weekday", "weekend", "seasonal", "travel", "local", "commute", "event", "shopping", "outdoor", "privacy_safe"],
     ["current", "affinity", "intent", "frequency", "coarse", "confidence", "suppression"]),
    ("social", "interaction_microcontext", "Expanded social graph and community interaction context", 2000, social_topics(),
     ["recent", "stable", "local", "interest_based", "professional", "family_explicit", "event_based", "creator", "safety", "privacy"],
     ["strength", "present", "role", "frequency", "affinity", "sensitivity", "suppression"]),
    ("tech", "environment_microcontext", "Expanded device, channel, and technical environment context", 2000, tech_topics(),
     ["primary", "secondary", "recent", "persistent", "constraint", "capability", "security", "delivery", "optimization", "user_control"],
     ["enabled", "capability", "constraint", "risk", "preference", "recency", "confidence"]),
    ("comm", "expression_microcontext", "Expanded language, format, density, and expression preferences", 1500, communication_topics(),
     ["default", "technical", "casual", "learning", "support", "marketing", "notification", "long_session", "short_session", "accessibility"],
     ["preference", "proficiency", "affinity", "enabled", "explicit", "confidence"]),
    ("life", "planning_microcontext", "Expanded explicit life planning and temporal intent facets", 1500, life_topics(),
     ["explicit", "research", "checklist", "commerce", "content", "short_window", "medium_window", "seasonal", "local", "suppressed"],
     ["intent", "planning_window_0_30d", "planning_window_31_180d", "content_interest", "commerce_interest", "confidence"]),
    ("privacy", "control_microcontext", "Expanded privacy, suppression, and user-control facets", 1500, privacy_topics(),
     ["global", "ads", "recommendations", "measurement", "partner", "location", "youth", "accessibility", "jurisdiction", "audit"],
     ["enabled", "allowed", "blocked", "suppression", "limit", "available"]),
    ("gov", "quality_microcontext", "Expanded governance, quality, and audit-control facets", 1000, governance_topics(),
     ["global", "ads", "ranking", "recommendation", "measurement", "safety", "source", "model", "user_control", "audit"],
     ["score", "available", "required", "status", "confidence", "freshness"]),
    ("values", "cause_microcontext", "Expanded broad values, causes, and nonprofit context", 1000, values_topics(),
     ["content", "local", "volunteer", "donation", "commerce", "event", "education", "community", "seasonal", "aggregate"],
     ["interest", "content_interest", "volunteer_intent", "donation_intent", "local_affinity", "commerce_preference"]),
]


def sensitivity_for(domain: str, subdomain: str, context: str, relation: str) -> str:
    if domain in {"gov", "privacy", "tech"}:
        if domain == "privacy" and ("youth" in context or "accessibility" in context):
            return "S3_RESTRICTED"
        return "S0_OPERATIONAL"
    if domain in {"life", "values"}:
        return "S2_PRIVATE"
    if domain == "demo":
        return "S2_PRIVATE"
    if domain == "comm" and "accessibility" in context:
        return "S3_RESTRICTED"
    return "S1_STANDARD"


def value_type_for(relation: str) -> str:
    if relation in {"enabled", "allowed", "blocked", "required", "available", "explicit", "suppression", "present"}:
        return "boolean"
    if relation in {"status", "primary", "secondary", "coarse", "role", "channel_preference", "format_preference"}:
        return "enum"
    return "float"


def half_life_for(domain: str, relation: str) -> Tuple[int | None, int | None]:
    if domain in {"gov", "privacy"} or relation in {"enabled", "allowed", "blocked", "available", "required", "status"}:
        return None, None
    if "window" in relation or relation == "recency":
        return 14, 90
    if "intent" in relation:
        return 30, 180
    if domain in {"tech", "comm"}:
        return 365, 1095
    if domain == "life":
        return 45, 240
    return 180, 720


def source_classes_for(domain: str, sensitivity: str) -> List[str]:
    if domain in {"gov", "privacy"}:
        return ["explicit", "system_state", "policy_engine"]
    if sensitivity == "S3_RESTRICTED":
        return ["explicit", "user_setting", "necessary_system_state"]
    sources = ["explicit", "first_party_engagement", "model_inferred"]
    if domain == "commerce":
        sources += ["search", "commerce"]
    if domain == "geo":
        sources += ["coarse_location"]
    if domain == "social":
        sources += ["social_graph_metadata"]
    return sources


def humanize(s: str) -> str:
    return s.replace("_", " ").capitalize()


def check_safe(facet_id: str) -> None:
    for pattern in PROHIBITED_SUBSTRINGS:
        if pattern and pattern in facet_id:
            raise ValueError(f"Prohibited/sensitive pattern generated: {facet_id} contains {pattern}")


def make_ext_facet(domain: str, subdomain: str, subdomain_name: str, topic: str, context: str, relation: str, ordinal: int) -> Dict:
    topic = slug(topic)
    context = slug(context)
    relation = slug(relation)
    facet_id = f"u.{domain}.{subdomain}.{topic}.{context}.{relation}"
    check_safe(facet_id)
    sensitivity = sensitivity_for(domain, subdomain, context, relation)
    half_life, ttl = half_life_for(domain, relation)
    val_type = value_type_for(relation)
    return {
        "facet_id": facet_id,
        "display_name": f"{humanize(topic)} {humanize(context).lower()} {humanize(relation).lower()}",
        "description": f"Extended governed {humanize(relation).lower()} facet for {humanize(topic).lower()} in {humanize(context).lower()} context.",
        "path": [domain, subdomain, topic, context, relation],
        "domain": domain,
        "domain_name": next((d[1] for d in base.DOMAIN_SPECS if d[0] == domain), humanize(domain)),
        "subdomain": subdomain,
        "subdomain_name": subdomain_name,
        "topic": topic,
        "context": context,
        "facet_kind": "extended_governed_profile_facet",
        "relationship_type": relation,
        "value_type": val_type,
        "value_range": [0, 1] if val_type == "float" else None,
        "scope": "user",
        "cardinality": "single_value",
        "source_classes": source_classes_for(domain, sensitivity),
        "confidence_model": "bayesian_decay_v3",
        "decay_model": "exponential_half_life" if half_life else "state_until_changed",
        "half_life_days": half_life,
        "ttl_days": ttl,
        "minimum_evidence_count": 1 if sensitivity in {"S0_OPERATIONAL", "S3_RESTRICTED"} else 3,
        "minimum_confidence_for_activation": 0.55 if sensitivity == "S0_OPERATIONAL" else 0.65 if sensitivity == "S1_STANDARD" else 0.80,
        "sensitivity_class": sensitivity,
        "proxy_risk_class": "P0_LOW" if sensitivity in {"S0_OPERATIONAL", "S1_STANDARD"} else "P2_REVIEW_REQUIRED" if sensitivity == "S2_PRIVATE" else "P3_RESTRICTED",
        "user_visible": False if domain == "gov" and "safety" in context else True,
        "user_editable": True if sensitivity in {"S2_PRIVATE", "S3_RESTRICTED"} or domain in {"comm", "privacy"} else False,
        "user_deletable": True if domain != "gov" else False,
        "allowed_uses": base.allowed_uses(sensitivity, domain, relation),
        "disallowed_uses": base.disallowed_uses(sensitivity),
        "explanation_template": f"This profile facet may be used because of settings or interactions related to {humanize(topic).lower()} in a {humanize(context).lower()} context.",
        "policy_notes": "Extended facet. Enforce purpose limitation, consent, confidence, decay, proxy-risk checks, and S4 boundary suppression before activation.",
        "owner_team": "profile_ontology",
        "review_status": "approved_generated_extended",
        "version": VERSION,
        "ordinal": ordinal,
        "created_at": GENERATED_AT,
        "updated_at": GENERATED_AT,
    }


def generate_delta(existing_ids: set[str]) -> List[Dict]:
    delta: List[Dict] = []
    ordinal = len(existing_ids)
    for domain, subdomain, subdomain_name, budget, topics, contexts, relations in EXPANSION_SPECS:
        made = 0
        idx = 0
        while made < budget:
            topic = topics[idx % len(topics)]
            c_idx = (idx // len(topics)) % len(contexts)
            r_idx = (idx // (len(topics) * len(contexts))) % len(relations)
            variant = idx // (len(topics) * len(contexts) * len(relations))
            context = contexts[c_idx]
            if variant:
                context = f"{context}_v{variant + 1}"
            relation = relations[r_idx]
            facet = make_ext_facet(domain, subdomain, subdomain_name, topic, context, relation, ordinal)
            idx += 1
            if facet["facet_id"] in existing_ids:
                continue
            existing_ids.add(facet["facet_id"])
            delta.append(facet)
            ordinal += 1
            made += 1
        assert made == budget, (domain, subdomain, made, budget)
    assert len(delta) == TARGET_DELTA, len(delta)
    return delta


def write_outputs(full: List[Dict], delta: List[Dict]) -> None:
    OUT_DATA.mkdir(parents=True, exist_ok=True)
    OUT_DOCS.mkdir(parents=True, exist_ok=True)

    full_jsonl = OUT_DATA / "facet_definitions_extended_50k.jsonl"
    delta_jsonl = OUT_DATA / "facet_expansion_delta_40k.jsonl"
    for path, rows in [(full_jsonl, full), (delta_jsonl, delta)]:
        with path.open("w") as f:
            for facet in rows:
                f.write(json.dumps(facet, sort_keys=True) + "\n")

    fields = [
        "facet_id", "display_name", "domain", "domain_name", "subdomain", "subdomain_name", "topic", "context",
        "relationship_type", "value_type", "sensitivity_class", "proxy_risk_class", "user_visible",
        "user_editable", "user_deletable", "half_life_days", "ttl_days", "minimum_confidence_for_activation",
        "allowed_uses", "disallowed_uses", "source_classes", "version",
    ]
    with (OUT_DATA / "facet_definitions_extended_50k.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for facet in full:
            row = {k: facet.get(k) for k in fields}
            for k in ["allowed_uses", "disallowed_uses", "source_classes"]:
                row[k] = ";".join(row[k])
            w.writerow(row)

    summary = {
        "generated_at": GENERATED_AT,
        "version": VERSION,
        "base_facets": len(full) - len(delta),
        "delta_facets": len(delta),
        "total_facets": len(full),
        "counts_by_domain": dict(sorted(Counter(f["domain"] for f in full).items())),
        "delta_counts_by_domain": dict(sorted(Counter(f["domain"] for f in delta).items())),
        "counts_by_sensitivity": dict(sorted(Counter(f["sensitivity_class"] for f in full).items())),
        "delta_counts_by_sensitivity": dict(sorted(Counter(f["sensitivity_class"] for f in delta).items())),
        "counts_by_value_type": dict(sorted(Counter(f["value_type"] for f in full).items())),
        "counts_by_proxy_risk": dict(sorted(Counter(f["proxy_risk_class"] for f in full).items())),
        "sha256_facet_definitions_extended_50k_jsonl": hashlib.sha256(full_jsonl.read_bytes()).hexdigest(),
        "sha256_facet_expansion_delta_40k_jsonl": hashlib.sha256(delta_jsonl.read_bytes()).hexdigest(),
    }
    (OUT_DATA / "summary_extended_50k.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")

    doc = f"""# Extended 50,000-Facet Consumer Profile Ontology

Generated: `{GENERATED_AT}`  
Version: `{VERSION}`

This is an expansion of the base governed 10,000-facet ontology.

## Counts

| Set | Facets |
|---|---:|
| Base ontology | {len(full) - len(delta):,} |
| Expansion delta | {len(delta):,} |
| Extended total | {len(full):,} |

## Files

| Path | Purpose |
|---|---|
| `data/consumer_profile_ontology/facet_definitions_extended_50k.jsonl` | Full 50,000-facet registry. |
| `data/consumer_profile_ontology/facet_expansion_delta_40k.jsonl` | Only the 40,000 newly generated facets. |
| `data/consumer_profile_ontology/facet_definitions_extended_50k.csv` | Spreadsheet-friendly 50,000-facet registry. |
| `data/consumer_profile_ontology/summary_extended_50k.json` | Counts and hashes. |

## Delta Allocation

| Domain | Added facets |
|---|---:|
"""
    for domain, count in summary["delta_counts_by_domain"].items():
        doc += f"| `{domain}` | {count:,} |\n"
    doc += f"""

## Safety Boundary

The extension preserves the base policy boundary:

- no S4 activation facets,
- no protected-trait inference facets,
- no sensitive-place targeting facets,
- no medical diagnosis, addiction, pregnancy, immigration, criminal-history, or precise-financial-distress inference facets,
- every facet carries allowed/disallowed uses and user-control metadata.

## Verification

```bash
wc -l data/consumer_profile_ontology/facet_definitions_extended_50k.jsonl
jq . data/consumer_profile_ontology/summary_extended_50k.json
```

SHA-256 full JSONL:

```text
{summary['sha256_facet_definitions_extended_50k_jsonl']}
```

SHA-256 delta JSONL:

```text
{summary['sha256_facet_expansion_delta_40k_jsonl']}
```
"""
    (OUT_DOCS / "EXTENDED_50K.md").write_text(doc)


def validate(full: List[Dict], delta: List[Dict]) -> None:
    assert len(delta) == TARGET_DELTA
    assert len(full) == TARGET_TOTAL
    ids = [f["facet_id"] for f in full]
    assert len(ids) == len(set(ids)), "duplicate facet IDs"
    assert not any(f["sensitivity_class"].startswith("S4") for f in full)
    for f in full:
        check_safe(f["facet_id"])
        assert "protected_trait_inference" in f["disallowed_uses"], f["facet_id"]
        assert f["allowed_uses"], f["facet_id"]


def main() -> None:
    base_facets = base.generate()
    existing = {f["facet_id"] for f in base_facets}
    delta = generate_delta(existing)
    full = base_facets + delta
    validate(full, delta)
    write_outputs(full, delta)
    summary = json.loads((OUT_DATA / "summary_extended_50k.json").read_text())
    print(json.dumps({
        "generated": True,
        "base_facets": summary["base_facets"],
        "delta_facets": summary["delta_facets"],
        "total_facets": summary["total_facets"],
        "delta_counts_by_domain": summary["delta_counts_by_domain"],
        "counts_by_sensitivity": summary["counts_by_sensitivity"],
        "sha256_full": summary["sha256_facet_definitions_extended_50k_jsonl"],
        "sha256_delta": summary["sha256_facet_expansion_delta_40k_jsonl"],
    }, indent=2))


if __name__ == "__main__":
    main()
