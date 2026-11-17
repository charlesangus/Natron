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

#include "Global/Macros.h"

#include "Engine/FileSystemModel.h"

#include <vector>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <SequenceParsing.h>

NATRON_NAMESPACE_USING

namespace {

class MockSortableView : public SortableViewI {
 public:
  MockSortableView() = default;
  ~MockSortableView() override {}

  // SortableViewI methods
  Qt::SortOrder sortIndicatorOrder() const override { return _sortOrder; }

  int sortIndicatorSection() const override { return _sortSection; }

  void onSortIndicatorChanged(int logicalIndex, Qt::SortOrder order) override {
    _sortSection = logicalIndex;
    _sortOrder = order;
  }

 private:
  int _sortSection = 0;
  Qt::SortOrder _sortOrder = Qt::AscendingOrder;
};

}  // namespace

TEST(FileSystemModelTest, DriveName) {
  MockSortableView sortableView;
  auto model = std::make_shared<FileSystemModel>();
  model->initialize(&sortableView);

  struct DriveNameTestCase {
    const char* input;
    bool isWindowsDriveName;
    bool startsWithWindowsDriveName;
    bool isUnixDriveName;
    bool startsWithUnixDriveName;
  };
  std::vector<DriveNameTestCase> testCases({
      {"", false, false, false, false},
      {"filename.txt", false, false, false, false},
      {"somedir/", false, false, false, false},
      {"somedir/filename.txt", false, false, false, false},
      {"/", false, false, true, true},
      {"/filename.txt", false, false, false, true},
      {"/somedir/", false, false, false, true},
      {"/somedir/filename.txt", false, false, false, true},
      {"c:", false, false, false, false},
      {"c:/", true, true, false, false},
      {"c:\\", true, true, false, false},
      {"c:/filename.txt", false, true, false, false},
      {"c:\\filename.txt", false, true, false, false},
      {"c:/somedir/", false, true, false, false},
      {"c:/somedir/filename.txt", false, true, false, false},
      {"c:\\somedir\\", false, true, false, false},
      {"c:\\somedir\\filename.txt", false, true, false, false},
      {"//", false, false, false, true},
      {"//somehost", false, false, false, true},
      {"//somehost/", true, true, false, true},
      {"//somehost/somedir/", false, true, false, true},
      {"//somehost/somedir/filename.txt", false, true, false, true},
  });

  for (const auto& testCase : testCases) {
    const QString input = QString::fromUtf8(testCase.input);
#ifdef __NATRON_WIN32__
    const bool expectedIsDriveName = testCase.isWindowsDriveName;
    const bool expectedStartsWithDriveName =
        testCase.startsWithWindowsDriveName;
#else
    const bool expectedIsDriveName = testCase.isUnixDriveName;
    const bool expectedStartsWithDriveName = testCase.startsWithUnixDriveName;
#endif

    EXPECT_EQ(expectedIsDriveName, FileSystemModel::isDriveName(input))
        << " input '" << testCase.input << "'";
    EXPECT_EQ(expectedStartsWithDriveName,
              FileSystemModel::startsWithDriveName(input))
        << " input '" << testCase.input << "'";
  }
}

TEST(FileSystemModelTest, GetSplitPath) {
  MockSortableView sortableView;
  auto model = std::make_shared<FileSystemModel>();
  model->initialize(&sortableView);

  struct SplitPathTestCase {
    const char* input;
    std::vector<const char*> expectedWindowsOutput;
    std::vector<const char*> expectedUnixOutput;
  };
  std::vector<SplitPathTestCase> testCases({
      {"", {}, {}},
      {"filename.txt", {"filename.txt"}, {"filename.txt"}},
      {"somedir/", {"somedir"}, {"somedir"}},
      {"somedir/filename.txt",
       {"somedir", "filename.txt"},
       {"somedir", "filename.txt"}},
      {"/", {""}, {"/", ""}},
      {"/filename.txt", {"", "filename.txt"}, {"/", "filename.txt"}},
      {"/somedir/", {"", "somedir"}, {"/", "somedir"}},
      {"/somedir/filename.txt",
       {"", "somedir", "filename.txt"},
       {"/", "somedir", "filename.txt"}},
      {"c:", {"c:"}, {"c:"}},
      {"c:/", {"c:/", ""}, {"c:"}},
      {"c:\\", {"c:\\", ""}, {"c:\\"}},
      {"c:/filename.txt", {"c:/", "filename.txt"}, {"c:", "filename.txt"}},
      {"c:\\filename.txt", {"c:\\", "filename.txt"}, {"c:\\filename.txt"}},
      {"c:/somedir/", {"c:/", "somedir"}, {"c:", "somedir"}},
      {"c:/somedir/filename.txt",
       {"c:/", "somedir", "filename.txt"},
       {"c:", "somedir", "filename.txt"}},
      {"c:\\somedir\\", {"c:\\", "somedir\\"}, {"c:\\somedir\\"}},
      {"c:\\somedir\\filename.txt",
       {"c:\\", "somedir\\filename.txt"},
       {"c:\\somedir\\filename.txt"}},
      {"//", {"", ""}, {"/", ""}},
      {"//somehost",
       {"", "", "somehost"},  // Not considered a network path because
                              // of missing slash after hostname.
       {"/", "", "somehost"}},
      {"//somehost/", {"//somehost/", ""}, {"/", "", "somehost"}},
      {"//somehost/somedir/",
       {"//somehost/", "somedir"},
       {"/", "", "somehost", "somedir"}},
      {"//somehost/somedir/filename.txt",
       {"//somehost/", "somedir", "filename.txt"},
       {"/", "", "somehost", "somedir", "filename.txt"}},
  });

  for (const auto& testCase : testCases) {
    const QString input = QString::fromUtf8(testCase.input);

#ifdef __NATRON_WIN32__
    const auto& expectedOutput = testCase.expectedWindowsOutput;
#else
    const auto& expectedOutput = testCase.expectedUnixOutput;
#endif

    const int expectedSize = static_cast<int>(expectedOutput.size());
    const auto output = FileSystemModel::getSplitPath(input);
    EXPECT_EQ(expectedSize, output.size()) << "input '" << testCase.input
                                           << "'";

    for (int i = 0; i < expectedSize && i < output.size(); ++i) {
      EXPECT_EQ(expectedOutput[i], output[i].toStdString())
          << "input '" << testCase.input << "' i " << i;
    }
  }
}

TEST(FileSystemModelTest, CleanPath) {
  MockSortableView sortableView;
  auto model = std::make_shared<FileSystemModel>();
  model->initialize(&sortableView);

  struct CleanPathTestCase {
    const char* input;
    const char* expectedWindowsOutput;  // nullptr if the expectation is the
                                        // same as input.
    const char* expectedUnixOutput;  // nullptr if the expectation is the same
                                     // as expectedWindowsOutput.
  };

  std::vector<CleanPathTestCase> testCases({
      // Paths that behave the same on all platforms.
      // - Verify trailing slashes are removed.
      {"", nullptr, nullptr},
      {"filename.txt", nullptr, nullptr},
      {"somedir/", "somedir", nullptr},
      {"somedir/filename.txt", nullptr, nullptr},
      {"/", nullptr, nullptr},
      {"/filename.txt", nullptr, nullptr},
      {"/somedir/", "/somedir", nullptr},
      {"/somedir/filename.txt", nullptr, nullptr},
      {"c:", nullptr, nullptr},
      // Platform specific path behaviors.
      // Paths with Windows drive letters and backslashes.
      // - Verify drive letters become capitalized on Windows
      // - Verify drives keep their trailing slash.
      // - Verify backslashes are converted to forward slashes on Windows.
      // - Verify Unix platforms do not treat forward slashes and drive
      //   letters in special way.
      {"c:/", "C:/", "c:"},
      {"c:\\", "C:/", "c:\\"},
      {"c:/filename.txt", "C:/filename.txt", "c:/filename.txt"},
      {"c:\\filename.txt", "C:/filename.txt", "c:\\filename.txt"},
      {"c:/somedir/", "C:/somedir", "c:/somedir"},
      {"c:/somedir/filename.txt", "C:/somedir/filename.txt", "c:/somedir/filename.txt"},
      {"c:\\somedir\\", "C:/somedir", "c:\\somedir\\"},
      {"c:\\somedir\\filename.txt", "C:/somedir/filename.txt",
       "c:\\somedir\\filename.txt"},
      // Windows Network Shares
      // - Verify that hostname is capitalized on Windows as long as it
      //   is followed by a /.
      // - Verify Unix just collapses the '//' to a single slash.
      {"//", "/", nullptr},
      {"//somehost", "//somehost", "/somehost"},
      {"//somehost/", "//SOMEHOST/", "/somehost"},
      {"//somehost/somedir/", "//SOMEHOST/somedir", "/somehost/somedir"},
      {"//somehost/somedir/filename.txt",
        "//SOMEHOST/somedir/filename.txt",
        "/somehost/somedir/filename.txt"},
  });

  for (const auto& testCase : testCases) {
    const QString input = QString::fromUtf8(testCase.input);
    std::string expectedOutput;

#ifdef __NATRON_WIN32__
    if (testCase.expectedWindowsOutput != nullptr) {
      expectedOutput = testCase.expectedWindowsOutput;
    } else {
      expectedOutput = testCase.input;
    }
#else
    if (testCase.expectedUnixOutput != nullptr) {
      expectedOutput = testCase.expectedUnixOutput;
    } else if (testCase.expectedWindowsOutput != nullptr) {
      expectedOutput = testCase.expectedWindowsOutput;
    } else {
      expectedOutput = testCase.input;
    }
#endif
    const QString output = FileSystemModel::cleanPath(input);
    ASSERT_TRUE(!output.isNull());
    EXPECT_EQ(expectedOutput, output.toStdString()) << " input '" << testCase.input << "'";
  }
}
// Non-image file knobs open their dialog with a bare `*`, which has to list every file.
TEST(FileSystemModelTest, RegexpFilters)
{
    MockSortableView sortableView;
    auto model = std::make_shared<FileSystemModel>();
    model->initialize(&sortableView);

    const QString usda = QString::fromUtf8("scene.usda");
    const QString txt = QString::fromUtf8("notes.txt");
    const QString bare = QString::fromUtf8("README");

    model->setRegexpFilters(QString::fromUtf8("*"));
    EXPECT_TRUE(model->isAcceptedByRegexps(usda));
    EXPECT_TRUE(model->isAcceptedByRegexps(txt));
    EXPECT_TRUE(model->isAcceptedByRegexps(bare));

    model->setRegexpFilters(QString::fromUtf8("*.usd *.usda *.usdc *.usdz *.abc"));
    EXPECT_TRUE(model->isAcceptedByRegexps(usda));
    EXPECT_TRUE(model->isAcceptedByRegexps(QString::fromUtf8("SCENE.USDA")));
    EXPECT_FALSE(model->isAcceptedByRegexps(txt));
    EXPECT_FALSE(model->isAcceptedByRegexps(bare));
    EXPECT_FALSE(model->isAcceptedByRegexps(QString::fromUtf8("scene.usda.bak")));

    // A `*` alongside extension patterns still lists everything.
    model->setRegexpFilters(QString::fromUtf8("*.usda *"));
    EXPECT_TRUE(model->isAcceptedByRegexps(txt));

    // `*.*` keeps rejecting files without an extension.
    model->setRegexpFilters(QString::fromUtf8("*.*"));
    EXPECT_TRUE(model->isAcceptedByRegexps(txt));
    EXPECT_FALSE(model->isAcceptedByRegexps(bare));

    model->setRegexpFilters(QString());
    EXPECT_TRUE(model->isAcceptedByRegexps(txt));
    EXPECT_TRUE(model->isAcceptedByRegexps(bare));
}

namespace {

struct ListedItem {
    QString name;
    bool isChunk;
    int first;
    int last;
};

void
writeFrames(const QString& dir, int first, int last)
{
    for (int f = first; f <= last; ++f) {
        QFile file(dir + QString::asprintf("/plate.%04d.exr", f));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    }
}

std::vector<ListedItem>
waitForItems(const FileSystemModelPtr& model, const QString& dir, std::size_t count)
{
    std::vector<ListedItem> items;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 10000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        FileSystemItemPtr parent = model->getFileSystemItem(dir);
        if (!parent || parent == model->getFileSystemItem(QString()) || (std::size_t)parent->childCount() != count) {
            continue;
        }
        items.clear();
        for (int i = 0; i < parent->childCount(); ++i) {
            FileSystemItemPtr child = parent->childAt(i);
            if (child) {
                items.push_back({ child->getUserFriendlyFilename(), child->isSequenceChunk(),
                                  child->getChunkFirstFrame(), child->getChunkLastFrame() });
            }
        }
        if (items.size() == count) {
            return items;
        }
    }
    return items;
}

class SplitOnGapsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(_tmp.isValid());
        _dir = QDir::cleanPath(_tmp.path());
        _model = std::make_shared<FileSystemModel>();
        _model->initialize(&_view);
        _model->setSequenceModeEnabled(true);
    }

    MockSortableView _view;
    QTemporaryDir _tmp;
    QString _dir;
    FileSystemModelPtr _model;
};

} // namespace

TEST(FileSystemModelTest, ContiguousFrameRuns)
{
    SequenceParsing::SequenceFromFiles sequence(false);
    for (int f : { 10, 11, 12, 20, 22, 23 }) {
        ASSERT_TRUE(sequence.tryInsertFile(SequenceParsing::FileNameContent(
            std::string("/shots/plate.") + QString::asprintf("%04d", f).toStdString() + ".exr")));
    }
    const std::vector<std::pair<int, int>> runs = FileSystemModel::contiguousFrameRuns(sequence);
    const std::vector<std::pair<int, int>> expected { { 10, 12 }, { 20, 20 }, { 22, 23 } };
    EXPECT_EQ(expected, runs);
}

TEST_F(SplitOnGapsTest, TwoRunsJoinedAndSplit)
{
    writeFrames(_dir, 10, 20);
    writeFrames(_dir, 30, 40);
    ASSERT_TRUE(_model->setRootPath(_dir));

    std::vector<ListedItem> joined = waitForItems(_model, _dir, 1);
    ASSERT_EQ(1u, joined.size());
    EXPECT_FALSE(joined[0].isChunk);

    _model->setSplitSequencesOnGaps(true);
    std::vector<ListedItem> split = waitForItems(_model, _dir, 2);
    ASSERT_EQ(2u, split.size());
    EXPECT_TRUE(split[0].isChunk);
    EXPECT_EQ(10, split[0].first);
    EXPECT_EQ(20, split[0].last);
    EXPECT_EQ(QString::fromUtf8("plate.####.exr 10-20"), split[0].name);
    EXPECT_TRUE(split[1].isChunk);
    EXPECT_EQ(30, split[1].first);
    EXPECT_EQ(40, split[1].last);
    EXPECT_EQ(QString::fromUtf8("plate.####.exr 30-40"), split[1].name);
}

TEST_F(SplitOnGapsTest, OneMissingFrameSplits)
{
    writeFrames(_dir, 10, 24);
    writeFrames(_dir, 26, 40);
    _model->setSplitSequencesOnGaps(true);
    ASSERT_TRUE(_model->setRootPath(_dir));

    std::vector<ListedItem> split = waitForItems(_model, _dir, 2);
    ASSERT_EQ(2u, split.size());
    EXPECT_EQ(10, split[0].first);
    EXPECT_EQ(24, split[0].last);
    EXPECT_EQ(26, split[1].first);
    EXPECT_EQ(40, split[1].last);
}

TEST_F(SplitOnGapsTest, SingleFrameChunk)
{
    writeFrames(_dir, 10, 20);
    writeFrames(_dir, 25, 25);
    _model->setSplitSequencesOnGaps(true);
    ASSERT_TRUE(_model->setRootPath(_dir));

    std::vector<ListedItem> split = waitForItems(_model, _dir, 2);
    ASSERT_EQ(2u, split.size());
    EXPECT_EQ(25, split[1].first);
    EXPECT_EQ(25, split[1].last);
    EXPECT_EQ(QString::fromUtf8("plate.####.exr 25"), split[1].name);
}

TEST_F(SplitOnGapsTest, OneSequenceSplitWithTheToggleOff)
{
    writeFrames(_dir, 10, 20);
    writeFrames(_dir, 30, 40);
    ASSERT_TRUE(_model->setRootPath(_dir));
    std::vector<ListedItem> joined = waitForItems(_model, _dir, 1);
    ASSERT_EQ(1u, joined.size());

    const QString sequencePath = _dir + QString::fromUtf8("/plate.####.exr");
    _model->setSequenceSplit(sequencePath, true);
    EXPECT_TRUE(_model->isSequenceSplit(sequencePath));
    EXPECT_FALSE(_model->isSplitSequencesOnGapsEnabled());
    std::vector<ListedItem> split = waitForItems(_model, _dir, 2);
    ASSERT_EQ(2u, split.size());
    EXPECT_TRUE(split[0].isChunk);

    _model->setSequenceSplit(sequencePath, false);
    joined = waitForItems(_model, _dir, 1);
    ASSERT_EQ(1u, joined.size());
    EXPECT_FALSE(joined[0].isChunk);
}
