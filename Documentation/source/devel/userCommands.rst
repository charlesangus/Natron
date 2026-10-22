.. _userCommands:

User menu commands
===================

In Natron you can add multiple menu commands that will then be available to the user via
the menu. You can also assign it a shortcut and the user will be able to modify it
via the shortcuts editor.

Project-wide menu commands:
---------------------------

.. figure:: menuCommand.png
    :width: 300px
    :align: center

To add a project-wipe menu command to the application's menu-bar, you need to use the
:func:`addMenuCommand(grouping,function,key,modifiers)<>` of the :ref:`PyGuiApplication<PyGuiApplication>` class
to register it::

    def createIconViewer():
        ...

    #Add a custom menu entry with a shortcut to create our icon viewer
    NatronGui.natron.addMenuCommand("Inria/Scripts/IconViewer","createIconViewer",QtCore.Qt.Key.Key_L,QtCore.Qt.KeyboardModifier.ShiftModifier)


Note that this function is to be called on the whole **application** via the **natron** variable
and is not *per-project* unlike most functions that are generally called on the **app** object.

.. note::

    A command registered with :func:`addMenuCommand<NatronGui.PyGuiApplication.addMenuCommand>`
    before the first window exists (for example from **init.py**) is queued and added when
    that window is built. Called later, for example from **initGui.py** or the Script Editor,
    it is inserted at once into the menu-bar of every open window.
    Windows opened afterwards also get it.

To add entries to the node menu instead of the menu-bar, see :ref:`nodesMenu`.
