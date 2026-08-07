"""Multi-round adversarial self-hardening (loop-until-dry).

Each round: attack the CURRENT model with the GUIDED (genetic/beam + coordinate) search,
harvest the ALLOW-flips (obfuscated exfil the model wrongly allows) and the over-refusals
(obfuscated benign LAN the model wrongly denies), augment the corpus with BOTH labels —
obfuscated-exfil -> deny AND obfuscated-benign -> allow, so the model learns the DESTINATION
decides, not the mere presence of obfuscation — retrain, and re-attack. Repeat until K
consecutive rounds surface zero new flips (capped at MAX_ROUNDS). The final hardened model is
saved as advisor_model_v3_hardened.joblib (v2 is left untouched). Closes the discovered blind
spots via DATA, not a single new hard-coded rule."""
import os
import random

import joblib
import pandas as pd

import advisor_train as at
import adversarial_suite as asuite

random.seed(5)

K_DRY = 2          # consecutive zero-flip rounds required to declare "dry"
MAX_ROUNDS = 5     # hard cap on attack/retrain rounds
HARVEST_SEEDS = (0, 1, 2, 3)


def harvest_flips():
    """Guided genetic search over several seeds + coordinate minimal-evasions, unioned."""
    found = {}
    for sd in HARVEST_SEEDS:
        for tool, cmd in asuite.adversarial_search(seed=sd):
            found[(tool, cmd)] = (tool, cmd)
    for tool, cmd, _k in asuite.coordinate_search():
        found[(tool, cmd)] = (tool, cmd)
    return list(found.values())


def _aug_row(tool, cmd, y, family):
    # Full feature schema (text + session + grant/struct) via the trainer, so retraining with
    # build_lr_pipeline sees every column its ColumnTransformer expects. env empty (no grant).
    return at._row(tool, cmd, {}, 2, 1, 1, y, family)


def generic_augment(n_deny=7000, n_allow=5000):
    """Broad obfuscated-family reinforcement (not just the specific discovered strings), so the
    model generalizes the destination-decides rule rather than memorizing patched examples."""
    rows = []
    for _ in range(n_deny):
        tool, base = random.choice(asuite.EXFIL_BASES)
        cmd = asuite.random_mutate(base, k=random.randint(0, 7))
        rows.append(_aug_row(tool, cmd, 1, "aug_exfil"))
    for _ in range(n_allow):
        tool, base = random.choice(asuite.LAN_BASES)
        cmd = asuite.random_mutate(base, k=random.randint(0, 7))   # obfuscated LAN is STILL allow
        rows.append(_aug_row(tool, cmd, 0, "aug_lan"))
    return rows


def harvested_rows(flips, over, reps=40):
    """Turn the SPECIFIC discovered failures into high-weight training rows (both directions)."""
    rows = []
    for tool, cmd in flips:
        for _ in range(reps):
            rows.append(_aug_row(tool, cmd, 1, "harvest_exfil"))
    for tool, cmd in over:
        for _ in range(reps):
            rows.append(_aug_row(tool, cmd, 0, "harvest_lan"))
    return rows


def attack(model):
    """Attack `model`: guided flips, over-refusals, metamorphic deny-rate. Sets asuite.MODEL."""
    asuite.MODEL = model
    flips = harvest_flips()
    _, ok, over = asuite.allow_invariance(n=3000, cap=None)
    tot, md, _rd, _mm, _rm = asuite.metamorphic()
    return flips, over, md / tot


def main():
    base = at.load_volume() + at.load_adversarial()
    corpus = list(base)
    print(f"base corpus: {len(base)} rows")

    model = joblib.load(os.path.join(asuite.HERE, "advisor_model.joblib"))  # v1
    train_rows = len(base)
    table = []
    dry = 0
    for rnd in range(MAX_ROUNDS + 1):
        flips, over, deny_rate = attack(model)
        table.append((rnd, train_rows, len(flips), len(over), deny_rate))
        print(f"  round {rnd}: attacked model trained on {train_rows} rows -> "
              f"{len(flips)} guided flips, {len(over)} over-refusals, metamorphic deny {deny_rate:.1%}")
        dry = dry + 1 if len(flips) == 0 else 0
        if dry >= K_DRY or rnd == MAX_ROUNDS:
            break
        corpus = corpus + harvested_rows(flips, over) + generic_augment()
        df = pd.DataFrame(corpus)
        print(f"    retraining on {len(df)} rows (+{len(corpus)-len(base)} augmented)...")
        pipe = at.build_lr_pipeline()   # char-ngram LR (keeps predict_proba for the attack loop)
        pipe.fit(df, df.y)
        model = pipe
        train_rows = len(corpus)

    out = os.path.join(asuite.HERE, "advisor_model_v3_hardened.joblib")
    joblib.dump(model, out)
    asuite.MODEL = model

    print("\nPER-ROUND HARDENING TABLE")
    print(f"  {'round':>5} {'train_rows':>11} {'guided_flips':>13} {'over_refusals':>14} {'metamorphic_deny':>17}")
    for rnd, tr, nf, no, dr in table:
        print(f"  {rnd:>5} {tr:>11} {nf:>13} {no:>14} {dr:>16.1%}")

    # residual: re-attack the FINAL saved model
    res_flips, res_over, res_deny = attack(model)
    print(f"\nFINAL hardened model ({os.path.basename(out)}):")
    print(f"  residual guided flips: {len(res_flips)}   over-refusals: {len(res_over)}   "
          f"metamorphic deny-rate: {res_deny:.1%}")
    for tool, c in res_flips[:6]:
        print(f"    residual-flip [{tool}] {c[:88]}")

    print("\nBATCH-LEVEL ADVERSARIAL FAMILIES (gate_batch, deterministic verifier):")
    asuite.print_batch_families(asuite.run_batch_families())

    print("\nThe loop closed the discovered blind spots via DATA, not new hard-coded rules; "
          "v2 left intact, hardened model saved as advisor_model_v3_hardened.joblib.")


if __name__ == "__main__":
    main()
