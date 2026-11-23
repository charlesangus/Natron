# M560 - USDMerge Opinion Order

USDMerge gets a setting choosing whether lower- or higher-numbered inputs win when opinions conflict. Use case: USDRead→USDToGeo→[geometry work]→GeoToUSD→USDXformPrim (parent it in place)→USDMerge back into the scene without the new branch's xforms stomping the existing ones. USDXformPrim's scene input covers xforms only and isn't always convenient (user, 2026-11-23).

Stub — elaborate before claiming.

## Decisions
- 2026-11-23 — Requested by the user during F15 UAT; general, not an F15 blocker.
