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

#include <memory>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QString>
#include <QThreadPool>
#include <QUndoStack>
#include <QWidget>

#include <gtest/gtest.h>

#include <ofxImageEffect.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Nodes/Channel/Shuffle.h"
#include "Engine/Project.h"

#include "Gui/Button.h"
#include "Gui/KnobGuiContainerI.h"
#include "Gui/KnobGuiShuffleMap.h"

NATRON_NAMESPACE_USING

namespace {

// The GuiTests target does not define NATRON_TESTS_FIXTURES_DIR, and CMake compiles every
// source by its absolute path, so the fixtures are found next to this file instead.
std::string
fixturePath(const char* name)
{
#ifdef NATRON_TESTS_FIXTURES_DIR
    const QString dir = QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR);
#else
    const QString dir = QFileInfo(QString::fromUtf8(__FILE__)).absolutePath() + QString::fromUtf8("/fixtures");
#endif

    return (dir + QString::fromUtf8("/") + QString::fromUtf8(name)).toStdString();
}

class MatrixTestContainer
    : public KnobGuiContainerI {
public:
    explicit MatrixTestContainer(QWidget* widget)
        : KnobGuiContainerI(widget)
        , _stack()
    {
    }

    virtual ~MatrixTestContainer() OVERRIDE
    {
    }

    virtual Gui* getGui() const OVERRIDE FINAL
    {
        return 0;
    }

    virtual const QUndoCommand* getLastUndoCommand() const OVERRIDE FINAL
    {
        return _stack.count() > 0 ? _stack.command(_stack.count() - 1) : 0;
    }

    virtual void pushUndoCommand(QUndoCommand* cmd) OVERRIDE FINAL
    {
        _stack.push(cmd);
    }

    virtual KnobGuiPtr getKnobGui(const KnobIPtr& /*knob*/) const OVERRIDE FINAL
    {
        return KnobGuiPtr();
    }

    virtual int getItemsSpacingOnSameLine() const OVERRIDE FINAL
    {
        return 0;
    }

    QUndoStack& getUndoStack()
    {
        return _stack;
    }

private:
    QUndoStack _stack;
};

void
flushEvents()
{
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents();
    }
}

} // namespace

// Read(flat-three-layers.exr) -> Shuffle (on B), with the mapping knob's GUI built into a
// bare widget. The fixture carries Color (RGBA), diffuse (RGB) and specular (RGB).
class ShuffleMatrixTest
    : public ::testing::Test {
protected:
    virtual void SetUp() OVERRIDE
    {
        _app = appPTR->getTopLevelInstance();
        ASSERT_TRUE(bool(_app));
        _app->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        destroyGui();
        _mapping.reset();
        _shuffle.reset();
        if (_app) {
            _app->getProject()->reset(false, true);
        }
        QThreadPool::globalInstance()->waitForDone();
    }

    void createShuffleOnFixture()
    {
        CreateNodeArgs readerArgs(PLUGINID_OFX_READOIIO, _app->getProject());
        readerArgs.addParamDefaultValue<std::string>(kOfxImageEffectFileParamName, fixturePath("flat-three-layers.exr"));
        NodePtr reader = _app->createNode(readerArgs);
        ASSERT_TRUE(bool(reader)) << "node creation failed for " << PLUGINID_OFX_READOIIO;

        CreateNodeArgs shuffleArgs(PLUGINID_NATRON_SHUFFLE, _app->getProject());
        _shuffle = _app->createNode(shuffleArgs);
        ASSERT_TRUE(bool(_shuffle));
        ASSERT_TRUE(_app->getProject()->connectNodes(Shuffle::eInputB, reader, _shuffle));

        _mapping = std::dynamic_pointer_cast<KnobShuffleMap>(_shuffle->getKnobByName(kShuffleParamMapping));
        ASSERT_TRUE(bool(_mapping));
    }

    void setLayer(const char* knobName,
                  const std::string& layerID)
    {
        KnobLayerSelectPtr knob = std::dynamic_pointer_cast<KnobLayerSelect>(_shuffle->getKnobByName(knobName));
        ASSERT_TRUE(bool(knob)) << knobName;
        knob->setLayer(layerID);
        flushEvents();
    }

    void createGui()
    {
        _panel.reset(new QWidget());
        _container.reset(new MatrixTestContainer(_panel.get()));
        _gui.reset(new KnobGuiShuffleMap(_mapping, _container.get()));
        _gui->initialize();

        QWidget* field = new QWidget(_panel.get());
        QHBoxLayout* layout = new QHBoxLayout(field);
        _gui->createGUI(field, 0, 0, 0, layout, true, 0, std::vector<KnobIPtr>());
        flushEvents();
    }

    void destroyGui()
    {
        // The widgets go first so nothing can reach them through the GUI object after.
        _panel.reset();
        _gui.reset();
        _container.reset();
    }

    int countCellButtons() const
    {
        int count = 0;
        for (int r = 0; r < _gui->getOutputRowCount(); ++r) {
            for (int c = 0; c < _gui->getSourceColumnCount(); ++c) {
                if (_gui->getCellButton(r, c)) {
                    ++count;
                }
            }
        }

        return count;
    }

    int findRow(int outSlot,
                int outIndex) const
    {
        for (int r = 0; r < _gui->getOutputRowCount(); ++r) {
            int slot = 0;
            int index = 0;
            if (_gui->getOutputRow(r, &slot, &index) && slot == outSlot && index == outIndex) {
                return r;
            }
        }

        return -1;
    }

    std::vector<ShuffleSource> out1Sources() const
    {
        std::vector<ShuffleSource> sources;
        for (int i = 0; i < 4; ++i) {
            sources.push_back(_mapping->getSource(1, i));
        }

        return sources;
    }

    AppInstancePtr _app;
    NodePtr _shuffle;
    std::shared_ptr<KnobShuffleMap> _mapping;
    std::unique_ptr<QWidget> _panel;
    std::unique_ptr<MatrixTestContainer> _container;
    std::shared_ptr<KnobGuiShuffleMap> _gui;
};

// in2 defaults to None, so it contributes no columns: 3 diffuse channels, keep, 0 and 1.
TEST_F(ShuffleMatrixTest, ColorFromDiffuseHasFourRowsOfSixButtons)
{
    createShuffleOnFixture();
    ASSERT_FALSE(HasFatalFailure());
    setLayer(kShuffleParamIn1, "diffuse");
    ASSERT_FALSE(HasFatalFailure());
    createGui();

    EXPECT_EQ(4, _gui->getOutputRowCount());
    EXPECT_EQ(6, _gui->getSourceColumnCount());
    EXPECT_EQ(24, countCellButtons());

    const QString in1Header = _gui->getSlotHeaderText(1);
    EXPECT_TRUE(in1Header.contains(QString::fromUtf8("diffuse"))) << in1Header.toStdString();
    EXPECT_FALSE(in1Header.contains(QString::fromUtf8("(not in input)"))) << in1Header.toStdString();
    EXPECT_TRUE(_gui->getSlotHeaderText(2).isEmpty());

    EXPECT_EQ(0, _gui->findSourceColumn(ShuffleSource::makeInput(1, 0)));
    EXPECT_EQ(1, _gui->findSourceColumn(ShuffleSource::makeInput(1, 1)));
    EXPECT_EQ(2, _gui->findSourceColumn(ShuffleSource::makeInput(1, 2)));
    EXPECT_EQ(3, _gui->findSourceColumn(ShuffleSource()));
    EXPECT_EQ(4, _gui->findSourceColumn(ShuffleSource::makeZero()));
    EXPECT_EQ(5, _gui->findSourceColumn(ShuffleSource::makeOne()));
    EXPECT_EQ(-1, _gui->findSourceColumn(ShuffleSource::makeInput(2, 0)));

    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(i, findRow(1, i));
    }

    ASSERT_TRUE(_gui->getCellButton(0, 0) != NULL);
    EXPECT_EQ(QString::fromUtf8("R"), _gui->getCellButton(0, 0)->text());
    EXPECT_EQ(QString::fromUtf8("G"), _gui->getCellButton(0, 1)->text());
    EXPECT_EQ(QString::fromUtf8("B"), _gui->getCellButton(0, 2)->text());

    // An empty mapping keeps every channel.
    const int keep = _gui->findSourceColumn(ShuffleSource());
    for (int r = 0; r < _gui->getOutputRowCount(); ++r) {
        for (int c = 0; c < _gui->getSourceColumnCount(); ++c) {
            EXPECT_EQ(c == keep, _gui->getCellButton(r, c)->isChecked()) << "row " << r << " column " << c;
        }
    }
    EXPECT_TRUE(_gui->getResetButton() != NULL);
}

TEST_F(ShuffleMatrixTest, OneClickChangesOneValueAndOneUndoRestoresIt)
{
    createShuffleOnFixture();
    ASSERT_FALSE(HasFatalFailure());
    setLayer(kShuffleParamIn1, "diffuse");
    ASSERT_FALSE(HasFatalFailure());
    createGui();

    const int row = findRow(1, 0);
    const int column = _gui->findSourceColumn(ShuffleSource::makeInput(1, 1));
    ASSERT_GE(row, 0);
    ASSERT_GE(column, 0);
    Button* cell = _gui->getCellButton(row, column);
    ASSERT_TRUE(cell != NULL);
    ASSERT_TRUE(cell->isEnabled());

    const std::vector<ShuffleSource> before = out1Sources();
    const int undoCountBefore = _container->getUndoStack().count();

    cell->click();
    flushEvents();

    EXPECT_EQ(undoCountBefore + 1, _container->getUndoStack().count());
    const std::vector<ShuffleSource> after = out1Sources();
    ASSERT_EQ(before.size(), after.size());
    for (std::size_t i = 0; i < after.size(); ++i) {
        if (i == 0) {
            EXPECT_TRUE(after[i] == ShuffleSource::makeInput(1, 1));
        } else {
            EXPECT_TRUE(after[i] == before[i]) << "output channel " << i;
        }
    }
    EXPECT_EQ(1u, _mapping->getRows().size());
    EXPECT_TRUE(cell->isChecked());

    _container->getUndoStack().undo();
    flushEvents();

    const std::vector<ShuffleSource> undone = out1Sources();
    for (std::size_t i = 0; i < undone.size(); ++i) {
        EXPECT_TRUE(undone[i] == before[i]) << "output channel " << i;
    }
    EXPECT_TRUE(_mapping->getRows().empty());
    EXPECT_FALSE(cell->isChecked());
    EXPECT_TRUE(_gui->getCellButton(row, _gui->findSourceColumn(ShuffleSource()))->isChecked());
}

TEST_F(ShuffleMatrixTest, DepthOutputHasOneRow)
{
    createShuffleOnFixture();
    ASSERT_FALSE(HasFatalFailure());
    createGui();
    EXPECT_EQ(4, _gui->getOutputRowCount());

    setLayer(kShuffleParamOut1, "depth");
    ASSERT_FALSE(HasFatalFailure());

    EXPECT_EQ(1, _gui->getOutputRowCount());
    EXPECT_EQ(0, findRow(1, 0));
}

TEST_F(ShuffleMatrixTest, Out2RowsShowOnlyWhileOut2IsSet)
{
    createShuffleOnFixture();
    ASSERT_FALSE(HasFatalFailure());
    createGui();
    EXPECT_EQ(4, _gui->getOutputRowCount());
    EXPECT_EQ(-1, findRow(2, 0));

    setLayer(kShuffleParamOut2, "depth");
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(5, _gui->getOutputRowCount());
    EXPECT_EQ(4, findRow(2, 0));

    setLayer(kShuffleParamOut2, std::string());
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(4, _gui->getOutputRowCount());
    EXPECT_EQ(-1, findRow(2, 0));
}

// depth is a project layer the fixture does not carry.
TEST_F(ShuffleMatrixTest, AbsentSlotLayerGreysItsColumnsAndShowsTheMarker)
{
    createShuffleOnFixture();
    ASSERT_FALSE(HasFatalFailure());
    setLayer(kShuffleParamIn1, "depth");
    ASSERT_FALSE(HasFatalFailure());
    createGui();

    const QString in1Header = _gui->getSlotHeaderText(1);
    EXPECT_TRUE(in1Header.contains(QString::fromUtf8("(not in input)"))) << in1Header.toStdString();

    const int column = _gui->findSourceColumn(ShuffleSource::makeInput(1, 0));
    ASSERT_EQ(0, column);
    EXPECT_EQ(-1, _gui->findSourceColumn(ShuffleSource::makeInput(1, 1)));
    for (int r = 0; r < _gui->getOutputRowCount(); ++r) {
        EXPECT_FALSE(_gui->getCellButton(r, column)->isEnabled()) << "row " << r;
        EXPECT_TRUE(_gui->getCellButton(r, _gui->findSourceColumn(ShuffleSource()))->isEnabled()) << "row " << r;
    }
}

TEST_F(ShuffleMatrixTest, ResetIsOneUndoStep)
{
    createShuffleOnFixture();
    ASSERT_FALSE(HasFatalFailure());
    _mapping->setSource(1, 0, ShuffleSource::makeZero());
    _mapping->setSource(1, 3, ShuffleSource::makeOne());
    createGui();

    ASSERT_TRUE(_gui->getResetButton() != NULL);
    const int undoCountBefore = _container->getUndoStack().count();
    _gui->getResetButton()->click();
    flushEvents();

    EXPECT_EQ(undoCountBefore + 1, _container->getUndoStack().count());
    EXPECT_TRUE(_mapping->getRows().empty());

    _container->getUndoStack().undo();
    flushEvents();

    EXPECT_TRUE(_mapping->getSource(1, 0) == ShuffleSource::makeZero());
    EXPECT_TRUE(_mapping->getSource(1, 3) == ShuffleSource::makeOne());
    EXPECT_EQ(2u, _mapping->getRows().size());
}
