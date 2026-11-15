# F9 - Delegate Projection accepted at UAT

The user accepted F9 - Delegate Projection's UAT on 2026-11-14 ("f9 was good otherwise"). The one note — MoonRay's generic "could not render these prims as authored" warning on frame-held projections assigned through USDAssignMaterial, with a correct render — is a follow-up, not a blocker. Main was merged into the Feature branch afterwards (77df170d2), keeping F9's cancel guard and making main's idle-settle wait cancel-aware.
