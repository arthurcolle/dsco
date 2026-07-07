# Kord — an Autonomous-Agent Spoken Language

**Core metaphor:** chaos → order convergence. Every linguistic layer *enacts*
the metaphor rather than merely naming it. Name = *Kaos→ORDer* / *chord* (order
from tones).

Design goals: compact (telegraphic) messages, robust error correction,
and a grammar where **certainty and speech-act are the same dimension**.

---

## 0. Design map

| Metaphor element            | Linguistic mechanism                                             |
|-----------------------------|-----------------------------------------------------------------|
| Matter is chaotic           | Consonantal **roots** = raw referents                           |
| Order is imposed on matter  | **Vowel melody** = ordering operation stamped onto the root     |
| Convergence is a gradient   | Vowel rises toward `i` as a claim matures: a→o→u→e→i            |
| Order can be undone         | **Rollback** = phase-inverse prefix `uz-`, not a new root       |
| Noise fights order          | Three stacked error-correcting codes                            |
| Convergence closes          | Every message ends in a **keystone seal** (checksum)            |

---

## 1. Phonology / channel alphabet

Consonants carry 4-bit values (0–15) — this is what makes the ECC real.

| p | t | k | b | d | g | f | s | ʃ | x | m | n | l | r | j | w |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |10 |11 |12 |13 |14 |15 |

`h`, `ə` (schwa) are structural separators/markers, excluded from parity.

**Vowels = convergence axis** (open/back = chaos, close/front = order):

| vowel | a | o | u | e | i |
|-------|---|---|---|---|---|
| σ value | 0 | 1 | 2 | 3 | 4 |
| phase | OBSERVE | PROPOSE | VERIFY | PROVE | COMMIT |

Off-axis control channels: `ai` = alarm, `au` = outward call.
Tones (optional redundancy): level `¯`; falling `ˋ` marks sealed/committed and warnings.

---

## 2. The eight agent acts

Classified by the metaphor, not as eight equal verbs:

- **5 convergence phases** (main axis, encoded as vowel melody):
  observe `a` → propose `o` → verify `u` → prove `e` → commit `i`
- **1 inverse operator:** rollback = `uz-` (moves a claim *down* the axis)
- **2 control channels** (off-axis, compositional):
  warn = `wai-` (falling tone), request-help = `hau-`

Core claim: to say "I've verified X" you literally raise the vowel of X.

---

## 3. Morphology

Word template (predicate):

    [Deixis] Root⟨Melody⟩ [:Mod] ·Parity

- **Root** = 2–3 consonants (referent / "matter"): s-l-k hazard, p-t-k plan.
- **Melody** = fill phase vowel into every root slot (vowel harmony):
  s_l_k + verify `u` → suluk.
- **Deixis prefix:** mi- self · tu- you · na- it · we- swarm (pro-drop when recoverable).
- **Case on arguments** = one suffixed vowel (separate from harmony, never
  collides with parity): -i obj · -u src · -e goal · -o loc · -a abs.
- **Modality:** ne- negation; low confidence = rising tone on melody.
- **Rollback:** uz- + the phase word being reverted (uz-kirit = undo committed contract).

One dense predicate word carries act + referent + confidence → telegraphic by construction.

---

## 4. Error correction (three stacked, orthogonal codes)

**Layer 1 — Vowel harmony = repetition code (protects the act/phase).**
Phase vowel repeats in every root slot; disharmony flags corruption.
Hardened mode: parity syllable schwa → phase vowel (kirit·ij) = 3 copies →
majority-correctable.

**Layer 2 — Per-word parity coda (single-error detection on consonants).**
Coda value P = XOR of root consonant values. Receiver XORs root+coda; must = 0.
Example s-l-k: 7⊕12⊕2 = 9 → x. hazard-observed = salak·əx (7⊕12⊕2⊕9 = 0 ✓).

**Layer 3 — Message keystone = XOR erasure code (recovers one lost word).**
Keystone K = XOR of every word's parity nibble Pᵢ, spoken hok·ə<C>.
If word j is erased: Pⱼ = K ⊕ (all other Pᵢ), combined with the surviving phase
vowel + dictionary lookup → reconstruct.

Parity is vowel-independent; harmony is consonant-independent → orthogonal.
A burst cannot defeat both "what" and "how-sure" at once.

---

## 5. Grammar (EBNF)

    Message  := Clause { Clause } Keystone
    Clause   := [Control] Pred { Arg } [Conf]
    Control  := "wai"(warn,falling) | "hau"(request-help)
    Pred     := [Deixis] Root<Melody> [":" Mod] "·" Parity
    Deixis   := "mi" | "tu" | "na" | "we"
    Melody   := harmonized(a|o|u|e|i) | "uz-" Pred
    Root     := C C [C]
    Mod      := "ne" | ...
    Arg      := Root CaseVowel "·" Parity
    CaseVowel:= "i"obj | "u"src | "e"goal | "o"loc | "a"abs
    Parity   := ("ə" | phaseVowel) Cx        ; Cx = value(XOR root consonants)
    Conf     := rising-tone(low) | "-" digit
    Keystone := "hok" ("ə") Ck               ; Ck = value(XOR of all Pi)

Numbered rules:
1. **Phase-monotonicity:** a referent's melody may only rise unless prefixed uz-.
2. **One-word preference:** collapse act+referent+confidence into one predicate.
3. **Pro-drop:** omit deixis when recoverable (default actor = sender).
4. **Control is compositional:** wai/hau attach to any phase word.
5. **Seal-terminality:** exactly one hok keystone ends every message.
6. **Parity obligatory:** every content word ends in its parity syllable.
7. **Case-by-vowel:** role marked by final vowel; argument order is free.

---

## 6. Lexicon (roots → coda)

| root  | meaning              | coda |
|-------|----------------------|------|
| s-l-k | hazard/risk          | x |
| p-t-k | plan/path            | b |
| s-n-r | sensor/reading       | t |
| r-k-t | result/outcome       | j |
| h-s-k | proof/hash           | g |
| k-r-t | contract/commit-target | j |
| n-d-l | node/peer            | b |
| t-r-k | task                 | j |
| g-r-d | system/grid state    | l |
| l-n-k | dependency/link      | g |
| m-l-k | resource             | d |
| d-t-g | datum/value          | p |

---

## 7. Worked examples (eight acts)

1. OBSERVE  "I observe a hazard, source node."
   mi-salak·əx  nadal·u·əb  hok·əm   (K = 9⊕3 = 10 → m)
2. WARN     "Swarm: warning — hazard." (falling)
   wai we-salak·əx  hok·əx
3. REQUEST-HELP "Help me verify the sensor."
   hau tu-sunur·ət  hok·ət
4. PROPOSE  "I propose a plan."
   mi-potok·əb  hok·əb
5. VERIFY   "I verified the plan." (o→u)
   mi-putuk·əb  hok·əb
6. PROVE    "I prove the result via hash."
   mi-reket·əj  hasak·o·əg  hok·ən
7. COMMIT   "I commit the contract." (ceiling i, falling)
   mi-kirit·əj  hok·əj
8. ROLLBACK "Undo the committed contract."
   mi-uz-kirit·əj  hok·əj

---

## 8. ECC in action

Send: mi-putuk·əb  mi-kirit·əj  hok·ər   (K = 3⊕14 = 13 → r ✓)
Channel flips kirit → kigit (r→g).

Decode:
1. word-2 root XOR: 2⊕5⊕1 = 6; coda = 14 → mismatch ⇒ error localized (Layer 2).
2. vowels harmonize (i,i) ⇒ COMMIT phase intact (Layer 1).
3. recover parity: P2 = K ⊕ P1 = 13 ⊕ 3 = 14; dictionary search for COMMIT-phase
   k_ _t root with XOR 14 → k-r-t (2⊕13⊕1 = 14) = contract. r restored (Layer 3).
