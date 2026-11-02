# Unified tiered cache (user, 2026-11-01)

Node outputs, deep images included, are cached under one logic across three storage tiers: host RAM, a persistent disk tier and device memory. Entries share a tier-neutral header: key, bounds, rendered-pixel bitmap, being-rendered registry, tier residency, last use, retention priority and recompute cost. Each tier has a budget and a demote step, and one cost-aware policy evicts only past a high-water mark. A demotion moves the entry down a tier when a recompute would cost more than the move; otherwise the entry is dropped. The device tier shares the GPU pool budget, and render working buffers win. Retention priority replaces `shouldCacheOutput`'s yes/no rule. Any new build invalidates the disk tier. The DiskCache node becomes a pin on its own budget. The viewer playback cache stays separate.

Why: a ~24MP raw chain re-ran on every downstream Grade edit because GPU region outputs never reached any cache and linear chains weren't cached. The user wants retention bounded by budget, not by a rule: "just not evict stuff until we need to".

Milestones: M276 - Upstream Reuse On Edits (measurement), M300 - Unified Tiered Cache, M301 - GPU Cache Tier, M303 - Interactive Edit Latency.
