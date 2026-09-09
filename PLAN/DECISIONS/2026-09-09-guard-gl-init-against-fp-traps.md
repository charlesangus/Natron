# The GL init path gets the same FP-trap guard every other foreign call has

`App/NatronApp_main.cpp` arms `FE_DIVBYZERO|FE_INVALID|FE_OVERFLOW` for the whole
process under `-DDEBUG`, and every place Natron enters third-party code disarms
them for the duration — `AppManager::exec()`, `Node.cpp`, `Project.cpp`,
`OfxImageEffectInstance.cpp` all take a
`boost_adaptbx::floating_point::exception_trapping trap(0)`.
`AppManager::initializeOpenGLFunctionsOnce()` does not, and it calls into the GL
driver. So a debug build dies of SIGFPE inside the software rasteriser on any host
without hardware GL, which is every CI runner and every headless gate.

It gets the same guard, in its own milestone. The alternative — leaving debug
builds unable to start under Xvfb — would keep every future headless GUI
verification on RelWithDebInfo only, which is the wrong constraint to accept for a
fix this contained. Pre-existing since upstream `300ddcbd0` (2018); not a
regression from any milestone here.

Decided by the user 2026-09-09, from the diagnosis recorded in M23's `## Decisions`.
