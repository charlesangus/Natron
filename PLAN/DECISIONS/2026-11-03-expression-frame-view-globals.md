# Expression frame/view globals are not a bug

`Knob::setExpression` sets `frame` and `view` in the shared Python globals. The PM reported this in the inbox as a leak (found by M118's GUI check, where a helper named `frame()` became the int 0) and proposed an "Expression Globals Isolation" stub. The user declined that stub on 2026-11-03: it is fine that user scripts can't use these two names. At most this behaviour gets documented. Don't file a stub for it again. GUI test scripts must not name helpers `frame` or `view`.
