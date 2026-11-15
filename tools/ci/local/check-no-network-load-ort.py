# Makes Natron load ONNX Runtime the way opening Preferences does, for check-no-network.sh.
import os
import time

import NatronEngine

settings = NatronEngine.natron.getSettings()
enabled = settings.getParam("aiEnabled")
if enabled.getValue():
    enabled.setValue(False)
    enabled.setValue(True)
else:
    # Refreshes the detected providers without enabling AI, so the disabled path runs too.
    path = settings.getParam("aiOrtLibraryPath")
    original = path.getValue()
    path.setValue(original + "-")
    path.setValue(original)

# The GUI keeps Python's output in its Script Editor, so the result goes to a file.
with open(os.path.join(os.environ["HOME"], "load-ort.txt"), "w") as report:
    report.write("aiEnabled=%s providers=%s\n"
                 % (enabled.getValue(), settings.getParam("aiDetectedProviders").getValue()))

# NatronRenderer -t exits after the script, unloading ONNX Runtime before its first upload. A
# sleeping interpreter ignores SIGTERM, so the wait ends by itself rather than at the deadline.
if NatronEngine.natron.isBackground():
    time.sleep(30)
