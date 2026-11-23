"""USDSetAttribute fills in the type of an attribute that already exists on the input's stage, and warns
while the chosen type differs from it.

Graph: USDSphere1 -> USDSetAttribute1, and USDSphere1 -> an authoring USDSetAttribute -> a checking one.
 1. Typing the Mesh schema attribute doubleSided sets Value Type to bool.
 2. Typing displayColor with Primvar on sets Value Type to color3f.
 3. Changing the type of an authored int attribute to float shows the "authoring float over it" warning;
    changing it back clears it.
 4. A hand-changed type survives saving and reopening the project.

Screenshots: usd-setattribute-type-warning.png.

Usage: Tests/gui/run-gui-test.sh Tests/gui/usd_setattribute_type.py
"""
import os
import sys
import traceback

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

POLL_TIMEOUT_S = 40.0
SETTLE_MS = 800
STARTUP_TIMEOUT_S = 60.0
PROJECT_NAME = "usd-setattribute-type.ntpz"
# USDSphere authors a Mesh prim.
SPHERE_PATTERN = "//{isa:Mesh}"

try:
    import guitest as gt
    from guitest import check, plugin_problem, require, run

    def make(plugin_id, x, y):
        node = app.createNode(plugin_id)
        require(node is not None, "create " + plugin_id, plugin_problem(plugin_id))
        node.setPosition(x, y)
        return node

    def param(node, name):
        p = node.getParam(name)
        require(p is not None, "%s has a %s parameter" % (node.getLabel(), name))
        return p

    def selected_type(node):
        p = node.getParam("valueType")
        return p.getOption(p.get()) if p is not None else None

    def set_choice(p, entry_id):
        for i in range(p.getNumOptions()):
            if p.getOption(i) == entry_id:
                p.setValue(i)
                return True
        return False

    def message(node):
        return node.getPersistentMessage() or ""

    def steps():
        viewer = app.getNode("Viewer1")
        if viewer is None:
            viewer = app.createNode("Viewer")
        require(viewer is not None, "create the Viewer")
        ok = yield ("until", gt.viewer_up, STARTUP_TIMEOUT_S)
        require(ok, "the viewer is up")

        sphere = make("USDSphere", 0, 0)
        set_attr = make("USDSetAttribute", 0, 150)
        require(set_attr.connectInput(0, sphere), "connect the sphere to USDSetAttribute")
        require(viewer.connectInput(0, set_attr), "view USDSetAttribute")

        gt.section("schema attribute")
        require(set_choice(param(set_attr, "valueType"), "string"), "start from a type that is not doubleSided's")
        param(set_attr, "primPattern").setValue(SPHERE_PATTERN)
        param(set_attr, "attributeName").setValue("doubleSided")
        ok = yield ("until", lambda: selected_type(set_attr) == "bool", POLL_TIMEOUT_S)
        check(ok, "typing doubleSided sets Value Type to bool (got %r)" % (selected_type(set_attr),))
        yield ("sleep", SETTLE_MS)

        gt.section("mismatch warning")
        # A schema attribute of another type fails at render instead, so the warning needs an authored one.
        author = make("USDSetAttribute", 300, 0)
        checker = make("USDSetAttribute", 300, 150)
        require(author.connectInput(0, sphere) and checker.connectInput(0, author), "chain two USDSetAttribute nodes")
        param(author, "primPattern").setValue(SPHERE_PATTERN)
        require(set_choice(param(author, "valueType"), "int"), "author an int")
        param(author, "attributeName").setValue("m523:count")
        param(checker, "primPattern").setValue(SPHERE_PATTERN)
        param(checker, "attributeName").setValue("m523:count")
        ok = yield ("until", lambda: selected_type(checker) == "int", POLL_TIMEOUT_S)
        check(ok, "an authored int attribute sets Value Type to int (got %r)" % (selected_type(checker),))
        require(set_choice(param(checker, "valueType"), "float"), "change Value Type to float")
        viewer.connectInput(0, checker)
        ok = yield ("until", lambda: "is a int attribute; authoring float over it" in message(checker), POLL_TIMEOUT_S)
        check(ok, "the warning names the mismatch (%r)" % (message(checker),))
        gt.shot("usd-setattribute-type-warning.png")
        require(set_choice(param(checker, "valueType"), "int"), "change Value Type back to int")
        ok = yield ("until", lambda: message(checker) == "", POLL_TIMEOUT_S)
        check(ok, "the warning clears when the types agree (%r)" % (message(checker),))
        viewer.connectInput(0, set_attr)

        gt.section("primvar")
        require(set_choice(param(set_attr, "valueType"), "string"), "start from another type")
        param(set_attr, "primvar").setValue(True)
        param(set_attr, "attributeName").setValue("displayColor")
        ok = yield ("until", lambda: selected_type(set_attr) == "color3f", POLL_TIMEOUT_S)
        check(ok, "typing displayColor with Primvar on sets Value Type to color3f (got %r)" % (selected_type(set_attr),))

        gt.section("saved type is kept")
        param(set_attr, "primvar").setValue(False)
        param(set_attr, "attributeName").setValue("doubleSided")
        ok = yield ("until", lambda: selected_type(set_attr) == "bool", POLL_TIMEOUT_S)
        require(ok, "doubleSided sets bool again")
        require(set_choice(param(set_attr, "valueType"), "half"), "hand-pick half")
        yield ("sleep", SETTLE_MS)
        set_attr_name = set_attr.getScriptName()
        path = os.path.join(gt.OUT, PROJECT_NAME)
        require(app.saveProjectAs(path), "save the project to " + path)
        require(app.resetProject(), "reset the project")
        yield ("sleep", SETTLE_MS)
        reloaded = app.loadProject(path)
        require(reloaded is not None, "reopen " + path)
        ok = yield ("until", lambda: reloaded.getNode(set_attr_name) is not None, POLL_TIMEOUT_S)
        require(ok, "the node is back after reopening")
        yield ("sleep", 2 * SETTLE_MS)
        back = reloaded.getNode(set_attr_name)
        check(selected_type(back) == "half", "the hand-picked type survives the load (got %r)" % (selected_type(back),))
        gt.finish()

    run(steps())
except Exception:
    try:
        check(False, "setup error: " + traceback.format_exc())
        gt.finish()
    except NameError:
        traceback.print_exc()
        os._exit(1)
