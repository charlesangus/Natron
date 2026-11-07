# MetadataModify

Tolerance: none recorded.

- `MetadataModify`: sets, removes or renames keys through eight operation slots.

`MetadataModify` has a fixed eight slots because a `KnobTable` packs its rows
into one string: its cells could neither take an expression nor animate, and a
`Set` value must do both. Each slot is a group of ordinary knobs instead.
