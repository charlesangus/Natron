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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <stdio.h>

#include <QApplication>
#include <QStringList>
#include <QtGlobal>

#include "gtest/gtest.h"

#include "Engine/AppManager.h"
#include "Engine/CLArgs.h"

using namespace NATRON_NAMESPACE;

namespace {
QApplication* g_qApp = 0;

// Natron's widgets need a QApplication, and AppManager::load() would otherwise build a
// QCoreApplication that a QApplication can no longer be created next to.
class WidgetTestAppManager
    : public AppManager {
public:
    // AppManager's defaults answer 72, a DPI rather than a ratio, which would scale every
    // TO_DPIX/TO_DPIY widget size 72 times over. A widget test lays out at the default DPI.
    virtual double getLogicalDPIXRATIO() const OVERRIDE FINAL
    {
        return 1.;
    }

    virtual double getLogicalDPIYRATIO() const OVERRIDE FINAL
    {
        return 1.;
    }

private:
    virtual void initializeQApp(int& argc,
                                char** argv) OVERRIDE FINAL
    {
        g_qApp = new QApplication(argc, argv);
    }
};
} // namespace

#if defined(_WIN32) && defined(UNICODE)
GTEST_API_ int
wmain(int argc, wchar_t** argv)
#else
GTEST_API_ int
main(int argc, char** argv)
#endif
{
    printf("Running main() from guimain.cpp\n");
    ::testing::InitGoogleTest(&argc, argv);
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    int retval;
    {
        WidgetTestAppManager manager;

        {
            int argc = 0;
            QStringList args;
#if defined(_WIN32) && defined(UNICODE)
            args << QString::fromWCharArray(argv[0]);
#else
            args << QString::fromUtf8(argv[0]);
#endif
            args << QString::fromUtf8("--clear-cache");
            args << QString::fromUtf8("--clear-openfx-cache");
            args << QString::fromUtf8("--no-settings");
            args << QString::fromUtf8("--settings") << QString::fromUtf8("useStdOFXPluginsLocation=False");
            CLArgs cl(args, true);
            if (!manager.load(argc, 0, cl)) {
                printf("Failed to load AppManager\n");

                return 1;
            }
        }
        retval = RUN_ALL_TESTS();
    }
    delete g_qApp;

    return retval;
}
