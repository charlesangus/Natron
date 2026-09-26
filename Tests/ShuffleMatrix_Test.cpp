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

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLayout>
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
#include "Engine/TimeLine.h"

#include "Gui/Button.h"
#include "Gui/ComboBox.h"
#include "Gui/KnobGuiContainerI.h"
#include "Gui/KnobGuiLayerSelect.h"
#include "Gui/KnobGuiShuffleMap.h"
#include "Gui/LayerChannelRow.h"

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
        , _knobGuis()
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

    virtual KnobGuiPtr getKnobGui(const KnobIPtr& knob) const OVERRIDE FINAL
    {
        std::map<KnobI*, KnobGuiPtr>::const_iterator found = _knobGuis.find(knob.get());

        return found == _knobGuis.end() ? KnobGuiPtr() : found->second;
    }

    void registerKnobGui(const KnobIPtr& knob,
                         const KnobGuiPtr& knobGui)
    {
        _knobGuis[knob.get()] = knobGui;
    }

    void clearKnobGuis()
    {
        _knobGuis.clear();
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
    std::map<KnobI*, KnobGuiPtr> _knobGuis;
};

void
flushEvents()
{
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents();
    }
}

int
comboIndexOf(const LayerChannelRow* row,
             const char* text)
{
    return row->getComboEntries().indexOf(QString::fromUtf8(text));
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
        ASSERT_TRUE(_app->getProject()->connectNodes(Shuffle::eInputMain, reader, _shuffle));

        _mapping = std::dynamic_pointer_cast<KnobShuffleMap>(_shuffle->getKnobByName(kShuffleParamMapping));
        ASSERT_TRUE(bool(_mapping));
    }

    Shuffle* shuffleEffect() const
    {
        return dynamic_cast<Shuffle*>(_shuffle->getEffectInstance().get());
    }

    void setLayer(const char* knobName,
                  const std::string& layerID)
    {
        KnobLayerSelectPtr knob = std::dynamic_pointer_cast<KnobLayerSelect>(_shuffle->getKnobByName(knobName));
        ASSERT_TRUE(bool(knob)) << knobName;
        knob->setLayer(layerID);
        flushEvents();
    }

    // The in/out knobs get a GUI object of their own, as in a real panel where they are
    // secret but still built, so the matrix's dropdowns have one to push undo steps through.
    void createGui()
    {
        _panel.reset(new QWidget());
        _container.reset(new MatrixTestContainer(_panel.get()));

        static const char* const layerKnobs[] = {
            kShuffleParamIn1, kShuffleParamIn2, kShuffleParamOut1, kShuffleParamOut2
        };
        for (std::size_t i = 0; i < sizeof(layerKnobs) / sizeof(layerKnobs[0]); ++i) {
            KnobIPtr knob = _shuffle->getKnobByName(layerKnobs[i]);
            ASSERT_TRUE(bool(knob)) << layerKnobs[i];
            std::shared_ptr<KnobGuiLayerSelect> knobGui(new KnobGuiLayerSelect(knob, _container.get()));
            knobGui->initialize();
            _container->registerKnobGui(knob, knobGui);
            _layerKnobGuis.push_back(knobGui);
        }

        _gui.reset(new KnobGuiShuffleMap(_mapping, _container.get()));
        _gui->initialize();

        QWidget* field = new QWidget(_panel.get());
        QHBoxLayout* layout = new QHBoxLayout(field);
        _gui->createGUI(field, 0, 0, 0, layout, true, 0, std::vector<KnobIPtr>());
        _panel->show();
        settle();
    }

    void destroyGui()
    {
        // The widgets go first so nothing can reach them through the GUI object after.
        _panel.reset();
        _gui.reset();
        if (_container) {
            _container->clearKnobGuis();
        }
        _layerKnobGuis.clear();
        _container.reset();
    }

    // Runs the deferred refreshes and the queued shows of rebuilt cells, then lays the
    // grid out so geometries are final.
    void settle()
    {
        flushEvents();
        QWidget* matrix = _gui ? _gui->getMatrixWidget() : 0;
        if (matrix && matrix->layout()) {
            matrix->layout()->activate();
        }
        flushEvents();
    }

    LayerChannelRow* layerRow(KnobGuiShuffleMap::LayerRowEnum which) const
    {
        return _gui->getLayerRow(which);
    }

    void chooseInRow(KnobGuiShuffleMap::LayerRowEnum which,
                     const char* entry)
    {
        LayerChannelRow* row = layerRow(which);
        ASSERT_TRUE(row != NULL);
        const int index = comboIndexOf(row, entry);
        ASSERT_GE(index, 0) << entry << " not in " << row->getComboEntries().join(QString::fromUtf8(", ")).toStdString();
        row->getComboBox()->setCurrentIndex(index);
        settle();
    }

    std::string layerOf(const char* knobName) const
    {
        KnobLayerSelectPtr knob = std::dynamic_pointer_cast<KnobLayerSelect>(_shuffle->getKnobByName(knobName));

        return knob ? knob->getLayer() : std::string("<no knob>");
    }

    std::vector<Button*> columnsOfRow(int row,
                                      int slot) const
    {
        std::vector<Button*> buttons;
        for (int i = 0;; ++i) {
            const int column = _gui->findSourceColumn(ShuffleSource::makeInput(slot, i));
            if (column < 0) {
                break;
            }
            buttons.push_back(_gui->getCellButton(row, column));
        }

        return buttons;
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
    std::vector<KnobGuiPtr> _layerKnobGuis;
    std::shared_ptr<KnobGuiShuffleMap> _gui;
};

// in2 defaults to None, so it contributes no columns: 3 diffuse channels, 0 and 1.
TEST_F(ShuffleMatrixTest, ColorFromDiffuseHasFourRowsOfFiveButtons)
{
    createShuffleOnFixture();
    ASSERT_FALSE(HasFatalFailure());
    setLayer(kShuffleParamIn1, "diffuse");
    ASSERT_FALSE(HasFatalFailure());
    createGui();

    EXPECT_EQ(4, _gui->getOutputRowCount());
    EXPECT_EQ(5, _gui->getSourceColumnCount());
    EXPECT_EQ(20, countCellButtons());

    LayerChannelRow* in1Row = layerRow(KnobGuiShuffleMap::eLayerRowIn1);
    LayerChannelRow* in2Row = layerRow(KnobGuiShuffleMap::eLayerRowIn2);
    ASSERT_TRUE(in1Row != NULL);
    ASSERT_TRUE(in2Row != NULL);
    EXPECT_EQ(std::string("diffuse"), in1Row->getCurrentLayerID());
    EXPECT_FALSE(in1Row->hasAbsentMarker()) << in1Row->getCurrentComboText().toStdString();
    EXPECT_TRUE(in2Row->getCurrentLayerID().empty());
    EXPECT_EQ(QString::fromUtf8("None"), in2Row->getCurrentComboText());

    EXPECT_EQ(0, _gui->findSourceColumn(ShuffleSource::makeInput(1, 0)));
    EXPECT_EQ(1, _gui->findSourceColumn(ShuffleSource::makeInput(1, 1)));
    EXPECT_EQ(2, _gui->findSourceColumn(ShuffleSource::makeInput(1, 2)));
    EXPECT_EQ(3, _gui->findSourceColumn(ShuffleSource::makeZero()));
    EXPECT_EQ(4, _gui->findSourceColumn(ShuffleSource::makeOne()));
    EXPECT_EQ(-1, _gui->findSourceColumn(ShuffleSource::makeInput(2, 0)));

    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(i, findRow(1, i));
    }

    ASSERT_TRUE(_gui->getCellButton(0, 0) != NULL);
    EXPECT_EQ(QString::fromUtf8("R"), _gui->getCellButton(0, 0)->text());
    EXPECT_EQ(QString::fromUtf8("G"), _gui->getCellButton(0, 1)->text());
    EXPECT_EQ(QString::fromUtf8("B"), _gui->getCellButton(0, 2)->text());

    // An empty mapping resolves every row to Shuffle::getEffectiveSource's default, and
    // exactly one button reflects it.
    Shuffle* shuffle = shuffleEffect();
    ASSERT_TRUE(shuffle != NULL);
    const double time = _app->getTimeLine()->currentFrame();
    for (int r = 0; r < _gui->getOutputRowCount(); ++r) {
        int outSlot = 0;
        int outIndex = 0;
        ASSERT_TRUE(_gui->getOutputRow(r, &outSlot, &outIndex));
        const int expected = _gui->findSourceColumn(shuffle->getEffectiveSource(outSlot, outIndex, time, ViewIdx(0)));
        ASSERT_GE(expected, 0) << "row " << r;
        int checkedCount = 0;
        for (int c = 0; c < _gui->getSourceColumnCount(); ++c) {
            const bool checked = _gui->getCellButton(r, c)->isChecked();
            EXPECT_EQ(c == expected, checked) << "row " << r << " column " << c;
            if (checked) {
                ++checkedCount;
            }
        }
        EXPECT_EQ(1, checkedCount) << "row " << r;
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
    EXPECT_TRUE(_gui->getCellButton(row, _gui->findSourceColumn(ShuffleSource::makeInput(1, 0)))->isChecked());
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

    LayerChannelRow* in1Row = layerRow(KnobGuiShuffleMap::eLayerRowIn1);
    ASSERT_TRUE(in1Row != NULL);
    EXPECT_TRUE(in1Row->hasAbsentMarker());
    const QString in1Text = in1Row->getCurrentComboText();
    EXPECT_TRUE(in1Text.contains(QString::fromUtf8("(not in input)"))) << in1Text.toStdString();

    const int column = _gui->findSourceColumn(ShuffleSource::makeInput(1, 0));
    ASSERT_EQ(0, column);
    EXPECT_EQ(-1, _gui->findSourceColumn(ShuffleSource::makeInput(1, 1)));
    for (int r = 0; r < _gui->getOutputRowCount(); ++r) {
        EXPECT_FALSE(_gui->getCellButton(r, column)->isEnabled()) << "row " << r;
        EXPECT_TRUE(_gui->getCellButton(r, _gui->findSourceColumn(ShuffleSource::makeZero()))->isEnabled()) << "row " << r;
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

TEST_F(ShuffleMatrixTest, LayerKnobsAreHiddenAndDrivenFromTheMatrix)
{
    createShuffleOnFixture();
    ASSERT_FALSE(HasFatalFailure());
    createGui();
    ASSERT_FALSE(HasFatalFailure());

    static const char* const layerKnobs[] = {
        kShuffleParamIn1, kShuffleParamIn2, kShuffleParamOut1, kShuffleParamOut2
    };
    for (std::size_t i = 0; i < sizeof(layerKnobs) / sizeof(layerKnobs[0]); ++i) {
        KnobIPtr knob = _shuffle->getKnobByName(layerKnobs[i]);
        ASSERT_TRUE(bool(knob)) << layerKnobs[i];
        EXPECT_TRUE(knob->getIsSecret()) << layerKnobs[i];
    }
    EXPECT_FALSE(_gui->shouldCreateLabel());

    for (int i = 0; i < 4; ++i) {
        LayerChannelRow* row = layerRow((KnobGuiShuffleMap::LayerRowEnum)i);
        ASSERT_TRUE(row != NULL) << "row " << i;
        EXPECT_TRUE(row->isVisible()) << "row " << i;
        EXPECT_EQ(_gui->getMatrixWidget(), row->parentWidget()) << "row " << i;
    }

    const QString newLayer = QString::fromUtf8("New layer...");
    EXPECT_TRUE(layerRow(KnobGuiShuffleMap::eLayerRowOut1)->getComboEntries().contains(newLayer));
    EXPECT_TRUE(layerRow(KnobGuiShuffleMap::eLayerRowOut2)->getComboEntries().contains(newLayer));
    EXPECT_FALSE(layerRow(KnobGuiShuffleMap::eLayerRowIn1)->getComboEntries().contains(newLayer));
    EXPECT_FALSE(layerRow(KnobGuiShuffleMap::eLayerRowIn2)->getComboEntries().contains(newLayer));

    const QString none = QString::fromUtf8("None");
    EXPECT_TRUE(layerRow(KnobGuiShuffleMap::eLayerRowIn2)->getComboEntries().contains(none));
    EXPECT_TRUE(layerRow(KnobGuiShuffleMap::eLayerRowOut2)->getComboEntries().contains(none));
    EXPECT_FALSE(layerRow(KnobGuiShuffleMap::eLayerRowOut1)->getComboEntries().contains(none));
}

TEST_F(ShuffleMatrixTest, ChoosingIn1InItsDropdownIsOneUndoStep)
{
    createShuffleOnFixture();
    ASSERT_FALSE(HasFatalFailure());
    createGui();
    ASSERT_FALSE(HasFatalFailure());

    const std::string before = layerOf(kShuffleParamIn1);
    ASSERT_NE(std::string("diffuse"), before);
    const int columnsBefore = _gui->getSourceColumnCount();
    LayerChannelRow* in1Row = layerRow(KnobGuiShuffleMap::eLayerRowIn1);
    ASSERT_TRUE(in1Row != NULL);
    const int undoCountBefore = _container->getUndoStack().count();

    chooseInRow(KnobGuiShuffleMap::eLayerRowIn1, "diffuse");
    ASSERT_FALSE(HasFatalFailure());

    EXPECT_EQ(std::string("diffuse"), layerOf(kShuffleParamIn1));
    EXPECT_EQ(undoCountBefore + 1, _container->getUndoStack().count());
    EXPECT_EQ(3 + 2, _gui->getSourceColumnCount());
    EXPECT_EQ(in1Row, layerRow(KnobGuiShuffleMap::eLayerRowIn1));
    EXPECT_EQ(std::string("diffuse"), in1Row->getCurrentLayerID());

    _container->getUndoStack().undo();
    settle();

    EXPECT_EQ(before, layerOf(kShuffleParamIn1));
    EXPECT_EQ(before, in1Row->getCurrentLayerID());
    EXPECT_EQ(columnsBefore, _gui->getSourceColumnCount());
}

TEST_F(ShuffleMatrixTest, In2AndOut2DropdownsAddAndRemoveTheirBlocks)
{
    createShuffleOnFixture();
    ASSERT_FALSE(HasFatalFailure());
    createGui();
    ASSERT_FALSE(HasFatalFailure());

    EXPECT_EQ(4 + 2, _gui->getSourceColumnCount());
    EXPECT_EQ(4, _gui->getOutputRowCount());

    chooseInRow(KnobGuiShuffleMap::eLayerRowIn2, "specular");
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(std::string("specular"), layerOf(kShuffleParamIn2));
    EXPECT_EQ(4 + 3 + 2, _gui->getSourceColumnCount());
    EXPECT_EQ(4, _gui->findSourceColumn(ShuffleSource::makeInput(2, 0)));

    chooseInRow(KnobGuiShuffleMap::eLayerRowOut2, "diffuse");
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(std::string("diffuse"), layerOf(kShuffleParamOut2));
    EXPECT_EQ(4 + 3, _gui->getOutputRowCount());
    EXPECT_EQ(4, findRow(2, 0));

    chooseInRow(KnobGuiShuffleMap::eLayerRowOut2, "None");
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_TRUE(layerOf(kShuffleParamOut2).empty());
    EXPECT_EQ(4, _gui->getOutputRowCount());
    EXPECT_EQ(-1, findRow(2, 0));
    EXPECT_TRUE(layerRow(KnobGuiShuffleMap::eLayerRowOut2)->isVisible());
}

// Color and specular in, Color and diffuse out: two column blocks and two row blocks.
TEST_F(ShuffleMatrixTest, GridKeepsBlocksAlignedAndSpacingConstant)
{
    createShuffleOnFixture();
    ASSERT_FALSE(HasFatalFailure());
    setLayer(kShuffleParamIn2, "specular");
    setLayer(kShuffleParamOut2, "diffuse");
    ASSERT_FALSE(HasFatalFailure());
    createGui();
    ASSERT_FALSE(HasFatalFailure());

    ASSERT_EQ(4 + 3, _gui->getOutputRowCount());
    ASSERT_EQ(4 + 3 + 2, _gui->getSourceColumnCount());

    const QSize cellSize = _gui->getCellButton(0, 0)->size();
    int buttonsRight = 0;
    int buttonsTop = 1 << 30;
    for (int r = 0; r < _gui->getOutputRowCount(); ++r) {
        for (int c = 0; c < _gui->getSourceColumnCount(); ++c) {
            Button* b = _gui->getCellButton(r, c);
            ASSERT_TRUE(b != NULL);
            EXPECT_TRUE(b->isVisible()) << "row " << r << " column " << c;
            EXPECT_EQ(cellSize, b->size()) << "row " << r << " column " << c;
            buttonsRight = std::max(buttonsRight, b->geometry().right());
            buttonsTop = std::min(buttonsTop, b->geometry().top());
        }
    }

    const std::vector<Button*> in1 = columnsOfRow(0, 1);
    const std::vector<Button*> in2 = columnsOfRow(0, 2);
    ASSERT_EQ(4u, in1.size());
    ASSERT_EQ(3u, in2.size());
    const int xStep = in1[1]->x() - in1[0]->x();
    const int cellSpacing = xStep - cellSize.width();
    EXPECT_GE(cellSpacing, 0);
    for (std::size_t i = 1; i < in1.size(); ++i) {
        EXPECT_EQ(xStep, in1[i]->x() - in1[i - 1]->x()) << "in1 column " << i;
    }
    for (std::size_t i = 1; i < in2.size(); ++i) {
        EXPECT_EQ(xStep, in2[i]->x() - in2[i - 1]->x()) << "in2 column " << i;
    }

    const int out1Last = findRow(1, 3);
    const int out2First = findRow(2, 0);
    ASSERT_GE(out1Last, 0);
    ASSERT_GE(out2First, 0);
    const int yStep = _gui->getCellButton(1, 0)->y() - _gui->getCellButton(0, 0)->y();
    for (int r = 1; r <= out1Last; ++r) {
        EXPECT_EQ(yStep, _gui->getCellButton(r, 0)->y() - _gui->getCellButton(r - 1, 0)->y()) << "out1 row " << r;
    }
    for (int r = out2First + 1; r < _gui->getOutputRowCount(); ++r) {
        EXPECT_EQ(yStep, _gui->getCellButton(r, 0)->y() - _gui->getCellButton(r - 1, 0)->y()) << "out2 row " << r;
    }

    const int inGap = in2[0]->x() - (in1.back()->x() + cellSize.width());
    const int outGap = _gui->getCellButton(out2First, 0)->y() - (_gui->getCellButton(out1Last, 0)->y() + cellSize.height());
    EXPECT_EQ(inGap, outGap);
    EXPECT_GT(inGap, cellSpacing);

    LayerChannelRow* in1Row = layerRow(KnobGuiShuffleMap::eLayerRowIn1);
    LayerChannelRow* in2Row = layerRow(KnobGuiShuffleMap::eLayerRowIn2);
    ASSERT_TRUE(in1Row != NULL);
    ASSERT_TRUE(in2Row != NULL);
    EXPECT_EQ(in1[0]->x(), in1Row->x());
    EXPECT_EQ(in2[0]->x(), in2Row->x());
    EXPECT_LT(in1Row->geometry().bottom(), buttonsTop);
    EXPECT_LT(in2Row->geometry().bottom(), buttonsTop);

    LayerChannelRow* out1Row = layerRow(KnobGuiShuffleMap::eLayerRowOut1);
    LayerChannelRow* out2Row = layerRow(KnobGuiShuffleMap::eLayerRowOut2);
    ASSERT_TRUE(out1Row != NULL);
    ASSERT_TRUE(out2Row != NULL);
    EXPECT_GT(out1Row->x(), buttonsRight);
    EXPECT_GT(out2Row->x(), buttonsRight);
    EXPECT_NEAR(_gui->getCellButton(0, 0)->geometry().center().y(), out1Row->geometry().center().y(), 1);
    EXPECT_NEAR(_gui->getCellButton(out2First, 0)->geometry().center().y(), out2Row->geometry().center().y(), 1);
    EXPECT_GE(out1Row->y(), _gui->getCellButton(0, 0)->y());
    EXPECT_LE(out1Row->geometry().bottom(), _gui->getCellButton(0, 0)->geometry().bottom());
    EXPECT_GE(out2Row->y(), _gui->getCellButton(out2First, 0)->y());
    EXPECT_LE(out2Row->geometry().bottom(), _gui->getCellButton(out2First, 0)->geometry().bottom());

    Button* reset = _gui->getResetButton();
    ASSERT_TRUE(reset != NULL);
    const QPoint resetTop = reset->mapTo(_panel.get(), QPoint(0, 0));
    const QPoint matrixBottom = _gui->getMatrixWidget()->mapTo(_panel.get(), QPoint(0, _gui->getMatrixWidget()->height()));
    EXPECT_GE(resetTop.y(), matrixBottom.y());
}
