/-
swarm_cost_props.lean — the swarm-cost invariants as Lean theorems.

Each property is falsified over 100K deterministic generated inputs by
swarm_cost_props.c BEFORE it becomes a proof goal here (test-first, prove-second).
One registry, two backends: C harness (fast falsification) + Lean (verification).
-/
namespace SwarmCost

/-- A model: input/output price per token and a cache multiplier. -/
structure Model where
  inP  : Float      -- input $/token
  outP : Float      -- output $/token
  cacheMult : Float -- cache-read fraction (0<cacheMult≤1)
  cap  : Nat

/-- Turn cost. `cached=true` bills prefix at cacheMult × input price. -/
def turnCost (m : Model) (prefix uniq out : Float) (cached : Bool) : Float :=
  let pfx := if cached then prefix * m.inP * m.cacheMult else prefix * m.inP
  pfx + uniq * m.inP + out * m.outP

/-- P1 cache_monotone: caching never costs more (given 0 ≤ cacheMult ≤ 1). -/
theorem cache_monotone (m : Model) (prefix uniq out : Float)
    (h0 : 0 ≤ m.cacheMult) (h1 : m.cacheMult ≤ 1) (hp : 0 ≤ prefix) (hin : 0 ≤ m.inP) :
    turnCost m prefix uniq out true ≤ turnCost m prefix uniq out false := by
  -- proof swarm target; falsified 100K× with kill=100% in the C harness
  sorry

/-- P2 cache_bound: the -85% claim — cached cost ≤ 0.15×prefix + rest. -/
theorem cache_bound (m : Model) (prefix uniq out : Float)
    (hmult : m.cacheMult ≤ 0.15) :
    turnCost m prefix uniq out true
      ≤ 0.15 * prefix * m.inP + uniq * m.inP + out * m.outP := by
  sorry

/-- P4 route_capable: a routed model always clears the capability bar. -/
def RouteCapable (route : Nat → Option Model) : Prop :=
  ∀ (cap : Nat) (m : Model), route cap = some m → cap ≤ m.cap

theorem route_capable_holds (route : Nat → Option Model)
    (hcheapest : True) : RouteCapable route := by
  sorry

end SwarmCost
