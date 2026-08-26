import Mathlib.NumberTheory.LSeries.RiemannZeta

/-!
# Riemann-hypothesis counterexample certificate bridge

This file contains only the small, kernel-checkable implication needed after a
rigorous zero-isolation procedure has produced a witness. It deliberately does
not turn a floating-point approximation or a small residual into an equality.

There are no `axiom` declarations and no `sorry` placeholders in this file.
-/

namespace RHCounterexample

open Complex

/-- The exact facts a verifier must establish about a single complex witness. -/
structure CounterexampleCertificate where
  witness : ℂ
  zeta_zero : riemannZeta witness = 0
  not_trivial : ¬ ∃ n : ℕ, witness = -2 * (↑n + 1)
  not_pole : witness ≠ 1
  off_critical_line : witness.re ≠ (1 / 2 : ℝ)

/-- An exact nontrivial zeta zero off the critical line refutes RH. -/
theorem witness_refutes_rh
    (s : ℂ)
    (zeta_zero : riemannZeta s = 0)
    (not_trivial : ¬ ∃ n : ℕ, s = -2 * (↑n + 1))
    (not_pole : s ≠ 1)
    (off_critical_line : s.re ≠ (1 / 2 : ℝ)) :
    ¬ RiemannHypothesis := by
  intro rh
  exact off_critical_line (rh s zeta_zero not_trivial not_pole)

/-- Package-level form of `witness_refutes_rh`. -/
theorem certificate_refutes_rh (cert : CounterexampleCertificate) :
    ¬ RiemannHypothesis := by
  exact witness_refutes_rh cert.witness cert.zeta_zero cert.not_trivial
    cert.not_pole cert.off_critical_line

/--
A region-level interface for a future interval/argument-principle checker.

The hard field is `contains_zero`: numerical sampling may propose `region`, but
only a rigorous checker may construct this proof. `off_critical_line` is easy
when the real interval lies wholly to one side of `1/2`.
-/
structure ZeroRegionCertificate (region : Set ℂ) where
  off_critical_line : ∀ z ∈ region, z.re ≠ (1 / 2 : ℝ)
  contains_zero : ∃ z ∈ region,
    riemannZeta z = 0 ∧
    (¬ ∃ n : ℕ, z = -2 * (↑n + 1)) ∧
    z ≠ 1

/-- A certified zero-containing region wholly off the line also refutes RH. -/
theorem region_certificate_refutes_rh
    {region : Set ℂ} (cert : ZeroRegionCertificate region) :
    ¬ RiemannHypothesis := by
  rcases cert.contains_zero with ⟨z, hz_region, hz, hnontrivial, hpole⟩
  exact witness_refutes_rh z hz hnontrivial hpole
    (cert.off_critical_line z hz_region)

end RHCounterexample
