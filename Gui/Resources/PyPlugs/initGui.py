# Bundled GUI start-up script, run once after the first window is created (see
# AppManager::getAllNonOFXPluginsPaths(), which finds it under <bin>/../Plugins/PyPlugs).
#
# Adds the Merge Presets submenu and the All Layers items to the nodes menu's Merge submenu, which
# the toolbox, the node graph's right-click menu and the tab menu all show. A preset is the Merge node with its operation set,
# not a plug-in of its own, so projects only ever hold Merge.
#
# Every initGui.py runs in the __main__ namespace, so everything here lives inside one function
# and only _natronMergePresetActions is left behind.

import NatronGui


def _natronAddMergePresets():
    # Menu label, then the Merge operation (an option ID of its "operation" choice, from MergeOps.h).
    # None of these carries a shortcut here: the nodes menu's Mask, Stencil and All Layers
    # actions are bound by object name in GuiApplicationManager10.cpp.
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
        ("Mask", "mask"),
        ("Stencil", "stencil"),
    ]
    # Label, then operation, of the actions that also set the Merge's "layers" to All.
    allLayersItems = [
        ("Mask All Layers", "mask"),
        ("Stencil All Layers", "stencil"),
    ]
    # Set on each preset action to its operation, so scripts and tests can tell the presets apart
    # from plug-in actions with the same label.
    presetProperty = "natronMergePreset"
    presetsMenuLabel = "Merge Presets"

    def setAllLayers(node):
        # The "layers" knob is a multi-select list with an All entry. Without the knob or the
        # entry the node keeps its default layers.
        param = node.getParam("layers")
        if param is None:
            return
        try:
            count = len(param.getOptions())
            names = [param.getOption(i) for i in range(count)]
        except Exception:
            return
        if "All" not in names:
            return
        try:
            param.set("All")
        except Exception:
            try:
                param.set(names.index("All"))
            except Exception:
                pass

    def createMerge(operation, allLayers=False):
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
        if allLayers:
            setAllLayers(node)
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
    # Another search path may hold a copy of this script: add each item once.
    presetsMenu = None
    for action in submenu.actions():
        if action.menu() is not None and action.objectName() == "MergePresetsMenu":
            keep.append(action)
            presetsMenu = action.menu()
            break
    if presetsMenu is None:
        presetsMenu = submenu.addMenu(presetsMenuLabel)
        presetsMenu.menuAction().setObjectName("MergePresetsMenu")
        for label, operation in presets:
            action = presetsMenu.addAction(label)
            # The object names of Mask and Stencil are the IDs of their shortcuts.
            action.setObjectName("MergePreset" + label)
            action.setProperty(presetProperty, operation)
            action.triggered.connect(lambda checked=False, op=operation: createMerge(op))
            keep.append(action)
        for label, operation in allLayersItems:
            action = submenu.addAction(label)
            action.setObjectName("Merge" + label.replace(" ", ""))
            action.triggered.connect(lambda checked=False, op=operation: createMerge(op, True))
            keep.append(action)
    keep.append(presetsMenu)
    return keep


_natronMergePresetActions = globals().get("_natronMergePresetActions", []) + _natronAddMergePresets()
del _natronAddMergePresets
