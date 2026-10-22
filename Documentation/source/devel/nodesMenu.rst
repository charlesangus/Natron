.. _nodesMenu:

The node menu
=============

The node toolbar on the left of the interface and the right-click menu of the node graph show
the same menu. There is a single instance for the whole process, a ``QMenu`` whose object name is
``NodesMenu``. It has one submenu per plug-in grouping (*Image*, *Filter*, *Merge*...) and one
``QAction`` per plug-in.

:func:`natron.nodesMenu()<NatronGui.PyGuiApplication.nodesMenu>` returns it. It returns *None* when
Natron runs without a user interface, so a script that may also run in a terminal session must
test for that.

Adding items
------------

Add items with plain PySide ``QAction``\ s, as in Nuke. This example adds a *Plus* preset to the
*Merge* submenu. It creates a Merge node and sets its *operation* parameter::

    import NatronGui

    def createPlusMerge():
        node = app.createNode("net.sf.openfx.MergePlugin")
        node.getParam("operation").set("plus")

    menu = NatronGui.natron.nodesMenu()
    if menu is not None:
        # mergeAction must stay referenced while its submenu is in use.
        mergeAction = next(a for a in menu.actions() if a.text() == "Merge")
        mergeAction.menu().addAction("Plus").triggered.connect(createPlusMerge)

The new item appears in the toolbar button of its submenu and in the graph's right-click menu.
A submenu you add to the top level of the node menu does not get a toolbar button; it
is only reachable from the right-click menu and the tab menu.

.. warning::

    A ``QMenu`` returned by ``QAction.menu()`` becomes "already deleted" in Python once the Python wrapper of that
    ``QAction`` is garbage-collected, although the C++ menu still exists. Keep a reference
    to the action for as long as you use its submenu, as the ``mergeAction`` variable does above.
    Do not chain ``.menu()`` onto an action you did not store.

Submenus can also be found without going through their action, since each one is a child of the menu above it::

    from PySide6.QtWidgets import QMenu

    mergeMenu = next(m for m in menu.findChildren(QMenu) if m.title() == "Merge")

Startup order
-------------

**initGui.py** runs once per process, after the first window and the node menu have been built,
so it can add to the live menu directly. **init.py** keeps its earlier timing and cannot reach the menu.
See :ref:`startupScripts`.

:func:`addMenuCommand<NatronGui.PyGuiApplication.addMenuCommand>` called while a window exists inserts
the command into the menubar of every open window at once. Called earlier, the command is queued
until the first window is built.

Nothing was kept for backward compatibility. A script that relied on **initGui.py** running before
any window existed has to adapt.

The tab menu
------------

The tab menu lists every item of the node menu, so items added from Python show up in it
under their path below ``NodesMenu``, for example ``Merge/Plus``. A plug-in without
a grouping is listed as ``Misc/<label>``.

A node created by choosing a tab-menu item is placed at the cursor, in the node graph the menu was opened over.
This includes nodes that a Python item creates with ``app.createNode(...)``: the item needs no positioning code.
