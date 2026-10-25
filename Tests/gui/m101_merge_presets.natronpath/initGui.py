# Runs the bundled Gui/Resources/PyPlugs/initGui.py, which a build-tree Natron does not find (only
# a staged bundle has it under Plugins/PyPlugs). The script adds each preset once, so a bundle that
# also runs its own copy still gets one set.
import os as _m101_os

_m101_bundled_init_gui = _m101_os.path.join(_m101_os.environ.get("NATRON_GUI_TEST_ROOT", _m101_os.getcwd()),
                                            "Gui", "Resources", "PyPlugs", "initGui.py")
with open(_m101_bundled_init_gui) as _m101_file:
    exec(compile(_m101_file.read(), _m101_bundled_init_gui, "exec"))
