# Content node hashing

Node hashes become content hashes (plugin identity, render-hashed knob values including full curves and expressions, inputs by index, file stamps, project digest), replacing `knobsAge`. Cache entries are evicted LRU, not purged on hash change. User decision 2026-10-11; implemented by M100 - Content Node Hashing. Overlaps M97 - Value-Based Node Hashing (see the open question on the board).
