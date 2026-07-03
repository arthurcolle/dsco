#!/usr/bin/env python3
"""
Generate a governed 10,000-facet consumer profile ontology.

This generator intentionally creates human-auditable facet definitions, not private
company targeting segments. It separates ontology definitions from user values and
assigns governance metadata to every leaf facet.
"""
from __future__ import annotations

import csv
import hashlib
import json
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, Iterable, List, Tuple

ROOT = Path(__file__).resolve().parents[2]
OUT_DATA = ROOT / "data" / "consumer_profile_ontology"
OUT_DOCS = ROOT / "docs" / "consumer_profile_ontology"
OUT_SCHEMAS = ROOT / "schemas" / "consumer_profile_ontology"
VERSION = "2026.06"
GENERATED_AT = datetime.now(timezone.utc).replace(microsecond=0).isoformat()

DOMAIN_SPECS = [
    ("gov", "Governance, provenance, and profile administration", 300, [
        ("profile", "Profile quality and identity resolution", 60),
        ("consent", "Consent and lawful basis", 50),
        ("source", "Source provenance", 40),
        ("recency", "Recency, decay, and TTL controls", 40),
        ("trust_safety", "Trust and safety model controls", 40),
        ("experiment", "Experiment and personalization eligibility", 35),
        ("audit", "Explanation and audit hooks", 35),
    ]),
    ("demo", "Demographics, life stage, and household context", 700, [
        ("age", "Age and life-stage", 75),
        ("gender", "Gender or self-presentation, explicit only", 40),
        ("household", "Household composition", 100),
        ("family_stage", "Relationship and family stage", 85),
        ("education", "Education level and stage", 70),
        ("work_status", "Occupation and employment status, broad", 85),
        ("affluence", "Affluence and spending capacity, broad and low-resolution", 60),
        ("housing", "Housing type and tenure, broad", 70),
        ("ownership", "Ownership, pets, and dependents", 75),
        ("routine", "Schedule and routine archetypes", 40),
    ]),
    ("geo", "Geography, mobility, and local context", 700, [
        ("place", "Country, region, city, and locality class", 70),
        ("local_area", "Neighborhood or local area type", 60),
        ("climate", "Climate and seasonality", 50),
        ("coarse_zones", "Home, work, or school coarse zones", 70),
        ("mobility", "Mobility radius and commute", 80),
        ("travel", "Travel history and travel intent", 110),
        ("venues", "Local venue affinity", 80),
        ("events", "Local events and seasonal context", 60),
        ("urbanicity", "Urban, suburban, rural context", 50),
        ("privacy", "Geo-privacy constraints", 70),
    ]),
    ("tech", "Device, connectivity, and technical environment", 500, [
        ("device", "Device type and hardware", 70),
        ("platform", "OS, app, and browser", 70),
        ("network", "Connectivity and network", 50),
        ("channel", "Notification and channel deliverability", 60),
        ("media_capability", "Media capture, camera, and audio capability", 40),
        ("immersive", "Gaming, VR, AR, and spatial hardware", 60),
        ("integrations", "Smart-home, wearable, and auto integrations", 50),
        ("security", "Security and authentication posture", 50),
        ("constraints", "Data, storage, and battery constraints", 50),
    ]),
    ("comm", "Language, communication, and media preferences", 350, [
        ("language", "Language proficiency", 80),
        ("locale", "Locale and cultural formatting", 40),
        ("format", "Reading, listening, and video preference", 60),
        ("tone", "Tone and formality preference", 40),
        ("density", "Message length and density", 35),
        ("accessibility", "Accessibility communication needs", 35),
        ("translation", "Translation and multilingual context", 30),
        ("medium_role", "Creator vs consumer role by medium", 30),
    ]),
    ("social", "Social graph, communities, and influence context", 650, [
        ("graph", "Tie strength and relationship types", 100),
        ("household_graph", "Household and family graph", 60),
        ("work_school_graph", "Work and school graph", 60),
        ("community", "Community membership", 110),
        ("influence", "Creator, follower, and influence position", 70),
        ("sharing", "Sharing and virality propensity", 60),
        ("group_role", "Group participation roles", 70),
        ("social_proof", "Social proof sensitivity", 40),
        ("events", "Event and network attendance", 50),
        ("safety", "Social safety and blocking constraints", 30),
    ]),
    ("content", "Content interests and knowledge domains", 2200, [
        ("news", "News and current-affairs formats", 90),
        ("science", "Science and technology", 200),
        ("software", "Software, AI, and data", 180),
        ("business", "Business and entrepreneurship", 120),
        ("finance", "Finance and investing education", 90),
        ("arts", "Arts, design, and architecture", 140),
        ("humanities", "History, humanities, and philosophy", 130),
        ("education", "Education and learning", 120),
        ("food", "Food, cooking, and nutrition content", 130),
        ("home", "Home, garden, and DIY", 120),
        ("sports", "Sports and outdoors", 170),
        ("autos", "Autos and mobility", 90),
        ("travel", "Travel and places", 130),
        ("parenting", "Parenting and family content", 80),
        ("pets", "Pets and animals", 70),
        ("hobbies", "Hobbies, maker, and crafts", 140),
        ("style", "Fashion, beauty, and style", 110),
        ("environment", "Environment and sustainability", 60),
        ("niche", "Miscellaneous local and niche taxonomy", 30),
    ]),
    ("entertainment", "Entertainment, culture, and fandoms", 850, [
        ("music", "Music genres and listening behavior", 120),
        ("film", "Film genres and moviegoing", 100),
        ("tv", "Television, streaming, and series", 100),
        ("gaming", "Gaming genres and platforms", 120),
        ("books", "Books, authors, and reading formats", 80),
        ("podcasts", "Podcasts and audio shows", 60),
        ("comedy", "Comedy and internet culture", 50),
        ("fandom", "Celebrity, creator, and influencer fandom", 80),
        ("live_events", "Live events and festivals", 60),
        ("collectibles", "Collectibles, fan merchandise, and fandom commerce", 50),
        ("taste", "Cultural participation and taste archetypes", 30),
    ]),
    ("commerce", "Commerce, brands, shopping, and product preferences", 1000, [
        ("behavior", "Shopping behavior and channel preference", 80),
        ("price", "Price sensitivity and deal behavior", 70),
        ("apparel", "Apparel, footwear, and accessories", 110),
        ("beauty", "Beauty, grooming, and personal care", 80),
        ("electronics", "Consumer electronics", 90),
        ("home", "Home, furniture, and appliances", 90),
        ("grocery", "Grocery, food, and beverage", 70),
        ("sports_outdoor", "Sports, outdoor, and fitness products", 70),
        ("travel", "Travel and hospitality commerce", 70),
        ("auto", "Auto, mobility, and transportation commerce", 60),
        ("financial_products", "Financial products education / shopping", 50),
        ("subscriptions", "Subscriptions, software, and digital services", 60),
        ("marketplace", "Marketplace seller / buyer behavior", 50),
        ("gifting", "Gifts, seasonal, and occasion shopping", 50),
    ]),
    ("life", "Life events, planning, and temporal intent", 650, [
        ("moving", "Moving and relocation", 70),
        ("home", "Home search and renovation", 70),
        ("education", "Education planning", 60),
        ("career", "Career transitions", 70),
        ("travel", "Travel planning", 70),
        ("events", "Event planning and celebrations", 80),
        ("family", "Family and household planning, explicit only", 60),
        ("finance", "Financial planning education", 60),
        ("seasonal", "Seasonal planning", 60),
        ("temporal", "Temporal intent windows", 50),
    ]),
    ("work", "Work, education, skills, and professional context", 600, [
        ("industry", "Industry and function, broad", 90),
        ("seniority", "Professional seniority and role type", 70),
        ("skills", "Skills and tools", 120),
        ("learning", "Learning and certification", 70),
        ("entrepreneurship", "Entrepreneurship and business ownership", 60),
        ("productivity", "Productivity and collaboration style", 60),
        ("hiring", "Hiring, recruiting, and job seeking", 50),
        ("creator", "Professional content creation", 40),
        ("work_mode", "Work schedule and work mode", 40),
    ]),
    ("lifestyle", "Wellness, fitness, and lifestyle needs", 500, [
        ("fitness", "Fitness activities", 100),
        ("nutrition", "Nutrition and cooking lifestyle", 70),
        ("sleep", "Sleep and recovery content", 40),
        ("mindfulness", "Mindfulness and stress-management content", 50),
        ("outdoors", "Outdoor and active lifestyle", 60),
        ("routines", "Personal routines and habit-building", 50),
        ("wellness_products", "Wellness products, non-medical", 50),
        ("comfort_accessibility", "Safety, comfort, and accessibility needs", 40),
        ("archetypes", "Lifestyle archetypes", 40),
    ]),
    ("values", "Values, causes, civic, and nonprofit context", 300, [
        ("causes", "Broad cause interest", 70),
        ("volunteering", "Volunteering and donation behavior", 50),
        ("sustainability", "Sustainability and environment", 50),
        ("community", "Community support and local involvement", 40),
        ("civic", "Civic information, non-partisan", 30),
        ("ethical_consumption", "Ethical consumption preferences", 40),
        ("governance", "Governance and suppression controls", 20),
    ]),
    ("privacy", "Privacy, safety, accessibility, and personalization constraints", 700, [
        ("settings", "User privacy settings", 100),
        ("ads", "Ad personalization controls", 80),
        ("sources", "Data source controls", 70),
        ("suppression", "Sensitive category suppression", 80),
        ("accessibility", "Accessibility preferences", 80),
        ("safety", "Safety, blocking, and content controls", 90),
        ("youth", "Youth and family safety controls", 60),
        ("fatigue", "Frequency, fatigue, and notification limits", 60),
        ("explain", "Explanation, transparency, and correction", 50),
        ("regulatory", "Regulatory and jurisdictional controls", 30),
    ]),
]

RELATION_PATTERNS: Dict[str, List[str]] = {
    "gov": ["score", "allowed", "strength", "available", "required", "status", "confidence", "freshness", "eligible", "blocked"],
    "demo": ["explicit", "band", "likelihood", "confidence", "stability", "editable", "source_strength", "suppression", "coarse", "verified"],
    "geo": ["current", "affinity", "intent", "radius", "frequency", "allowed", "suppression", "coarse", "recency", "confidence"],
    "tech": ["primary", "enabled", "capability", "constraint", "risk", "preference", "recency", "confidence", "eligible", "status"],
    "comm": ["primary", "secondary", "preference", "proficiency", "affinity", "enabled", "likely", "explicit", "confidence", "suppression"],
    "social": ["strength", "count_band", "present", "role", "frequency", "affinity", "sensitivity", "enabled", "suppression", "confidence"],
    "content": ["affinity", "recency", "expertise", "creator_role", "learning_intent"],
    "entertainment": ["affinity", "recency", "fandom_strength", "creator_role", "event_intent", "purchase_intent", "format_preference", "discovery_orientation", "social_sharing", "suppression"],
    "commerce": ["affinity", "purchase_intent", "purchase_window_0_7d", "purchase_window_8_30d", "price_sensitivity", "premium_preference", "budget_preference", "brand_loyalty", "channel_preference", "purchase_history_strength"],
    "life": ["explicit", "intent", "planning_window_0_30d", "planning_window_31_180d", "content_interest", "commerce_interest", "recency", "confidence", "suppression", "ttl_short"],
    "work": ["affinity", "explicit", "expertise", "learning_intent", "tool_interest", "creator_role", "seniority_band", "recency", "confidence", "suppression"],
    "lifestyle": ["affinity", "content_interest", "routine_fit", "product_interest", "goal_explicit", "recency", "confidence", "suppression", "format_preference", "community_interest"],
    "values": ["interest", "content_interest", "volunteer_intent", "donation_intent", "local_affinity", "commerce_preference", "recency", "confidence", "suppression", "aggregate_only"],
    "privacy": ["enabled", "allowed", "blocked", "suppression", "limit", "default", "status", "available", "required", "jurisdictional"],
}

TOPICS: Dict[str, Dict[str, List[str]]] = {
    "gov": {
        "profile": ["completeness", "identity", "multi_account_link", "household_link", "profile_age", "coverage", "high_confidence_facets", "low_confidence_facets", "contradiction_rate", "review_state"],
        "consent": ["ads_personalization", "email_personalization", "sms_personalization", "partner_data", "precise_location", "cross_device_linking", "lookalike_modeling", "measurement", "data_sale_opt_out", "sensitive_data_opt_out"],
        "source": ["explicit_profile", "first_party_behavior", "partner_data", "model_inference", "commerce_signal", "location_signal", "social_graph_signal", "search_signal", "device_signal", "support_signal"],
        "recency": ["profile", "long_term_interest", "short_term_intent", "stale_facet_ratio", "last_refresh", "evidence_freshness", "source_freshness", "consent_freshness", "policy_freshness", "explanation_freshness"],
        "trust_safety": ["abuse_risk", "spam_risk", "account_integrity", "youth_safety", "sensitive_category", "restricted_surface", "policy_block", "review_queue", "appeal_state", "safety_hold"],
        "experiment": ["ranking_experiment", "recommendation_experiment", "ads_experiment", "measurement_experiment", "privacy_experiment", "accessibility_experiment", "new_facet_shadow", "holdout", "treatment", "eligibility"],
        "audit": ["profile_view", "explanation", "high_risk_facet", "policy_review", "user_review", "download", "delete", "correction", "activation_log", "lineage"],
    },
    "demo": {
        "age": ["age_13_17", "age_18_24", "age_25_34", "age_35_44", "age_45_54", "age_55_64", "age_65_plus", "adult_status", "minor_status", "age_confidence"],
        "gender": ["self_described_gender", "pronoun_preference", "presentation_preference", "gendered_content_opt_in", "gendered_shopping_opt_in"],
        "household": ["single_adult", "multi_adult", "has_children", "has_teens", "elder_care_responsibility", "pet_owner", "dog_owner", "cat_owner", "home_size", "shared_household"],
        "family_stage": ["student_household", "new_parent_explicit", "school_age_family_explicit", "teen_family_explicit", "empty_nest_explicit", "retirement_transition", "caregiver_explicit", "partnered_explicit", "single_explicit", "family_content_preference"],
        "education": ["current_student", "high_school", "undergraduate", "graduate_school", "professional_training", "lifelong_learning", "online_learning", "campus_affinity", "alumni_affinity", "test_prep"],
        "work_status": ["full_time", "part_time", "self_employed", "student", "retired", "career_break", "remote_work", "shift_work", "hybrid_work", "commuter"],
        "affluence": ["spending_capacity_low_resolution", "premium_commerce", "budget_commerce", "deal_sensitivity", "luxury_content", "mass_market_content", "financial_planning_content", "price_comparison", "installment_interest", "savings_orientation"],
        "housing": ["renter", "owner", "apartment", "single_family_home", "shared_housing", "dorm_student_housing", "home_office", "outdoor_space", "small_space", "moving_relevant"],
        "ownership": ["vehicle_owner", "bike_owner", "pet_owner", "smart_home_owner", "gaming_console_owner", "camera_owner", "fitness_tracker_owner", "home_gym_owner", "garden_owner", "tool_owner"],
        "routine": ["weekday_morning_active", "weekday_evening_active", "weekend_active", "night_owl", "early_bird", "commute_routine", "school_calendar", "seasonal_routine", "travel_routine", "homebody_routine"],
    },
    "geo": {
        "place": ["country", "region", "city", "metro_area", "locality", "college_town", "tourist_area", "border_region", "coastal_area", "mountain_area"],
        "local_area": ["high_transit", "car_dependent", "walkable", "bikeable", "downtown", "suburban_center", "rural_center", "shopping_corridor", "restaurant_density", "park_access"],
        "climate": ["cold_winter", "hot_summer", "rainy_season", "dry_season", "snow_context", "coastal_climate", "mountain_climate", "seasonal_shift", "outdoor_season", "storm_disruption"],
        "coarse_zones": ["home_zone_coarse", "work_zone_coarse", "school_zone_coarse", "frequent_area", "weekend_area", "travel_area", "shopping_area", "event_area", "airport_area", "transit_hub_area"],
        "mobility": ["home_radius_small", "home_radius_medium", "home_radius_large", "commute_short", "commute_medium", "commute_long", "weekend_travel_radius", "air_travel", "public_transit", "biking"],
        "travel": ["domestic", "international", "business", "family", "solo", "adventure", "luxury", "budget", "last_minute", "planning_window"],
        "venues": ["restaurants", "coffee_shops", "gyms", "parks", "museums", "theaters", "stadiums", "retail_centers", "hotels", "coworking"],
        "events": ["local_festival", "concert", "sports_event", "farmers_market", "conference", "community_event", "seasonal_event", "family_event", "outdoor_event", "ticketed_event"],
        "urbanicity": ["urban", "suburban", "rural", "exurban", "college_town", "tourist_town", "industrial_area", "residential_area", "mixed_use", "low_density"],
        "privacy": ["precise_location", "location_history", "location_ads", "sensitive_place_suppression", "coarse_location_only", "geo_fencing", "background_location", "location_explanation", "geo_delete", "geo_download"],
    },
    "content": {
        "news": ["headlines", "explainers", "local_news", "business_news", "science_news", "technology_news", "sports_news", "culture_news", "weather_news", "longform_news", "data_journalism", "solutions_journalism", "breaking_news", "newsletter_news", "audio_news", "video_news", "fact_checking", "international_news"],
        "science": ["physics", "chemistry", "biology", "astronomy", "geology", "climate_science", "neuroscience", "mathematics", "statistics", "engineering", "robotics", "materials_science", "biotechnology", "medicine_research", "ecology", "oceanography", "space_exploration", "energy_science", "quantum_science", "science_history", "science_education", "science_policy", "laboratory_methods", "popular_science", "citizen_science", "science_visualization", "research_papers", "scientific_instruments", "field_research", "computational_science", "systems_science", "complexity_science", "nanotechnology", "genetics", "evolution", "microbiology", "pharmacology", "public_health_science", "agricultural_science", "environmental_science"],
        "software": ["artificial_intelligence", "machine_learning", "data_science", "software_engineering", "web_development", "mobile_development", "cybersecurity", "cloud_computing", "databases", "distributed_systems", "devops", "programming_languages", "open_source", "developer_tools", "productivity_software", "blockchain_technology", "computer_graphics", "user_experience", "api_design", "testing_quality", "embedded_systems", "operating_systems", "networking", "data_engineering", "analytics", "automation", "low_code", "technical_writing", "computer_architecture", "privacy_engineering", "observability", "sre", "edge_computing", "quantum_computing", "prompt_engineering", "robotics_software"],
        "business": ["entrepreneurship", "startups", "management", "leadership", "marketing", "sales", "operations", "strategy", "product_management", "customer_success", "small_business", "ecommerce", "creator_business", "fundraising", "venture_capital", "business_models", "pricing", "branding", "analytics", "negotiation", "remote_work", "hiring", "finance_ops", "legal_ops"],
        "finance": ["personal_finance", "budgeting", "investing_education", "retirement_planning", "tax_education", "credit_education", "mortgage_education", "insurance_education", "crypto_education", "markets_education", "economics", "financial_news", "small_business_finance", "saving", "portfolio_concepts", "risk_management", "financial_literacy", "estate_planning"],
        "arts": ["graphic_design", "architecture", "interior_design", "painting", "drawing", "photography", "sculpture", "typography", "illustration", "industrial_design", "fashion_design", "product_design", "landscape_design", "design_history", "art_history", "museum_culture", "public_art", "craft_design", "motion_design", "visual_identity", "color_theory", "creative_process", "design_tools", "design_critique", "art_markets", "ceramics", "printmaking", "textiles"],
        "humanities": ["history", "ancient_history", "medieval_history", "modern_history", "philosophy", "ethics", "literature", "poetry", "linguistics", "classics", "comparative_belief_systems_academic", "cultural_studies", "anthropology", "archaeology", "law_public_education", "political_theory_nonpartisan", "media_studies", "writing", "rhetoric", "critical_thinking", "intellectual_history", "translation", "language_history", "world_cultures", "book_reviews", "historical_maps"],
        "education": ["online_courses", "study_skills", "language_learning", "test_prep", "stem_learning", "arts_learning", "professional_learning", "children_learning", "adult_learning", "tutorials", "how_to_guides", "lecture_content", "credential_programs", "peer_learning", "learning_tools", "memory_methods", "research_methods", "curriculum_design", "education_policy", "homeschool_resources", "college_planning", "graduate_school", "vocational_training", "maker_education"],
        "food": ["cooking", "baking", "meal_prep", "restaurants", "coffee", "tea", "wine_food_pairing", "barbecue", "vegetarian_cooking", "vegan_cooking", "regional_cuisine", "world_cuisine", "healthy_cooking", "budget_meals", "family_meals", "quick_recipes", "kitchen_tools", "food_science", "gardening_food", "desserts", "fermentation", "grilling", "breakfast", "seafood", "street_food", "food_photography"],
        "home": ["diy", "home_improvement", "gardening", "interior_decor", "organization", "cleaning", "woodworking", "smart_home", "home_security", "energy_efficiency", "appliance_repair", "small_space_living", "rental_living", "home_office", "landscaping", "tools", "craft_room", "nursery_design", "outdoor_living", "maintenance", "renovation", "sustainable_home", "home_entertaining", "storage"],
        "sports": ["running", "trail_running", "cycling", "mountain_biking", "hiking", "camping", "skiing", "snowboarding", "surfing", "swimming", "soccer", "basketball", "baseball", "football", "tennis", "golf", "yoga", "strength_training", "climbing", "fishing", "boating", "martial_arts", "dance_fitness", "team_sports", "outdoor_gear", "endurance_sports", "sports_training", "sports_fandom", "motorsports", "skateboarding", "pickleball", "triathlon", "fitness_tracking", "sports_events"],
        "autos": ["car_reviews", "electric_vehicles", "hybrid_cars", "used_cars", "car_maintenance", "road_trips", "motorcycles", "bicycles_transport", "public_transit", "ride_share", "auto_technology", "car_safety", "family_cars", "luxury_cars", "truck_suv", "fleet_mobility", "charging_infrastructure", "auto_insurance_education"],
        "travel": ["beach_travel", "city_travel", "adventure_travel", "family_travel", "business_travel", "solo_travel", "budget_travel", "luxury_travel", "road_trips", "international_travel", "domestic_travel", "hotels", "flights", "travel_planning", "travel_photography", "food_travel", "cultural_travel", "outdoor_travel", "theme_parks", "cruises", "train_travel", "travel_rewards", "packing", "travel_safety", "weekend_getaways", "remote_work_travel"],
        "parenting": ["parenting_tips", "family_activities", "child_education", "teen_activities", "baby_products_explicit", "school_calendar", "family_meals", "family_travel", "child_safety", "parenting_community", "homework_help", "kids_sports", "family_budgeting", "toy_research", "childrens_books", "summer_camps"],
        "pets": ["dogs", "cats", "pet_training", "pet_food", "pet_health_general", "aquariums", "birds", "small_pets", "pet_travel", "pet_grooming", "pet_adoption", "pet_photography", "pet_products", "dog_parks"],
        "hobbies": ["photography_hobby", "crafts", "knitting", "sewing", "woodworking_hobby", "electronics_maker", "3d_printing", "board_games", "tabletop_rpg", "collecting", "model_building", "gardening_hobby", "birdwatching", "astronomy_hobby", "writing_hobby", "music_making", "home_brewing", "fishing_hobby", "hunting_outdoors", "puzzles", "chess", "diy_repairs", "calligraphy", "scrapbooking", "cosplay", "podcasting", "video_creation", "streaming", "genealogy"],
        "style": ["fashion", "menswear", "womenswear", "streetwear", "sustainable_fashion", "beauty", "skincare_general", "haircare", "fragrance", "jewelry", "watches", "sneakers", "accessories", "personal_style", "thrift_style", "luxury_style", "workwear", "outdoor_style", "makeup_artistry", "grooming_general", "nail_art", "seasonal_style"],
        "environment": ["sustainability", "conservation", "renewable_energy", "recycling", "climate_adaptation", "gardening_native_plants", "electric_transport", "energy_efficiency", "repair_reuse", "wildlife", "water_conservation", "local_environment"],
        "niche": ["local_guides", "community_calendar", "neighborhood_recommendations", "specialty_forums", "niche_collecting", "micro_hobbies"],
    },
}

# Generic topics used for every domain/subdomain not explicitly listed above.
GENERIC_TOPICS: Dict[str, List[str]] = {
    "device": ["mobile", "desktop", "tablet", "multi_device", "high_end_phone", "low_storage", "low_battery", "camera", "audio", "large_screen"],
    "platform": ["ios", "android", "windows", "macos", "linux", "chrome", "safari", "firefox", "edge", "app_version"],
    "network": ["wifi", "cellular", "low_bandwidth", "high_latency", "offline_usage", "metered_data", "roaming", "vpn", "home_network", "public_network"],
    "channel": ["push", "email", "sms", "in_app", "push_fatigue", "email_engagement", "sms_engagement", "quiet_hours", "digest", "transactional"],
    "media_capability": ["camera", "microphone", "video_upload", "live_stream", "screen_share", "photo_edit", "audio_playback", "captions", "hdr", "low_light"],
    "immersive": ["console_gaming", "pc_gaming", "mobile_gaming", "vr", "ar", "spatial_audio", "controller", "cloud_gaming", "esports_viewing", "immersive_content"],
    "integrations": ["smart_home", "wearable", "car_integration", "voice_assistant", "calendar", "contacts", "photos", "health_app_connection", "payment_wallet", "cloud_storage"],
    "security": ["mfa", "login_risk", "account_recovery", "suspicious_activity", "trusted_device", "passwordless", "security_notifications", "device_lock", "session_age", "privacy_review"],
    "constraints": ["data_saver", "battery_saver", "storage_limit", "older_device", "app_crash", "slow_startup", "accessibility_runtime", "low_memory", "download_limit", "background_refresh"],
    "language": ["primary_language", "secondary_language", "reading", "speaking", "translation", "code_switching", "locale_language", "learning_language", "multilingual", "plain_language"],
    "locale": ["date_format", "time_format", "currency", "measurement_units", "number_format", "timezone", "calendar", "address_format", "name_order", "regional_content"],
    "format": ["short_text", "longform_text", "video", "audio", "visual_summary", "step_by_step", "tables", "examples", "interactive", "downloadable"],
    "tone": ["formal", "casual", "direct", "encouraging", "technical", "plain_language", "humorous", "serious", "concise", "detailed"],
    "density": ["brief", "detailed", "high_density", "low_density", "summary_first", "examples_first", "bullet_points", "narrative", "visual_dense", "slow_paced"],
    "accessibility": ["captions", "screen_reader", "large_text", "low_motion", "high_contrast", "audio_descriptions", "keyboard_navigation", "simplified_language", "color_safe", "transcript"],
    "translation": ["auto_translate", "human_translation", "bilingual_content", "subtitle_translation", "interface_translation", "comment_translation", "search_translation", "creator_translation", "locale_fallback", "language_detection"],
    "medium_role": ["text_creator", "video_creator", "audio_creator", "image_creator", "streamer", "commenter", "curator", "lurker", "teacher", "reviewer"],
    "graph": ["tie_strength", "close_friend_cluster", "family_cluster", "work_cluster", "school_cluster", "local_cluster", "creator_cluster", "interest_cluster", "message_cluster", "event_cluster"],
    "community": ["local_community", "hobby_community", "professional_community", "creator_community", "parent_community", "pet_community", "sports_community", "gaming_community", "learning_community", "marketplace_community"],
    "behavior": ["online_shopper", "in_store_shopper", "marketplace_buyer", "marketplace_seller", "subscription_buyer", "research_before_purchase", "impulse_purchase", "cart_abandonment", "wishlist", "review_reader"],
    "price": ["deal_seeking", "coupon", "premium", "budget", "installment", "free_shipping", "price_comparison", "sale_event", "loyalty_points", "bundle"],
}

PROHIBITED_PATTERNS = [
    "race", "ethnicity", "religion", "sexual_orientation", "political_persuasion", "union_membership",
    "health_diagnosis", "addiction", "pregnancy_inferred", "immigration_status", "criminal_history",
    "disability_inferred", "financial_distress", "sensitive_place_visit", "grief_inferred",
    "divorce_inferred", "bankruptcy_inferred", "mental_health_inferred",
]

SENSITIVE_SUBDOMAINS = {
    "demo": "S2_PRIVATE",
    "life": "S2_PRIVATE",
    "values": "S2_PRIVATE",
}
RESTRICTED_SUBDOMAINS = {
    ("comm", "accessibility"),
    ("privacy", "accessibility"),
    ("privacy", "youth"),
    ("tech", "security"),
    ("geo", "privacy"),
}
OPERATIONAL_DOMAINS = {"gov", "tech", "privacy"}


def slug(s: str) -> str:
    return s.lower().replace("/", "_").replace(" ", "_").replace("-", "_").replace(",", "").replace(".", "")


def topics_for(domain: str, subdomain: str) -> List[str]:
    if domain in TOPICS and subdomain in TOPICS[domain]:
        return TOPICS[domain][subdomain]
    if subdomain in GENERIC_TOPICS:
        return GENERIC_TOPICS[subdomain]
    # Domain-specific broad fallback. These are intentionally generic and safe.
    stems = [
        subdomain, f"{subdomain}_general", f"{subdomain}_preference", f"{subdomain}_activity",
        f"{subdomain}_content", f"{subdomain}_commerce", f"{subdomain}_planning",
        f"{subdomain}_community", f"{subdomain}_tools", f"{subdomain}_events",
        f"{subdomain}_education", f"{subdomain}_local", f"{subdomain}_seasonal",
        f"{subdomain}_creator", f"{subdomain}_consumer", f"{subdomain}_safety",
        f"{subdomain}_privacy", f"{subdomain}_quality", f"{subdomain}_frequency", f"{subdomain}_intent",
    ]
    return [slug(x) for x in stems]


def sensitivity_for(domain: str, subdomain: str, topic: str, relation: str) -> str:
    if (domain, subdomain) in RESTRICTED_SUBDOMAINS:
        return "S3_RESTRICTED"
    if domain in OPERATIONAL_DOMAINS:
        return "S0_OPERATIONAL"
    if domain in SENSITIVE_SUBDOMAINS:
        return SENSITIVE_SUBDOMAINS[domain]
    if "explicit" in relation or "youth" in topic or "accessibility" in topic:
        return "S2_PRIVATE"
    return "S1_STANDARD"


def value_type_for(relation: str) -> str:
    if relation in {"allowed", "enabled", "blocked", "required", "available", "explicit", "eligible", "present", "default", "jurisdictional"}:
        return "boolean"
    if "band" in relation or relation in {"primary", "secondary", "status", "coarse"}:
        return "enum"
    if "at" in relation and relation.endswith("at"):
        return "timestamp"
    return "float"


def half_life_for(domain: str, relation: str) -> Tuple[int | None, int | None]:
    if domain == "gov" or relation in {"allowed", "enabled", "blocked", "required", "available"}:
        return None, None
    if "purchase_window" in relation or "planning_window" in relation or relation == "recency":
        return 14, 90
    if "intent" in relation:
        return 30, 180
    if domain in {"content", "entertainment", "commerce"}:
        return 180, 720
    if domain in {"life"}:
        return 45, 240
    if domain in {"comm", "tech"}:
        return 365, 1095
    return 180, 720


def allowed_uses(sensitivity: str, domain: str, relation: str) -> List[str]:
    if sensitivity == "S0_OPERATIONAL":
        uses = ["delivery", "security", "localization", "product_reliability", "user_controls", "measurement_aggregate"]
        if domain == "tech":
            uses.append("experience_optimization")
        return uses
    if sensitivity == "S1_STANDARD":
        return ["feed_ranking", "recommendation", "ads_personalized_if_consented", "ads_contextual", "measurement_aggregate"]
    if sensitivity == "S2_PRIVATE":
        return ["recommendation_limited", "user_requested_personalization", "measurement_aggregate", "safety_if_relevant"]
    if sensitivity == "S3_RESTRICTED":
        return ["user_requested_utility", "accessibility", "safety", "compliance", "measurement_aggregate_privacy_preserving"]
    return []


def disallowed_uses(sensitivity: str) -> List[str]:
    base = ["protected_trait_inference", "eligibility_decisioning"]
    if sensitivity in {"S2_PRIVATE", "S3_RESTRICTED"}:
        base += ["special_ad_category_targeting", "exploitative_targeting", "lookalike_seed_without_review"]
    if sensitivity == "S3_RESTRICTED":
        base += ["ads_personalized", "third_party_sharing", "broad_activation"]
    return sorted(set(base))


def source_classes(domain: str, sensitivity: str) -> List[str]:
    if domain in {"gov", "privacy"}:
        return ["explicit", "system_state", "policy_engine"]
    if sensitivity == "S3_RESTRICTED":
        return ["explicit", "user_setting", "necessary_system_state"]
    sources = ["explicit", "first_party_engagement", "model_inferred"]
    if domain in {"commerce", "life"}:
        sources += ["search", "commerce"]
    if domain == "geo":
        sources += ["coarse_location"]
    if domain == "social":
        sources += ["social_graph_metadata"]
    return sources


def humanize(s: str) -> str:
    return s.replace("_", " ").strip().capitalize()


def make_facet(domain: str, domain_name: str, subdomain: str, subdomain_name: str, topic: str, relation: str, ordinal: int) -> Dict:
    topic = slug(topic)
    relation = slug(relation)
    facet_id = f"u.{domain}.{subdomain}.{topic}.{relation}"
    sensitivity = sensitivity_for(domain, subdomain, topic, relation)
    half_life, ttl = half_life_for(domain, relation)
    val_type = value_type_for(relation)
    display = f"{humanize(topic)} {humanize(relation).lower()}"
    desc = f"Governed {humanize(relation).lower()} facet for {humanize(topic).lower()} in {subdomain_name.lower()}."
    if any(p in facet_id for p in PROHIBITED_PATTERNS):
        raise ValueError(f"Prohibited facet pattern generated: {facet_id}")
    return {
        "facet_id": facet_id,
        "display_name": display,
        "description": desc,
        "path": [domain, subdomain, topic, relation],
        "domain": domain,
        "domain_name": domain_name,
        "subdomain": subdomain,
        "subdomain_name": subdomain_name,
        "topic": topic,
        "facet_kind": "governed_profile_facet",
        "relationship_type": relation,
        "value_type": val_type,
        "value_range": [0, 1] if val_type == "float" else None,
        "scope": "user",
        "cardinality": "single_value",
        "source_classes": source_classes(domain, sensitivity),
        "confidence_model": "bayesian_decay_v3",
        "decay_model": "exponential_half_life" if half_life else "state_until_changed",
        "half_life_days": half_life,
        "ttl_days": ttl,
        "minimum_evidence_count": 1 if sensitivity in {"S0_OPERATIONAL", "S3_RESTRICTED"} else 3,
        "minimum_confidence_for_activation": 0.55 if sensitivity == "S0_OPERATIONAL" else 0.65 if sensitivity == "S1_STANDARD" else 0.80,
        "sensitivity_class": sensitivity,
        "proxy_risk_class": "P0_LOW" if sensitivity in {"S0_OPERATIONAL", "S1_STANDARD"} else "P2_REVIEW_REQUIRED" if sensitivity == "S2_PRIVATE" else "P3_RESTRICTED",
        "user_visible": False if domain == "gov" and subdomain == "trust_safety" else True,
        "user_editable": True if sensitivity in {"S2_PRIVATE", "S3_RESTRICTED"} or domain in {"comm", "privacy"} else False,
        "user_deletable": True if domain not in {"gov"} else False,
        "allowed_uses": allowed_uses(sensitivity, domain, relation),
        "disallowed_uses": disallowed_uses(sensitivity),
        "explanation_template": f"This facet may be used because of your settings or interactions related to {humanize(topic).lower()}. You can review available controls in profile settings.",
        "policy_notes": "Do not combine with S4 prohibited traits or sensitive-place signals. Enforce purpose limitation before activation.",
        "owner_team": "profile_ontology",
        "review_status": "approved_generated",
        "version": VERSION,
        "ordinal": ordinal,
        "created_at": GENERATED_AT,
        "updated_at": GENERATED_AT,
    }


def generate() -> List[Dict]:
    facets: List[Dict] = []
    ordinal = 0
    for domain, domain_name, domain_budget, subdomains in DOMAIN_SPECS:
        made_domain = 0
        relations = RELATION_PATTERNS[domain]
        for subdomain, subdomain_name, budget in subdomains:
            made = 0
            t = topics_for(domain, subdomain)
            idx = 0
            while made < budget:
                topic = t[idx % len(t)]
                cycle = idx // len(t)
                relation = relations[cycle % len(relations)]
                # If budget exceeds topic*relation combos, add deterministic variant suffixes.
                variant = cycle // len(relations)
                if variant:
                    topic = f"{topic}_v{variant + 1}"
                facet = make_facet(domain, domain_name, subdomain, subdomain_name, topic, relation, ordinal)
                facets.append(facet)
                ordinal += 1
                made += 1
                made_domain += 1
                idx += 1
            assert made == budget, (domain, subdomain, made, budget)
        assert made_domain == domain_budget, (domain, made_domain, domain_budget)
    assert len(facets) == 10000, len(facets)
    ids = [f["facet_id"] for f in facets]
    if len(ids) != len(set(ids)):
        dupes = [x for x, c in Counter(ids).items() if c > 1]
        raise ValueError(f"Duplicate facet IDs: {dupes[:10]}")
    return facets


def write_json_schema_files() -> None:
    OUT_SCHEMAS.mkdir(parents=True, exist_ok=True)
    facet_schema = {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$id": "https://distributed.systems/schemas/consumer_profile_ontology/facet_definition.schema.json",
        "title": "Governed Profile Facet Definition",
        "type": "object",
        "required": ["facet_id", "display_name", "domain", "subdomain", "relationship_type", "value_type", "sensitivity_class", "allowed_uses", "disallowed_uses", "version"],
        "properties": {
            "facet_id": {"type": "string", "pattern": "^u\\.[a-z0-9_]+(\\.[a-z0-9_]+)+$"},
            "display_name": {"type": "string"},
            "description": {"type": "string"},
            "path": {"type": "array", "items": {"type": "string"}},
            "domain": {"type": "string"},
            "subdomain": {"type": "string"},
            "topic": {"type": "string"},
            "relationship_type": {"type": "string"},
            "value_type": {"enum": ["float", "boolean", "enum", "integer", "timestamp", "distribution", "vector_ref"]},
            "value_range": {"type": ["array", "null"], "items": {"type": "number"}},
            "source_classes": {"type": "array", "items": {"type": "string"}},
            "half_life_days": {"type": ["integer", "null"]},
            "ttl_days": {"type": ["integer", "null"]},
            "minimum_confidence_for_activation": {"type": "number", "minimum": 0, "maximum": 1},
            "sensitivity_class": {"enum": ["S0_OPERATIONAL", "S1_STANDARD", "S2_PRIVATE", "S3_RESTRICTED", "S4_PROHIBITED_FOR_INFERENCE_OR_ACTIVATION"]},
            "proxy_risk_class": {"enum": ["P0_LOW", "P1_MONITOR", "P2_REVIEW_REQUIRED", "P3_RESTRICTED"]},
            "user_visible": {"type": "boolean"},
            "user_editable": {"type": "boolean"},
            "user_deletable": {"type": "boolean"},
            "allowed_uses": {"type": "array", "items": {"type": "string"}},
            "disallowed_uses": {"type": "array", "items": {"type": "string"}},
            "explanation_template": {"type": "string"},
            "policy_notes": {"type": "string"},
            "owner_team": {"type": "string"},
            "review_status": {"type": "string"},
            "version": {"type": "string"},
            "created_at": {"type": "string"},
            "updated_at": {"type": "string"},
        },
        "additionalProperties": True,
    }
    value_schema = {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$id": "https://distributed.systems/schemas/consumer_profile_ontology/user_facet_value.schema.json",
        "title": "User Facet Value",
        "type": "object",
        "required": ["user_id", "facet_id", "value", "confidence", "freshness_score", "last_computed_at", "activation_state", "model_version"],
        "properties": {
            "user_id": {"type": "string"},
            "facet_id": {"type": "string"},
            "value": {},
            "confidence": {"type": "number", "minimum": 0, "maximum": 1},
            "freshness_score": {"type": "number", "minimum": 0, "maximum": 1},
            "stability_score": {"type": ["number", "null"], "minimum": 0, "maximum": 1},
            "evidence_count_7d": {"type": "integer", "minimum": 0},
            "evidence_count_30d": {"type": "integer", "minimum": 0},
            "evidence_count_180d": {"type": "integer", "minimum": 0},
            "source_mix": {"type": "object"},
            "last_computed_at": {"type": "string"},
            "expires_at": {"type": ["string", "null"]},
            "activation_state": {"enum": ["eligible", "suppressed", "expired", "blocked", "needs_review"]},
            "model_version": {"type": "string"},
        },
        "additionalProperties": True,
    }
    evidence_schema = {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$id": "https://distributed.systems/schemas/consumer_profile_ontology/evidence_event.schema.json",
        "title": "Facet Evidence Event",
        "type": "object",
        "required": ["event_id", "user_id", "event_type", "event_time", "source_class", "facet_impacts"],
        "properties": {
            "event_id": {"type": "string"},
            "user_id": {"type": "string"},
            "event_type": {"type": "string"},
            "event_time": {"type": "string"},
            "source_class": {"type": "string"},
            "source_system": {"type": "string"},
            "entity_refs": {"type": "array"},
            "facet_impacts": {"type": "array", "items": {"type": "object", "required": ["facet_id", "direction", "strength"]}},
            "privacy_scope": {"type": "string"},
            "retention_class": {"type": "string"},
        },
        "additionalProperties": True,
    }
    activation_schema = {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$id": "https://distributed.systems/schemas/consumer_profile_ontology/activation_request.schema.json",
        "title": "Facet Activation Request",
        "type": "object",
        "required": ["request_id", "user_id", "purpose", "surface", "facet_ids", "jurisdiction", "request_time"],
        "properties": {
            "request_id": {"type": "string"},
            "user_id": {"type": "string"},
            "purpose": {"type": "string"},
            "surface": {"type": "string"},
            "facet_ids": {"type": "array", "items": {"type": "string"}},
            "advertiser_category": {"type": ["string", "null"]},
            "jurisdiction": {"type": "string"},
            "request_time": {"type": "string"},
        },
        "additionalProperties": True,
    }
    for name, schema in {
        "facet_definition.schema.json": facet_schema,
        "user_facet_value.schema.json": value_schema,
        "evidence_event.schema.json": evidence_schema,
        "activation_request.schema.json": activation_schema,
    }.items():
        (OUT_SCHEMAS / name).write_text(json.dumps(schema, indent=2) + "\n")


def write_docs(facets: List[Dict], summaries: Dict) -> None:
    OUT_DOCS.mkdir(parents=True, exist_ok=True)
    readme = f"""# Governed Consumer Profile Ontology

Generated: `{GENERATED_AT}`  
Version: `{VERSION}`  
Leaf facets: **{len(facets):,}**

This package implements a product-grade consumer profile ontology as a governed 10,000-leaf facet taxonomy. It is intentionally not a flat ad-interest list. Each facet carries sensitivity, provenance, decay, allowed uses, disallowed uses, user-control metadata, explanation hooks, and policy notes.

## Artifacts

| Path | Purpose |
|---|---|
| `data/consumer_profile_ontology/facet_definitions.jsonl` | Canonical 10,000 facet definitions, one JSON object per line. |
| `data/consumer_profile_ontology/facet_definitions.csv` | Spreadsheet-friendly facet registry. |
| `data/consumer_profile_ontology/facet_budget.csv` | Domain/subdomain budget allocation. |
| `data/consumer_profile_ontology/summary.json` | Counts by domain, sensitivity class, and value type. |
| `data/consumer_profile_ontology/prohibited_patterns.json` | S4 prohibited inference/activation boundaries. |
| `schemas/consumer_profile_ontology/*.schema.json` | JSON Schemas for definitions, values, evidence, and activation. |
| `scripts/consumer_profile_ontology/generate_ontology.py` | Deterministic generator. |

## Core Invariant

A facet is not a tag. A facet is a modeled user attribute with governance.

Every facet includes:

- `facet_id`
- semantic path
- value type
- source classes
- confidence model
- half-life / TTL where relevant
- sensitivity class
- proxy risk class
- user visibility/editability/deletability
- allowed uses
- disallowed uses
- explanation template
- policy notes

## Sensitivity Classes

| Class | Meaning |
|---|---|
| `S0_OPERATIONAL` | Technical, delivery, consent, security, reliability, localization, or control-plane state. |
| `S1_STANDARD` | Ordinary personalization facets such as broad interests, formats, and non-sensitive commerce categories. |
| `S2_PRIVATE` | Potentially private context or life-stage information. Requires stronger controls. |
| `S3_RESTRICTED` | Explicit/necessary/user-visible restricted data for accessibility, youth safety, precise controls, or security. |
| `S4_PROHIBITED_FOR_INFERENCE_OR_ACTIVATION` | Boundary class. Do not infer or activate for general personalization/targeting. |

## Domain Budget

| Domain | Facets |
|---|---:|
"""
    for domain, count in summaries["counts_by_domain"].items():
        name = next(d[1] for d in DOMAIN_SPECS if d[0] == domain)
        readme += f"| `{domain}` — {name} | {count:,} |\n"
    readme += f"""
| **Total** | **{len(facets):,}** |

## Usage

Regenerate artifacts:

```bash
python3 scripts/consumer_profile_ontology/generate_ontology.py
```

Inspect exact counts:

```bash
jq -r '.counts_by_domain, .counts_by_sensitivity' data/consumer_profile_ontology/summary.json
wc -l data/consumer_profile_ontology/facet_definitions.jsonl
```

Example facet:

```json
{json.dumps(facets[0], indent=2)}
```

## Policy Boundary

The generated ontology intentionally does **not** create activation facets for protected traits, sensitive-place visits, medical diagnosis inference, addiction status, pregnancy inference, immigration status, criminal history, precise financial distress, or other S4 categories.

Those categories are represented only in `prohibited_patterns.json` as governance boundaries.
"""
    (OUT_DOCS / "README.md").write_text(readme)

    policy = """# Policy and Activation Rules

## Purpose-Limited Activation

A facet must never be used merely because it exists. Downstream systems must evaluate:

1. requested purpose,
2. sensitivity class,
3. consent state,
4. jurisdiction,
5. user age/safety state,
6. source class,
7. confidence/freshness,
8. proxy risk,
9. advertiser or campaign category where applicable,
10. user suppression/deletion controls.

## Default Purpose Matrix

| Sensitivity | Ranking | Recommendation | Personalized Ads | Contextual Ads | Measurement | Safety | Eligibility Decisions |
|---|---:|---:|---:|---:|---:|---:|---:|
| S0 Operational | Yes | Sometimes | Usually no | Sometimes | Yes | Yes | Sometimes |
| S1 Standard | Yes | Yes | Yes, if consented | Yes | Aggregate | No unless relevant | No |
| S2 Private | Limited | Limited | Usually no / review | Sometimes | Aggregate only | Sometimes | No |
| S3 Restricted | Strict | Strict | No | No | Privacy-preserving aggregate | Yes if necessary | No |
| S4 Prohibited | No | No | No | No | Fairness audit only | Special systems only | No |

## Non-Negotiable Boundaries

Do not infer or activate general-purpose facets for:

- race or ethnicity,
- religion,
- sexual orientation,
- gender identity beyond explicit product need,
- political persuasion,
- union membership,
- health diagnosis,
- addiction status,
- pregnancy inference,
- immigration status,
- criminal history,
- disability inference,
- precise financial distress,
- sensitive-place visits,
- proxies for the above.

## Sensitive Place Rule

Sensitive-place detection, if implemented, may be used for suppression and safety controls. It must not become an activation signal for targeting.

## User Controls

Every non-operational profile surface should support, where applicable:

- profile view,
- explanation,
- interest deletion/suppression,
- correction of explicit facts,
- ad personalization opt-out,
- partner-data opt-out,
- precise-location opt-out,
- data download,
- data deletion,
- appeal/report.
"""
    (OUT_DOCS / "POLICY.md").write_text(policy)

    architecture = """# Architecture

```text
                    ┌────────────────────┐
                    │  Ontology Registry │
                    │  10,000 facets     │
                    └─────────┬──────────┘
                              │
                              ▼
┌──────────────┐      ┌──────────────────┐      ┌────────────────┐
│ Raw Signals  │ ───▶ │ Evidence Builder │ ───▶ │ Profile Store  │
└──────────────┘      └──────────────────┘      └───────┬────────┘
                                                         │
                                                         ▼
                    ┌────────────────────┐      ┌────────────────┐
                    │ Consent / Privacy  │ ───▶ │ Policy Engine  │
                    └────────────────────┘      └───────┬────────┘
                                                         │
                                                         ▼
         ┌────────────────────────────────────────────────────────┐
         │ Activation: ranking, recs, ads, measurement, safety     │
         └────────────────────────────────────────────────────────┘
                                                         │
                                                         ▼
                    ┌────────────────────┐
                    │ Audit + Explanation│
                    └────────────────────┘
```

## Storage Tables

Recommended tables:

- `facet_definitions`
- `user_facet_values`
- `facet_evidence`
- `consent_grants`
- `activation_audit_log`
- `user_suppressions`
- `policy_decisions`

See the JSON Schemas for concrete object contracts.
"""
    (OUT_DOCS / "ARCHITECTURE.md").write_text(architecture)


def write_outputs(facets: List[Dict]) -> None:
    OUT_DATA.mkdir(parents=True, exist_ok=True)
    OUT_DOCS.mkdir(parents=True, exist_ok=True)
    OUT_SCHEMAS.mkdir(parents=True, exist_ok=True)

    jsonl_path = OUT_DATA / "facet_definitions.jsonl"
    with jsonl_path.open("w") as f:
        for facet in facets:
            f.write(json.dumps(facet, sort_keys=True) + "\n")

    fields = [
        "facet_id", "display_name", "domain", "domain_name", "subdomain", "subdomain_name", "topic",
        "relationship_type", "value_type", "sensitivity_class", "proxy_risk_class", "user_visible",
        "user_editable", "user_deletable", "half_life_days", "ttl_days", "minimum_confidence_for_activation",
        "allowed_uses", "disallowed_uses", "source_classes", "version",
    ]
    with (OUT_DATA / "facet_definitions.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for facet in facets:
            row = {k: facet.get(k) for k in fields}
            for k in ["allowed_uses", "disallowed_uses", "source_classes"]:
                row[k] = ";".join(row[k])
            w.writerow(row)

    with (OUT_DATA / "facet_budget.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["domain", "domain_name", "domain_budget", "subdomain", "subdomain_name", "subdomain_budget"])
        for domain, domain_name, domain_budget, subdomains in DOMAIN_SPECS:
            for subdomain, subdomain_name, budget in subdomains:
                w.writerow([domain, domain_name, domain_budget, subdomain, subdomain_name, budget])

    summaries = {
        "generated_at": GENERATED_AT,
        "version": VERSION,
        "total_facets": len(facets),
        "counts_by_domain": dict(Counter(f["domain"] for f in facets)),
        "counts_by_sensitivity": dict(Counter(f["sensitivity_class"] for f in facets)),
        "counts_by_value_type": dict(Counter(f["value_type"] for f in facets)),
        "counts_by_proxy_risk": dict(Counter(f["proxy_risk_class"] for f in facets)),
        "domain_subdomain_counts": dict(Counter(f"{f['domain']}.{f['subdomain']}" for f in facets)),
        "sha256_facet_definitions_jsonl": hashlib.sha256(jsonl_path.read_bytes()).hexdigest(),
    }
    (OUT_DATA / "summary.json").write_text(json.dumps(summaries, indent=2, sort_keys=True) + "\n")

    prohibited = {
        "sensitivity_class": "S4_PROHIBITED_FOR_INFERENCE_OR_ACTIVATION",
        "note": "These are boundaries, not activation facets. Do not infer or target general-purpose personalization with these attributes or proxies.",
        "patterns": PROHIBITED_PATTERNS,
        "sensitive_place_examples": [
            "clinic", "hospital", "religious_institution", "shelter", "addiction_center", "political_office",
            "protest_site", "immigration_office", "domestic_violence_resource", "legal_aid_center",
        ],
        "vulnerability_examples": [
            "financial_distress", "grief", "loneliness", "mental_health_vulnerability", "addiction_vulnerability",
            "relationship_crisis", "housing_insecurity", "employment_desperation",
        ],
    }
    (OUT_DATA / "prohibited_patterns.json").write_text(json.dumps(prohibited, indent=2, sort_keys=True) + "\n")

    sample_profile = {
        "user_id": "hash:user:example",
        "facets": {
            "u.content.sports.trail_running.affinity": {"value": 0.88, "confidence": 0.89, "freshness_score": 0.84},
            "u.commerce.apparel.footwear.purchase_intent": {"value": 0.72, "confidence": 0.70, "freshness_score": 0.91},
            "u.comm.format.longform_text.preference": {"value": 0.83, "confidence": 0.86, "freshness_score": 0.80},
        },
        "privacy_controls": {
            "ads_personalization_allowed": True,
            "partner_data_allowed": False,
            "precise_location_allowed": False,
        },
    }
    (OUT_DATA / "example_user_profile.json").write_text(json.dumps(sample_profile, indent=2) + "\n")

    write_json_schema_files()
    write_docs(facets, summaries)


def validate_outputs(facets: List[Dict]) -> None:
    assert len(facets) == 10000
    assert sum(d[2] for d in DOMAIN_SPECS) == 10000
    assert not any(f["sensitivity_class"] == "S4_PROHIBITED_FOR_INFERENCE_OR_ACTIVATION" for f in facets)
    by_domain = Counter(f["domain"] for f in facets)
    for domain, _, budget, _ in DOMAIN_SPECS:
        assert by_domain[domain] == budget, (domain, by_domain[domain], budget)
    for f in facets:
        assert f["facet_id"].startswith("u.")
        assert f["allowed_uses"]
        assert "protected_trait_inference" in f["disallowed_uses"]


def main() -> None:
    facets = generate()
    validate_outputs(facets)
    write_outputs(facets)
    summary = json.loads((OUT_DATA / "summary.json").read_text())
    print(json.dumps({
        "generated": True,
        "total_facets": summary["total_facets"],
        "counts_by_domain": summary["counts_by_domain"],
        "counts_by_sensitivity": summary["counts_by_sensitivity"],
        "sha256": summary["sha256_facet_definitions_jsonl"],
    }, indent=2))


if __name__ == "__main__":
    main()
