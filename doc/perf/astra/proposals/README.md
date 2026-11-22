# Preserved, unapplied proposal

`receiver-support.patch` is the paused bounded reflection receiver-support experiment prepared against the initial ae1f063eb7 checkout. It is not part of the compiled renderer, has not been measured, and may need rebasing. Preserve it for review rather than treating it as a completed optimization.

Modes distinguish original behavior, counters only, search plus counters, and search without counters. The surrounding footprint has at most12 extra taps; valid black remains valid, and no-support retains the defined best-neighbor fallback. No extra rays are introduced by the proposed search. This does not prove image correctness or a frame-time benefit.

Before pursuing it: measure missing-support incidence with actual path/extent identities, compare full/half options with their changed ray budgets accounted for, separate counter overhead, and use complete-scene A/A/A-B images. It remains opt-in until total critical-path and image/noise evidence supports it. Owner-directed stochastic/GI and dense occluded-light work superseded this campaign.
