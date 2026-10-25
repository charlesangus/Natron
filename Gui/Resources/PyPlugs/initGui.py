# Bundled GUI start-up script, run once after the first window is created (see
# AppManager::getAllNonOFXPluginsPaths(), which finds it under <bin>/../Plugins/PyPlugs).
#
# Adds the Merge presets to the nodes menu's Merge submenu, which the toolbox, the node graph's
# right-click menu and the tab menu all show. A preset is the Merge node with its operation set,
# not a plug-in of its own, so projects only ever hold Merge.
#
# Every initGui.py runs in the __main__ namespace, so everything here lives inside one function
# and only _natronMergePresetActions is left behind.

import NatronGui


def _natronAddMergePresets():
    # Menu label, then the Merge operation (an option ID of its "operation" choice, from MergeOps.h).
    presets = [
        ("Plus", "plus"),
        ("Matte", "matte"),
        ("Multiply", "multiply"),
        ("In", "in"),
        ("Out", "out"),
        ("Screen", "screen"),
        ("Max", "max"),
        ("Min", "min"),
        ("Difference", "difference"),
    ]
    # Set on each preset action to its operation, so scripts and tests can tell the presets apart
    # from plug-in actions with the same label.
    presetProperty = "natronMergePreset"

    def createPreset(operation):
        # Created in the active project with no group, so a tab menu's creation context, when the
        # action is chosen from one, decides the graph and the position.
        app = NatronGui.natron.getActiveInstance()
        if app is None:
            return None
        node = app.createNode("Merge")
        if node is None:
            return None
        param = node.getParam("operation")
        if param is not None:
            param.set(operation)
        return node

    # PySide ties a returned child's wrapper to its owner's, so the menus and actions are kept for
    # the life of the process.
    keep = []
    menu = NatronGui.natron.nodesMenu()
    if menu is None:
        return keep
    keep.append(menu)
    submenu = None
    # The plug-in submenus carry their grouping as the natronGrouping property.
    for action in menu.actions():
        if action.property("natronGrouping") == "Merge" and action.menu() is not None:
            keep.append(action)
            submenu = action.menu()
            break
    if submenu is None:
        submenu = menu.addMenu("Merge")
    keep.append(submenu)
    # Another search path may hold a copy of this script: add each preset once.
    present = set(a.property(presetProperty) for a in submenu.actions())
    for label, operation in presets:
        if operation in present:
            continue
        action = submenu.addAction(label)
        action.setObjectName("MergePreset" + label)
        action.setProperty(presetProperty, operation)
        action.triggered.connect(lambda checked=False, op=operation: createPreset(op))
        keep.append(action)
    return keep


_natronMergePresetActions = globals().get("_natronMergePresetActions", []) + _natronAddMergePresets()
del _natronAddMergePresets
