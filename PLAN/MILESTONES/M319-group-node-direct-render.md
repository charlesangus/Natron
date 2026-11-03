# M319 - Group Node Direct Render

Stub. Found by M121 - Native Routing And Mix (2026-11-02): calling renderRoI on a NodeGroup (an OutputEffectInstance with no render or pass-through of its own) returns an image whose memory was never written, so callers read leftover data (values like 3.5e-41, or a previous render's colours) and results vary between runs of the same binary. Downstream nodes are fine because they read through the node feeding the group's Output. Tests work around it with a Dot below the group (CacheEquivalence_Test consumerOf, LightWrapRender_Test).

Fix: a group node renders as an identity of its Output node's input (or fails cleanly), so any direct render returns correct pixels; drop the Dot workarounds; add a test that renders a group directly.

Blocked on: nothing; elaborate when claimed.
