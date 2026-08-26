# RH counterexample sampler + Lean bridge

This is an executable research scaffold, **not a claimed solution of the Riemann hypothesis**.

## Sound architecture

```text
adaptive numerical sampler
        ↓ untrusted lead (small residual)
high-precision root refinement
        ↓ still untrusted
rigorous interval / argument-principle zero isolation
        ↓ certificate: a zero exists in an off-line box
Lean theorem bridge
        ↓ kernel checks implication
¬ RiemannHypothesis
```

The crucial third stage is not implemented here. Omitting it and converting a floating-point residual into `riemannZeta s = 0` would be unsound. Mathlib's `riemannZeta` is noncomputable; Lean is a proof checker here, not an efficient numerical oracle.

## Included

- `search.py`: seeded adaptive exploration, exploitation around low-residual elites, complex root refinement, a precision ladder, JSON provenance, and explicit rejection of roots that converge to the critical line.
- `lean/RHCounterexample/Bridge.lean`: no `sorry` and no custom axioms. It proves that either an exact witness or a rigorously certified zero-containing off-line region refutes mathlib's `RiemannHypothesis`.
- pinned Python and Lean dependencies.

## Run the lead generator

```sh
cd proposals/riemann-counterexample-sampler
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt
python search.py --generations 20 --population 256 --t-max 1000 \
  --output runs/first.json
```

Expected behavior under RH: refinements near actual zeros converge to `Re(s)=1/2` and fail `passes_numerical_screen`. A screen pass is only a lead, never a certificate.

## Check the Lean bridge

```sh
cd lean
lake update
lake build
```

## What remains for a genuine counterexample

1. Evaluate zeta over complex balls with directed rounding and proved truncation errors (Arb is a practical candidate engine).
2. Cover an off-line rectangle whose closure is disjoint from `Re(s)=1/2`.
3. Prove by the argument principle/Rouché machinery that the rectangle contains at least one zero, while excluding the pole and trivial zeros.
4. Verify the native certificate independently, then connect its semantics to mathlib's `riemannZeta`. This connection—not the final three-line contradiction—is the major formalization project.
5. Reproduce on independent hardware/software and obtain expert review before announcing anything.

## Acceptance boundary

A run is a **counterexample** only if Lean checks a theorem of type `¬ RiemannHypothesis` without `sorry`, undeclared trust assumptions, or an axiom that simply asserts the numerical result. The current project checks the logical bridge only; it does not yet construct its hard premise.
