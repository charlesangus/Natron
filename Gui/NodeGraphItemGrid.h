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

#ifndef Gui_NodeGraphItemGrid_h
#define Gui_NodeGraphItemGrid_h

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <QRectF>

NATRON_NAMESPACE_ENTER

// A uniform grid of rectangles, each stored under a key with a payload, answering which of them
// overlap a query rectangle. A rectangle spanning more than maxCellsPerItem cells, or one that is
// not finite, goes to a list that every query tests instead, so that one huge backdrop does not
// fill thousands of cells. Overlap is inclusive of the borders: a broad phase must never miss what
// an exact test afterwards would find.
template <typename Key, typename Payload>
class NodeGraphItemGrid {
public:
    explicit NodeGraphItemGrid(double cellSize = 256.,
                               std::size_t maxCellsPerItem = 64)
        : _cellSize(cellSize > 0. ? cellSize : 256.)
        , _maxCellsPerItem(std::max<std::size_t>(1, maxCellsPerItem))
    {
    }

    std::size_t size() const
    {
        return _entries.size();
    }

    void clear()
    {
        _entries.clear();
        _cells.clear();
        _oversized.clear();
    }

    void insertOrUpdate(const Key& key,
                        const Payload& payload,
                        const QRectF& rect)
    {
        typename std::unordered_map<Key, Entry>::iterator found = _entries.find(key);
        if (found == _entries.end()) {
            found = _entries.emplace(key, Entry()).first;
        } else {
            unlink(&found->second);
        }
        Entry& entry = found->second;
        entry.payload = payload;
        entry.rect = rect;
        link(&entry);
    }

    bool remove(const Key& key)
    {
        typename std::unordered_map<Key, Entry>::iterator found = _entries.find(key);
        if (found == _entries.end()) {
            return false;
        }
        unlink(&found->second);
        _entries.erase(found);

        return true;
    }

    bool contains(const Key& key) const
    {
        return _entries.find(key) != _entries.end();
    }

    bool rectOf(const Key& key,
                QRectF* rect) const
    {
        typename std::unordered_map<Key, Entry>::const_iterator found = _entries.find(key);
        if (found == _entries.end()) {
            return false;
        }
        *rect = found->second.rect;

        return true;
    }

    // Appends, once each and in no particular order, the payloads of the rectangles that overlap rect.
    void query(const QRectF& rect,
               std::vector<Payload>* out)
    {
        if (_entries.empty()) {
            return;
        }
        if (++_stamp == 0) {
            for (typename std::unordered_map<Key, Entry>::iterator it = _entries.begin(); it != _entries.end(); ++it) {
                it->second.stamp = 0;
            }
            _stamp = 1;
        }
        CellRange range;
        if (!cellRange(rect, &range) || (range.cellCount() > _entries.size())) {
            for (typename std::unordered_map<Key, Entry>::iterator it = _entries.begin(); it != _entries.end(); ++it) {
                if (overlaps(it->second.rect, rect)) {
                    out->push_back(it->second.payload);
                }
            }

            return;
        }
        for (typename std::vector<Entry*>::const_iterator it = _oversized.begin(); it != _oversized.end(); ++it) {
            visit(*it, rect, out);
        }
        for (std::int64_t y = range.y0; y <= range.y1; ++y) {
            for (std::int64_t x = range.x0; x <= range.x1; ++x) {
                typename std::unordered_map<std::uint64_t, std::vector<Entry*>>::const_iterator cell = _cells.find(cellKey(x, y));
                if (cell == _cells.end()) {
                    continue;
                }
                for (typename std::vector<Entry*>::const_iterator it = cell->second.begin(); it != cell->second.end(); ++it) {
                    visit(*it, rect, out);
                }
            }
        }
    }

private:
    struct CellRange {
        std::int64_t x0 = 0, y0 = 0, x1 = -1, y1 = -1;

        std::size_t cellCount() const
        {
            return (std::size_t)(x1 - x0 + 1) * (std::size_t)(y1 - y0 + 1);
        }
    };

    struct Entry {
        Payload payload = Payload();
        QRectF rect;
        CellRange range;
        bool oversized = false;
        unsigned stamp = 0;
    };

    // Keeps cell indices, and so the products in cellCount(), far from overflowing whatever the
    // coordinates are.
    static constexpr double kCellIndexLimit = 1 << 24;

    static bool overlaps(const QRectF& a,
                         const QRectF& b)
    {
        const QRectF na = a.normalized();
        const QRectF nb = b.normalized();

        return (na.left() <= nb.right()) && (nb.left() <= na.right()) && (na.top() <= nb.bottom()) && (nb.top() <= na.bottom());
    }

    static std::uint64_t cellKey(std::int64_t x,
                                 std::int64_t y)
    {
        return ((std::uint64_t)(std::uint32_t)(std::int32_t)x << 32) | (std::uint64_t)(std::uint32_t)(std::int32_t)y;
    }

    std::int64_t cellIndex(double v) const
    {
        const double c = std::floor(v / _cellSize);

        return (std::int64_t)std::max(-kCellIndexLimit, std::min(kCellIndexLimit, c));
    }

    bool cellRange(const QRectF& rect,
                   CellRange* range) const
    {
        const QRectF r = rect.normalized();
        if (!std::isfinite(r.left()) || !std::isfinite(r.right()) || !std::isfinite(r.top()) || !std::isfinite(r.bottom())) {
            return false;
        }
        range->x0 = cellIndex(r.left());
        range->x1 = cellIndex(r.right());
        range->y0 = cellIndex(r.top());
        range->y1 = cellIndex(r.bottom());

        return true;
    }

    void visit(Entry* entry,
               const QRectF& rect,
               std::vector<Payload>* out)
    {
        if (entry->stamp == _stamp) {
            return;
        }
        entry->stamp = _stamp;
        if (overlaps(entry->rect, rect)) {
            out->push_back(entry->payload);
        }
    }

    void link(Entry* entry)
    {
        entry->stamp = 0;
        entry->oversized = !cellRange(entry->rect, &entry->range) || (entry->range.cellCount() > _maxCellsPerItem);
        if (entry->oversized) {
            _oversized.push_back(entry);

            return;
        }
        for (std::int64_t y = entry->range.y0; y <= entry->range.y1; ++y) {
            for (std::int64_t x = entry->range.x0; x <= entry->range.x1; ++x) {
                _cells[cellKey(x, y)].push_back(entry);
            }
        }
    }

    static void eraseFrom(std::vector<Entry*>* list,
                          Entry* entry)
    {
        typename std::vector<Entry*>::iterator found = std::find(list->begin(), list->end(), entry);
        if (found != list->end()) {
            *found = list->back();
            list->pop_back();
        }
    }

    void unlink(Entry* entry)
    {
        if (entry->oversized) {
            eraseFrom(&_oversized, entry);

            return;
        }
        for (std::int64_t y = entry->range.y0; y <= entry->range.y1; ++y) {
            for (std::int64_t x = entry->range.x0; x <= entry->range.x1; ++x) {
                typename std::unordered_map<std::uint64_t, std::vector<Entry*>>::iterator cell = _cells.find(cellKey(x, y));
                if (cell == _cells.end()) {
                    continue;
                }
                eraseFrom(&cell->second, entry);
                if (cell->second.empty()) {
                    _cells.erase(cell);
                }
            }
        }
    }

    double _cellSize;
    std::size_t _maxCellsPerItem;
    unsigned _stamp = 0;
    // Node-based, so the Entry pointers the cells hold survive rehashing.
    std::unordered_map<Key, Entry> _entries;
    std::unordered_map<std::uint64_t, std::vector<Entry*>> _cells;
    std::vector<Entry*> _oversized;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_NodeGraphItemGrid_h
