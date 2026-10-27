# Up axis and units policy

2026-10-27, user, answering M226 - Scene Hierarchy From Topology's UAT item 7 (moved to M241 - Up Axis And Units Policy).

The project's Up Axis and Meters Per Unit win: every layer Natron authors carries them, and USDWrite writes them on every layer it saves. A file's own metrics count only inside its USDRead's layer. They are never converted silently. USDRead converts a file only when the user turns on Match Project Up Axis or Match Project Units (separate toggles, both off by default). The conversion is a corrective transform over the file's topmost xformable prims, authored in USDRead's layer. A Use File's Metrics For Project button adopts a file's metrics for the project in one undoable edit. Meters Per Unit defaults to 1.0 (metres).

Rationale: this follows Solaris, where up axis and metersPerUnit are hints that only the stage root layer applies, and Asset Reference has an opt-in Align To Up Axis. It also follows USD, whose stage metrics are advisory. Automatic conversion was rejected because nothing can tell which attributes are lengths, and a silent rotation breaks prim paths and patterns that users wrote against the file.
