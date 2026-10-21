"""UAT for the GPU page of the Preferences window (M108): the page lists GPU compute, GPU device and
GPU memory budget, the device menu offers Automatic plus every Vulkan device, the Threading page no
longer carries those knobs, no OpenGL preference text is left anywhere in the dialog, and choosing
the llvmpipe device and saving stores its UUID as the gpuDevice choice ID. A screenshot of the GPU
page goes to $NATRON_GUI_TEST_OUT/m108-gpu-prefs.png (default build/gui-test-out/).

Usage:
    VK_ICD_FILENAMES=<path to lvp_icd.x86_64.json> \\
        Tests/gui/run-gui-test.sh Tests/gui/m108_gpu_prefs.py <Natron.AppImage or release binary>
run in the natron-dev container. VK_ICD_FILENAMES must name the lavapipe ICD (and only it), so
that the one Vulkan device is llvmpipe; without it the script fails at "a device entry containing
llvmpipe". run-gui-test.sh gives the AppImage run a throwaway HOME, so the saved gpuDevice never
reaches the container's own Natron.conf.

Manual next-launch check (not automated: the script cannot relaunch Natron). Preferences are stored
by QSettings in $HOME/.config/INRIA/Natron.conf (organisation "INRIA", application "Natron",
INI format, under $XDG_CONFIG_HOME when that is set), one `name=value` line per knob in the [General]
section, and a choice knob is stored as its entry ID. Create a HOME that holds

    [General]
    gpuDevice=<lavapipe uuid>

where <lavapipe uuid> is the 32 lowercase hex digits this script prints in its
"NOTE llvmpipe device ID" line. Then run, with VK_ICD_FILENAMES listing both the RADV and the
lavapipe ICD json files separated by a colon:

    HOME=<that home> VK_ICD_FILENAMES=<radv_icd.json>:<lvp_icd.json> NatronRenderer -h

and check that the output has a line starting

    GPU: Preferences device "<lavapipe uuid>" (llvmpipe

so the saved device wins over the one automatic selection would have picked (RADV). The line is
logged by Engine/AppManager.cpp.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.environ.get("NATRON_GUI_TEST_ROOT", os.getcwd()), "Tests", "gui"))

STARTUP_TIMEOUT_S = 60.0
POLL_TIMEOUT_S = 20.0
SETTLE_MS = 400

# Phrases of the removed OpenGL preferences (lower case; matched against lower-cased text).
BANNED_TEXT = ("opengl renderer", "no. of opengl contexts", "opengl rendering")
# Labels of the GPU page's knobs (Settings.cpp initializeKnobsGPU), lower case.
GPU_LABELS = ("gpu compute", "gpu device", "gpu memory budget")
# The auto entry of gpuDevice has this ID (kGpuDeviceAutoID) and this label.
AUTO_LABEL = "Automatic"
LLVMPIPE = "llvmpipe"

try:
    from PySide6.QtCore import QMetaObject, QRect, QSettings, Qt, Q_ARG
    from PySide6.QtGui import QGuiApplication
    from PySide6.QtWidgets import (QAbstractButton, QApplication, QGroupBox, QLabel,
                                   QTreeWidget)

    import NatronEngine
    import guitest as gt
    from guitest import check, report, require

    def gui_widget():
        found = gt.widgets_of("Gui")
        return found[0] if found else None

    def node_graph_up():
        return any(v.isVisible() for v in gt.widgets_of("NodeGraph"))

    def open_prefs():
        for w in gt.widgets_of("PreferencesPanel"):
            if w.isVisible():
                return w
        return None

    def page_tree(prefs):
        """The tree that lists the pages: the QTreeWidget that has a Threading item."""
        for tree in prefs.findChildren(QTreeWidget):
            if tree.findItems("Threading", Qt.MatchFlag.MatchExactly | Qt.MatchFlag.MatchRecursive, 0):
                return tree
        return None

    def select_page(prefs, name):
        tree = page_tree(prefs)
        if tree is None:
            return False
        items = tree.findItems(name, Qt.MatchFlag.MatchExactly | Qt.MatchFlag.MatchRecursive, 0)
        if not items:
            return False
        tree.setCurrentItem(items[0])
        return True

    def visible_labels(prefs):
        out = []
        for w in prefs.findChildren(QLabel):
            try:
                if w.isVisible() and w.text().strip():
                    out.append(w.text().strip())
            except RuntimeError:
                continue
        return out

    def visible_combos(prefs):
        """The Natron ComboBox widgets that are on screen, as (widget, items) pairs. KnobComboBox
        has no Q_OBJECT, so its meta-object class name is ComboBox; the items come from the
        natronItems property (Gui/ComboBox.h)."""
        out = []
        for w in gt.widgets_of("ComboBox"):
            try:
                if w.isVisible() and prefs.isAncestorOf(w):
                    out.append((w, [str(i) for i in (w.property("natronItems") or [])]))
            except RuntimeError:
                continue
        return out

    def dialog_texts(prefs):
        """Every piece of text in the dialog, on any page, visible or not: labels, buttons, group
        titles, combo items and the page tree. Tooltips are left out on purpose: the Plugins
        page's OpenGL column tooltip legitimately says "OpenGL rendering"."""
        texts = []
        for w in prefs.findChildren(QLabel):
            texts.append(w.text())
        for w in prefs.findChildren(QAbstractButton):
            texts.append(w.text())
        for w in prefs.findChildren(QGroupBox):
            texts.append(w.title())
        for w in gt.widgets_of("ComboBox"):
            try:
                if prefs.isAncestorOf(w):
                    texts.extend(str(i) for i in (w.property("natronItems") or []))
            except RuntimeError:
                continue

        def walk(item):
            for c in range(item.columnCount()):
                texts.append(item.text(c))
            for k in range(item.childCount()):
                walk(item.child(k))

        for tree in prefs.findChildren(QTreeWidget):
            hdr = tree.headerItem()
            if hdr is not None:
                walk(hdr)
            for t in range(tree.topLevelItemCount()):
                walk(tree.topLevelItem(t))
        return [t for t in texts if t]

    def screen_crop(name, widget):
        QApplication.processEvents()
        screen = QGuiApplication.primaryScreen().grabWindow(0)
        rect = QRect(widget.mapToGlobal(widget.rect().topLeft()), widget.size())
        rect = rect.intersected(QRect(0, 0, screen.width(), screen.height()))
        path = os.path.join(gt.OUT, name)
        if screen.copy(rect).save(path):
            report("SHOT " + path)
        else:
            report("NOTE could not save screenshot " + path)

    def steps():
        gt.section("gpu-prefs")
        settings = NatronEngine.natron.getSettings()

        ok = yield ("until", lambda: node_graph_up() and gui_widget() is not None, STARTUP_TIMEOUT_S)
        require(ok, "the main window and node graph are on screen (looked for widget classes Gui and NodeGraph)")

        gpu_device = settings.getParam("gpuDevice")
        require(gpu_device is not None, "Settings has a parameter named gpuDevice (Natron.getSettings().getParam)")
        require(settings.getParam("gpuComputeMode") is not None, "Settings has a parameter named gpuComputeMode")
        require(settings.getParam("gpuMemoryBudgetMB") is not None, "Settings has a parameter named gpuMemoryBudgetMB")

        # Open Preferences the way m56_visual.py does.
        QMetaObject.invokeMethod(gui_widget(), "showSettings")
        ok = yield ("until", lambda: open_prefs() is not None, POLL_TIMEOUT_S)
        prefs = open_prefs()
        require(ok and prefs is not None, "the Preferences window opened (widget class PreferencesPanel, slot Gui.showSettings)")
        prefs.raise_()
        yield ("sleep", SETTLE_MS)

        require(page_tree(prefs) is not None,
                "found the page tree (a QTreeWidget in the Preferences window with a \"Threading\" item)")

        # --- The GPU page ---
        require(select_page(prefs, "GPU"), "the page tree has a \"GPU\" item")
        yield ("sleep", SETTLE_MS)

        def gpu_page_shown():
            low = [l.lower() for l in visible_labels(prefs)]
            return all(any(g in l for l in low) for g in GPU_LABELS)

        ok = yield ("until", gpu_page_shown, POLL_TIMEOUT_S)
        labels = visible_labels(prefs)
        report("NOTE GPU page labels: %r" % (labels,))
        for want in GPU_LABELS:
            check(any(want in l.lower() for l in labels),
                  "the GPU page shows a label containing \"%s\" (looked at visible QLabel texts)" % want)
        check(ok, "the GPU page became visible within %ds" % POLL_TIMEOUT_S)

        device_combo, device_items = None, []
        for combo, items in visible_combos(prefs):
            if AUTO_LABEL in items:
                device_combo, device_items = combo, items
                break
        require(device_combo is not None,
                "the GPU page has a combo whose items include \"%s\" (looked at visible ComboBox natronItems; "
                "combos seen: %r)" % (AUTO_LABEL, [i for _, i in visible_combos(prefs)]))
        report("NOTE GPU device items: %r" % (device_items,))
        check(device_items[0] == AUTO_LABEL, "the first GPU device entry is \"%s\" (got %r)" % (AUTO_LABEL, device_items[:1]))
        check(len(device_items) >= 2,
              "the GPU device menu lists a Vulkan device after Automatic (got %r; is VK_ICD_FILENAMES set?)" % (device_items,))
        llvmpipe_rows = [i for i, t in enumerate(device_items) if LLVMPIPE in t.lower()]
        require(bool(llvmpipe_rows),
                "a device entry containing \"%s\" (VK_ICD_FILENAMES=%r; items %r)"
                % (LLVMPIPE, os.environ.get("VK_ICD_FILENAMES"), device_items))
        row = llvmpipe_rows[0]
        check(device_items[row].endswith(")") and " (" in device_items[row],
              "the llvmpipe entry is labelled \"name (type)\" (got %r)" % (device_items[row],))

        modes = [items for _, items in visible_combos(prefs) if "Off" in items and "Auto" in items]
        check(bool(modes), "the GPU page has a GPU compute combo with items Off and Auto (looked at visible ComboBox natronItems)")

        # The menu IDs: ChoiceParam.getOption(i) returns the entry ID, not the label.
        check(gpu_device.getNumOptions() == len(device_items),
              "gpuDevice has as many entries as the menu shows (%d vs %d)" % (gpu_device.getNumOptions(), len(device_items)))
        check(gpu_device.getOption(0) == "auto", "gpuDevice entry 0 has ID \"auto\" (got %r)" % (gpu_device.getOption(0),))
        llvmpipe_id = gpu_device.getOption(row)
        report("NOTE llvmpipe device ID %s" % llvmpipe_id)
        check(len(llvmpipe_id) == 32 and all(c in "0123456789abcdef" for c in llvmpipe_id),
              "the llvmpipe entry ID is a 32-digit lower-case hex UUID (got %r)" % (llvmpipe_id,))

        # --- The Threading page ---
        require(select_page(prefs, "Threading"), "the page tree has a \"Threading\" item")
        yield ("sleep", SETTLE_MS)
        ok = yield ("until", lambda: not any("gpu device" in l.lower() for l in visible_labels(prefs))
                    and any("thread" in l.lower() for l in visible_labels(prefs)), POLL_TIMEOUT_S)
        labels = visible_labels(prefs)
        report("NOTE Threading page labels: %r" % (labels,))
        check(ok, "the Threading page became visible (a visible label containing \"thread\")")
        for want in GPU_LABELS:
            check(not any(want in l.lower() for l in labels),
                  "the Threading page no longer shows a label containing \"%s\"" % want)
        check(not any(AUTO_LABEL in items for _, items in visible_combos(prefs)),
              "the Threading page has no combo with the \"%s\" device entry" % AUTO_LABEL)

        # --- No OpenGL preference text anywhere ---
        texts = dialog_texts(prefs)
        report("NOTE %d text(s) scanned in the Preferences window" % len(texts))
        for banned in BANNED_TEXT:
            hits = [t for t in texts if banned in t.lower()]
            check(not hits, "no text in the Preferences window reads \"%s\" (found %r)" % (banned, hits[:3]))

        # --- Pick the llvmpipe device, screenshot, save ---
        require(select_page(prefs, "GPU"), "the page tree has a \"GPU\" item (second look)")
        yield ("sleep", SETTLE_MS)
        QMetaObject.invokeMethod(device_combo, "setCurrentIndex", Q_ARG(int, row))
        yield ("sleep", SETTLE_MS)
        check(gpu_device.getValue() == row,
              "picking the llvmpipe entry in the GPU device combo set gpuDevice to index %d (got %r)" % (row, gpu_device.getValue()))
        prefs.raise_()
        yield ("sleep", SETTLE_MS)
        screen_crop("m108-gpu-prefs.png", prefs)

        save = [b for b in prefs.findChildren(QAbstractButton) if b.isVisible() and b.text() == "Save"]
        require(len(save) == 1, "the Preferences window has one visible \"Save\" button (found %d)" % len(save))
        save[0].click()
        ok = yield ("until", lambda: open_prefs() is None, POLL_TIMEOUT_S)
        check(ok, "the Preferences window closed after Save")

        saved = NatronEngine.natron.getSettings().getParam("gpuDevice")
        saved_id = saved.getOption(saved.getValue())
        check(saved_id == llvmpipe_id,
              "natron.getSettings().getParam(\"gpuDevice\") holds the llvmpipe ID %s (got %r)" % (llvmpipe_id, saved_id))

        store = QSettings("INRIA", "Natron")
        store.sync()
        stored = str(store.value("gpuDevice"))
        check(stored == llvmpipe_id,
              "Natron.conf [General] gpuDevice is the llvmpipe ID %s (got %r in %s)" % (llvmpipe_id, stored, store.fileName()))

    gt.run(steps())
except Exception:
    import traceback
    with open(os.path.join(os.environ.get("NATRON_GUI_TEST_OUT", "."), "results.txt"), "a") as f:
        f.write("FAIL script error: " + traceback.format_exc() + "\n")
    os._exit(1)
