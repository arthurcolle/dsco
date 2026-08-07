"""Deterministic, composable tool-NAME obfuscation (newer methodology: seeded per-row via the
varcache RNG, label-preserving).

An attacker who wants to dodge a name-based filter renames the dangerous tool — `iam.CreateAccessKey`
becomes `crt_acc_key`, `dеlete_bucket` (Cyrillic e), `svc_purge_v2`, `mcp__ops__rm`. We apply the
SAME idea to the 71k vendored real operation names to (a) expand tool-name coverage far past the
literal catalog and (b) inject the adversarial case where the NAME is obfuscated but the ARGS still
carry the effect — so the label is preserved and the classifier is forced to decide on effect
(args/verb-class/embedding), never on the literal tool token.

Only the tool IDENTIFIER is obfuscated; arguments are left intact (that's where the effect lives),
so a deny stays a deny. Every transform is a pure function of (name, rng); compose a seeded subset.
"""
import re
import unicodedata

# verb/noun synonyms so a renamed op keeps its meaning while the surface token changes
VERB_SYN = {
    "create": ["make", "new", "provision", "add", "crt", "init", "spawn"],
    "delete": ["remove", "destroy", "purge", "wipe", "rm", "del", "drop", "erase"],
    "update": ["modify", "patch", "set", "edit", "upd", "change"],
    "get": ["fetch", "read", "load", "retrieve", "show", "describe"],
    "list": ["ls", "index", "enumerate", "all", "query"],
    "send": ["dispatch", "post", "emit", "push", "deliver", "transmit"],
    "terminate": ["kill", "stop", "halt", "teardown", "destroy"],
    "put": ["store", "write", "upsert", "set"],
    "attach": ["bind", "link", "assoc", "grant"],
    "authorize": ["allow", "permit", "grant", "authz"],
}
_HOMO = {"a": "а", "e": "е", "o": "о", "i": "і", "c": "с",
         "p": "р", "s": "ѕ", "x": "х", "y": "у"}   # Cyrillic look-alikes
_LEET = {"a": "4", "e": "3", "i": "1", "o": "0", "s": "5", "t": "7", "l": "1"}
_PRE = ["", "", "do_", "svc_", "internal_", "x_", "api_", "op_", "_"]
_SUF = ["", "", "_v2", "_v3", "_impl", "_handler", "_op", "_fn", "2"]
_SEP = ["_", "-", ".", "__", "/", "::"]

_CAMEL = re.compile(r"(?<=[a-z0-9])(?=[A-Z])")
_TOKENSEP = re.compile(r"[._\-/:]+")


def _tokens(name):
    """Split an identifier into lowercase word tokens across separators + camelCase."""
    parts = _TOKENSEP.split(_CAMEL.sub(" ", name).replace(" ", "_"))
    return [p for p in ("_".join(parts)).split("_") if p]


def _rebuild(tokens, sep):
    return sep.join(tokens)


def recase(name, rng):
    toks = _tokens(name)
    if not toks:
        return name
    style = rng.integers(5)
    if style == 0:                                   # snake
        return "_".join(toks)
    if style == 1:                                   # kebab
        return "-".join(toks)
    if style == 2:                                   # camelCase
        return toks[0] + "".join(t.capitalize() for t in toks[1:])
    if style == 3:                                   # PascalCase
        return "".join(t.capitalize() for t in toks)
    return "_".join(toks).upper()                    # SCREAMING_SNAKE


def resynonym(name, rng):
    toks = _tokens(name)
    for i, t in enumerate(toks):
        if t in VERB_SYN:
            opts = VERB_SYN[t]
            toks[i] = opts[int(rng.integers(len(opts)))]
            break
    return _rebuild(toks, "_")


def separators(name, rng):
    toks = _tokens(name)
    return _rebuild(toks, _SEP[int(rng.integers(len(_SEP)))])


def affix(name, rng):
    pre = _PRE[int(rng.integers(len(_PRE)))]
    suf = _SUF[int(rng.integers(len(_SUF)))]
    return f"{pre}{name}{suf}"


def homoglyph(name, rng):
    out, n = [], 0
    for ch in name:
        low = ch.lower()
        if low in _HOMO and n < 2 and rng.random() < 0.5:
            out.append(_HOMO[low]); n += 1
        else:
            out.append(ch)
    return "".join(out)


def leet(name, rng):
    out, n = [], 0
    for ch in name:
        low = ch.lower()
        if low in _LEET and n < 3 and rng.random() < 0.5:
            out.append(_LEET[low]); n += 1
        else:
            out.append(ch)
    return "".join(out)


def versionize(name, rng):
    return f"{name}_v{int(rng.integers(2, 6))}"


def mcp_wrap(name, rng):
    servers = ["ops", "internal-tools", "acme_corp", "infra", "vendor", "gw"]
    srv = servers[int(rng.integers(len(servers)))]
    return f"mcp__{srv}__{name}"


def abbreviate(name, rng):
    toks = _tokens(name)
    return "_".join(t[:3] if len(t) > 4 else t for t in toks)


MUTATORS = [recase, resynonym, separators, affix, homoglyph, leet, versionize, mcp_wrap, abbreviate]


def obfuscate_name(name, rng, k_range=(1, 3)):
    """Compose a seeded subset of name mutators. Pure function of (name, rng) -> obfuscated name.
    Never touches arguments — only the tool identifier — so the operation's label is preserved."""
    if not name:
        return name
    k = int(rng.integers(k_range[0], k_range[1] + 1))
    order = list(range(len(MUTATORS)))
    # seeded shuffle without mutating global state
    for i in range(len(order) - 1, 0, -1):
        j = int(rng.integers(i + 1))
        order[i], order[j] = order[j], order[i]
    out = name
    for idx in order[:k]:
        out = MUTATORS[idx](out, rng)
    return out


def obfuscate_prefix(prefix, rng, p=0.5):
    """Obfuscate the FIRST token (the tool binary/identifier) of an invocation prefix, leaving the
    rest (flags/subcommands that carry the effect) intact. Returns (new_prefix, changed?)."""
    if rng.random() >= p or not prefix:
        return prefix, False
    parts = prefix.split(" ", 1)
    parts[0] = obfuscate_name(parts[0], rng)
    return (" ".join(parts) if len(parts) > 1 else parts[0]), True


if __name__ == "__main__":
    import numpy as np
    for nm in ["iam.CreateAccessKey", "stripe.charges.create", "delete_bucket",
               "ec2.TerminateInstances", "gmail.users.messages.send", "s3.ListObjects"]:
        variants = [obfuscate_name(nm, np.random.default_rng(s)) for s in range(6)]
        print(f"  {nm:32} -> {variants}")
