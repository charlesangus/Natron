## Pending

### 2026-10-12T00:00:00Z — change-request
- refs: M96
- From the M69 - Minor Cleanup UAT (user, 2026-10-12), add this to M96 - Weighted Curve Tangents' acceptance sketch. In bezier mode a curve-editor handle has two states. **Unbroken:** the in and out handles stay aligned, and changing one handle's length changes the other's too. **Broken:** each handle's length and angle change independently of the other. Weighted handles must also change the curve, not snap back on release. The user decided M69 does not implement length; M69 instead drops its visual-only follow-the-cursor handle drawing.
