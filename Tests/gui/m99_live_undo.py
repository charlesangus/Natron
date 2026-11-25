"""A two-process GUI test of per-user undo in a live session. A hosts, B joins (see
Tests/gui/live_peers.py), and Edit > Undo on either side undoes only that side's own edits:

- A drags k1 (TintA's mix) and B drags k2 (TintB's mix); A undoes, and only k1 reverts, on both
  sides; A redoes, and k1 is back on both.
- A creates Made and B moves it; A undoes the create, and Made is gone on both sides; A redoes,
  and it is back on both.
- A drags Doomed's mix and B deletes Doomed; A's edit of Doomed leaves A's undo history, an undo
  and a redo still work, and nothing crashes.

Usage (natron-dev container; see Tests/gui/run-gui-test.sh):
    NATRON_GUI_TEST_OUT=build/m99-undo/out GUI_TEST_TIMEOUT=300 \\
        Tests/gui/run-gui-test.sh Tests/gui/m99_live_undo.py <AppImage or build/release/App/Natron>
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

WATCHDOG_S = 280

try:
    import NatronEngine
    import guitest as gt
    from guitest import check, report, require
    import live_peers as lp

    import faulthandler
    _watchdog_file = open(os.path.join(gt.OUT, "watchdog.txt"), "w")
    faulthandler.dump_traceback_later(WATCHDOG_S, exit=True, file=_watchdog_file)

    ROLE, SYNC = lp.bind(app, "M99_UNDO_ROLE")

    def mix(name):
        node = app.getNode(name)
        return node.getParam("mix").get() if node is not None else None

    def near(a, b):
        return a is not None and b is not None and abs(float(a) - float(b)) < 1e-4

    def client_command(words):
        verb = words[0]
        if verb == "slider":
            return (yield from lp.drag_mix_slider(words[1]))
        if verb == "move":
            return (yield from lp.move_node(words[1], float(words[2]), float(words[3])))
        if verb == "delete":
            return (yield from lp.delete_node(words[1]))
        return "unknown command " + verb

    def client_mix(name, expected):
        answer = yield from lp.until_client("get %s mix" % name, repr(expected))
        return answer

    def host_steps():
        ok = yield ("until", lambda: lp.main_gui() is not None, lp.STARTUP_TIMEOUT_S)
        require(ok, "the main window is up")
        gt.close_all_panels()
        src = app.createNode("Constant")
        tint_a = app.createNode("Grade")
        tint_b = app.createNode("Grade")
        doomed = app.createNode("Grade")
        require(None not in (src, tint_a, tint_b, doomed), "create the starting comp")
        src.setScriptName("Src")
        tint_a.setScriptName("TintA")
        tint_b.setScriptName("TintB")
        doomed.setScriptName("Doomed")
        src.setPosition(0, 0)
        tint_a.setPosition(0, 150)
        tint_b.setPosition(250, 150)
        doomed.setPosition(500, 150)
        for node in (tint_a, tint_b, doomed):
            node.connectInput(0, src)
        project_file = os.path.join(gt.OUT, "m99_undo_host.ntpz")
        require(app.saveProjectAs(project_file), "save the host project to " + project_file)

        gt.section("join")
        yield from lp.host_and_join("M99_UNDO_ROLE", "Ann", "Bea")

        # ---- knobs
        gt.section("knobs")
        k1_before = mix("TintA")
        answer = yield from lp.drag_mix_slider("TintA")
        require(answer == "ok", "A drags k1", answer)
        k1_after = mix("TintA")
        answer = yield from lp.client_do("slider TintB")
        require(answer == "ok", "B drags k2", answer)
        ok = yield ("until", lambda: not near(mix("TintB"), 1.0), 10.0)
        check(ok, "B's k2 reaches A")
        k2_after = mix("TintB")
        answer = yield from client_mix("TintA", k1_after)
        check(answer == repr(k1_after), "A's k1 reaches B (%s)" % answer)
        check(lp.undo_text() != "", "A can undo: %r" % lp.undo_text())
        require(lp.trigger_undo(), "A undoes")
        ok = yield ("until", lambda: near(mix("TintA"), k1_before), 10.0)
        check(ok, "A's undo reverts k1 on A (%r)" % mix("TintA"))
        check(near(mix("TintB"), k2_after), "A's undo leaves B's k2 on A (%r)" % mix("TintB"))
        answer = yield from client_mix("TintA", k1_before)
        check(answer == repr(k1_before), "A's undo reverts k1 on B (%s)" % answer)
        answer = yield from lp.client_do("get TintB mix")
        check(answer == repr(k2_after), "A's undo leaves k2 on B (%s)" % answer)
        require(lp.trigger_undo(redo=True), "A redoes")
        ok = yield ("until", lambda: near(mix("TintA"), k1_after), 10.0)
        check(ok, "A's redo puts k1 back on A (%r)" % mix("TintA"))
        answer = yield from client_mix("TintA", k1_after)
        check(answer == repr(k1_after), "A's redo puts k1 back on B (%s)" % answer)

        # ---- create
        gt.section("create")
        props = {"CreateNodeArgsPropAddUndoRedoCommand": NatronEngine.BoolNodeCreationProperty(True)}
        made = app.createNode("Blur", -1, None, props)
        require(made is not None, "A creates a Blur as an undoable step")
        made.setScriptName("Made")
        made.setPosition(0, 300)
        answer = yield from lp.until_client("has Made", "yes")
        require(answer == "yes", "Made reaches B", answer)
        answer = yield from lp.client_do("move Made 120 60")
        check(answer == "ok", "B moves Made: %s" % answer)
        create_text = ""
        for _ in range(4):
            text = lp.undo_text()
            if not text:
                break
            if any(word in text.lower() for word in ("made", "blur", "create", "add", "node")):
                create_text = text
                break
            # Steps after the create (the rename and the move made by script) are undone first.
            lp.trigger_undo()
            yield ("sleep", lp.SETTLE_MS)
        report("NOTE the create step reads %r" % create_text)
        require(lp.trigger_undo(), "A undoes the create")
        ok = yield ("until", lambda: app.getNode("Made") is None, 10.0)
        check(ok, "A's undo removes Made on A")
        answer = yield from lp.until_client("has Made", "no")
        check(answer == "no", "A's undo removes Made on B (%s)" % answer)
        require(lp.trigger_undo(redo=True), "A redoes the create")
        ok = yield ("until", lambda: app.getNode("Made") is not None or app.getNode("Blur1") is not None, 10.0)
        check(ok, "A's redo brings the Blur back on A")
        name = "Made" if app.getNode("Made") is not None else "Blur1"
        answer = yield from lp.until_client("has " + name, "yes")
        check(answer == "yes", "A's redo brings the Blur back on B (%s)" % answer)

        # ---- remote delete
        gt.section("remote-delete")
        answer = yield from lp.drag_mix_slider("Doomed")
        require(answer == "ok", "A drags Doomed's mix", answer)
        doomed_text = lp.undo_text()
        report("NOTE A's last step reads %r" % doomed_text)
        answer = yield from lp.client_do("delete Doomed")
        require(answer == "ok", "B deletes Doomed", answer)
        ok = yield ("until", lambda: app.getNode("Doomed") is None, 10.0)
        require(ok, "B's delete reaches A")
        ok = yield ("until", lambda: lp.undo_text() != doomed_text, 5.0)
        check(ok, "A's edit of Doomed leaves the undo history (%r)" % lp.undo_text())
        if lp.undo_text():
            check(lp.trigger_undo(), "A undoes past the dropped step")
            yield ("sleep", lp.SETTLE_MS)
            check(lp.trigger_undo(redo=True), "A redoes it")
            yield ("sleep", lp.SETTLE_MS)
        check(app.getNode("Doomed") is None, "Doomed stays deleted on A")
        answer = yield from lp.client_do("has Doomed")
        check(answer == "no", "Doomed stays deleted on B (%s)" % answer)
        check(lp.main_gui() is not None and lp.status_text().startswith("Live: hosting"),
              "A is still hosting (%r)" % lp.status_text())

        answer = yield from lp.client_do("quit")
        check(answer == "bye", "the client quits (%s)" % answer)

    lp.run(ROLE, host_steps, client_command)
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
