# Startup script for Tests/gui/m161_startup_compositor.py, modelled on a user's initGui.py that
# made the startup compositor crash reproducible: it adds an action under the Merge submenu of
# the nodes menu while the main window is being built.
import os

from PySide6.QtGui import QAction
import NatronGui

m = NatronGui.natron.nodesMenu()
# Held at module level: PySide invalidates the submenu's wrapper once this one is collected.
sub = [a for a in m.actions() if a.text() == "Merge"][0]
act = QAction("M161 Merge Plus", m)
act.triggered.connect(lambda: NatronGui.natron.getGuiInstance(0).createNode("Merge"))
sub.menu().addAction(act)

# Natron's sys.stderr catcher has no flush(), so the marker goes to the descriptor itself.
os.write(2, b"[m161-fixture] initGui.py ran\n")
