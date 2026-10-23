import builtins

import NatronGui
from PySide6.QtWidgets import QApplication

builtins.m158_startup_order = getattr(builtins, "m158_startup_order", []) + ["initGui.py"]

_menu = NatronGui.natron.nodesMenu()
_windows = [w for w in QApplication.topLevelWidgets() if w.metaObject().className().split("::")[-1] == "Gui"]

builtins.m158_init_gui_state = {
    "runs": getattr(builtins, "m158_init_gui_state", {}).get("runs", 0) + 1,
    "had_nodes_menu": _menu is not None,
    "nodes_menu_actions": len(_menu.actions()) if _menu is not None else 0,
    "windows": len(_windows),
    "had_app1": "app1" in globals(),
    "startup_fired": 0,
    "live_fired": 0,
}

if _menu is not None:
    _menu.addAction("M158 initGui item")


def m158StartupCommand():
    builtins.m158_init_gui_state["startup_fired"] += 1


NatronGui.natron.addMenuCommand("M158 Init/Startup command", "m158StartupCommand")
