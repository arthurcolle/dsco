#!/usr/bin/env python3
"""Stable, raw-text-free features for the Chimera model router.

The router deliberately does not tokenize prompts with a model-specific
tokenizer.  Task text is mapped into a fixed-width signed feature hash, while
model names, descriptions, and structured catalog fields are mapped into a
small set of numeric capabilities plus another signed feature hash.  A model
added years after training can therefore be scored without changing the input
shape.  Full model IDs are never hashed as a single identity feature.

Only Python's standard library and NumPy are required.  The hash contract and
all numeric transforms are versioned constants; do not replace ``blake2b`` or
change namespaces without incrementing the corresponding version.
"""

from __future__ import annotations

import hashlib
import math
import re
import unicodedata
from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple

import numpy as np


DEFAULT_TASK_DIM = 512
DEFAULT_MODEL_DIM = 128
TASK_RESERVED = 32
MODEL_RESERVED = 50
TASK_FEATURE_VERSION = "chimera-task-blake2b-v1"
MODEL_FEATURE_VERSION = "chimera-model-semantic-v2"
MODEL_FAMILY_VERSION = "chimera-model-family-v1"
# These columns come directly from OpenRouter Artificial Analysis values.
# Auxiliary benchmark supervision must call model_features(...,
# include_benchmark_priors=False), which zeros all four columns.
MODEL_BENCHMARK_DERIVED_INDICES: Tuple[int, ...] = (36, 37, 38, 39)

_WORD_RE = re.compile(r"[\w]+(?:[./:+-][\w]+)*|[^\w\s]", re.UNICODE)
_URL_RE = re.compile(r"(?:https?://|www\.)", re.IGNORECASE)
_PATH_RE = re.compile(r"(?:^|\s)(?:~?/|\.{1,2}/|[A-Za-z]:[\\/])")
_SHELL_RE = re.compile(
    r"(?:^|\s)(?:sudo|bash|zsh|sh|make|cmake|npm|pnpm|yarn|pip|python|cargo|git|curl|ssh)(?:\s|$)",
    re.IGNORECASE,
)
_ACTION_RE = re.compile(
    r"\b(?:build|check|debug|deploy|fix|implement|inspect|open|repair|run|search|test|train|verify)\b",
    re.IGNORECASE,
)
_CURRENT_RE = re.compile(
    r"\b(?:current|currently|latest|live|newest|now|today|up[- ]to[- ]date)\b",
    re.IGNORECASE,
)
_CODE_RE = re.compile(
    r"\b(?:api|class|compile|database|function|html|javascript|json|python|repo|rust|sql|typescript)\b",
    re.IGNORECASE,
)
_MATH_RE = re.compile(
    r"(?:\\(?:frac|sum|mathbb|mathbf)|\$[^$]+\$|\b(?:derive|equation|matrix|probability|theorem)\b)",
    re.IGNORECASE,
)
_IMAGE_RE = re.compile(
    r"\b(?:diagram|figure|image|photo|render|screenshot|visual)\b", re.IGNORECASE
)
_RISK_RE = re.compile(
    r"\b(?:credential|delete|password|production|secret|token|wipe)\b", re.IGNORECASE
)
_IMPERATIVE_RE = re.compile(
    r"^\s*(?:add|build|check|create|debug|define|deploy|explain|find|fix|implement|inspect|make|open|repair|run|show|test|train|use|verify)\b",
    re.IGNORECASE,
)

# Split version/name punctuation so a display name like ``gpt-5.6-sol`` never
# enters the feature hash as one identity token.  Individual words and local
# bigrams retain useful semantics without recreating a closed model-ID enum.
_MODEL_WORD_RE = re.compile(r"[a-z0-9]+", re.IGNORECASE)
_MODEL_VARIANT_TOKENS = frozenset(
    {
        "batch",
        "beta",
        "chat",
        "extended",
        "fast",
        "free",
        "highspeed",
        "instruct",
        "latest",
        "online",
        "preview",
        "pro",
        "reasoning",
        "turbo",
    }
)
_MODEL_SIZE_RE = re.compile(r"^(?:a)?\d+(?:[.]\d+)?[bmt]$", re.IGNORECASE)
_MODEL_DATE_RE = re.compile(r"^(?:19|20)\d{2}(?:\d{2})?(?:\d{2})?$")
_MODEL_QUANT_RE = re.compile(
    r"^(?:bf16|fp\d+|int\d+|q\d+(?:_[a-z0-9]+)?|awq|gptq|gguf)$", re.IGNORECASE
)

# Coarse, versioned semantic concepts deliberately generalize across vendors.
# They are extracted from display name + description, then feature-hashed; the
# original strings and a vocabulary are never emitted.
_MODEL_SEMANTIC_PATTERNS: Tuple[Tuple[str, re.Pattern[str]], ...] = (
    ("agentic", re.compile(r"\b(?:agent|agentic|autonomous|workflow)\b", re.I)),
    ("audio", re.compile(r"\b(?:audio|speech|voice|transcri)\w*\b", re.I)),
    ("coding", re.compile(r"\b(?:code|coding|program|software|developer)\w*\b", re.I)),
    ("creative", re.compile(r"\b(?:creative|story|writing|roleplay)\w*\b", re.I)),
    (
        "fast",
        re.compile(r"\b(?:fast|latency|real[- ]?time|speed|throughput)\w*\b", re.I),
    ),
    (
        "instruction",
        re.compile(r"\b(?:chat|instruct|instruction|assistant)\w*\b", re.I),
    ),
    ("long_context", re.compile(r"\b(?:context|long[- ]?context|document)\w*\b", re.I)),
    (
        "math_science",
        re.compile(r"\b(?:math|science|stem|physics|research)\w*\b", re.I),
    ),
    ("multilingual", re.compile(r"\b(?:language|multilingual|translation)\w*\b", re.I)),
    (
        "reasoning",
        re.compile(r"\b(?:reason|reasoning|thinking|logic|solve)\w*\b", re.I),
    ),
    ("retrieval", re.compile(r"\b(?:grounded|retrieval|search|web)\w*\b", re.I)),
    ("safety", re.compile(r"\b(?:moderation|safe|safety|secure)\w*\b", re.I)),
    (
        "small_efficient",
        re.compile(r"\b(?:compact|edge|efficient|lightweight|small)\w*\b", re.I),
    ),
    (
        "structured",
        re.compile(r"\b(?:json|schema|structured|tool[- ]?call)\w*\b", re.I),
    ),
    ("video", re.compile(r"\b(?:video|temporal|frame)\w*\b", re.I)),
    ("vision", re.compile(r"\b(?:image|multimodal|vision|visual)\w*\b", re.I)),
)


TASK_NUMERIC_NAMES: Tuple[str, ...] = (
    "numeric:bias",
    "numeric:char_count_log1p_div10",
    "numeric:utf8_byte_count_log1p_div10",
    "numeric:word_count_log1p_div8",
    "numeric:line_count_log1p_div5",
    "numeric:digit_ratio",
    "numeric:whitespace_ratio",
    "numeric:punctuation_ratio",
    "numeric:uppercase_letter_ratio",
    "numeric:newline_ratio",
    "numeric:code_punctuation_ratio",
    "numeric:question_count_log1p_div4",
    "numeric:exclamation_count_log1p_div4",
    "bool:contains_url",
    "bool:contains_code_fence",
    "bool:json_like",
    "bool:shell_like",
    "bool:path_like",
    "bool:action_request",
    "bool:math_task",
    "bool:image_task",
    "bool:code_task",
    "numeric:non_ascii_ratio",
    "numeric:utf8_byte_entropy_div8",
    "numeric:unique_word_ratio",
    "numeric:max_line_length_log1p_div10",
    "numeric:mean_word_length_div12",
    "bool:imperative_opening",
    "bool:time_sensitive",
    "bool:risk_sensitive",
    "numeric:number_token_count_log1p_div5",
    "numeric:colon_ratio",
)


MODEL_NUMERIC_NAMES: Tuple[str, ...] = (
    "numeric:bias",
    "bool:catalog_match",
    "numeric:context_length_log2_div24",
    "numeric:max_completion_tokens_log2_div20",
    "bool:input_text",
    "bool:input_image",
    "bool:input_audio",
    "bool:input_video",
    "bool:input_file",
    "bool:output_text",
    "bool:output_image",
    "bool:output_audio",
    "bool:supports_tools",
    "bool:supports_reasoning",
    "bool:supports_structured_outputs",
    "bool:supports_response_format",
    "bool:supports_seed",
    "bool:supports_logprobs",
    "bool:supports_top_logprobs",
    "bool:supports_frequency_penalty",
    "bool:supports_stop",
    "bool:supports_temperature",
    "bool:supports_top_p",
    "bool:is_moderated",
    "bool:has_prompt_price",
    "bool:has_completion_price",
    "bool:free_prompt",
    "bool:free_completion",
    "numeric:prompt_usd_per_million_log1p",
    "numeric:completion_usd_per_million_log1p",
    "numeric:image_price_log1p",
    "numeric:audio_price_log1p",
    "numeric:web_search_price_milli_log1p",
    "numeric:cache_read_usd_per_million_log1p",
    "numeric:cache_write_usd_per_million_log1p",
    "numeric:reasoning_usd_per_million_log1p",
    "numeric:benchmark_intelligence_div100",
    "numeric:benchmark_coding_div100",
    "numeric:benchmark_agentic_div100",
    "bool:has_benchmark",
    "bool:reasoning_mandatory",
    "bool:reasoning_default_enabled",
    "bool:batch_variant",
    "bool:preview_or_beta",
    "numeric:created_unix_div2e9",
    "numeric:knowledge_cutoff_year_div2100",
    "bool:has_expiration_date",
    "bool:has_per_request_limits",
    "bool:has_tokenizer_name",
    "numeric:model_path_depth_div4",
)


def _as_text(value: Any) -> str:
    return "" if value is None else str(value)


def _as_float(value: Any, default: float = 0.0) -> float:
    try:
        number = float(value)
    except (TypeError, ValueError, OverflowError):
        return default
    return number if math.isfinite(number) else default


def _as_bool(value: Any) -> float:
    if isinstance(value, str):
        return 1.0 if value.strip().lower() in {"1", "true", "yes", "on"} else 0.0
    return 1.0 if bool(value) else 0.0


def _mapping(value: Any) -> Mapping[str, Any]:
    return value if isinstance(value, Mapping) else {}


def _string_list(value: Any) -> List[str]:
    if isinstance(value, str):
        return [value]
    if not isinstance(value, Sequence):
        return []
    return [_as_text(item) for item in value if item is not None]


def _normalise_text(text: str) -> str:
    return unicodedata.normalize("NFKC", text).casefold()


def _digest64(namespace: str, token: str) -> int:
    """Return a platform-independent unsigned 64-bit hash."""

    payload = (namespace + "\x1f" + token).encode("utf-8", "surrogatepass")
    return int.from_bytes(hashlib.blake2b(payload, digest_size=8).digest(), "little")


def _hashed_add(
    vector: np.ndarray, start: int, namespace: str, token: str, weight: float = 1.0
) -> None:
    width = int(vector.shape[0]) - start
    if width <= 0 or not token:
        return
    digest = _digest64(namespace, token)
    bucket = start + (digest % width)
    sign = 1.0 if ((digest >> 63) & 1) == 0 else -1.0
    vector[bucket] += np.float32(sign * weight)


def _finish_hash_block(vector: np.ndarray, start: int) -> np.ndarray:
    block = vector[start:]
    if block.size:
        block[:] = np.sign(block) * np.log1p(np.abs(block))
        norm = float(np.linalg.norm(block))
        if norm > 0.0:
            block /= np.float32(norm)
    return vector


def _byte_entropy(text: str) -> float:
    raw = text.encode("utf-8", "surrogatepass")
    if not raw:
        return 0.0
    counts = np.bincount(np.frombuffer(raw, dtype=np.uint8), minlength=256)
    probabilities = counts[counts > 0].astype(np.float64) / float(len(raw))
    return float(-np.sum(probabilities * np.log2(probabilities)))


def task_feature_names(dim: int = DEFAULT_TASK_DIM) -> List[str]:
    if dim < TASK_RESERVED:
        raise ValueError("task feature dimension must be at least %d" % TASK_RESERVED)
    names = list(TASK_NUMERIC_NAMES)
    names.extend("hash:task:%d" % i for i in range(dim - TASK_RESERVED))
    return names


def model_feature_names(dim: int = DEFAULT_MODEL_DIM) -> List[str]:
    if dim < MODEL_RESERVED:
        raise ValueError("model feature dimension must be at least %d" % MODEL_RESERVED)
    names = list(MODEL_NUMERIC_NAMES)
    names.extend("hash:model_descriptor:%d" % i for i in range(dim - MODEL_RESERVED))
    return names


def task_features(prompt: str, dim: int = DEFAULT_TASK_DIM) -> np.ndarray:
    """Map prompt text to a deterministic fixed-width float32 vector.

    The prompt itself and a token dictionary are never returned.  Hashed
    lexical features use the following immutable namespaces:
    ``task.word.v1``, ``task.bigram.v1``, ``task.trigram.v1``,
    ``task.prefix.v1``, ``task.suffix.v1``, and ``task.charclass.v1``.
    """

    if dim < TASK_RESERVED:
        raise ValueError("task feature dimension must be at least %d" % TASK_RESERVED)
    if not isinstance(prompt, str):
        prompt = _as_text(prompt)

    vector = np.zeros(dim, dtype=np.float32)
    normalised = _normalise_text(prompt)
    chars = len(prompt)
    raw_bytes = prompt.encode("utf-8", "surrogatepass")
    safe_chars = max(chars, 1)
    word_tokens = [token for token in _WORD_RE.findall(normalised) if token.strip()]
    lexical_words = [
        token for token in word_tokens if any(ch.isalnum() for ch in token)
    ]
    lines = prompt.splitlines() or [prompt]
    letters = [ch for ch in prompt if ch.isalpha()]
    punctuation = sum(1 for ch in prompt if unicodedata.category(ch).startswith("P"))
    code_punctuation = sum(prompt.count(ch) for ch in "{}[]()<>=;`|&")
    non_ascii = sum(1 for ch in prompt if ord(ch) > 127)
    number_tokens = sum(
        1 for token in lexical_words if any(ch.isdigit() for ch in token)
    )
    unique_words = len(set(lexical_words))
    mean_word_length = (
        sum(len(token) for token in lexical_words) / float(len(lexical_words))
        if lexical_words
        else 0.0
    )

    vector[:TASK_RESERVED] = np.asarray(
        [
            1.0,
            math.log1p(chars) / 10.0,
            math.log1p(len(raw_bytes)) / 10.0,
            math.log1p(len(lexical_words)) / 8.0,
            math.log1p(len(lines)) / 5.0,
            sum(ch.isdigit() for ch in prompt) / float(safe_chars),
            sum(ch.isspace() for ch in prompt) / float(safe_chars),
            punctuation / float(safe_chars),
            (sum(ch.isupper() for ch in letters) / float(max(len(letters), 1))),
            prompt.count("\n") / float(safe_chars),
            code_punctuation / float(safe_chars),
            math.log1p(prompt.count("?")) / 4.0,
            math.log1p(prompt.count("!")) / 4.0,
            float(bool(_URL_RE.search(prompt))),
            float("```" in prompt),
            float(
                ("{" in prompt and "}" in prompt) or ("[" in prompt and "]" in prompt)
            ),
            float(bool(_SHELL_RE.search(prompt))),
            float(bool(_PATH_RE.search(prompt))),
            float(bool(_ACTION_RE.search(prompt))),
            float(bool(_MATH_RE.search(prompt))),
            float(bool(_IMAGE_RE.search(prompt))),
            float(bool(_CODE_RE.search(prompt))),
            non_ascii / float(safe_chars),
            _byte_entropy(prompt) / 8.0,
            unique_words / float(max(len(lexical_words), 1)),
            math.log1p(max((len(line) for line in lines), default=0)) / 10.0,
            mean_word_length / 12.0,
            float(bool(_IMPERATIVE_RE.search(prompt))),
            float(bool(_CURRENT_RE.search(prompt))),
            float(bool(_RISK_RE.search(prompt))),
            math.log1p(number_tokens) / 5.0,
            prompt.count(":") / float(safe_chars),
        ],
        dtype=np.float32,
    )

    # Bound work on pasted files while retaining both the instruction prefix
    # and likely payload tail.  Scalar length features above still use all text.
    capped_tokens = word_tokens[:2048]
    if len(word_tokens) > 2304:
        capped_tokens.extend(word_tokens[-256:])
    for token in capped_tokens:
        _hashed_add(vector, TASK_RESERVED, "task.word.v1", token)
        if len(token) >= 4:
            _hashed_add(vector, TASK_RESERVED, "task.prefix.v1", token[:4], 0.5)
            _hashed_add(vector, TASK_RESERVED, "task.suffix.v1", token[-4:], 0.5)
    for left, right in zip(capped_tokens, capped_tokens[1:]):
        _hashed_add(
            vector, TASK_RESERVED, "task.bigram.v1", left + "\x1e" + right, 0.75
        )
    for first, second, third in zip(
        capped_tokens, capped_tokens[1:], capped_tokens[2:]
    ):
        _hashed_add(
            vector,
            TASK_RESERVED,
            "task.trigram.v1",
            first + "\x1e" + second + "\x1e" + third,
            0.35,
        )

    char_classes = "".join(
        "a" if ch.isalpha() else "0" if ch.isdigit() else "_" if ch.isspace() else ch
        for ch in normalised[:4096]
    )
    for index in range(max(0, len(char_classes) - 4)):
        _hashed_add(
            vector,
            TASK_RESERVED,
            "task.charclass.v1",
            char_classes[index : index + 5],
            0.15,
        )
    return _finish_hash_block(vector, TASK_RESERVED)


def _price(pricing: Mapping[str, Any], key: str) -> float:
    return max(0.0, _as_float(pricing.get(key), 0.0))


def _known_price(pricing: Mapping[str, Any], key: str) -> Optional[float]:
    """Return a finite non-negative price only when the key is explicit."""

    if key not in pricing or pricing.get(key) is None:
        return None
    try:
        value = float(pricing.get(key))
    except (TypeError, ValueError, OverflowError):
        return None
    if not math.isfinite(value) or value < 0.0:
        return None
    return value


def _price_per_million_feature(price_per_token: float) -> float:
    return math.log1p(max(0.0, price_per_token) * 1_000_000.0)


def _nested(mapping: Mapping[str, Any], *keys: str) -> Any:
    current: Any = mapping
    for key in keys:
        if not isinstance(current, Mapping):
            return None
        current = current.get(key)
    return current


def _benchmark_value(model: Mapping[str, Any], key: str) -> float:
    value = _nested(model, "benchmarks", "artificial_analysis", key)
    return max(0.0, _as_float(value, 0.0))


def _knowledge_year(value: Any) -> float:
    match = re.search(r"(?:19|20)\d{2}", _as_text(value))
    return float(match.group(0)) if match else 0.0


def canonical_model_id(model_id: str) -> str:
    """Canonicalize only transport decoration, never invent a vendor alias."""

    value = _as_text(model_id).strip()
    while value.casefold().startswith("openrouter/"):
        value = value[len("openrouter/") :]
    value = re.sub(r"\[re:[^\]]+\]$", "", value, flags=re.IGNORECASE)
    return value.strip()


def _model_text_tokens(value: Any, limit: int) -> List[str]:
    normalised = _normalise_text(_as_text(value))
    return _MODEL_WORD_RE.findall(normalised)[:limit]


def _family_slug_tokens(slug: str) -> List[str]:
    tokens = [
        token
        for token in re.split(r"[^a-z0-9]+", slug.casefold())
        if token
        and token not in _MODEL_VARIANT_TOKENS
        and not _MODEL_SIZE_RE.match(token)
        and not _MODEL_DATE_RE.match(token)
        and not _MODEL_QUANT_RE.match(token)
    ]
    return tokens


def model_family_id(model_id: str) -> str:
    """Return a deterministic coarse family key for cold-start evaluation.

    This is intentionally an evaluation heuristic, not a routing alias.  It
    groups common vendor variants without claiming that family members are
    interchangeable.  Unknown future slugs fall back to their first three
    stable lexical components.
    """

    canonical = canonical_model_id(model_id).casefold().lstrip("~")
    provider = canonical.split("/", 1)[0] if "/" in canonical else "unknown"
    slug = canonical.split("/", 1)[1] if "/" in canonical else canonical
    tokens = _family_slug_tokens(slug)
    if not tokens:
        return provider + "/unknown"

    roles = ("haiku", "sonnet", "opus", "fable")
    if "claude" in tokens:
        role = next((item for item in roles if item in tokens), None)
        return provider + "/claude" + ("-" + role if role else "")

    first = tokens[0]
    versioned_prefixes = ("gemini", "glm", "gpt", "grok", "llama")
    if first in versioned_prefixes:
        family = [first]
        for token in tokens[1:]:
            if token.isdigit() and len(family) < 3:
                family.append(token)
            else:
                break
        return provider + "/" + "-".join(family)

    # Qwen and Kimi often join the generation number to the brand token.
    if re.match(r"^(?:qwen|kimi|deepseek)[a-z]*\d*$", first):
        family = [first]
        if len(tokens) > 1 and tokens[1].isdigit():
            family.append(tokens[1])
        return provider + "/" + "-".join(family)

    return provider + "/" + "-".join(tokens[:3])


def _price_tier(price_per_token: Optional[float]) -> str:
    if price_per_token is None:
        return "unknown"
    per_million = price_per_token * 1_000_000.0
    if per_million == 0.0:
        return "free"
    if per_million <= 0.10:
        return "micro"
    if per_million <= 1.0:
        return "low"
    if per_million <= 5.0:
        return "medium"
    if per_million <= 20.0:
        return "high"
    return "premium"


def model_features(
    model: Mapping[str, Any],
    dim: int = DEFAULT_MODEL_DIM,
    requested_model_id: Optional[str] = None,
    catalog_match: bool = True,
    include_benchmark_priors: bool = True,
) -> np.ndarray:
    """Map one OpenRouter-style model descriptor to a stable vector.

    ``requested_model_id`` is used for an unknown/future model or when a local
    alias resolved to a catalog entry.  Descriptors may contain additional
    fields; unknown fields are intentionally ignored until a feature-version
    change.  Set ``include_benchmark_priors=False`` when these features are
    used to predict the catalog's own benchmark values; this suppresses all
    four label-derived columns and prevents direct target leakage.
    """

    if dim < MODEL_RESERVED:
        raise ValueError("model feature dimension must be at least %d" % MODEL_RESERVED)
    if not isinstance(model, Mapping):
        model = {}

    model_id = canonical_model_id(
        _as_text(model.get("id") or requested_model_id or "unknown")
    )
    architecture = _mapping(model.get("architecture"))
    pricing = _mapping(model.get("pricing"))
    top_provider = _mapping(model.get("top_provider"))
    reasoning = _mapping(model.get("reasoning"))
    default_parameters = _mapping(model.get("default_parameters"))
    supported = {
        item.casefold() for item in _string_list(model.get("supported_parameters"))
    }
    input_modalities = {
        item.casefold() for item in _string_list(architecture.get("input_modalities"))
    }
    output_modalities = {
        item.casefold() for item in _string_list(architecture.get("output_modalities"))
    }

    # Older catalog snapshots sometimes only expose architecture.modality.
    modality = _as_text(architecture.get("modality")).casefold()
    if not input_modalities and "->" in modality:
        input_modalities.update(
            part.strip() for part in modality.split("->", 1)[0].split("+")
        )
    if not output_modalities and "->" in modality:
        output_modalities.update(
            part.strip() for part in modality.split("->", 1)[1].split("+")
        )

    context_length = max(
        0.0,
        _as_float(model.get("context_length"), 0.0),
        _as_float(top_provider.get("context_length"), 0.0),
    )
    max_completion = max(0.0, _as_float(top_provider.get("max_completion_tokens"), 0.0))
    known_prompt_price = _known_price(pricing, "prompt")
    known_completion_price = _known_price(pricing, "completion")
    prompt_price = known_prompt_price if known_prompt_price is not None else 0.0
    completion_price = (
        known_completion_price if known_completion_price is not None else 0.0
    )
    intelligence = (
        _benchmark_value(model, "intelligence_index")
        if include_benchmark_priors
        else 0.0
    )
    coding = (
        _benchmark_value(model, "coding_index") if include_benchmark_priors else 0.0
    )
    agentic = (
        _benchmark_value(model, "agentic_index") if include_benchmark_priors else 0.0
    )
    has_benchmark = include_benchmark_priors and any(
        value > 0.0 for value in (intelligence, coding, agentic)
    )
    tokenizer = _as_text(architecture.get("tokenizer"))
    display_name = _as_text(model.get("name"))
    description = _as_text(model.get("description"))
    lowered_id = model_id.casefold()

    vector = np.zeros(dim, dtype=np.float32)
    vector[:MODEL_RESERVED] = np.asarray(
        [
            1.0,
            float(catalog_match),
            (math.log2(context_length) / 24.0) if context_length > 0 else 0.0,
            (math.log2(max_completion) / 20.0) if max_completion > 0 else 0.0,
            float("text" in input_modalities),
            float("image" in input_modalities),
            float("audio" in input_modalities),
            float("video" in input_modalities),
            float("file" in input_modalities),
            float("text" in output_modalities),
            float("image" in output_modalities),
            float("audio" in output_modalities),
            float("tools" in supported or "tool_choice" in supported),
            float(
                "reasoning" in supported
                or "include_reasoning" in supported
                or bool(reasoning)
            ),
            float("structured_outputs" in supported),
            float("response_format" in supported),
            float("seed" in supported),
            float("logprobs" in supported),
            float("top_logprobs" in supported),
            float("frequency_penalty" in supported),
            float("stop" in supported),
            float("temperature" in supported),
            float("top_p" in supported),
            _as_bool(top_provider.get("is_moderated")),
            float(known_prompt_price is not None),
            float(known_completion_price is not None),
            float(
                known_prompt_price == 0.0 if known_prompt_price is not None else False
            ),
            float(
                known_completion_price == 0.0
                if known_completion_price is not None
                else False
            ),
            _price_per_million_feature(prompt_price),
            _price_per_million_feature(completion_price),
            math.log1p(_price(pricing, "image")),
            math.log1p(_price(pricing, "audio")),
            math.log1p(_price(pricing, "web_search") * 1000.0),
            _price_per_million_feature(_price(pricing, "input_cache_read")),
            _price_per_million_feature(_price(pricing, "input_cache_write")),
            _price_per_million_feature(_price(pricing, "internal_reasoning")),
            intelligence / 100.0,
            coding / 100.0,
            agentic / 100.0,
            float(has_benchmark),
            _as_bool(reasoning.get("mandatory")),
            _as_bool(reasoning.get("default_enabled")),
            float(lowered_id.endswith(":batch")),
            float("preview" in lowered_id or "beta" in lowered_id),
            max(0.0, _as_float(model.get("created"), 0.0)) / 2_000_000_000.0,
            _knowledge_year(model.get("knowledge_cutoff")) / 2100.0,
            float(bool(model.get("expiration_date"))),
            float(bool(model.get("per_request_limits"))),
            float(bool(tokenizer)),
            min(4, model_id.count("/") + 1) / 4.0,
        ],
        dtype=np.float32,
    )

    provider = model_id.split("/", 1)[0] if "/" in model_id else "unknown"
    family = model_family_id(model_id)
    categorical: List[Tuple[str, str, float]] = [
        ("model.provider.v1", provider.casefold(), 1.0),
        ("model.family.v2", family, 0.8),
        ("model.tokenizer.v1", tokenizer.casefold(), 0.8),
        ("model.modality.v1", modality, 0.8),
        (
            "model.instruct.v1",
            _as_text(architecture.get("instruct_type")).casefold(),
            0.7,
        ),
    ]
    for segment in re.split(r"[/_.:-]+", lowered_id):
        categorical.append(("model.slug_segment.v1", segment, 0.35))

    name_tokens = _model_text_tokens(display_name, 64)
    description_tokens = _model_text_tokens(description, 512)
    for token in name_tokens:
        categorical.append(("model.name_word.v2", token, 0.55))
    for left, right in zip(name_tokens, name_tokens[1:]):
        categorical.append(("model.name_bigram.v2", left + "\x1e" + right, 0.30))
    for token in description_tokens:
        categorical.append(("model.description_word.v2", token, 0.12))
    for left, right in zip(description_tokens, description_tokens[1:]):
        categorical.append(("model.description_bigram.v2", left + "\x1e" + right, 0.06))

    semantic_text = (display_name + "\n" + description)[:32768]
    for concept, pattern in _MODEL_SEMANTIC_PATTERNS:
        if pattern.search(semantic_text):
            categorical.append(("model.semantic_concept.v2", concept, 0.7))

    if context_length > 0:
        categorical.append(
            (
                "model.context_bucket.v2",
                "log2:%d" % int(math.log2(context_length)),
                0.45,
            )
        )
    if max_completion > 0:
        categorical.append(
            (
                "model.output_bucket.v2",
                "log2:%d" % int(math.log2(max_completion)),
                0.35,
            )
        )
    categorical.append(
        ("model.prompt_price_tier.v2", _price_tier(known_prompt_price), 0.35)
    )
    categorical.append(
        (
            "model.completion_price_tier.v2",
            _price_tier(known_completion_price),
            0.35,
        )
    )
    modality_signature = (
        "+".join(sorted(input_modalities)) + "->" + "+".join(sorted(output_modalities))
    )
    categorical.append(("model.modality_signature.v2", modality_signature, 0.45))
    for item in sorted(input_modalities):
        categorical.append(("model.input_modality.v1", item, 0.5))
    for item in sorted(output_modalities):
        categorical.append(("model.output_modality.v1", item, 0.5))
    for item in sorted(supported):
        categorical.append(("model.parameter.v1", item, 0.25))
    for item in sorted(str(key).casefold() for key in default_parameters):
        categorical.append(("model.default_parameter.v2", item, 0.18))
    for item in sorted(_string_list(reasoning.get("supported_efforts"))):
        categorical.append(("model.reasoning_effort.v1", item.casefold(), 0.4))
    for namespace, token, weight in categorical:
        _hashed_add(vector, MODEL_RESERVED, namespace, token, weight)
    return _finish_hash_block(vector, MODEL_RESERVED)


def feature_contract(
    task_dim: int = DEFAULT_TASK_DIM, model_dim: int = DEFAULT_MODEL_DIM
) -> Dict[str, Any]:
    """Return serializable metadata sufficient to reproduce both vectors."""

    return {
        "task_feature_version": TASK_FEATURE_VERSION,
        "model_feature_version": MODEL_FEATURE_VERSION,
        "task_dim": int(task_dim),
        "model_dim": int(model_dim),
        "task_reserved": TASK_RESERVED,
        "model_reserved": MODEL_RESERVED,
        "model_family_version": MODEL_FAMILY_VERSION,
        "benchmark_derived_feature_indices": list(MODEL_BENCHMARK_DERIVED_INDICES),
        "catalog_quality_feature_policy": "model_features(include_benchmark_priors=False)",
        "full_model_id_feature": False,
        "task_feature_names": task_feature_names(task_dim),
        "model_feature_names": model_feature_names(model_dim),
        "hash_algorithm": "blake2b-64-little-endian-signed",
        "task_hash_namespaces": [
            "task.word.v1",
            "task.bigram.v1",
            "task.trigram.v1",
            "task.prefix.v1",
            "task.suffix.v1",
            "task.charclass.v1",
        ],
        "model_hash_namespaces": [
            "model.provider.v1",
            "model.family.v2",
            "model.tokenizer.v1",
            "model.modality.v1",
            "model.instruct.v1",
            "model.slug_segment.v1",
            "model.name_word.v2",
            "model.name_bigram.v2",
            "model.description_word.v2",
            "model.description_bigram.v2",
            "model.semantic_concept.v2",
            "model.context_bucket.v2",
            "model.output_bucket.v2",
            "model.prompt_price_tier.v2",
            "model.completion_price_tier.v2",
            "model.modality_signature.v2",
            "model.input_modality.v1",
            "model.output_modality.v1",
            "model.parameter.v1",
            "model.default_parameter.v2",
            "model.reasoning_effort.v1",
        ],
        "model_semantic_text_fields": ["name", "description"],
        "model_semantic_text_limits": {"name_tokens": 64, "description_tokens": 512},
    }


__all__ = [
    "DEFAULT_MODEL_DIM",
    "DEFAULT_TASK_DIM",
    "MODEL_BENCHMARK_DERIVED_INDICES",
    "MODEL_FAMILY_VERSION",
    "MODEL_FEATURE_VERSION",
    "MODEL_RESERVED",
    "TASK_FEATURE_VERSION",
    "TASK_RESERVED",
    "canonical_model_id",
    "feature_contract",
    "model_family_id",
    "model_feature_names",
    "model_features",
    "task_feature_names",
    "task_features",
]
