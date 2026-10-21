# Merge

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Merge and its nine presets | exact / transcendental | `toleranceFor()` per operator | Porter-Duff operators (ATop, ConjointOver, Copy, DisjointOver, In, Mask, Matte, Out, Over, Stencil, Under, XOR) exact; blend modes transcendental, and mask and mix too. `op-divide` measured 3.05e-5 on large values. The presets share the class and test that each carries its operation |
