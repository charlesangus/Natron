# Features save rebase churn, CI and UAT time

Group milestones into Features to avoid rebase churn, wait less on CI and builds, and make the best use of limited UAT time (user, 2026-11-05). A member need not be unacceptable on its own, which is a wider test than PLAN-FORMAT §9b's. The cost is that rows outside a Feature that depend on a member wait for its landing, so each group was checked for those rows and for cycles between Features.

A member's gate lines that ask for the user (UAT AppImage, screenshots to approve, manual hardware checks) are collected once at the Feature's UAT, and the member's automated checks still run at its own gate. Decisions a member needs in order to continue, such as a spike's mechanism choice, still go to the user as `response`.

Registered on this basis: F3 - Cycles And MoonRay Delegates through F14 - Map Blend I-Nodes, and M345 - VDB Rendering Fixes moved into F1 - VDB Volumes. Landing order follows the dependencies: F3 - Cycles And MoonRay Delegates first; then F4 - Heavy Scene Viewer and F9 - Delegate Projection; then F1 - VDB Volumes, F2 - Native Shader Networks and F7 - Natron Utility Core.

Builds drop less than CI and UAT, because §9b still runs each member's gate at its merge and every gate again at ship.
