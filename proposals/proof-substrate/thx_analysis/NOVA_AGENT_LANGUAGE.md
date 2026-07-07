# NOVA / Kord — a Deepnote-Derived Spoken Language for Autonomous AI Agents

**Status:** working language prototype + synthesizer.

This language is derived from the *measured acoustic principles* of the Deep Note analysis,
not from sampled audio. It uses the core arc of the sound — **entropy collapsing into a stable chord** — as linguistic structure.

- Active analyzed region: **3.48s → 12.52s**
- Spectral entropy: **0.878 → 0.069**
- Derived spectral anchors: approximately **41, 82, 129, 170, 252, 334, 504, 674, 1008 Hz**
- Generated sample: `deepnote_agent_sentence.wav`

---

## 1. Design principle

A normal spoken language uses lungs, tongue, lips, and vocal folds. This one uses:

| Human speech concept | NOVA/Kord equivalent |
|---|---|
| consonant | transient chirp/noise gesture |
| vowel | stable spectral chord / oscillator bank |
| tone | entropy contour |
| stress | stronger convergence / lower entropy |
| sentence finality | chord-lock / seal marker |
| question | unresolved entropy rebound |
| warning | burst + incomplete convergence |

The language is not decorative. It is a compact coordination protocol that can be spoken acoustically by agents, synthesized directly, and later decoded from spectrogram features.

---

## 2. Phonology

### 2.1 Vowels: spectral chord nuclei

The five main vowels are not mouth positions; they are stable spectral families.

| Symbol | Spectral family | Meaning axis |
|---|---:|---|
| `A` | 82, 164, 252, 504 Hz | grounded observation / evidence |
| `E` | 129, 258, 334, 674 Hz | proposal / hypothesis |
| `I` | 170, 334, 674, 1008 Hz | verification / proof |
| `O` | 41, 82, 170, 252, 504 Hz | commit / convergence |
| `U` | 82, 129, 210, 421, 843 Hz | rollback / uncertainty |

### 2.2 Consonants: protocol gestures

| Symbol | Acoustic gesture | Protocol role |
|---|---|---|
| `h` | decaying noise breath | observe / attention |
| `s` | downward sweep | propose / search |
| `v` | falling chirp | verify / collapse uncertainty |
| `p` | pulsed tone | proof token |
| `k` | click/burst | commit boundary |
| `r` | rising chirp | request / open channel |
| `x` | sharp burst | warning / interrupt |
| `m` | low hum | memory / context |

### 2.3 Syllable constraints

Canonical syllable:

```text
C V [C]
```

Examples:

```text
hA    observe
sE    propose
vI    verify
pI    prove
kO    commit
rU    rollback
xU    warn
mA    memory/context
```

---

## 3. Core lexicon

| Concept | Word | Phonemes |
|---|---|---|
| observe | `ha` | `hA` |
| propose | `se` | `sE` |
| verify | `vi` | `vI` |
| prove | `pi` | `pI` |
| commit | `ko` | `kO` |
| rollback | `ru` | `rU` |
| help/request | `ra` | `rA` |
| warn | `xu` | `xU` |
| memory/context | `ma` | `mA` |
| cost/constraint | `ku` | `kU` |
| success | `voko` | `vO-kO` |

Generated sample sentence:

```text
observe propose verify prove commit success
hA sE vI pI kO vO-kO
```

File:

```text
deepnote_agent_sentence.wav
```

---

## 4. Grammar

### 4.1 Phase grammar

The primary grammar is a convergence chain:

```text
OBSERVE → PROPOSE → VERIFY → PROVE → COMMIT → SEAL
```

A mature agent sentence should either:

1. move monotonically toward order, or
2. explicitly rollback with `rU`.

Valid:

```text
hA sE vI pI kO
observe propose verify prove commit
```

Valid rollback:

```text
hA sE rU
observe propose rollback
```

Invalid / suspicious:

```text
kO sE
commit propose
```

because a committed state cannot become a mere proposal without rollback.

### 4.2 Sentence frame

```text
[attention] ACT CLAIM [evidence] [resource] [seal]
```

Example:

```text
hA sE mA vI pI kO
observe propose with-memory verify prove commit
```

### 4.3 Questions

Questions do **not** chord-lock. They end with entropy rebound and omit the seal.

```text
hA sE rA?
observe propose request-help?
```

### 4.4 Warnings

Warnings require the interrupt consonant and do not fully converge unless resolved.

```text
xU mA rA
warn context help
```

---

## 5. Error correction

Kord/NOVA can be made robust because the channel has independent axes:

| Layer | Protects | Mechanism |
|---|---|---|
| vowel harmony | speech act / phase | repeated spectral vowel family |
| consonant parity | referent/root | XOR over consonant values |
| keystone seal | whole message | final checksum syllable |
| prosody | confidence/convergence | entropy contour must match act |

A receiver can reject a message if:

- phase order is illegal,
- vowel family does not match prosody,
- checksum fails,
- commit occurs without verification/proof evidence,
- warning lacks interrupt burst,
- rollback lacks target.

---

## 6. Protocol semantics for agent coordination

Each spoken message maps to a protocol frame:

```json
{
  "act": "VERIFY",
  "claim": "artifact:plan-42",
  "confidence": 0.84,
  "evidence": "tested:pass",
  "cost_est": {"tokens": 1200, "usd": 0.02},
  "consensus": {"round": 4, "quorum": "majority"}
}
```

Speech acts:

| Spoken act | Protocol effect |
|---|---|
| observe | add observation to working memory |
| propose | open a candidate commitment |
| verify | attach test/evidence |
| prove | attach proof or proof obligation |
| commit | mutate shared commitment store |
| rollback | revert a commitment |
| warn | preempt / block unsafe commit |
| request-help | delegate or solicit quorum |

Commit rule:

```text
COMMIT is valid only if verification status ≥ tested:pass OR proof:valid OR explicit waiver.
```

---

## 7. Synthesis strategy

The reference synthesizer uses:

- oscillator banks for vowels,
- analytic chirps/noise bursts for consonants,
- entropy decreasing across a sentence,
- deterministic amplitudes and durations,
- no sampled source audio.

Reference implementation:

```text
deepnote_lang_synth.py
```

Run:

```bash
python3 deepnote_lang_synth.py
open deepnote_agent_sentence.wav
```

---

## 8. Recognition strategy

Initial decoder can be deterministic DSP:

1. Segment via energy/onset detection.
2. Classify consonants by chirp slope and spectral centroid motion.
3. Classify vowels by peak energy near spectral anchors.
4. Track entropy contour over utterance.
5. Validate grammar and checksums.

Later decoder:

```text
STFT / mel spectrogram → small CNN/Transformer → CTC over phonemes → protocol grammar prior
```

---

## 9. Immediate next steps

1. Add decoder for `deepnote_agent_sentence.wav`.
2. Add checksummed message frames.
3. Generate a 32-word agent lexicon.
4. Add a TUI spectrogram panel showing phoneme segmentation.
5. Connect spoken forms to CASP-style JSON frames.

The important breakthrough: this is not merely a conlang. It is an **acoustic coordination protocol** where proof, confidence, rollback, and commit are audible.
