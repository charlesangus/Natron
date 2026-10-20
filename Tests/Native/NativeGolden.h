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

#ifndef NATRON_TESTS_NATIVEGOLDEN_H
#define NATRON_TESTS_NATIVEGOLDEN_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <string>
#include <vector>

#include "NativeParity.h"

#include "Engine/EngineFwd.h"
#include "Engine/RectI.h"

#define kGoldenWidth 64
#define kGoldenHeight 48

NATRON_NAMESPACE_ENTER

/**
 * @brief NATRON_GOLDEN_RECORD_DIR, or empty when the harness replays.
 **/
std::string goldenRecordDir();

/**
 * @brief Where replay reads references: NATRON_TESTS_FIXTURES_DIR/native-golden.
 **/
std::string goldenReferenceDir();

/**
 * @brief The window every case renders: the 64x48 project format at the origin, in pixel
 * coordinates at `mipmapLevel`.
 **/
RectI goldenWindow(unsigned mipmapLevel);

/**
 * @brief Renders every plane `node` presents over goldenWindow(mipmapLevel) and either records
 * each one under goldenRecordDir(), or compares it bit for bit with the reference under
 * goldenReferenceDir(). Two NaNs match whatever their payload. Recording also writes
 * <dir>/STAMP, the NATRON_GOLDEN_CONFIG of the build. A replay under a different configuration
 * compares within ParityTolerance::iir() instead and prints one line saying so. Files are
 * <dir>/<group>/<caseName>.f32 for the colour plane and <dir>/<group>/<caseName>.<layer>.f32 for
 * any other, in NativeParity's NPAR format.
 *
 * In replay, a missing reference, a recorded plane the node no longer presents, a size mismatch
 * and any differing value are gtest failures. Returns whether the case passed.
 **/
bool checkGolden(const NodePtr& node,
                 const std::string& group,
                 const std::string& caseName,
                 unsigned mipmapLevel,
                 double time = 1.);

/**
 * @brief A node under test. `name` prefixes every case and names its fixture directory. A
 * generator gets no source; every other node gets a noisy parity source on input 0 and one more on
 * each input labelled in `extraInputs`, each at its own origin.
 **/
struct GoldenNodeSpec {
    std::string name;
    std::string pluginID;
    int major = -1;
    bool generator = false;
    std::string maskInput;
    std::vector<std::string> extraInputs;
};

/**
 * @brief The nodes one case renders. `inputs` holds the sources on `extraInputs`, in order.
 **/
struct GoldenGraph {
    NodePtr node;
    NodePtr source;
    NodePtr mask;
    std::vector<NodePtr> inputs;
};

/**
 * @brief One knob setting of a node. With `mask`, a parity source is connected to the spec's
 * mask input, which is enabled on its alpha. `apply` sets the knobs and returns false after a
 * gtest failure.
 **/
struct GoldenVariant {
    std::string name;
    bool mask = false;
    std::function<bool(const GoldenGraph&)> apply;
};

/**
 * @brief For each variant, builds the graph in a reset 64x48 project twice: once on RGBA, once
 * with the node working on a non-colour layer, and checks both at render scale 1 and 0.5 with GPU
 * placement off. The non-colour layer is the "depth" target for a node whose layer knob is a
 * target, and otherwise the sources' "spec" plane, selected on the node's channel set or layer
 * select. Cases are named <name>_<variant>_<layer>_<s1|s05>.
 **/
void runGoldenCases(const AppInstancePtr& app,
                    const GoldenNodeSpec& spec,
                    const std::vector<GoldenVariant>& variants);

/**
 * @brief Variant helpers that report a missing knob or option through gtest and return false.
 **/
bool setGoldenChannelSelect(const NodePtr& node, const std::string& name, const std::string& value);
bool pressGoldenButton(const NodePtr& node, const std::string& name);

NATRON_NAMESPACE_EXIT

#endif // NATRON_TESTS_NATIVEGOLDEN_H
