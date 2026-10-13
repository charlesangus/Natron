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

#include <algorithm>
#include <cstdlib>
#include <list>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "SyntheticProjectGraphs.h"

#include "Engine/AppInstance.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_USING

namespace {

// Elements that hold a plain, machine- or time-dependent value and nothing else: the Natron
// build's version/git/OS/bits header, the project-level creation timestamp, and each node's
// KnobsAge (a monotonic counter whose value depends on how many edits a session happened to
// make before saving) and CacheID (seeded from a process-local counter, not from the knob
// values it is meant to disambiguate).
const char* const kVolatileLeafTags[] = {
    "VersionMajor", "VersionMinor", "VersionRev", "GitBranch", "GitCommit",
    "OS", "Bits", "CreationDate", "KnobsAge", "CacheID"
};

// The project info-page String knobs: in practice only lastSaveDate ever actually differs from
// its Default (the others are set to the same value as their Default within one process, so the
// serializer's "skip knobs at default" rule omits them), but normalizing all five keeps the
// golden robust against that implementation detail changing.
const char* const kVolatileKnobNames[] = {
    "lastSaveDate", "lastAuthor", "originalAuthor", "softwareVersion", "creationDate"
};

// Replaces the text content of every <tagName>...</tagName> occurrence with a fixed placeholder.
QString
normalizeLeafTag(const QString& xml,
                 const char* tagName)
{
    const QString tag = QString::fromUtf8(tagName);
    QRegularExpression pattern(QString::fromUtf8("<%1>[^<]*</%1>").arg(tag));

    return QString(xml).replace(pattern, QString::fromUtf8("<%1>NORMALIZED</%1>").arg(tag));
}

// Replaces the Value and Default of the named knob (always a single-dimension String on the
// Project's info page) with a fixed placeholder, wherever that <Name> is followed by them.
QString
normalizeKnobValueAndDefault(const QString& xml,
                             const char* knobName)
{
    QRegularExpression pattern(QString::fromUtf8("(<Name>%1</Name>.*?<Value>)[^<]*(</Value>.*?<Default>)[^<]*(</Default>)").arg(QString::fromUtf8(knobName)),
                               QRegularExpression::DotMatchesEverythingOption);

    return QString(xml).replace(pattern, QString::fromUtf8("\\1NORMALIZED\\2NORMALIZED\\3"));
}

// Every case saves into (or loads its fixture copy from) this run's QTemporaryDir, whose name is
// freshly randomized by the OS on every run. That path gets recorded verbatim into the project's
// "projectPaths" knob (the "Project" path variable) and, when a case points a knob at a file under
// it, into that knob's Value/Default too -- plain substring replacement catches both, since neither
// contains XML markup characters that would need escaping.
QString
normalizeTempDirPath(const QString& xml,
                     const QString& tempDirPath)
{
    return QString(xml).replace(tempDirPath, QString::fromUtf8("NORMALIZED_TMP_DIR"));
}

// A case that reads one of the committed test fixtures (e.g. via createWorkingSpaceRead()) points
// a Read node's filename knob at a file under NATRON_TESTS_FIXTURES_DIR -- the build's absolute
// checkout path, which is machine- and CI-run-specific. Normalize both the literal macro value and
// its canonicalized form, since nothing guarantees which one a knob ends up storing.
QString
normalizeFixturesDirPath(const QString& xml)
{
    const QString rawFixturesDir = QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR);
    const QString canonicalFixturesDir = QFileInfo(rawFixturesDir).canonicalFilePath();

    QString normalized = QString(xml).replace(rawFixturesDir, QString::fromUtf8("NORMALIZED_FIXTURES_DIR"));
    if (!canonicalFixturesDir.isEmpty() && (canonicalFixturesDir != rawFixturesDir)) {
        normalized = normalized.replace(canonicalFixturesDir, QString::fromUtf8("NORMALIZED_FIXTURES_DIR"));
    }

    return normalized;
}

QString
normalizeGoldenXml(const QString& savedProjectXml,
                   const QString& tempDirPath)
{
    QString normalized = normalizeTempDirPath(savedProjectXml, tempDirPath);

    normalized = normalizeFixturesDirPath(normalized);

    for (const char* tagName : kVolatileLeafTags) {
        normalized = normalizeLeafTag(normalized, tagName);
    }
    for (const char* knobName : kVolatileKnobNames) {
        normalized = normalizeKnobValueAndDefault(normalized, knobName);
    }

    return normalized;
}

// A compact unified-style diff of two texts: only the changed lines and a little context around
// each change are reported. Goldens are a few thousand lines at most and the common head and tail
// are trimmed first, so a plain LCS over the differing middle is cheap enough.
std::string
summarizeLineDifferences(const QString& expectedText,
                         const QString& actualText)
{
    const QStringList expectedLines = expectedText.split(QLatin1Char('\n'));
    const QStringList actualLines = actualText.split(QLatin1Char('\n'));
    const int expectedLineCount = static_cast<int>(expectedLines.size());
    const int actualLineCount = static_cast<int>(actualLines.size());

    int commonPrefixLength = 0;
    while ((commonPrefixLength < expectedLineCount) && (commonPrefixLength < actualLineCount) && (expectedLines[commonPrefixLength] == actualLines[commonPrefixLength])) {
        ++commonPrefixLength;
    }
    int commonSuffixLength = 0;
    while ((commonSuffixLength < expectedLineCount - commonPrefixLength) && (commonSuffixLength < actualLineCount - commonPrefixLength) && (expectedLines[expectedLineCount - 1 - commonSuffixLength] == actualLines[actualLineCount - 1 - commonSuffixLength])) {
        ++commonSuffixLength;
    }

    const int expectedMiddleCount = expectedLineCount - commonPrefixLength - commonSuffixLength;
    const int actualMiddleCount = actualLineCount - commonPrefixLength - commonSuffixLength;

    std::vector<std::vector<int>> longestCommonTail(expectedMiddleCount + 1, std::vector<int>(actualMiddleCount + 1, 0));
    for (int i = expectedMiddleCount - 1; i >= 0; --i) {
        for (int j = actualMiddleCount - 1; j >= 0; --j) {
            if (expectedLines[commonPrefixLength + i] == actualLines[commonPrefixLength + j]) {
                longestCommonTail[i][j] = longestCommonTail[i + 1][j + 1] + 1;
            } else {
                longestCommonTail[i][j] = std::max(longestCommonTail[i + 1][j], longestCommonTail[i][j + 1]);
            }
        }
    }

    struct DiffLine {
        char marker;
        int goldenLineNumber;
        QString text;
    };
    std::vector<DiffLine> editScript;
    for (int k = 0; k < commonPrefixLength; ++k) {
        editScript.push_back(DiffLine { ' ', k + 1, expectedLines[k] });
    }
    int i = 0;
    int j = 0;
    while ((i < expectedMiddleCount) || (j < actualMiddleCount)) {
        const int goldenLineNumber = commonPrefixLength + i + 1;
        if ((i < expectedMiddleCount) && (j < actualMiddleCount) && (expectedLines[commonPrefixLength + i] == actualLines[commonPrefixLength + j])) {
            editScript.push_back(DiffLine { ' ', goldenLineNumber, expectedLines[commonPrefixLength + i] });
            ++i;
            ++j;
        } else if ((i < expectedMiddleCount) && ((j == actualMiddleCount) || (longestCommonTail[i + 1][j] >= longestCommonTail[i][j + 1]))) {
            editScript.push_back(DiffLine { '-', goldenLineNumber, expectedLines[commonPrefixLength + i] });
            ++i;
        } else {
            editScript.push_back(DiffLine { '+', goldenLineNumber, actualLines[commonPrefixLength + j] });
            ++j;
        }
    }
    for (int k = expectedLineCount - commonSuffixLength; k < expectedLineCount; ++k) {
        editScript.push_back(DiffLine { ' ', k + 1, expectedLines[k] });
    }

    const int contextLineCount = 3;
    const int maxReportedLineCount = 200;
    const int editScriptLength = static_cast<int>(editScript.size());
    std::vector<bool> isReported(editScriptLength, false);
    for (int k = 0; k < editScriptLength; ++k) {
        if (editScript[k].marker != ' ') {
            for (int contextIndex = std::max(0, k - contextLineCount); contextIndex <= std::min(editScriptLength - 1, k + contextLineCount); ++contextIndex) {
                isReported[contextIndex] = true;
            }
        }
    }

    std::ostringstream summary;
    summary << "--- golden\n+++ actual\n";
    int reportedLineCount = 0;
    bool previousWasReported = false;
    for (int k = 0; k < editScriptLength; ++k) {
        if (!isReported[k]) {
            previousWasReported = false;
            continue;
        }
        if (reportedLineCount >= maxReportedLineCount) {
            summary << "... (truncated)\n";
            break;
        }
        if (!previousWasReported) {
            summary << "@@ golden line " << editScript[k].goldenLineNumber << " @@\n";
        }
        summary << editScript[k].marker << editScript[k].text.toStdString() << '\n';
        previousWasReported = true;
        ++reportedLineCount;
    }

    return summary.str();
}

} // namespace

class GoldenXmlTest
    : public SyntheticProjectGraphTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    // Copies `sourcePath` into `tmp` and loads it as the current project. Returns false, with
    // *reason set to something worth printing on failure, on any failure to load -- including a
    // thrown exception, which a project referencing a plugin the test environment doesn't have
    // can trigger.
    bool loadFixtureProject(const std::string& sourcePath,
                            const QTemporaryDir& tmp,
                            std::string* reason)
    {
        QFileInfo info(QString::fromStdString(sourcePath));

        if (!info.exists()) {
            *reason = "fixture not found: " + sourcePath;

            return false;
        }

        const QString dirPath = tmp.path() + QLatin1Char('/');
        const QString fileName = info.fileName();

        if (!QFile::copy(info.absoluteFilePath(), dirPath + fileName)) {
            *reason = "could not copy fixture into a temp dir: " + sourcePath;

            return false;
        }

        bool loaded = false;
        try {
            loaded = getApp()->getProject()->loadProject(dirPath, fileName);
        } catch (const std::exception& e) {
            *reason = "loadProject() threw for " + sourcePath + ": " + e.what();

            return false;
        }
        if (!loaded) {
            *reason = "loadProject() returned false for " + sourcePath;
        }

        return loaded;
    }

    // Loads `sourcePath` as the current project, failing the test (rather than silently
    // skipping it) if the load does not succeed. CI and test.sh both set OFX_PLUGIN_PATH, so a
    // load failure here means a missing fixture or an actual regression, not an environment
    // without the OFX plugin bundle.
    void assertFixtureProjectLoaded(const std::string& sourcePath,
                                    const QTemporaryDir& tmp)
    {
        std::string reason;
        ASSERT_TRUE(loadFixtureProject(sourcePath, tmp, &reason)) << reason;
    }

    // Saves the project currently in memory, normalizes the result, and either (re)writes it as
    // the golden when NATRON_REGEN_GOLDEN is set, or compares it byte-for-byte against the
    // committed golden at Tests/fixtures/golden-ntp/<caseName>.ntp.
    void compareAgainstGolden(const std::string& caseName,
                              const QTemporaryDir& tmp)
    {
        ProjectPtr project = getApp()->getProject();
        const QString dirPath = tmp.path() + QLatin1Char('/');
        const QString saveFileName = QString::fromStdString(caseName + "-save.ntp");

        QString savedFilePath;
        ASSERT_TRUE(project->saveProject(dirPath, saveFileName, &savedFilePath)) << caseName;
        ASSERT_TRUE(QFile::exists(savedFilePath)) << caseName;

        QFile savedFile(savedFilePath);
        ASSERT_TRUE(savedFile.open(QIODevice::ReadOnly)) << caseName;
        const QString normalized = normalizeGoldenXml(QString::fromUtf8(savedFile.readAll()), tmp.path());
        savedFile.close();

        const QString goldenPath = QString::fromUtf8(NATRON_TESTS_FIXTURES_DIR "/golden-ntp/") + QString::fromStdString(caseName) + QString::fromUtf8(".ntp");

        if (std::getenv("NATRON_REGEN_GOLDEN") != NULL) {
            QFile goldenFile(goldenPath);
            ASSERT_TRUE(goldenFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) << goldenPath.toStdString();
            goldenFile.write(normalized.toUtf8());
            goldenFile.close();

            return;
        }

        QFile goldenFile(goldenPath);
        if (!goldenFile.open(QIODevice::ReadOnly)) {
            FAIL() << "missing golden " << goldenPath.toStdString()
                   << " -- run with NATRON_REGEN_GOLDEN=1 to generate it, review the diff, and commit it.";
        }
        const QString golden = QString::fromUtf8(goldenFile.readAll());
        goldenFile.close();

        if (golden == normalized) {
            return;
        }

        // The temp dir is deleted when the case ends, so keep the actual output beside it for
        // post-mortem diffing against the golden.
        const QString actualDumpPath = QDir::temp().filePath(QString::fromStdString("golden-mismatch-" + caseName + ".ntp"));
        QFile actualDumpFile(actualDumpPath);
        if (actualDumpFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            actualDumpFile.write(normalized.toUtf8());
            actualDumpFile.close();
        }
        ADD_FAILURE() << "golden mismatch for " << caseName << " (temp dir " << tmp.path().toStdString() << ", actual written to " << actualDumpPath.toStdString() << ")\n"
                      << summarizeLineDifferences(golden, normalized);
    }
};

TEST_F(GoldenXmlTest, FixtureChannelSetLegacyDefaults)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    ASSERT_NO_FATAL_FAILURE(assertFixtureProjectLoaded(std::string(NATRON_TESTS_FIXTURES_DIR "/channel-set-legacy-defaults.ntp"), tmp));
    compareAgainstGolden("channel-set-legacy-defaults", tmp);
}

TEST_F(GoldenXmlTest, FixtureM65LegacyColor)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    ASSERT_NO_FATAL_FAILURE(assertFixtureProjectLoaded(std::string(NATRON_TESTS_FIXTURES_DIR "/m65-legacy-color.ntp"), tmp));
    compareAgainstGolden("m65-legacy-color", tmp);
}

TEST_F(GoldenXmlTest, FixtureReadTimeOffset)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    ASSERT_NO_FATAL_FAILURE(assertFixtureProjectLoaded(std::string(NATRON_TESTS_FIXTURES_DIR "/read-time-offset.ntp"), tmp));
    compareAgainstGolden("read-time-offset", tmp);
}

TEST_F(GoldenXmlTest, FixtureCgMultipass)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const std::string path = std::string(NATRON_TESTS_FIXTURES_DIR) + "/../../tools/bench/comps/cg_multipass.ntp";
    ASSERT_NO_FATAL_FAILURE(assertFixtureProjectLoaded(path, tmp));
    compareAgainstGolden("cg_multipass", tmp);
}

TEST_F(GoldenXmlTest, FixtureDefocusRetime)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const std::string path = std::string(NATRON_TESTS_FIXTURES_DIR) + "/../../tools/bench/comps/defocus_retime.ntp";
    ASSERT_NO_FATAL_FAILURE(assertFixtureProjectLoaded(path, tmp));
    compareAgainstGolden("defocus_retime", tmp);
}

TEST_F(GoldenXmlTest, FixtureKeyingGrade)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const std::string path = std::string(NATRON_TESTS_FIXTURES_DIR) + "/../../tools/bench/comps/keying_grade.ntp";
    ASSERT_NO_FATAL_FAILURE(assertFixtureProjectLoaded(path, tmp));
    compareAgainstGolden("keying_grade", tmp);
}

TEST_F(GoldenXmlTest, SyntheticGeneratorGradeWrite)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    ASSERT_TRUE(buildGeneratorGradeWriteGraph(tmp));
    compareAgainstGolden("synthetic-generator-grade-write", tmp);
}

TEST_F(GoldenXmlTest, SyntheticGroupWithUserKnobAndNestedGroup)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    ASSERT_TRUE(buildGroupWithUserKnobAndNestedGroupGraph());
    compareAgainstGolden("synthetic-group-with-nested-group", tmp);
}

TEST_F(GoldenXmlTest, SyntheticRotoPaintAndTracker)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    ASSERT_TRUE(buildRotoPaintAndTrackerGraph());
    compareAgainstGolden("synthetic-rotopaint-stroke-bezier-tracker", tmp);
}
