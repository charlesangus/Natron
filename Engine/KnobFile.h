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

#ifndef NATRON_ENGINE_KNOBFILE_H
#define NATRON_ENGINE_KNOBFILE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>
#include <map>

CLANG_DIAG_OFF(deprecated)
#include <QObject>
CLANG_DIAG_ON(deprecated)
#include <QMutex>
#include <QString>

#include "Engine/KnobTypes.h"
#include "Engine/ViewIdx.h"

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/******************************KnobFile**************************************/

class KnobFile
    : public QObject, public AnimatingKnobStringHelper
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static KnobHelper * BuildKnob(KnobHolder* holder,
                                  const std::string &description,
                                  int dimension,
                                  bool declaredByPlugin = true)
    {
        return new KnobFile(holder, description, dimension, declaredByPlugin);
    }

    KnobFile(KnobHolder* holder,
             const std::string &description,
             int dimension,
             bool declaredByPlugin);

    virtual ~KnobFile();

    static const std::string & typeNameStatic();

    void setAsInputImage()
    {
        _isInputImage = true;
    }

    bool isInputImageFile() const
    {
        return _isInputImage;
    }

    /**
     * @brief The extensions the file dialog filters on, such as "usda". An empty list shows every
     * file. Input-image knobs filter on the readers' formats instead.
     **/
    void setFileExtensions(const std::vector<std::string>& extensions)
    {
        _fileExtensions = extensions;
    }

    const std::vector<std::string>& getFileExtensions() const
    {
        return _fileExtensions;
    }

    /**
     * @brief Whether the knob shows its own reload icon. Nodes with a Reload button of their own
     * turn it off, so their panel has one reload.
     **/
    void setShowsReloadButton(bool shows)
    {
        _showsReloadButton = shows;
    }

    bool showsReloadButton() const
    {
        return _showsReloadButton;
    }

    void open_file()
    {
        Q_EMIT openFile();
    }

    /**
     * @brief What a Reload does: stamps the file or sequence again, so the node re-keys when the
     * file on disk differs from its last stamp, and drops what the holder cached of it. A file is
     * otherwise only looked at the first time it is hashed in a session.
     **/
    void reloadFile();

    /**
     * @brief getRandomFrameName
     * @param f The index of the frame.
     */
    std::string getFileName(int time, ViewSpec view);

    /**
     * @brief Resolved path of the single file the knob currently names, or an empty string
     * when the value is empty or names a frame sequence.
     **/
    std::string getSingleFilePath();

    /**
     * @brief The knob's current value with project variables expanded: a single file or a
     * frame sequence pattern.
     **/
    std::string getResolvedPath();

    /**
     * @brief Forces the file or sequence the knob names to be stamped again on the next
     * content hash, and marks the holder's hash dirty.
     **/
    void invalidateFileStamp();

    /**
     * @brief The part of a Reload that reaches the other nodes: every other node with a file knob
     * covering the same files has its stamps dropped and its caches purged, and is re-keyed. Both
     * the knob's own reload and a node's Reload button call it, after reloading their own node.
     **/
    void reloadOtherNodesReadingTheseFiles();

Q_SIGNALS:

    void openFile();

private:
    virtual void appendUnmemoizedContentHash(ContentHasher& h) const OVERRIDE FINAL;
    virtual bool hasUnmemoizedContentHash() const OVERRIDE FINAL
    {
        return true;
    }

    /**
     * @brief a KnobFile is never animated but it's value may change, indicate this to the plug-in
     **/
    virtual bool evaluateValueChangeOnTimeChange() const OVERRIDE FINAL { return true; }

    virtual bool canAnimate() const OVERRIDE FINAL;
    virtual const std::string & typeName() const OVERRIDE FINAL;
    static const std::string _typeNameStr;
    int _isInputImage;
    bool _showsReloadButton;
    std::vector<std::string> _fileExtensions;
};

/******************************KnobOutputFile**************************************/

class KnobOutputFile
    :  public QObject, public KnobStringBase
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static KnobHelper * BuildKnob(KnobHolder* holder,
                                  const std::string &description,
                                  int dimension,
                                  bool declaredByPlugin = true)
    {
        return new KnobOutputFile(holder, description, dimension, declaredByPlugin);
    }

    KnobOutputFile(KnobHolder* holder,
                   const std::string &description,
                   int dimension,
                   bool declaredByPlugin);
    static const std::string & typeNameStatic();

    void setAsOutputImageFile()
    {
        _isOutputImage = true;
    }

    bool isOutputImageFile() const
    {
        return _isOutputImage;
    }

    void open_file()
    {
        Q_EMIT openFile(_sequenceDialog && _isOutputImage);
    }

    void rewriteFile();

    void turnOffSequences()
    {
        _sequenceDialog = false;
    }

    bool isSequencesDialogEnabled() const
    {
        return _sequenceDialog;
    }

    QString generateFileNameAtTime(SequenceTime time, ViewSpec view);

Q_SIGNALS:

    void openFile(bool);

private:

    virtual bool evaluateValueChangeOnTimeChange() const OVERRIDE FINAL { return false; }

    virtual bool canAnimate() const OVERRIDE FINAL;
    virtual const std::string & typeName() const OVERRIDE FINAL;

private:
    static const std::string _typeNameStr;
    bool _isOutputImage;
    bool _sequenceDialog;
};


/******************************KnobPath**************************************/


/**
 * @brief The string value is encoded the following way:
 * <Name>Lala</Name><Value>MyValue</Value>
 **/
class KnobPath
    : public KnobTable
{
public:

    static KnobHelper * BuildKnob(KnobHolder* holder,
                                  const std::string &description,
                                  int dimension,
                                  bool declaredByPlugin = true)
    {
        return new KnobPath(holder, description, dimension, declaredByPlugin);
    }

    KnobPath(KnobHolder* holder,
             const std::string &description,
             int dimension,
             bool declaredByPlugin);
    static const std::string & typeNameStatic();

    void setMultiPath(bool b);

    bool isMultiPath() const;

    /*
       @brief same as setMultiPath except that there will be only variable names, no values
     */
    void setAsStringList(bool b);
    bool getIsStringList() const;

    void getPaths(std::list<std::string>* paths);
    void prependPath(const std::string& path);
    void appendPath(const std::string& path);


    virtual int getColumnsCount() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return _isStringList ? 1 : 2;
    }

    virtual std::string getColumnLabel(int col) const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        switch (col) {
        case 0:

            return "Name";
        case 1:

            return "Value";
        default:

            return "";
        }
    }

    virtual bool isCellEnabled(int row, int col, const QStringList& values) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isCellBracketDecorated(int row, int col) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool useEditButton() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return _isMultiPath && !_isStringList;
    }

private:

    static std::string generateUniquePathID(const std::list<std::vector<std::string> >& paths);
    virtual const std::string & typeName() const OVERRIDE FINAL;

private:
    static const std::string _typeNameStr;
    bool _isMultiPath;
    bool _isStringList;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_KNOBFILE_H
