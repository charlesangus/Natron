/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#include "LensBrowser.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

CLANG_DIAG_OFF(uninitialized)
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTableView>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Optics/Glass.h"
#include "Engine/Optics/LensDatabase.h"
#include "Engine/Optics/Paraxial.h"
#include "Engine/Optics/Trace.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/KnobGui.h"
#include "Gui/KnobUndoCommand.h"

NATRON_NAMESPACE_ENTER

using Optics::LensDatabase;
using Optics::LensInfo;
using Optics::Prescription;

namespace {

enum Column {
    eColumnName,
    eColumnFocalLength,
    eColumnFNumber,
    eColumnElements,
    eColumnAspheric,
    eColumnAnamorphic,
    eColumnCount
};

const int kLensIndexRole = Qt::UserRole + 1;
const int kVendorRole = Qt::UserRole + 2;

// Any, then a flag that must be on, then one that must be off.
enum FlagFilter {
    eFlagAny,
    eFlagYes,
    eFlagNo
};

struct NamedProvider {
    QString name;
    LensBrowser::PreviewProvider provider;
};

std::vector<NamedProvider>&
previewProviders()
{
    static std::vector<NamedProvider> providers;

    return providers;
}

std::vector<QPointer<LensBrowser>>&
openBrowsers()
{
    static std::vector<QPointer<LensBrowser>> browsers;

    return browsers;
}

bool
flagPasses(int filter,
           bool value)
{
    return filter == eFlagAny || (filter == eFlagYes) == value;
}

class LensFilterModel
    : public QSortFilterProxyModel {
public:
    explicit LensFilterModel(QObject* parent)
        : QSortFilterProxyModel(parent)
    {
    }

    const std::vector<LensInfo>* lenses = 0;
    QString vendor;
    std::string query;
    double focalMin = 0.;
    double focalMax = std::numeric_limits<double>::infinity();
    double fNumberMin = 0.;
    double fNumberMax = std::numeric_limits<double>::infinity();
    int elementsMin = 0;
    int elementsMax = std::numeric_limits<int>::max();
    int aspheric = eFlagAny;
    int anamorphic = eFlagAny;

    void refilter() { invalidateFilter(); }

protected:
    virtual bool filterAcceptsRow(int sourceRow,
                                  const QModelIndex& sourceParent) const OVERRIDE FINAL
    {
        const QModelIndex index = sourceModel()->index(sourceRow, eColumnName, sourceParent);
        const int i = index.data(kLensIndexRole).toInt();
        if (!lenses || i < 0 || i >= (int)lenses->size()) {
            return false;
        }
        const LensInfo& lens = (*lenses)[i];
        if (!vendor.isEmpty() && index.data(kVendorRole).toString() != vendor) {
            return false;
        }

        return lens.focalLength >= focalMin && lens.focalLength <= focalMax && lens.fNumber >= fNumberMin && lens.fNumber <= fNumberMax && lens.elementCount >= elementsMin && lens.elementCount <= elementsMax && flagPasses(aspheric, lens.aspheric) && flagPasses(anamorphic, lens.anamorphic) && Optics::lensMatches(lens, query);
    }
};

struct RayPath {
    std::vector<QPointF> points;
    bool marginal = false;
};

// The meridional (y, z) profile of surface i, sampled across its clear aperture.
std::vector<QPointF>
surfaceProfile(const Optics::TraceSystem& system,
               int i,
               double semiDiameter)
{
    const double vertexZ = system.prescription().layout()[i].vertexZ;
    std::vector<QPointF> points;
    const int samples = 24;
    for (int k = 0; k <= samples; ++k) {
        const double y = semiDiameter * (2. * k / samples - 1.);
        const Optics::Ray probe { Optics::Vec3 { 0., y, vertexZ - 1000. }, Optics::Vec3 { 0., 0., 1. } };
        Optics::SurfaceHit hit;
        const double z = system.intersect(i, probe, &hit) ? hit.position.z : vertexZ;
        points.push_back(QPointF(z, y));
    }

    return points;
}

bool
refract(const Optics::Vec3& d,
        const Optics::Vec3& n,
        double eta,
        Optics::Vec3* out)
{
    const double cosi = d.x * n.x + d.y * n.y + d.z * n.z;
    const double k = 1. - eta * eta * (1. - cosi * cosi);
    if (k < 0.) {
        return false;
    }
    const double s = eta * cosi + std::sqrt(k);
    Optics::Vec3 r { d.x * eta - n.x * s, d.y * eta - n.y * s, d.z * eta - n.z * s };
    const double len = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
    *out = Optics::Vec3 { r.x / len, r.y / len, r.z / len };

    return true;
}

// The vertices of a sequential trace at the d line, ending on the image plane; the tracer itself only
// reports where a ray lands.
RayPath
traceDrawn(const Optics::TraceSystem& system,
           double height,
           bool marginal)
{
    RayPath path;
    path.marginal = marginal;
    Optics::Ray ray = system.rayAtFrontSurface(0., height, Optics::Vec3 { 0., 0., 1. });
    path.points.push_back(QPointF(ray.position.z, ray.position.y));
    const int n = (int)system.prescription().surfaces.size();
    for (int i = 0; i < n; ++i) {
        Optics::SurfaceHit hit;
        if (!system.intersect(i, ray, &hit)) {
            return path;
        }
        ray.position = hit.position;
        path.points.push_back(QPointF(ray.position.z, ray.position.y));
        const double n1 = system.ior(i - 1, 0), n2 = system.ior(i, 0);
        if (n1 != n2 && !refract(ray.direction, hit.normal, n1 / n2, &ray.direction)) {
            return path;
        }
    }
    if (std::abs(ray.direction.z) > 1e-9) {
        const double t = (system.imagePlaneZ() - ray.position.z) / ray.direction.z;
        if (t > 0.) {
            path.points.push_back(QPointF(system.imagePlaneZ(), ray.position.y + ray.direction.y * t));
        }
    }

    return path;
}

class LensDiagram
    : public QWidget {
public:
    explicit LensDiagram(QWidget* parent)
        : QWidget(parent)
    {
        setMinimumSize(480, 260);
    }

    void setLens(const Prescription* lens)
    {
        _elements.clear();
        _stops.clear();
        _rays.clear();
        _valid = false;
        if (lens && !lens->surfaces.empty()) {
            build(*lens);
        }
        update();
    }

protected:
    virtual void paintEvent(QPaintEvent*) OVERRIDE FINAL
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor(32, 32, 32));
        if (!_valid) {
            return;
        }
        const double margin = 16.;
        const double w = std::max(1., width() - 2. * margin), h = std::max(1., height() - 2. * margin);
        const double spanZ = std::max(1e-6, _maxZ - _minZ), spanY = std::max(1e-6, 2. * _maxY);
        const double scale = std::min(w / spanZ, h / spanY);
        const double originX = margin + (w - spanZ * scale) / 2. - _minZ * scale;
        const double originY = height() / 2.;
        const auto map = [&](const QPointF& zy) { return QPointF(originX + zy.x() * scale, originY - zy.y() * scale); };

        p.setPen(QPen(QColor(90, 90, 90), 1., Qt::DashLine));
        p.drawLine(map(QPointF(_minZ, 0.)), map(QPointF(_maxZ, 0.)));
        p.drawLine(map(QPointF(_imageZ, -_maxY)), map(QPointF(_imageZ, _maxY)));

        p.setPen(QPen(QColor(150, 190, 230), 1.2));
        p.setBrush(QColor(110, 150, 200, 90));
        for (const QPolygonF& element : _elements) {
            QPolygonF mapped;
            for (const QPointF& zy : element) {
                mapped << map(zy);
            }
            p.drawPolygon(mapped);
        }

        p.setPen(QPen(QColor(230, 230, 230), 2.));
        for (const std::pair<double, double>& stop : _stops) {
            const double bar = std::max(stop.second * 0.25, 1.);
            p.drawLine(map(QPointF(stop.first, stop.second)), map(QPointF(stop.first, stop.second + bar)));
            p.drawLine(map(QPointF(stop.first, -stop.second)), map(QPointF(stop.first, -stop.second - bar)));
        }

        p.setBrush(Qt::NoBrush);
        for (const RayPath& ray : _rays) {
            p.setPen(QPen(ray.marginal ? QColor(240, 150, 60) : QColor(240, 220, 110), 1.));
            QPolygonF mapped;
            for (const QPointF& zy : ray.points) {
                mapped << map(zy);
            }
            p.drawPolyline(mapped);
        }
    }

private:
    void build(const Prescription& lens)
    {
        const Optics::GlassCatalogue noCatalogue;
        Optics::TraceSystem system(lens, noCatalogue, std::string(), 0., std::vector<double>(1, Optics::kWavelengthDNm));
        const std::vector<Optics::SurfaceLayout> layout = lens.layout();
        const int n = (int)lens.surfaces.size();
        _minZ = 0.;
        _maxZ = std::max(layout.back().vertexZ, system.imagePlaneZ());
        _imageZ = system.imagePlaneZ();
        _maxY = 0.;
        for (const Optics::Surface& s : lens.surfaces) {
            _maxY = std::max(_maxY, s.semiDiameter);
        }
        if (_maxY <= 0.) {
            _maxY = std::max(1., lens.totalTrack() / 4.);
        }

        for (int i = 0; i < n; ++i) {
            const Optics::Surface& s = lens.surfaces[i];
            if (s.type == Optics::SurfaceType::Stop) {
                _stops.push_back(std::make_pair(layout[i].vertexZ, s.semiDiameter > 0. ? s.semiDiameter : _maxY));
                continue;
            }
            if (!s.isGlass() || i + 1 >= n) {
                continue;
            }
            const Optics::Surface& next = lens.surfaces[i + 1];
            const double front = s.semiDiameter > 0. ? s.semiDiameter : _maxY;
            const double back = next.semiDiameter > 0. ? next.semiDiameter : front;
            std::vector<QPointF> a = surfaceProfile(system, i, front);
            std::vector<QPointF> b = surfaceProfile(system, i + 1, back);
            QPolygonF element;
            for (const QPointF& point : a) {
                element << point;
            }
            for (auto it = b.rbegin(); it != b.rend(); ++it) {
                element << *it;
            }
            _elements.push_back(element);
            for (const QPointF& point : a) {
                _minZ = std::min(_minZ, point.x());
            }
        }

        Optics::ParaxialFacts facts;
        std::string error;
        double pupil = 0.;
        if (Optics::paraxialFacts(lens, &facts, &error)) {
            pupil = facts.entrancePupilSemiDiameter;
        }
        if (pupil <= 0.) {
            pupil = lens.surfaces.front().semiDiameter * 0.5;
        }
        _minZ -= std::max(2., lens.totalTrack() * 0.15);
        const double fractions[] = { -1., -0.5, 0., 0.5, 1. };
        for (double f : fractions) {
            _rays.push_back(traceDrawn(system, f * pupil * 0.999, std::abs(f) == 1.));
        }
        _valid = true;
    }

    std::vector<QPolygonF> _elements;
    std::vector<std::pair<double, double>> _stops;
    std::vector<RayPath> _rays;
    double _minZ = 0.;
    double _maxZ = 1.;
    double _imageZ = 1.;
    double _maxY = 1.;
    bool _valid = false;
};

struct PreviewTab {
    PreviewTab(const NamedProvider& p)
        : provider(p.provider)
        , name(p.name)
    {
    }

    LensBrowser::PreviewProvider provider;
    QString name;
    QLabel* label = 0;
    QFutureWatcher<QImage>* watcher = 0;
    std::shared_ptr<std::atomic<bool>> cancelled;
    QString lensId;
};

QString
formatted(double value,
          int decimals)
{
    return QString::number(value, 'f', decimals);
}

} // namespace

struct LensBrowserPrivate {
    KnobStringWPtr lens;
    LensDatabase* database = 0;
    QStandardItemModel* model = 0;
    LensFilterModel* filter = 0;
    QTreeWidget* vendors = 0;
    QLineEdit* search = 0;
    QDoubleSpinBox* focalMin = 0;
    QDoubleSpinBox* focalMax = 0;
    QDoubleSpinBox* fNumberMin = 0;
    QDoubleSpinBox* fNumberMax = 0;
    QSpinBox* elementsMin = 0;
    QSpinBox* elementsMax = 0;
    QComboBox* aspheric = 0;
    QComboBox* anamorphic = 0;
    QTableView* table = 0;
    QLabel* details = 0;
    QTabWidget* tabs = 0;
    LensDiagram* diagram = 0;
    QDialogButtonBox* buttons = 0;
    std::vector<PreviewTab> previews;
};

void
LensBrowser::addPreviewProvider(const QString& name,
                                const PreviewProvider& provider)
{
    previewProviders().push_back(NamedProvider { name, provider });
}

LensBrowser*
LensBrowser::open(const KnobStringPtr& lens)
{
    std::vector<QPointer<LensBrowser>>& browsers = openBrowsers();
    browsers.erase(std::remove_if(browsers.begin(), browsers.end(), [](const QPointer<LensBrowser>& b) { return b.isNull(); }),
                   browsers.end());
    for (const QPointer<LensBrowser>& b : browsers) {
        if (b->_imp->lens.lock() == lens) {
            b->show();
            b->raise();
            b->activateWindow();

            return b.data();
        }
    }
    QWidget* parent = 0;
    if (lens && lens->getHolder()) {
        if (GuiAppInstancePtr app = std::dynamic_pointer_cast<GuiAppInstance>(lens->getHolder()->getApp())) {
            parent = app->getGui();
        }
    }
    LensBrowser* browser = new LensBrowser(lens, parent);
    browser->setAttribute(Qt::WA_DeleteOnClose);
    browsers.push_back(browser);
    browser->show();

    return browser;
}

LensBrowser::LensBrowser(const KnobStringPtr& lens,
                         QWidget* parent)
    : QDialog(parent)
    , _imp(new LensBrowserPrivate)
{
    _imp->lens = lens;
    _imp->database = &LensDatabase::shared();
    setObjectName(QString::fromUtf8("LensBrowser"));
    setWindowTitle(tr("Lens Browser"));
    setModal(false);

    const std::vector<LensInfo>& lenses = _imp->database->lenses();

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    QSplitter* splitter = new QSplitter(Qt::Horizontal, this);
    mainLayout->addWidget(splitter, 1);

    _imp->vendors = new QTreeWidget(splitter);
    _imp->vendors->setObjectName(QString::fromUtf8("lensVendors"));
    _imp->vendors->setHeaderLabels(QStringList() << tr("Vendor") << tr("Lenses"));
    _imp->vendors->setRootIsDecorated(false);
    {
        std::map<std::string, int> counts;
        for (const LensInfo& info : lenses) {
            ++counts[info.vendor];
        }
        QTreeWidgetItem* all = new QTreeWidgetItem(_imp->vendors, QStringList() << tr("All Lenses") << QString::number(lenses.size()));
        all->setData(0, Qt::UserRole, QString());
        for (const std::string& vendor : _imp->database->vendors()) {
            QTreeWidgetItem* item = new QTreeWidgetItem(_imp->vendors, QStringList() << QString::fromStdString(vendor) << QString::number(counts[vendor]));
            item->setData(0, Qt::UserRole, QString::fromStdString(vendor));
        }
        _imp->vendors->setCurrentItem(all);
    }
    _imp->vendors->resizeColumnToContents(0);

    QWidget* centre = new QWidget(splitter);
    QVBoxLayout* centreLayout = new QVBoxLayout(centre);
    centreLayout->setContentsMargins(0, 0, 0, 0);
    _imp->search = new QLineEdit(centre);
    _imp->search->setObjectName(QString::fromUtf8("lensSearch"));
    _imp->search->setPlaceholderText(tr("Search names, patents and sources"));
    _imp->search->setClearButtonEnabled(true);
    centreLayout->addWidget(_imp->search);

    QGroupBox* filters = new QGroupBox(tr("Filters"), centre);
    QFormLayout* filterLayout = new QFormLayout(filters);
    const auto range = [&](const char* name, double max, int decimals, const QString& suffix,
                           QDoubleSpinBox** lo, QDoubleSpinBox** hi) {
        QWidget* row = new QWidget(filters);
        QHBoxLayout* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        for (int k = 0; k < 2; ++k) {
            QDoubleSpinBox* box = new QDoubleSpinBox(row);
            box->setObjectName(QString::fromUtf8(name) + (k == 0 ? QString::fromUtf8("Min") : QString::fromUtf8("Max")));
            box->setDecimals(decimals);
            box->setRange(0., max);
            box->setValue(k == 0 ? 0. : max);
            box->setSuffix(suffix);
            layout->addWidget(box);
            if (k == 0) {
                layout->addWidget(new QLabel(tr("to"), row));
                *lo = box;
            } else {
                *hi = box;
            }
        }
        layout->addStretch();

        return row;
    };
    filterLayout->addRow(tr("Focal Length"), range("focal", 10000., 1, tr(" mm"), &_imp->focalMin, &_imp->focalMax));
    filterLayout->addRow(tr("F-Number"), range("fNumber", 100., 2, QString(), &_imp->fNumberMin, &_imp->fNumberMax));
    {
        QWidget* row = new QWidget(filters);
        QHBoxLayout* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        _imp->elementsMin = new QSpinBox(row);
        _imp->elementsMin->setObjectName(QString::fromUtf8("elementsMin"));
        _imp->elementsMin->setRange(0, 100);
        _imp->elementsMax = new QSpinBox(row);
        _imp->elementsMax->setObjectName(QString::fromUtf8("elementsMax"));
        _imp->elementsMax->setRange(0, 100);
        _imp->elementsMax->setValue(100);
        layout->addWidget(_imp->elementsMin);
        layout->addWidget(new QLabel(tr("to"), row));
        layout->addWidget(_imp->elementsMax);
        layout->addStretch();
        filterLayout->addRow(tr("Elements"), row);
    }
    const auto flag = [&](const char* name) {
        QComboBox* box = new QComboBox(filters);
        box->setObjectName(QString::fromUtf8(name));
        box->addItem(tr("Any"), (int)eFlagAny);
        box->addItem(tr("Yes"), (int)eFlagYes);
        box->addItem(tr("No"), (int)eFlagNo);

        return box;
    };
    _imp->aspheric = flag("asphericFilter");
    _imp->anamorphic = flag("anamorphicFilter");
    filterLayout->addRow(tr("Aspheric"), _imp->aspheric);
    filterLayout->addRow(tr("Anamorphic"), _imp->anamorphic);
    centreLayout->addWidget(filters);

    _imp->model = new QStandardItemModel(0, eColumnCount, this);
    _imp->model->setHorizontalHeaderLabels(QStringList() << tr("Name") << tr("Focal Length") << tr("F-Number")
                                                         << tr("Elements") << tr("Aspheric") << tr("Anamorphic"));
    for (size_t i = 0; i < lenses.size(); ++i) {
        const LensInfo& info = lenses[i];
        QList<QStandardItem*> row;
        QStandardItem* name = new QStandardItem(QString::fromStdString(info.name));
        name->setData((int)i, kLensIndexRole);
        name->setData(QString::fromStdString(info.vendor), kVendorRole);
        name->setData(QString::fromStdString(info.id), Qt::UserRole);
        name->setToolTip(QString::fromStdString(info.id));
        row << name;
        QStandardItem* focal = new QStandardItem;
        focal->setData(std::round(info.focalLength * 10.) / 10., Qt::DisplayRole);
        row << focal;
        QStandardItem* fNumber = new QStandardItem;
        fNumber->setData(std::round(info.fNumber * 100.) / 100., Qt::DisplayRole);
        row << fNumber;
        QStandardItem* elements = new QStandardItem;
        elements->setData(info.elementCount, Qt::DisplayRole);
        row << elements;
        row << new QStandardItem(info.aspheric ? tr("Yes") : QString());
        row << new QStandardItem(info.anamorphic ? tr("Yes") : QString());
        for (QStandardItem* item : row) {
            item->setEditable(false);
        }
        _imp->model->appendRow(row);
    }
    _imp->filter = new LensFilterModel(this);
    _imp->filter->lenses = &lenses;
    _imp->filter->setSourceModel(_imp->model);
    _imp->filter->setSortRole(Qt::DisplayRole);

    _imp->table = new QTableView(centre);
    _imp->table->setObjectName(QString::fromUtf8("lensTable"));
    _imp->table->setModel(_imp->filter);
    _imp->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    _imp->table->setSelectionMode(QAbstractItemView::SingleSelection);
    _imp->table->setSortingEnabled(true);
    _imp->table->sortByColumn(eColumnName, Qt::AscendingOrder);
    _imp->table->verticalHeader()->hide();
    _imp->table->horizontalHeader()->setSectionResizeMode(eColumnName, QHeaderView::Stretch);
    _imp->table->setAlternatingRowColors(true);
    centreLayout->addWidget(_imp->table, 1);

    QWidget* right = new QWidget(splitter);
    QVBoxLayout* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    _imp->tabs = new QTabWidget(right);
    _imp->tabs->setObjectName(QString::fromUtf8("lensPreviews"));
    _imp->diagram = new LensDiagram(_imp->tabs);
    _imp->diagram->setObjectName(QString::fromUtf8("lensDiagram"));
    _imp->tabs->addTab(_imp->diagram, tr("Cross Section"));
    for (const NamedProvider& p : previewProviders()) {
        _imp->previews.emplace_back(p);
        PreviewTab& tab = _imp->previews.back();
        tab.label = new QLabel(_imp->tabs);
        tab.label->setAlignment(Qt::AlignCenter);
        tab.label->setMinimumSize(480, 260);
        tab.watcher = new QFutureWatcher<QImage>(this);
        QObject::connect(tab.watcher, SIGNAL(finished()), this, SLOT(onPreviewReady()));
        _imp->tabs->addTab(tab.label, p.name);
    }
    rightLayout->addWidget(_imp->tabs, 3);
    _imp->details = new QLabel(right);
    _imp->details->setObjectName(QString::fromUtf8("lensDetails"));
    _imp->details->setWordWrap(true);
    _imp->details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    _imp->details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rightLayout->addWidget(_imp->details, 2);

    splitter->addWidget(_imp->vendors);
    splitter->addWidget(centre);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 3);
    splitter->setStretchFactor(2, 2);
    splitter->setSizes(QList<int>() << 220 << 700 << 560);

    _imp->buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    mainLayout->addWidget(_imp->buttons);
    QObject::connect(_imp->buttons, SIGNAL(accepted()), this, SLOT(accept()));
    QObject::connect(_imp->buttons, SIGNAL(rejected()), this, SLOT(reject()));

    QObject::connect(_imp->vendors, SIGNAL(currentItemChanged(QTreeWidgetItem*, QTreeWidgetItem*)), this, SLOT(onVendorChanged()));
    QObject::connect(_imp->search, SIGNAL(textChanged(QString)), this, SLOT(onFiltersChanged()));
    for (QDoubleSpinBox* box : { _imp->focalMin, _imp->focalMax, _imp->fNumberMin, _imp->fNumberMax }) {
        QObject::connect(box, SIGNAL(valueChanged(double)), this, SLOT(onFiltersChanged()));
    }
    for (QSpinBox* box : { _imp->elementsMin, _imp->elementsMax }) {
        QObject::connect(box, SIGNAL(valueChanged(int)), this, SLOT(onFiltersChanged()));
    }
    for (QComboBox* box : { _imp->aspheric, _imp->anamorphic }) {
        QObject::connect(box, SIGNAL(currentIndexChanged(int)), this, SLOT(onFiltersChanged()));
    }
    QObject::connect(_imp->table->selectionModel(), SIGNAL(selectionChanged(QItemSelection, QItemSelection)), this, SLOT(onSelectionChanged()));

    resize(1500, 860);

    const QString current = lens ? QString::fromStdString(lens->getValue()) : QString();
    for (int r = 0; r < _imp->filter->rowCount(); ++r) {
        const QModelIndex index = _imp->filter->index(r, eColumnName);
        if (index.data(Qt::UserRole).toString() == current) {
            _imp->table->selectRow(r);
            _imp->table->scrollTo(index, QAbstractItemView::PositionAtCenter);
            break;
        }
    }
    onSelectionChanged();
}

LensBrowser::~LensBrowser()
{
    for (PreviewTab& tab : _imp->previews) {
        if (tab.cancelled) {
            *tab.cancelled = true;
        }
        if (tab.watcher) {
            QObject::disconnect(tab.watcher, 0, this, 0);
            tab.watcher->waitForFinished();
        }
    }
}

QString
LensBrowser::selectedId() const
{
    const QModelIndexList rows = _imp->table->selectionModel()->selectedRows(eColumnName);
    if (rows.isEmpty()) {
        return QString();
    }

    return rows.front().data(Qt::UserRole).toString();
}

void
LensBrowser::onVendorChanged()
{
    QTreeWidgetItem* item = _imp->vendors->currentItem();
    _imp->filter->vendor = item ? item->data(0, Qt::UserRole).toString() : QString();
    _imp->filter->refilter();
    onSelectionChanged();
}

void
LensBrowser::onFiltersChanged()
{
    LensFilterModel* f = _imp->filter;
    f->query = _imp->search->text().toStdString();
    f->focalMin = _imp->focalMin->value();
    f->focalMax = _imp->focalMax->value() >= _imp->focalMax->maximum() ? std::numeric_limits<double>::infinity() : _imp->focalMax->value();
    f->fNumberMin = _imp->fNumberMin->value();
    f->fNumberMax = _imp->fNumberMax->value() >= _imp->fNumberMax->maximum() ? std::numeric_limits<double>::infinity() : _imp->fNumberMax->value();
    f->elementsMin = _imp->elementsMin->value();
    f->elementsMax = _imp->elementsMax->value() >= _imp->elementsMax->maximum() ? std::numeric_limits<int>::max() : _imp->elementsMax->value();
    f->aspheric = _imp->aspheric->currentData().toInt();
    f->anamorphic = _imp->anamorphic->currentData().toInt();
    f->refilter();
    onSelectionChanged();
}

void
LensBrowser::onSelectionChanged()
{
    const QString id = selectedId();
    _imp->buttons->button(QDialogButtonBox::Ok)->setEnabled(!id.isEmpty());

    Prescription lens;
    std::string error;
    const LensInfo* info = id.isEmpty() ? 0 : _imp->database->find(id.toStdString());
    const bool loaded = info && _imp->database->prescription(info->id, &lens, &error);

    for (PreviewTab& tab : _imp->previews) {
        if (tab.lensId == id && tab.cancelled && !*tab.cancelled) {
            continue;
        }
        if (tab.cancelled) {
            *tab.cancelled = true;
        }
        tab.lensId = id;
        tab.label->clear();
        if (!loaded) {
            continue;
        }
        tab.cancelled = std::make_shared<std::atomic<bool>>(false);
        const std::shared_ptr<std::atomic<bool>> cancelled = tab.cancelled;
        const PreviewProvider provider = tab.provider;
        const QSize size = tab.label->size().expandedTo(QSize(256, 256));
        tab.watcher->setFuture(QtConcurrent::run([provider, lens, size, cancelled]() { return provider(lens, size, *cancelled); }));
    }

    if (!info) {
        _imp->diagram->setLens(0);
        _imp->details->clear();

        return;
    }
    _imp->diagram->setLens(loaded ? &lens : 0);

    QString text;
    text += QString::fromUtf8("<b>%1</b><br/>").arg(QString::fromStdString(info->name).toHtmlEscaped());
    if (!info->title.empty() && info->title != info->name) {
        text += QString::fromStdString(info->title).toHtmlEscaped() + QString::fromUtf8("<br/>");
    }
    text += QString::fromUtf8("<table>");
    const auto row = [&text](const QString& label, const QString& value) {
        text += QString::fromUtf8("<tr><td>%1&nbsp;&nbsp;</td><td>%2</td></tr>").arg(label, value.toHtmlEscaped());
    };
    row(tr("Id"), QString::fromStdString(info->id));
    row(tr("Focal Length"), formatted(info->focalLength, 2) + tr(" mm"));
    row(tr("F-Number"), formatted(info->fNumber, 2));
    row(tr("Elements"), QString::number(info->elementCount));
    if (loaded) {
        Optics::ParaxialFacts facts;
        std::string factsError;
        if (Optics::paraxialFacts(lens, &facts, &factsError)) {
            row(tr("Paraxial F-Number"), formatted(facts.paraxialFNumber, 2));
            row(tr("Back Focal Distance"), formatted(facts.backFocalDistance, 2) + tr(" mm"));
            row(tr("Entrance Pupil"), tr("%1 mm diameter, %2 mm behind the front vertex").arg(formatted(2. * facts.entrancePupilSemiDiameter, 2), formatted(facts.entrancePupilZ, 2)));
        }
        row(tr("Surfaces"), QString::number(lens.surfaces.size()));
        row(tr("Total Track"), formatted(lens.totalTrack(), 2) + tr(" mm"));
    } else {
        row(tr("Error"), QString::fromStdString(error));
    }
    row(tr("Aspheric"), info->aspheric ? tr("Yes") : tr("No"));
    row(tr("Anamorphic"), info->anamorphic ? tr("Yes") : tr("No"));
    if (!info->source.empty()) {
        row(tr("Source"), QString::fromStdString(info->source));
    }
    if (!info->sourcePath.empty()) {
        row(tr("Database File"), QString::fromStdString(info->sourcePath));
    }
    text += QString::fromUtf8("</table>");
    _imp->details->setText(text);
}

void
LensBrowser::onPreviewReady()
{
    QFutureWatcher<QImage>* watcher = dynamic_cast<QFutureWatcher<QImage>*>(sender());
    for (PreviewTab& tab : _imp->previews) {
        if (tab.watcher != watcher || !tab.cancelled || *tab.cancelled) {
            continue;
        }
        const QImage image = watcher->result();
        tab.label->setPixmap(image.isNull() ? QPixmap() : QPixmap::fromImage(image));
    }
}

void
LensBrowser::accept()
{
    const QString id = selectedId();
    KnobStringPtr lens = _imp->lens.lock();
    if (lens && !id.isEmpty()) {
        const std::string oldValue = lens->getValue();
        const std::string newValue = id.toStdString();
        if (oldValue != newValue) {
            KnobGuiPtr knobGui = std::dynamic_pointer_cast<KnobGui>(lens->getKnobGuiPointer());
            if (knobGui) {
                knobGui->pushUndoCommand(new KnobUndoCommand<std::string>(knobGui, oldValue, newValue));
            } else {
                lens->setValue(newValue);
            }
        }
    }
    QDialog::accept();
}

void
LensBrowser::closeEvent(QCloseEvent* e)
{
    for (PreviewTab& tab : _imp->previews) {
        if (tab.cancelled) {
            *tab.cancelled = true;
        }
    }
    QDialog::closeEvent(e);
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_LensBrowser.cpp"
