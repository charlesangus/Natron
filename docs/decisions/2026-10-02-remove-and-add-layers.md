# RemoveLayers and AddLayers

2026-10-02, M37 (user answers Q1–Q6, 2026-09-27; Q3 extended 2026-10-02).

- **Two native Channel nodes.** `fr.natron.RemoveLayers` has a remove/keep operation (Q1 (a), Q6 (b)). `fr.natron.AddLayers` zero-fills the chosen project-registry layers wherever the input lacks them, with no fill-colour knob (Q5 (a)). Both select whole layers through a button-less channel set (Q2 (a)) and reuse the Regex rows for wildcards (Q4 (a)). Selections resolve per frame, and a row naming an absent layer is silent.
- **Colour views narrow or drop the colour plane (Q3 (c), extended).** Let S be the input's colour storage bits and C the bits the rows select (a view row contributes its whole view mask). Then K = S & ~C in remove mode and S & C in keep mode.
  - K = S: the plane passes through.
  - K empty: the plane is dropped.
  - Otherwise: the plane narrows to the narrowest layout covering K.

  So removing `alpha` leaves RGB, removing `rgb` leaves Alpha, and removing `rgba` drops colour. Keeping `spec.*` keeps specular only, keeping None empties the stream, and regex `.*` matches the colour views too.
- **AddLayers widens colour bit by bit.** Adding a colour view the input doesn't cover widens to the narrowest layout covering both. The input's bits are copied and the added bits are zero, so `rgba` on RGB gives A = 0.
- **A stream may carry no colour plane.** It still lists `rgba`/`rgb`/`alpha`, which read zero. Write All writes no R/G/B/A for it. All and regex rows never fabricate a colour plane over it, but an explicit colour-view row produces zeros.
- **Engine hook.** `EffectInstance::filterPassThroughLayers` lets a node drop layers it would otherwise pass through from its input. It can never add or reshape them.
