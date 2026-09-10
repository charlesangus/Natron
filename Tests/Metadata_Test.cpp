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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppManager.h"
#include "Engine/Node.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/OfxHost.h"
#include "Engine/Plugin.h"

#include <ofxMetadata.h>
#include <ofxProperty.h>

NATRON_NAMESPACE_USING

namespace {
// Node creation alone doesn't prove the OFX host can resolve the plugin by ID; a plugin
// that failed cache registration but still had a stale Node subclass around could pass
// createNode() and fail everywhere else. Checking both catches that gap.
struct MetadataPluginFixture
    : public BaseTest
{
    void assertPluginInstantiates(const char* pluginID)
    {
        const QString id = QString::fromUtf8(pluginID);

        Plugin* binary = appPTR->getPluginBinary(id, -1, -1, false);
        EXPECT_TRUE(binary != NULL) << "plugin cache lookup failed for " << pluginID;

        NodePtr node = createNode(id);
        ASSERT_TRUE(bool(node)) << "node creation failed for " << pluginID;
    }
};
} // namespace

// Each of the metadataView/Contribute/TimeCode example plugins loads via OFX_PLUGIN_PATH
// and is known to be enumerated by the plugin cache (see tools/ci/smoke_test.py). Neither
// fact implies the host can actually build a working Node around it -- describe/create
// action failures, missing clip declarations, or a param layout the host can't build knobs
// for would all show up only at node-creation time. This is that stronger check.
TEST_F(MetadataPluginFixture, MetadataViewInstantiatesAsNode)
{
    assertPluginInstantiates("org.openfx.examples.metadataView");
}

TEST_F(MetadataPluginFixture, MetadataContributeInstantiatesAsNode)
{
    assertPluginInstantiates("org.openfx.examples.metadataContribute");
}

TEST_F(MetadataPluginFixture, MetadataTimeCodeInstantiatesAsNode)
{
    assertPluginInstantiates("org.openfx.examples.metadataTimeCode");
}

// Support/Library/ofxsMetadata.cpp fetches the metadata suite optionally and quietly no-ops
// when it is absent, so every other test in this file would keep passing even if the host
// never actually vended the suite. This is the one case that would catch that.
TEST_F(MetadataPluginFixture, MetadataAndPropertySuiteAreFetchableFromOfxHost)
{
    const QString id = QString::fromUtf8("org.openfx.examples.metadataView");
    NodePtr node = createNode(id);
    ASSERT_TRUE(bool(node)) << "node creation failed for " << id.toStdString();

    OfxEffectInstance* ofxEffect = dynamic_cast<OfxEffectInstance*>( node->getEffectInstance().get() );
    ASSERT_TRUE(ofxEffect != NULL) << "node's effect instance is not backed by the OFX host";

    // ImageEffectPlugin/PluginHandle keep no back-pointer to the OfxHost that instantiated
    // them, so the host is reached via Natron's AppManager singleton rather than through the
    // node's plugin handle. AppManager only hands out a const OfxHost*, but fetchSuite() does
    // nothing but look suites up, so the const_cast is safe here.
    const Natron::OfxHost* constHost = appPTR->getOFXHost();
    ASSERT_TRUE(constHost != NULL);
    Natron::OfxHost* host = const_cast<Natron::OfxHost*>(constHost);

    EXPECT_TRUE(host->fetchSuite(kOfxPropertySuite, 1) != NULL) << "property suite v1 not vended";
    EXPECT_TRUE(host->fetchSuite(kOfxPropertySuite, 2) != NULL) << "property suite v2 not vended";
    EXPECT_TRUE(host->fetchSuite(kOfxMetadataSuite, 1) != NULL) << "metadata suite not vended";
}
