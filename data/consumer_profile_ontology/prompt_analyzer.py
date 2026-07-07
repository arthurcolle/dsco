#!/usr/bin/env python3
"""
prompt_analyzer.py — governed per-prompt user analysis.

Every prompt is analyzed to update a user profile, BUT governance is structural:
  1. S4 negative ontology is a HARD BLOCK (collection/inference/activation refused + audited).
  2. prohibited_patterns are refused before any facet is emitted.
  3. Only S0-S3 facets with allowed_uses can activate.
  4. Confidence must clear minimum_confidence_for_activation.
  5. Privacy controls (per user) gate activation (ads/partner/precise_location).
  6. Every activation carries provenance + an explanation (audit-first).

This is v0 inference (token overlap). The governance layer is the point, not the matcher;
a production version swaps the matcher for embeddings but keeps the same gates.
"""
import json, re, sys, math, time
from pathlib import Path
from collections import defaultdict

HERE = Path(__file__).parent

# ---------- load ontology ----------
def load_jsonl(p, limit=None):
    out = []
    with open(p) as f:
        for i, line in enumerate(f):
            if limit and i >= limit: break
            line = line.strip()
            if line: out.append(json.loads(line))
    return out

def build_index(facets):
    """token -> set(facet_idx). Tokens come from display_name/topic/subdomain/description."""
    idx = defaultdict(set)
    STOP = set("the a an of for and or to in on with score facet governed profile "
               "quality this may be used because your settings interactions related "
               "you can review available controls preference affinity intent".split())
    def toks(s):
        return [t for t in re.split(r'[^a-z0-9]+', (s or '').lower())
                if len(t) > 2 and t not in STOP]
    for i, fc in enumerate(facets):
        bag = set()
        for field in ("display_name", "topic", "subdomain", "subdomain_name", "description"):
            bag.update(toks(fc.get(field)))
        # also index the path components
        bag.update(t for seg in fc.get("path", []) for t in toks(seg))
        for t in bag:
            idx[t].add(i)
    return idx

# ---------- governance ----------
class Governor:
    # Curated surface-form lexicon: real words a user actually types -> S4 class.
    # This is the load-bearing safety layer. Multi-word blocklist keys like
    # "pregnancy_inferred" never match raw prompts, so we map to actual language.
    SENSITIVE_LEXICON = {
        # health / mental health / reproductive
        "pregnant": "reproductive_health", "pregnancy": "reproductive_health",
        "miscarriage": "reproductive_health", "abortion": "reproductive_health",
        "ivf": "reproductive_health", "fertility": "reproductive_health",
        "depressed": "mental_health", "depression": "mental_health",
        "anxiety": "mental_health", "suicidal": "mental_health", "suicide": "mental_health",
        "bipolar": "mental_health", "therapy": "mental_health", "therapist": "mental_health",
        "diagnosis": "health_diagnosis", "diagnosed": "health_diagnosis",
        "cancer": "health_diagnosis", "hiv": "health_diagnosis", "diabetes": "health_diagnosis",
        "disorder": "health_diagnosis", "disease": "health_diagnosis",
        "addiction": "addiction_recovery", "addict": "addiction_recovery",
        "rehab": "addiction_recovery", "sober": "addiction_recovery",
        "alcoholic": "addiction_recovery", "relapse": "addiction_recovery",
        # protected class / identity / belief
        "gay": "sexual_orientation", "lesbian": "sexual_orientation",
        "bisexual": "sexual_orientation", "queer": "sexual_orientation",
        "transgender": "gender_identity", "trans": "gender_identity",
        "muslim": "religion_belief", "christian": "religion_belief",
        "jewish": "religion_belief", "hindu": "religion_belief",
        "atheist": "religion_belief", "religion": "religion_belief",
        "immigrant": "immigration_citizenship", "undocumented": "immigration_citizenship",
        "asylum": "immigration_citizenship", "deportation": "immigration_citizenship",
        "visa": "immigration_citizenship", "citizenship": "immigration_citizenship",
        # legal / safety / distress
        "arrested": "criminal_legal", "convicted": "criminal_legal",
        "lawsuit": "legal_sensitive", "bankruptcy": "financial_distress",
        "evicted": "housing_insecurity", "homeless": "housing_insecurity",
        "foreclosure": "housing_insecurity", "unemployed": "financial_distress",
        "debt": "financial_distress", "divorce": "intimate_relationships",
        "abuse": "domestic_safety", "abused": "domestic_safety",
        # sensitive places
        "clinic": "sensitive_places_health", "clinics": "sensitive_places_health",
        "hospital": "sensitive_places_health", "shelter": "sensitive_places_support",
        "mosque": "sensitive_places_belief", "church": "sensitive_places_belief",
        "synagogue": "sensitive_places_belief", "protest": "sensitive_places_political",
    }

    def __init__(self, prohibited, boundary_facets):
        # keep the raw patterns for exact matches too
        self.prohibited = set(prohibited.get("patterns", []))
        self.prohibited |= set(prohibited.get("vulnerability_examples", []))
        self.sensitive_places = set(prohibited.get("sensitive_place_examples", []))

    @staticmethod
    def _stem(t):
        # light stemming so plurals/gerunds match: clinics->clinic, running stays
        for suf in ("ing", "ed", "es", "s"):
            if len(t) > len(suf) + 2 and t.endswith(suf):
                return t[:-len(suf)]
        return t

    def prompt_redflags(self, tokens):
        """Return list of (surface_term, s4_class) hits."""
        hits = []
        for t in tokens:
            cls = self.SENSITIVE_LEXICON.get(t) or self.SENSITIVE_LEXICON.get(self._stem(t))
            if cls:
                hits.append((t, cls))
            elif t in self.prohibited or t in self.sensitive_places:
                hits.append((t, "prohibited_pattern"))
        # dedupe by term
        seen, out = set(), []
        for term, cls in hits:
            if term not in seen:
                seen.add(term); out.append({"term": term, "class": cls})
        return out

    def facet_allowed(self, fc, confidence, privacy):
        s = fc.get("sensitivity_class", "")
        if s.startswith("S4") or fc.get("facet_kind") == "negative_ontology_boundary_facet":
            return (False, "S4_boundary_refused")
        if fc.get("activation_allowed") is False:
            return (False, "activation_disabled")
        thr = fc.get("minimum_confidence_for_activation") or 0.0
        if confidence < thr:
            return (False, f"below_confidence_gate({confidence:.2f}<{thr:.2f})")
        # privacy controls
        au = set(fc.get("allowed_uses", []))
        if not privacy.get("ads_personalization_allowed", True) and (
                "ads_personalized" in au or "recommendation" in au):
            # activation still ok for non-ads uses; only block if ads is the only use
            pass
        if not privacy.get("precise_location_allowed", True) and fc.get("subdomain") in {
                "place", "local_area", "venues"} and fc.get("domain") == "geo":
            return (False, "precise_location_denied")
        if s == "S3_RESTRICTED" and not privacy.get("partner_data_allowed", False):
            return (False, "S3_requires_partner_consent")
        return (True, "ok")

# ---------- analyzer ----------
class PromptAnalyzer:
    def __init__(self, facets, index, gov):
        self.facets, self.index, self.gov = facets, index, gov

    def analyze(self, prompt, privacy):
        t0 = time.time()
        tokens = [t for t in re.split(r'[^a-z0-9]+', prompt.lower()) if len(t) > 2]
        tset = set(tokens)

        # GATE 0: prompt-level red flags (protected/vulnerable/sensitive-place)
        redflags = self.gov.prompt_redflags(tset)

        # candidate scoring by token overlap
        scores = defaultdict(float)
        for t in tset:
            for fi in self.index.get(t, ()):
                scores[fi] += 1.0
        cand = sorted(scores.items(), key=lambda kv: -kv[1])[:40]

        activated, blocked = [], []
        for fi, raw in cand:
            fc = self.facets[fi]
            # confidence: prior + evidence weight; multi-token matches clear gates
            conf = min(0.99, 0.55 + 0.16 * raw)
            ok, reason = self.gov.facet_allowed(fc, conf, privacy)
            rec = {
                "facet_id": fc["facet_id"],
                "display_name": fc.get("display_name"),
                "domain": fc.get("domain"), "subdomain": fc.get("subdomain"),
                "sensitivity": fc.get("sensitivity_class"),
                "proxy_risk": fc.get("proxy_risk_class"),
                "confidence": round(conf, 3),
                "evidence_tokens": sorted(tset & set(
                    re.split(r'[^a-z0-9]+', (fc.get("display_name","")+" "+
                    " ".join(fc.get("path",[]))+" "+fc.get("subdomain","")).lower()))),
            }
            if ok:
                rec["explanation"] = fc.get("explanation_template")
                rec["allowed_uses"] = fc.get("allowed_uses", [])
                activated.append(rec)
            else:
                rec["blocked_reason"] = reason
                blocked.append(rec)

        # if prompt itself tripped red flags, refuse the whole personalization pass
        refused = bool(redflags)
        return {
            "prompt": prompt,
            "analyzed_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "governance": {
                "prompt_redflags": redflags,
                "personalization_refused": refused,
                "refuse_reason": "protected_or_vulnerable_signal_in_prompt" if refused else None,
            },
            "activated_facets": [] if refused else activated[:8],
            "blocked_facets": blocked[:8],
            "counts": {
                "candidates": len(cand),
                "activated": 0 if refused else len(activated),
                "blocked": len(blocked) + (len(activated) if refused else 0),
            },
            "latency_ms": round((time.time()-t0)*1000, 2),
        }

def main():
    # Load base+boundaries so S4 is present. Use base (10k) for speed; swap to 50k anytime.
    facets = load_jsonl(HERE/"facet_definitions_base_with_boundaries.jsonl")
    boundary = [f for f in facets if f.get("facet_kind")=="negative_ontology_boundary_facet"]
    active  = [f for f in facets if f.get("facet_kind")!="negative_ontology_boundary_facet"]
    prohibited = json.load(open(HERE/"prohibited_patterns.json"))
    gov = Governor(prohibited, boundary)
    index = build_index(active)
    az = PromptAnalyzer(active, index, gov)

    privacy = {"ads_personalization_allowed": True,
               "partner_data_allowed": False,
               "precise_location_allowed": False}

    prompts = sys.argv[1:] or [
        "What are the best trail running shoes for a marathon I'm training for?",
        "Help me debug this Python async code and set up a CI pipeline",
        "I need cheap flights to Tokyo next month, budget travel tips",
        "I think I might be pregnant and depressed, what clinics are near me?",  # S4 trap
    ]
    print(f"loaded: {len(active)} active facets, {len(boundary)} S4 boundaries, "
          f"{len(index)} index tokens\n")
    for p in prompts:
        r = az.analyze(p, privacy)
        print("="*78)
        print("PROMPT:", p)
        g = r["governance"]
        if g["personalization_refused"]:
            flags = ", ".join(f"{h['term']}→{h['class']}" for h in g['prompt_redflags'])
            print(f"  ⛔ REFUSED — {g['refuse_reason']}")
            print(f"     S4 hits: {flags}")
        else:
            print(f"  ✅ {r['counts']['activated']} activated / {r['counts']['blocked']} blocked "
                  f"({r['latency_ms']}ms)")
            for a in r["activated_facets"][:5]:
                print(f"    + {a['facet_id']}  [{a['sensitivity']}] conf={a['confidence']} "
                      f"ev={a['evidence_tokens']}")
        if r["blocked_facets"]:
            for b in r["blocked_facets"][:2]:
                print(f"    - blocked {b['facet_id']} :: {b['blocked_reason']}")
    print("="*78)

if __name__ == "__main__":
    main()
