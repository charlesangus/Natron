# An infinite image size is an error

**Decision (user, 2026-11-13, M268 - Edge-Aware Region Independence UAT):** an infinite-sized image is never a valid case and is not an edge case to design for. A node that finds an infinite region of definition on an input it needs as a whole frame fails the render with a clear error ("something has gone wrong upstream"), rather than clamping it to the format. UAT steps never ask for an "infinite" constant.

This supersedes M268's design of clamping infinite RoDs to the format. It applies to new code; existing Natron handling elsewhere is changed only when a milestone touches it.
