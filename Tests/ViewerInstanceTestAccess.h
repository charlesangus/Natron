/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

#ifndef Tests_ViewerInstanceTestAccess_h
#define Tests_ViewerInstanceTestAccess_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/ViewerInstance.h"
#include "Engine/ViewerInstancePrivate.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Stands in for a render request starting and for it reaching the display, so that a test can drive a
 * viewer's stale state without rendering.
 **/
struct ViewerInstanceTestAccess {
    // Returns the age of the request.
    static U64 requestRender(ViewerInstance* viewer,
                             int textureIndex,
                             bool canAbort,
                             bool isSequential)
    {
        return viewer->_imp->createNewRenderRequest(textureIndex, canAbort, isSequential)->getRenderAge();
    }

    static void renderShown(ViewerInstance* viewer,
                            int textureIndex,
                            U64 renderAge)
    {
        viewer->_imp->checkAndUpdateDisplayAge(textureIndex, renderAge);
    }
};

NATRON_NAMESPACE_EXIT

#endif // Tests_ViewerInstanceTestAccess_h
