/*  Copyright (C) 2017 Eric Wasylishen

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

See file, 'COPYING', for details.
*/

#include "mainwindow.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDockWidget>
#include <QString>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QFileSystemWatcher>
#include <QFileInfo>
#include <QInputDialog>
#include <QFormLayout>
#include <QLineEdit>
#include <QSplitter>
#include <QCheckBox>
#include <QPushButton>
#include <QSlider>
#include <QSettings>
#include <QMenu>
#include <QMenuBar>
#include <QActionGroup>
#include <QFileDialog>
#include <QGroupBox>
#include <QRadioButton>
#include <QTimer>
#include <QScrollArea>
#include <QSpinBox>
#include <QScrollBar>
#include <QFrame>
#include <QLabel>
#include <QTextEdit>
#include <QStatusBar>
#include <QStringList>
#include <QThread>
#include <QApplication>
#include <QDesktopServices>
#include <QCloseEvent>
#include <optional>

#include <common/bspfile.hh>
#include <common/litfile.hh>
#include <common/bsputils.hh>
#include <qbsp/qbsp.hh>
#include <vis/vis.hh>
#include <light/light.hh>
#include <common/bspinfo.hh>
#include <fmt/core.h>
#include <fmt/chrono.h>
#include <QMessageBox>

#include "glview.h"
#include "stats.h"

// Recent files

static constexpr auto RECENT_SETTINGS_KEY = "recent_files";
static constexpr size_t MAX_RECENTS = 10;

static void ClearRecents()
{
    QSettings s;
    s.setValue(RECENT_SETTINGS_KEY, QStringList());
}

/**
 * Updates the recent files settings by pushing the given file to the front
 * and trimming the list to SETTINGS_MAX.
 *
 * @param file the file to push
 * @return the new recent files list
 */
static QStringList AddRecent(const QString &file)
{
    QSettings s;
    QStringList recents = s.value(RECENT_SETTINGS_KEY).toStringList();

    recents.removeOne(file); // no-op if not present
    recents.push_front(file);
    while (recents.size() > MAX_RECENTS) {
        recents.pop_back();
    }

    s.setValue(RECENT_SETTINGS_KEY, recents);

    return recents;
}

static QStringList GetRecents()
{
    QSettings s;
    QStringList recents = s.value(RECENT_SETTINGS_KEY).toStringList();
    return recents;
}

// Camera Bookmarks

static constexpr auto CAMERA_BOOKMARKS_SETTINGS_KEY = "camera_bookmarks";
static constexpr size_t MAX_CAMERA_BOOKMARKS = 10;

struct camera_bookmark_t
{
    qvec3f origin;
    qvec3f forward;
};

static void ClearCameraBookmarks()
{
    QSettings s;
    s.setValue(CAMERA_BOOKMARKS_SETTINGS_KEY, QStringList());
}

static QString CameraBookmarkToQString(const camera_bookmark_t &b)
{
    return QString("%1 %2 %3 %4 %5 %6")
        .arg(b.origin[0])
        .arg(b.origin[1])
        .arg(b.origin[2])
        .arg(b.forward[0])
        .arg(b.forward[1])
        .arg(b.forward[2]);
}

static std::optional<camera_bookmark_t> CameraBookmarkFromQString(const QString &string)
{
    QStringList parts = string.split(' ', Qt::SkipEmptyParts);
    if (parts.length() != 6)
        return std::nullopt;

    camera_bookmark_t result;
    result.origin[0] = parts[0].toFloat();
    result.origin[1] = parts[1].toFloat();
    result.origin[2] = parts[2].toFloat();

    result.forward[0] = parts[3].toFloat();
    result.forward[1] = parts[4].toFloat();
    result.forward[2] = parts[5].toFloat();

    return {result};
}

/**
 * Updates the recent camera bookmarks settings by pushing the given camera pos to the front
 * and trimming the list to MAX_CAMERA_BOOKMARKS.
 */
static void AddCameraBookmark(const camera_bookmark_t &b)
{
    QSettings s;
    QStringList qt_strings = s.value(CAMERA_BOOKMARKS_SETTINGS_KEY).toStringList();

    qt_strings.push_front(CameraBookmarkToQString(b));

    while (qt_strings.size() > MAX_CAMERA_BOOKMARKS) {
        qt_strings.pop_back();
    }

    s.setValue(CAMERA_BOOKMARKS_SETTINGS_KEY, qt_strings);
}

static std::vector<camera_bookmark_t> GetCameraBookmarks()
{
    QSettings s;
    QStringList qt_strings = s.value(CAMERA_BOOKMARKS_SETTINGS_KEY).toStringList();

    std::vector<camera_bookmark_t> result;
    for (const QString &qt_string : qt_strings) {
        if (auto parsed = CameraBookmarkFromQString(qt_string); parsed) {
            result.push_back(*parsed);
        }
    }
    return result;
}

// ETLogWidget
ETLogWidget::ETLogWidget(QWidget *parent)
    : QTabWidget(parent)
{
    for (size_t i = 0; i < std::size(logTabNames); i++) {
        m_textEdits[i] = new QTextEdit();

        auto *formLayout = new QFormLayout();
        auto *form = new QWidget();
        formLayout->addRow(m_textEdits[i]);
        form->setLayout(formLayout);
        setTabText(i, logTabNames[i]);
        addTab(form, logTabNames[i]);
        formLayout->setContentsMargins(0, 0, 0, 0);
    }
}

// MainWindow

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    // create the menu first as it is used by other things (dock widgets)
    setupMenu();

    // gl view
    glView = new GLView(this);
    setCentralWidget(glView);

    setAcceptDrops(true);

    createPropertiesSidebar();
    createOutputLog();
    createStatsSidebar();

    createStatusBar();

    QSettings s;
    if (s.contains("window_geometry")) {
        restoreGeometry(s.value("window_geometry").toByteArray());
        restoreState(s.value("window_state").toByteArray());
    } else {
        resize(1024, 768);
    }
}

void MainWindow::createPropertiesSidebar()
{
    QDockWidget *dock = new QDockWidget(tr("Build & View"), this);

    auto *formLayout = new QFormLayout();

    vis_checkbox = new QCheckBox(tr("Run vis"));
    light_checkbox = new QCheckBox(tr("Run light"));
    light_checkbox->setChecked(true);

    common_options = new QLineEdit();
    qbsp_options = new QLineEdit();
    vis_options = new QLineEdit();
    light_options = new QLineEdit();
    auto *reload_button = new QPushButton(tr("Reload"));
    reload_button->setShortcut(QKeySequence::Refresh);

    render_lightmapped = new QRadioButton(tr("Lightmapped"));
    render_lightmapped->setChecked(true);
    render_lightmap_only = new QRadioButton(tr("Lightmap Only"));
    render_fullbright = new QRadioButton(tr("Fullbright"));
    render_normals = new QRadioButton(tr("Normals"));
    render_flat = new QRadioButton(tr("Flat shading"));
    render_hull0 = new QRadioButton(tr("Leafs"));
    render_hull1 = new QRadioButton(tr("Hull 1"));
    render_hull2 = new QRadioButton(tr("Hull 2"));
    render_hull3 = new QRadioButton(tr("Hull 3"));
    render_hull4 = new QRadioButton(tr("Hull 4"));
    render_hull5 = new QRadioButton(tr("Hull 5"));

    render_lightmapped->setShortcut(QKeySequence("Alt+1"));
    render_lightmap_only->setShortcut(QKeySequence("Alt+2"));
    render_fullbright->setShortcut(QKeySequence("Alt+3"));
    render_normals->setShortcut(QKeySequence("Alt+4"));
    render_flat->setShortcut(QKeySequence("Alt+5"));
    render_hull0->setShortcut(QKeySequence("Alt+6"));

    render_lightmapped->setToolTip("Lightmapped textures (Alt+1)");
    render_lightmap_only->setToolTip("Lightmap only (Alt+2)");
    render_fullbright->setToolTip("Textures without lightmap (Alt+3)");
    render_normals->setToolTip("Visualize normals (Alt+4)");
    render_flat->setToolTip("Flat-shaded polygons (Alt+5)");

    auto *rendermode_layout = new QVBoxLayout();
    rendermode_layout->addWidget(render_lightmapped);
    rendermode_layout->addWidget(render_lightmap_only);
    rendermode_layout->addWidget(render_fullbright);
    rendermode_layout->addWidget(render_normals);
    rendermode_layout->addWidget(render_flat);
    rendermode_layout->addWidget(render_hull0);
    rendermode_layout->addWidget(render_hull1);
    rendermode_layout->addWidget(render_hull2);
    rendermode_layout->addWidget(render_hull3);
    rendermode_layout->addWidget(render_hull4);
    rendermode_layout->addWidget(render_hull5);

    auto *rendermode_group = new QGroupBox(tr("Render mode"));
    rendermode_group->setLayout(rendermode_layout);

    draw_portals = new QCheckBox(tr("Draw Portals (PRT)"));
    draw_leak = new QCheckBox(tr("Draw Leak (PTS/LIN)"));
    draw_lightgrid = new QCheckBox(tr("Draw Lightgrid"));
    show_click_ray = new QCheckBox(tr("Show Selection Ray"));
    show_hud = new QCheckBox(tr("HUD Overlay"));
    show_hud->setToolTip("Toggle HUD overlay (H)");

    show_tris = new QCheckBox(tr("Show Tris"));
    show_tris_seethrough = new QCheckBox(tr("Show Tris (See Through)"));
    vis_culling = new QCheckBox(tr("Vis Culling"));
    vis_culling->setChecked(true);

    keep_position = new QCheckBox(tr("Keep Camera Pos"));
    keep_cull_frustum = new QCheckBox(tr("Keep Cull Frustum"));
    keep_cull_position = new QCheckBox(tr("Keep Cull Pos"));

    nearest = new QCheckBox(tr("Nearest Filter"));

    bspx_decoupled_lm = new QCheckBox(tr("BSPX: Decoupled Lightmap"));
    bspx_decoupled_lm->setChecked(true);

    bspx_normals = new QCheckBox(tr("BSPX: Face Normals"));
    bspx_normals->setChecked(true);

    draw_opaque = new QCheckBox(tr("Draw Translucency as Opaque"));
    show_bmodels = new QCheckBox(tr("Show Bmodels"));
    show_bmodels->setChecked(true);

    formLayout->addRow(tr("common"), common_options);
    formLayout->addRow(tr("qbsp"), qbsp_options);
    formLayout->addRow(vis_checkbox, vis_options);
    formLayout->addRow(light_checkbox, light_options);
    formLayout->addRow(reload_button);
    formLayout->addRow(rendermode_group);
    formLayout->addRow(draw_portals);
    formLayout->addRow(draw_leak);
    formLayout->addRow(draw_lightgrid);
    formLayout->addRow(show_click_ray);
    formLayout->addRow(show_hud);
    formLayout->addRow(show_tris);
    formLayout->addRow(show_tris_seethrough);
    formLayout->addRow(vis_culling);
    formLayout->addRow(keep_cull_position);
    formLayout->addRow(keep_cull_frustum);
    formLayout->addRow(keep_position);
    formLayout->addRow(nearest);
    formLayout->addRow(bspx_decoupled_lm);
    formLayout->addRow(bspx_normals);
    formLayout->addRow(draw_opaque);
    formLayout->addRow(show_bmodels);

    lightstyles = new QVBoxLayout();

    auto *lightstyles_group = new QGroupBox(tr("Lightstyles"));
    lightstyles_group->setLayout(lightstyles);

    formLayout->addRow(lightstyles_group);

    // brightness slider
    auto *brightnessSlider = new QSlider(Qt::Horizontal);
    brightnessSlider->setMinimum(-100);
    brightnessSlider->setMaximum(100);
    brightnessSlider->setSliderPosition(0);

    auto *brightnessLabel = new QLabel(QString("0.0"));
    auto *brightnessLayout = new QHBoxLayout();
    auto *brightnessReset = new QPushButton(tr("Reset"));
    brightnessLayout->addWidget(brightnessSlider, 1);
    brightnessLayout->addWidget(brightnessLabel, 0);
    brightnessLayout->addWidget(brightnessReset, 0);
    formLayout->addRow(tr("Exposure"), brightnessLayout);

    // wrap formLayout in a scroll area
    auto *form = new QWidget();
    form->setLayout(formLayout);

    auto *scrollArea = new QScrollArea();
    scrollArea->setWidgetResizable(true);
    scrollArea->setWidget(form);
    scrollArea->setBackgroundRole(QPalette::Window);
    scrollArea->setFrameShadow(QFrame::Plain);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);

    // finish dock setup
    dock->setWidget(scrollArea);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
    viewMenu->addAction(dock->toggleViewAction());

    // load state persisted in settings
    QSettings s;
    common_options->setText(s.value("common_options").toString());
    qbsp_options->setText(s.value("qbsp_options").toString());
    vis_checkbox->setChecked(s.value("vis_enabled").toBool());
    light_checkbox->setChecked(s.value("light_enabled", true).toBool());
    keep_cull_position->setEnabled(vis_checkbox->isChecked());
    keep_cull_frustum->setEnabled(keep_cull_position->isChecked());
    keep_cull_frustum->setChecked(true);
    vis_options->setText(s.value("vis_options").toString());
    light_options->setText(s.value("light_options").toString());
    nearest->setChecked(s.value("nearest").toBool());
    if (nearest->isChecked()) {
        glView->setMagFilter(QOpenGLTexture::Nearest);
    }
    show_click_ray->setChecked(s.value("show_click_ray").toBool());
    glView->setShowClickRay(show_click_ray->isChecked());
    show_hud->setChecked(s.value("show_hud", true).toBool());
    glView->setShowHud(show_hud->isChecked());
    draw_portals->setChecked(s.value("draw_portals").toBool());
    glView->setDrawPortals(draw_portals->isChecked());
    draw_leak->setChecked(s.value("draw_leak").toBool());
    glView->setDrawLeak(draw_leak->isChecked());
    draw_lightgrid->setChecked(s.value("draw_lightgrid").toBool());
    glView->setDrawLightgrid(draw_lightgrid->isChecked());
    show_tris->setChecked(s.value("show_tris").toBool());
    glView->setShowTris(show_tris->isChecked());
    show_tris_seethrough->setChecked(s.value("show_tris_seethrough").toBool());
    glView->setShowTrisSeeThrough(show_tris_seethrough->isChecked());
    vis_culling->setChecked(s.value("vis_culling", true).toBool());
    glView->setVisCulling(vis_culling->isChecked());
    keep_cull_position->setChecked(s.value("keep_cull_position").toBool());
    keep_cull_frustum->setChecked(s.value("keep_cull_frustum", true).toBool());
    keep_position->setChecked(s.value("keep_position").toBool());
    draw_opaque->setChecked(s.value("draw_opaque").toBool());
    glView->setDrawTranslucencyAsOpaque(draw_opaque->isChecked());
    show_bmodels->setChecked(s.value("show_bmodels", true).toBool());
    glView->setShowBmodels(show_bmodels->isChecked());
    bspx_decoupled_lm->setChecked(s.value("bspx_decoupled_lm", true).toBool());
    bspx_normals->setChecked(s.value("bspx_normals", true).toBool());
    keep_cull_position->setEnabled(vis_culling->isChecked());
    keep_cull_frustum->setEnabled(keep_cull_position->isChecked());
    glView->setKeepCullOrigin(keep_cull_position->isChecked());
    glView->setKeepCullFrustum(keep_cull_frustum->isChecked());
    glView->setKeepOrigin(keep_position->isChecked());

    const QString render_mode = s.value("render_mode", "lightmapped").toString();
    if (render_mode == "lightmap_only") {
        render_lightmap_only->setChecked(true);
    } else if (render_mode == "fullbright") {
        render_fullbright->setChecked(true);
    } else if (render_mode == "normals") {
        render_normals->setChecked(true);
    } else if (render_mode == "flat") {
        render_flat->setChecked(true);
    } else if (render_mode == "hull0") {
        render_hull0->setChecked(true);
    } else if (render_mode == "hull1") {
        render_hull1->setChecked(true);
    } else if (render_mode == "hull2") {
        render_hull2->setChecked(true);
    } else if (render_mode == "hull3") {
        render_hull3->setChecked(true);
    } else if (render_mode == "hull4") {
        render_hull4->setChecked(true);
    } else if (render_mode == "hull5") {
        render_hull5->setChecked(true);
    } else {
        render_lightmapped->setChecked(true);
    }

    // setup event handlers

    connect(reload_button, &QAbstractButton::clicked, this, &MainWindow::reload);
    connect(render_lightmap_only, &QAbstractButton::toggled, this,
        [this](bool checked) { glView->setLighmapOnly(checked); });
    connect(render_fullbright, &QAbstractButton::toggled, this,
        [this](bool checked) { glView->setFullbright(checked); });
    connect(render_normals, &QAbstractButton::toggled, this,
        [this](bool checked) { glView->setDrawNormals(checked); });
    connect(show_tris, &QAbstractButton::toggled, this, [this](bool checked) { glView->setShowTris(checked); });
    connect(show_tris_seethrough, &QAbstractButton::toggled, this,
        [this](bool checked) { glView->setShowTrisSeeThrough(checked); });
    connect(vis_culling, &QAbstractButton::toggled, this, [this](bool checked) {
        glView->setVisCulling(checked);
        keep_cull_position->setEnabled(checked);
        keep_cull_frustum->setEnabled(keep_cull_position->isEnabled());
    });
    connect(keep_cull_position, &QAbstractButton::toggled, this, [this](bool checked) {
        glView->setKeepCullOrigin(checked);
        keep_cull_frustum->setEnabled(checked);
    });
    connect(keep_cull_frustum, &QAbstractButton::toggled, this,
        [this](bool checked) { glView->setKeepCullFrustum(checked); });
    connect(render_flat, &QAbstractButton::toggled, this, [this](bool checked) { glView->setDrawFlat(checked); });
    connect(render_hull0, &QAbstractButton::toggled, this,
        [this](bool checked) { glView->setDrawLeafs(checked ? std::optional<int>{0} : std::nullopt); });
    connect(render_hull1, &QAbstractButton::toggled, this,
        [this](bool checked) { glView->setDrawLeafs(checked ? std::optional<int>{1} : std::nullopt); });
    connect(render_hull2, &QAbstractButton::toggled, this,
        [this](bool checked) { glView->setDrawLeafs(checked ? std::optional<int>{2} : std::nullopt); });
    connect(render_hull3, &QAbstractButton::toggled, this,
        [this](bool checked) { glView->setDrawLeafs(checked ? std::optional<int>{3} : std::nullopt); });
    connect(render_hull4, &QAbstractButton::toggled, this,
        [this](bool checked) { glView->setDrawLeafs(checked ? std::optional<int>{4} : std::nullopt); });
    connect(render_hull5, &QAbstractButton::toggled, this,
        [this](bool checked) { glView->setDrawLeafs(checked ? std::optional<int>{5} : std::nullopt); });
    connect(draw_portals, &QAbstractButton::toggled, this, [this](bool checked) { glView->setDrawPortals(checked); });
    connect(draw_leak, &QAbstractButton::toggled, this, [this](bool checked) { glView->setDrawLeak(checked); });
    connect(draw_lightgrid, &QAbstractButton::toggled, this,
        [this](bool checked) { glView->setDrawLightgrid(checked); });
    connect(show_click_ray, &QAbstractButton::toggled, this, [this](bool checked) { glView->setShowClickRay(checked); });
    connect(show_hud, &QAbstractButton::toggled, this, [this](bool checked) { glView->setShowHud(checked); });
    connect(keep_position, &QAbstractButton::toggled, this, [this](bool checked) { glView->setKeepOrigin(checked); });
    connect(nearest, &QAbstractButton::toggled, this,
        [this](bool checked) { glView->setMagFilter(checked ? QOpenGLTexture::Nearest : QOpenGLTexture::Linear); });
    connect(draw_opaque, &QAbstractButton::toggled, this,
        [this](bool checked) { glView->setDrawTranslucencyAsOpaque(checked); });
    connect(glView, &GLView::cameraMoved, this, &MainWindow::displayCameraPositionInfo);
    connect(glView, &GLView::selectedFaceChanged, this, &MainWindow::displaySelectedFaceInfo);
    connect(show_bmodels, &QAbstractButton::toggled, this, [this](bool checked) { glView->setShowBmodels(checked); });
    connect(brightnessSlider, &QAbstractSlider::valueChanged, this, [this, brightnessLabel](int value) {
        float brightness = value / 10.0f;
        brightnessLabel->setText(QString::fromLatin1("%1").arg(brightness, 0, 'f', 2));
        glView->setBrightness(brightness);
    });
    connect(brightnessReset, &QAbstractButton::pressed, this,
        [this, brightnessSlider]() { brightnessSlider->setValue(0); });

    // set up load timer
    m_fileReloadTimer = std::make_unique<QTimer>();

    m_fileReloadTimer->setSingleShot(true);
    m_fileReloadTimer->connect(m_fileReloadTimer.get(), &QTimer::timeout, this, &MainWindow::fileReloadTimerExpired);

    setupSettingsMenu();
}

void MainWindow::createStatsSidebar()
{
    QDockWidget *dock = new QDockWidget(tr("Stats"), this);

    stats_panel = new StatsPanel();

    // finish dock setup
    dock->setWidget(stats_panel);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    viewMenu->addAction(dock->toggleViewAction());
}

void MainWindow::logWidgetSetText(ETLogTab tab, const std::string &str)
{
    m_outputLogWidget->setTabText((int32_t)tab, str.c_str());
}

void MainWindow::hub_percent_callback(std::optional<uint32_t> percent, std::optional<duration> elapsed)
{
    int32_t tabIndex = (int32_t)m_activeLogTab;

    if (elapsed.has_value()) {
        hub_log_callback(
            logging::flag::PROGRESS, fmt::format("finished in: {:.3}\n", elapsed.value()).c_str());
        QMetaObject::invokeMethod(
            this, std::bind(&MainWindow::logWidgetSetText, this, m_activeLogTab, ETLogWidget::logTabNames[tabIndex]));
    } else {
        if (percent.has_value()) {
            QMetaObject::invokeMethod(
                this, std::bind(&MainWindow::logWidgetSetText, this, m_activeLogTab,
                          fmt::format("{} [{:>3}%]", ETLogWidget::logTabNames[tabIndex], percent.value())));
        } else {
            QMetaObject::invokeMethod(this, std::bind(&MainWindow::logWidgetSetText, this, m_activeLogTab,
                                                fmt::format("{} (...)", ETLogWidget::logTabNames[tabIndex])));
        }
    }
}

void MainWindow::hub_log_callback(logging::flag flags, const char *str)
{
    if (bitflags(flags) & logging::flag::PERCENT)
        return;

    if (QApplication::instance()->thread() != QThread::currentThread()) {
        QMetaObject::invokeMethod(this,
            std::bind([this, flags](const std::string &s) -> void { hub_log_callback(flags, s.c_str()); },
                std::string(str)));
        return;
    }

    auto *textEdit = m_outputLogWidget->textEdit(m_activeLogTab);
    const bool atBottom = textEdit->verticalScrollBar()->value() == textEdit->verticalScrollBar()->maximum();
    QTextDocument *doc = textEdit->document();
    QTextCursor cursor(doc);
    cursor.movePosition(QTextCursor::End);
    cursor.beginEditBlock();
    cursor.insertBlock();
    cursor.insertHtml(QString::asprintf("%s\n", str));
    cursor.endEditBlock();

    // scroll scrollarea to bottom if it was at bottom when we started
    //(we don't want to force scrolling to bottom if user is looking at a
    // higher position)
    if (atBottom) {
        QScrollBar *bar = textEdit->verticalScrollBar();
        bar->setValue(bar->maximum());
    }
}

void MainWindow::createOutputLog()
{
    QDockWidget *dock = new QDockWidget(tr("Tool Logs"), this);

    m_outputLogWidget = new ETLogWidget();

    // finish dock widget setup
    dock->setWidget(m_outputLogWidget);

    addDockWidget(Qt::BottomDockWidgetArea, dock);
    viewMenu->addAction(dock->toggleViewAction());

    logging::set_print_callback(
        std::bind(&MainWindow::hub_log_callback, this, std::placeholders::_1, std::placeholders::_2));
    logging::set_percent_callback(
        std::bind(&MainWindow::hub_percent_callback, this, std::placeholders::_1, std::placeholders::_2));
}

void MainWindow::createStatusBar()
{
    m_cameraStatus = new QLabel();
    m_selectionStatus = new QLabel();
    statusBar()->addWidget(m_cameraStatus);
    statusBar()->addPermanentWidget(m_selectionStatus);
}

void MainWindow::setupSettingsMenu()
{
    if (!settingsMenu) {
        return;
    }

    settingsMenu->clear();

    auto addToggle = [this](QMenu *menu, const QString &label, QCheckBox *box, const QKeySequence &shortcut = {}) {
        if (!box)
            return static_cast<QAction *>(nullptr);
        QAction *action = menu->addAction(label);
        action->setCheckable(true);
        action->setChecked(box->isChecked());
        if (!shortcut.isEmpty()) {
            action->setShortcut(shortcut);
        }
        connect(action, &QAction::toggled, box, &QCheckBox::setChecked);
        connect(box, &QCheckBox::toggled, action, &QAction::setChecked);
        return action;
    };

    QMenu *buildMenu = settingsMenu->addMenu(tr("Build Options"));
    addToggle(buildMenu, tr("Run vis"), vis_checkbox);
    addToggle(buildMenu, tr("Run light"), light_checkbox);

    QMenu *renderMenu = settingsMenu->addMenu(tr("Render Mode"));
    QActionGroup *renderGroup = new QActionGroup(renderMenu);
    renderGroup->setExclusive(true);

    auto addRender = [&](const QString &label, QRadioButton *button, const QKeySequence &shortcut = {}) {
        if (!button)
            return;
        QAction *action = renderMenu->addAction(label);
        action->setCheckable(true);
        action->setChecked(button->isChecked());
        if (!shortcut.isEmpty()) {
            action->setShortcut(shortcut);
        }
        action->setActionGroup(renderGroup);
        connect(action, &QAction::triggered, button, &QRadioButton::setChecked);
        connect(button, &QRadioButton::toggled, action, &QAction::setChecked);
    };

    addRender(tr("Lightmapped"), render_lightmapped, QKeySequence("Alt+1"));
    addRender(tr("Lightmap Only"), render_lightmap_only, QKeySequence("Alt+2"));
    addRender(tr("Fullbright"), render_fullbright, QKeySequence("Alt+3"));
    addRender(tr("Normals"), render_normals, QKeySequence("Alt+4"));
    addRender(tr("Flat Shading"), render_flat, QKeySequence("Alt+5"));
    renderMenu->addSeparator();
    addRender(tr("Leafs"), render_hull0, QKeySequence("Alt+6"));
    addRender(tr("Hull 1"), render_hull1);
    addRender(tr("Hull 2"), render_hull2);
    addRender(tr("Hull 3"), render_hull3);
    addRender(tr("Hull 4"), render_hull4);
    addRender(tr("Hull 5"), render_hull5);

    QMenu *overlayMenu = settingsMenu->addMenu(tr("Overlays"));
    addToggle(overlayMenu, tr("HUD Overlay"), show_hud, QKeySequence("H"));
    addToggle(overlayMenu, tr("Selection Ray"), show_click_ray);
    addToggle(overlayMenu, tr("Draw Portals (PRT)"), draw_portals);
    addToggle(overlayMenu, tr("Draw Leak (PTS/LIN)"), draw_leak);
    addToggle(overlayMenu, tr("Draw Lightgrid"), draw_lightgrid);
    addToggle(overlayMenu, tr("Show Tris"), show_tris);
    addToggle(overlayMenu, tr("Show Tris (See Through)"), show_tris_seethrough);

    QMenu *visibilityMenu = settingsMenu->addMenu(tr("Visibility"));
    addToggle(visibilityMenu, tr("Vis Culling"), vis_culling);
    addToggle(visibilityMenu, tr("Keep Cull Pos"), keep_cull_position);
    addToggle(visibilityMenu, tr("Keep Cull Frustum"), keep_cull_frustum);
    addToggle(visibilityMenu, tr("Keep Camera Pos"), keep_position);

    QMenu *materialsMenu = settingsMenu->addMenu(tr("Materials"));
    addToggle(materialsMenu, tr("Nearest Filter"), nearest);
    addToggle(materialsMenu, tr("Draw Translucency as Opaque"), draw_opaque);
    addToggle(materialsMenu, tr("Show Bmodels"), show_bmodels);
    addToggle(materialsMenu, tr("BSPX: Decoupled Lightmap"), bspx_decoupled_lm);
    addToggle(materialsMenu, tr("BSPX: Face Normals"), bspx_normals);

    settingsMenu->addSeparator();
    settingsMenu->addAction(tr("Reset Settings..."), this, &MainWindow::resetSettings);
}

void MainWindow::resetSettings()
{
    auto result = QMessageBox::question(
        this, tr("Reset Settings"), tr("Reset all saved settings to defaults?"), QMessageBox::Yes | QMessageBox::No);
    if (result != QMessageBox::Yes) {
        return;
    }

    QSettings s;
    s.clear();
    s.sync();

    if (common_options)
        common_options->clear();
    if (qbsp_options)
        qbsp_options->clear();
    if (vis_options)
        vis_options->clear();
    if (light_options)
        light_options->clear();

    if (render_lightmapped)
        render_lightmapped->setChecked(true);
    if (render_lightmap_only)
        render_lightmap_only->setChecked(false);
    if (render_fullbright)
        render_fullbright->setChecked(false);
    if (render_normals)
        render_normals->setChecked(false);
    if (render_flat)
        render_flat->setChecked(false);
    if (render_hull0)
        render_hull0->setChecked(false);
    if (render_hull1)
        render_hull1->setChecked(false);
    if (render_hull2)
        render_hull2->setChecked(false);
    if (render_hull3)
        render_hull3->setChecked(false);
    if (render_hull4)
        render_hull4->setChecked(false);
    if (render_hull5)
        render_hull5->setChecked(false);

    if (vis_checkbox)
        vis_checkbox->setChecked(false);
    if (light_checkbox)
        light_checkbox->setChecked(true);
    if (nearest)
        nearest->setChecked(false);
    if (bspx_decoupled_lm)
        bspx_decoupled_lm->setChecked(true);
    if (bspx_normals)
        bspx_normals->setChecked(true);
    if (show_click_ray)
        show_click_ray->setChecked(false);
    if (show_hud)
        show_hud->setChecked(true);
    if (draw_portals)
        draw_portals->setChecked(false);
    if (draw_leak)
        draw_leak->setChecked(false);
    if (draw_lightgrid)
        draw_lightgrid->setChecked(false);
    if (show_tris)
        show_tris->setChecked(false);
    if (show_tris_seethrough)
        show_tris_seethrough->setChecked(false);
    if (vis_culling)
        vis_culling->setChecked(true);
    if (keep_cull_position)
        keep_cull_position->setChecked(false);
    if (keep_cull_frustum)
        keep_cull_frustum->setChecked(true);
    if (keep_position)
        keep_position->setChecked(false);
    if (draw_opaque)
        draw_opaque->setChecked(false);
    if (show_bmodels)
        show_bmodels->setChecked(true);

    resize(1024, 768);
    setupSettingsMenu();
}

/**
 * Precondition: openRecentMenu is created.
 *
 * Clears and rebuilds the menu given the list of files that should be displayed in it.
 */
void MainWindow::updateRecentsSubmenu(const QStringList &recents)
{
    openRecentMenu->clear();

    for (const QString &recent : recents) {
        auto *action = openRecentMenu->addAction(recent);
        connect(action, &QAction::triggered, this, [this, recent]() { loadFile(recent); });
    }

    openRecentMenu->addSeparator();
    openRecentMenu->addAction(tr("Clear Recents"), this, [this]() {
        ClearRecents();
        this->updateRecentsSubmenu(GetRecents());
    });
}

void MainWindow::updateCameraBookmarksSubmenu()
{
    cameraBookmarksMenu->clear();

    cameraBookmarksMenu->addAction(tr("Bookmark Current Camera Position"), this, [this]() {
        camera_bookmark_t b{.origin = this->glView->cameraPosition(), .forward = this->glView->cameraForward()};
        AddCameraBookmark(b);
        this->updateCameraBookmarksSubmenu();
    });
    cameraBookmarksMenu->addSeparator();

    auto bookmarks = GetCameraBookmarks();
    for (const auto &bookmark : bookmarks) {
        auto *action = cameraBookmarksMenu->addAction(CameraBookmarkToQString(bookmark));
        connect(action, &QAction::triggered, this,
            [this, bookmark]() { this->glView->setCamera(bookmark.origin, bookmark.forward); });
    }

    cameraBookmarksMenu->addSeparator();
    cameraBookmarksMenu->addAction(tr("Clear Camera Bookmarks"), this, [this]() {
        ClearCameraBookmarks();
        this->updateCameraBookmarksSubmenu();
    });
}

MainWindow::~MainWindow() { }

static void OpenHelpFile(const QString &file)
{
    QString fileString = QCoreApplication::applicationDirPath() + QStringLiteral("/doc/") + file;
    QUrl fileUrl = QUrl::fromLocalFile(fileString);

    QDesktopServices::openUrl(fileUrl);
};

void MainWindow::setupMenu()
{
    auto *menu = menuBar()->addMenu(tr("&File"));

    auto *open = menu->addAction(tr("&Open"), this, &MainWindow::fileOpen);
    open->setShortcut(QKeySequence::Open);

    openRecentMenu = menu->addMenu(tr("Open &Recent"));
    updateRecentsSubmenu(GetRecents());

    menu->addSeparator();

    auto *reloadAction = menu->addAction(tr("&Reload"), this, &MainWindow::reload);
    reloadAction->setShortcut(QKeySequence::Refresh);

    menu->addAction(tr("Save Screenshot..."), this, &MainWindow::takeScreenshot);

    menu->addSeparator();

    auto *exit = menu->addAction(tr("E&xit"), this, &QWidget::close);
    exit->setShortcut(QKeySequence::Quit);

    // edit menu

    auto *editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(tr("&Copy Camera Position"), this, [this]() {
        qvec3f pos = this->glView->cameraPosition();

        std::string cpp_str = fmt::format("{}", pos);

        QClipboard *clipboard = QGuiApplication::clipboard();
        clipboard->setText(QString::fromStdString(cpp_str));
    });

    // view menu

    viewMenu = menuBar()->addMenu(tr("&View"));
    cameraBookmarksMenu = viewMenu->addMenu(tr("Camera Bookmarks"));
    updateCameraBookmarksSubmenu();
    viewMenu->addAction(tr("&Move camera to..."), this, [this]() {
        bool ok = false;
        QString text = QInputDialog::getText(
            this, tr("Move camera to"), tr("Enter X Y Z coords, space-separated"), QLineEdit::Normal, QString(), &ok);

        QStringList comps = text.split(QString::fromLatin1(" "), Qt::SkipEmptyParts);

        if (comps.length() >= 3) {
            this->glView->setCamera(qvec3d{comps[0].toDouble(), comps[1].toDouble(), comps[2].toDouble()});
        }
    });

    auto *selectionMenu = viewMenu->addMenu(tr("Selection"));
    m_actionCopySelectedFace = selectionMenu->addAction(tr("Copy Selected Face Info"), this, [this]() {
        if (m_selectedFaceInfo.empty())
            return;
        QClipboard *clipboard = QGuiApplication::clipboard();
        clipboard->setText(QString::fromStdString(m_selectedFaceInfo));
    });
    m_actionCopySelectedTexture = selectionMenu->addAction(tr("Copy Selected Texture"), this, [this]() {
        if (m_selectedTexture.empty())
            return;
        QClipboard *clipboard = QGuiApplication::clipboard();
        clipboard->setText(QString::fromStdString(m_selectedTexture));
    });
    selectionMenu->addSeparator();
    m_actionFocusSelectedFace = selectionMenu->addAction(tr("Focus Camera on Selected Face"), this, [this]() {
        const auto *bsp = std::get_if<mbsp_t>(&m_bspdata.bsp);
        if (!bsp || m_selectedFaceIndex < 0 || m_selectedFaceIndex >= static_cast<int>(bsp->dfaces.size()))
            return;
        const auto &face = bsp->dfaces.at(m_selectedFaceIndex);
        const qvec3f centroid = Face_Centroid(bsp, &face);
        const qvec3d normal = Face_Normal(bsp, &face);
        const qvec3d camera_pos = qvec3d{centroid[0], centroid[1], centroid[2]} + normal * 32.0;
        const qvec3d forward = normal * -1.0;
        glView->setCamera(camera_pos, forward);
    });
    m_actionFocusSelectedLeaf = selectionMenu->addAction(tr("Focus Camera on Selected Leaf"), this, [this]() {
        const auto *bsp = std::get_if<mbsp_t>(&m_bspdata.bsp);
        if (!bsp || m_selectedFaceIndex < 0 || m_selectedFaceIndex >= static_cast<int>(bsp->dfaces.size()))
            return;
        const auto &face = bsp->dfaces.at(m_selectedFaceIndex);
        const qvec3f centroid = Face_Centroid(bsp, &face);
        const qvec3d centroid_d{centroid[0], centroid[1], centroid[2]};
        const mleaf_t *leaf = BSP_FindLeafAtPoint(bsp, &bsp->dmodels[0], centroid_d);
        if (!leaf)
            return;
        const qvec3f leaf_center = (leaf->mins + leaf->maxs) * 0.5f;
        glView->setCamera(qvec3d{leaf_center[0], leaf_center[1], leaf_center[2]});
    });
    selectionMenu->addSeparator();
    m_actionSoloSelectedTexture = selectionMenu->addAction(tr("Solo Selected Texture"));
    m_actionSoloSelectedTexture->setCheckable(true);
    m_actionSoloSelectedTexture->setShortcut(QKeySequence("T"));
    connect(m_actionSoloSelectedTexture, &QAction::toggled, this, [this](bool checked) {
        if (!checked) {
            glView->setTextureFilter(std::nullopt);
            return;
        }
        if (m_selectedTexture.empty()) {
            m_actionSoloSelectedTexture->blockSignals(true);
            m_actionSoloSelectedTexture->setChecked(false);
            m_actionSoloSelectedTexture->blockSignals(false);
            glView->setTextureFilter(std::nullopt);
            return;
        }
        glView->setTextureFilter(std::optional<std::string>{m_selectedTexture});
    });
    m_actionClearSelection = selectionMenu->addAction(tr("Clear Selection"), this, [this]() { glView->clearSelection(); });

    if (m_actionCopySelectedFace)
        m_actionCopySelectedFace->setEnabled(false);
    if (m_actionCopySelectedTexture)
        m_actionCopySelectedTexture->setEnabled(false);
    if (m_actionFocusSelectedFace)
        m_actionFocusSelectedFace->setEnabled(false);
    if (m_actionFocusSelectedLeaf)
        m_actionFocusSelectedLeaf->setEnabled(false);
    if (m_actionSoloSelectedTexture)
        m_actionSoloSelectedTexture->setEnabled(false);
    if (m_actionClearSelection)
        m_actionClearSelection->setEnabled(false);

    settingsMenu = menuBar()->addMenu(tr("&Settings"));

    // help menu

    auto *helpMenu = menuBar()->addMenu(tr("&Help"));

    helpMenu->addAction(tr("&Hub Documentation"), this, []() { OpenHelpFile("hub.html"); });
    helpMenu->addAction(tr("&About"), this,
        [this]() { QMessageBox::about(this, tr("About vmt-hub"), tr("VibeyMapTools " VIBEYMAPTOOLS_VERSION)); });
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    auto urls = event->mimeData()->urls();
    if (!urls.empty()) {
        const QUrl &url = urls[0];
        if (url.isLocalFile()) {
            loadFile(url.toLocalFile());

            event->acceptProposedAction();
        }
    }
}

void MainWindow::showEvent(QShowEvent *event)
{
    // FIXME: move command-line parsing somewhere else?
    // FIXME: support more command-line options?
    auto args = QCoreApplication::arguments();
    if (args.size() == 2) {
        QTimer::singleShot(0, this, [this, args] { loadFile(args.at(1)); });
    }
}

void MainWindow::fileOpen()
{
    // open the file browser in the directory containing the currently open file, if there is one
    QString currentDir;
    if (!m_mapFile.isEmpty()) {
        currentDir = QFileInfo(m_mapFile).absolutePath();
    }

    QString fileName = QFileDialog::getOpenFileName(this, tr("Open File"), currentDir, tr("Map (*.map);; BSP (*.bsp)"));

    if (!fileName.isEmpty())
        loadFile(fileName);
}

void MainWindow::takeScreenshot()
{
    QString fileName = QFileDialog::getSaveFileName(this, tr("Save Screenshot"), "", tr("PNG (*.png)"));

    if (!fileName.isEmpty())
        glView->takeScreenshot(fileName, 3840, 2160);
}

void MainWindow::fileReloadTimerExpired()
{
    qint64 currentSize = QFileInfo(m_mapFile).size();

    // it was rewritten...
    if (currentSize != m_fileSize) {
        qDebug() << "size changed since last write, restarting timer";
        m_fileReloadTimer->start(150);
        return;
    }

    // good to go? maybe?
    qDebug() << "size not changed, good to go";
    loadFileInternal(m_mapFile, true);

    m_fileSize = -1;
}

void MainWindow::loadFile(const QString &file)
{
    qDebug() << "load " << file;

    // update recents
    updateRecentsSubmenu(AddRecent(file));

    m_mapFile = file;

    if (m_watcher) {
        delete m_watcher;
    }
    m_watcher = new QFileSystemWatcher(this);
    m_fileSize = -1;

    // start watching it and related outputs
    qDebug() << "adding path: " << m_watcher->addPath(file);

    auto addWatchPath = [this](const QString &path) {
        if (QFileInfo::exists(path)) {
            qDebug() << "adding path: " << m_watcher->addPath(path);
        } else {
            qDebug() << "file not found: " << path;
        }
    };

    QFileInfo fileInfo(file);
    const QString basePath = fileInfo.path() + "/" + fileInfo.completeBaseName();
    addWatchPath(basePath + ".lit");
    addWatchPath(basePath + ".prt");
    addWatchPath(basePath + ".pts");
    addWatchPath(basePath + ".lin");
    if (fileInfo.suffix().compare("map", Qt::CaseInsensitive) == 0) {
        addWatchPath(basePath + ".bsp");
    }

    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [&](const QString &path) {
        qDebug() << "got change notif for " << m_mapFile;

        // check current files' size
        m_fileSize = QFileInfo(m_mapFile).size();

        // start timer
        m_fileReloadTimer->start(150);
    });

    loadFileInternal(file, false);
}

std::filesystem::path MakeFSPath(const QString &string)
{
    return std::filesystem::path{string.toStdU16String()};
}

bspdata_t MainWindow::QbspVisLight_Common(const std::filesystem::path &name, std::vector<std::string> extra_common_args,
    std::vector<std::string> extra_qbsp_args, std::vector<std::string> extra_vis_args,
    std::vector<std::string> extra_light_args, bool run_vis, bool run_light)
{
    auto resetActiveTabText = [&]() {
        QMetaObject::invokeMethod(this, std::bind(&MainWindow::logWidgetSetText, this, m_activeLogTab,
                                            ETLogWidget::logTabNames[(int32_t)m_activeLogTab]));
    };

    auto bsp_path = name;
    bsp_path.replace_extension(".bsp");

    std::vector<std::string> args{
        "", // the exe path, which we're ignoring in this case
    };
    for (auto &extra : extra_common_args) {
        args.push_back(extra);
    }
    for (auto &extra : extra_qbsp_args) {
        args.push_back(extra);
    }
    args.push_back(name.string());

    // run qbsp
    m_activeLogTab = ETLogTab::TAB_BSP;

    InitQBSP(args);
    ProcessFile();

    resetActiveTabText();

    // run vis
    if (run_vis) {
        m_activeLogTab = ETLogTab::TAB_VIS;
        std::vector<std::string> vis_args{
            "", // the exe path, which we're ignoring in this case
        };
        for (auto &extra : extra_common_args) {
            vis_args.push_back(extra);
        }
        for (auto &extra : extra_vis_args) {
            vis_args.push_back(extra);
        }
        vis_args.push_back(name.string());
        vis_main(vis_args);
    }

    resetActiveTabText();

    // run light
    if (run_light) {
        m_activeLogTab = ETLogTab::TAB_LIGHT;
        std::vector<std::string> light_args{
            "", // the exe path, which we're ignoring in this case
        };
        for (auto &extra : extra_common_args) {
            light_args.push_back(extra);
        }
        for (auto &arg : extra_light_args) {
            light_args.push_back(arg);
        }
        light_args.push_back(name.string());

        light_main(light_args);
    }

    resetActiveTabText();

    m_activeLogTab = ETLogTab::TAB_hub;

    // serialize obj
    {
        bspdata_t bspdata;
        LoadBSPFile(bsp_path, &bspdata);

        ConvertBSPFormat(&bspdata, &bspver_generic);

        return bspdata;
    }
}

static std::vector<std::string> ParseArgs(const QLineEdit *line_edit)
{
    std::vector<std::string> result;

    QString text = line_edit->text().trimmed();
    if (text.isEmpty())
        return result;

    bool inside_quotes = false;
    for (const auto &str : text.split('"')) {
        qDebug() << "got token " << str << " inside quote? " << inside_quotes;

        if (inside_quotes) {
            result.push_back(str.toStdString());
        } else {
            // split by spaces
            for (const auto &str2 : str.split(' ', Qt::SkipEmptyParts)) {
                qDebug() << "got sub token " << str2;
                result.push_back(str2.toStdString());
            }
        }

        inside_quotes = !inside_quotes;
    }

    return result;
}

void MainWindow::reload()
{
    if (m_mapFile.isEmpty())
        return;

    loadFileInternal(m_mapFile, true);
}

class QLightStyleSlider : public QFrame
{
public:
    int32_t style_id;

    QLightStyleSlider(int32_t style_id, GLView *glView)
        : QFrame(),
          style_id(style_id),
          glView(glView)
    {
        auto *style_layout = new QHBoxLayout();

        auto *style = new QSpinBox();
        style->setRange(0, 200);
        style->setValue(100);
        style->setSingleStep(10);
        // style->setTickPosition(QSlider::TicksBothSides);
        // style->setTickInterval(50);

        connect(style, QOverload<int>::of(&QSpinBox::valueChanged), this, &QLightStyleSlider::setValue);

        auto *style_label = new QLabel();
        style_label->setText(QString::asprintf("%i", style_id));

        style_layout->addWidget(style_label);
        style_layout->addWidget(style);

        setLayout(style_layout);
        setFrameShadow(QFrame::Plain);
        setFrameShape(QFrame::NoFrame);
    }

private:
    void setValue(int value) { glView->setLightStyleIntensity(style_id, value); }

    GLView *glView;
};

int MainWindow::compileMap(const QString &file, bool is_reload)
{
    fs::path fs_path = MakeFSPath(file);

    m_bspdata = {};
    render_settings.reset();

    try {
        if (fs_path.extension().compare(".bsp") == 0) {

            LoadBSPFile(fs_path, &m_bspdata);

            auto opts = ParseArgs(common_options);

            std::vector<const char *> argPtrs;

            argPtrs.push_back("");

            for (const std::string &arg : opts) {
                argPtrs.push_back(arg.data());
            }

            render_settings.preinitialize(argPtrs.size(), argPtrs.data());
            render_settings.initialize(argPtrs.size() - 1, argPtrs.data() + 1);
            render_settings.postinitialize(argPtrs.size(), argPtrs.data());

            m_bspdata.version->game->init_filesystem(fs_path, render_settings);

            ConvertBSPFormat(&m_bspdata, &bspver_generic);

        } else {
            m_bspdata =
                QbspVisLight_Common(fs_path, ParseArgs(common_options), ParseArgs(qbsp_options), ParseArgs(vis_options),
                    ParseArgs(light_options), vis_checkbox->isChecked(), light_checkbox->isChecked());

            // FIXME: move to a hub_settings
            settings::common_settings settings;

            // FIXME: copy the -path args from light
            settings.paths.copy_from(::light_options.paths);

            m_bspdata.loadversion->game->init_filesystem(file.toStdString(), settings);
        }
    } catch (const settings::parse_exception &p) {
        // FIXME: threading error: don't call Qt widgets code from background thread
        auto *textEdit = m_outputLogWidget->textEdit(m_activeLogTab);
        textEdit->append(QString::fromUtf8(p.what()) + QString::fromLatin1("\n"));
        m_activeLogTab = ETLogTab::TAB_hub;
        return 1;
    } catch (const settings::quit_after_help_exception &p) {
        // FIXME: threading error: don't call Qt widgets code from background thread
        auto *textEdit = m_outputLogWidget->textEdit(m_activeLogTab);
        textEdit->append(QString::fromUtf8(p.what()) + QString::fromLatin1("\n"));
        m_activeLogTab = ETLogTab::TAB_hub;
        return 1;
    } catch (const std::exception &other) {
        // FIXME: threading error: don't call Qt widgets code from background thread
        auto *textEdit = m_outputLogWidget->textEdit(m_activeLogTab);
        textEdit->append(QString::fromUtf8(other.what()) + QString::fromLatin1("\n"));
        m_activeLogTab = ETLogTab::TAB_hub;
        return 1;
    }

    // try to load .lit
    auto lit_path = fs_path;
    lit_path.replace_extension(".lit");

    m_hdr_litdata = {};
    m_litdata = {};

    try {
        if (const auto *bsp = std::get_if<mbsp_t>(&m_bspdata.bsp)) {
            auto lit_variant = LoadLitFile(lit_path, *bsp);

            if (auto *lit1_ptr = std::get_if<lit1_t>(&lit_variant)) {
                m_litdata = std::move(lit1_ptr->rgbdata);
            } else if (auto *lit_hdr_ptr = std::get_if<lit_hdr>(&lit_variant)) {
                m_hdr_litdata = std::move(lit_hdr_ptr->samples);
            }
        }
    } catch (const std::runtime_error &error) {
        logging::print("error loading lit: {}", error.what());
        m_litdata = {};
        m_hdr_litdata = {};
    }

    return 0;
}

void MainWindow::compileThreadExited()
{
    // clear lightstyle widgets
    while (QWidget *w = lightstyles->parentWidget()->findChild<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
        delete w;
    }

    delete m_compileThread;
    m_compileThread = nullptr;

    if (!std::holds_alternative<mbsp_t>(m_bspdata.bsp)) {
        return;
    }
    const auto &bsp = std::get<mbsp_t>(m_bspdata.bsp);

    auto ents = EntData_Parse(bsp);

    // build lightmap atlas
    auto atlas = build_lightmap_atlas(
        bsp, m_bspdata.bspx.entries, m_litdata, m_hdr_litdata, false, bspx_decoupled_lm->isChecked());

    glView->renderBSP(m_mapFile, bsp, m_bspdata.bspx.entries, ents, atlas, render_settings, bspx_normals->isChecked());

    if (!m_fileWasReload && !glView->getKeepOrigin()) {
        for (auto &ent : ents) {
            if (ent.get("classname") == "info_player_start") {
                qvec3f origin;
                ent.get_vector("origin", origin);

                qvec3f angles{};

                if (ent.has("angles")) {
                    ent.get_vector("angles", angles);
                    angles = {angles[1], -angles[0], angles[2]}; // -pitch yaw roll -> yaw pitch roll
                } else if (ent.has("angle"))
                    angles = {ent.get_float("angle"), 0, 0};
                else if (ent.has("mangle"))
                    ent.get_vector("mangle", angles);

                glView->setCamera(origin, qv::vec_from_mangle(angles));
                break;
            }
        }
    }

    // set lightstyle data
    for (auto &style_entry : atlas.style_to_lightmap_atlas) {

        auto *style = new QLightStyleSlider(style_entry.first, glView);
        lightstyles->addWidget(style);
    }

    stats_panel->updateWithBSP(&bsp, m_bspdata.bspx.entries);
}

void MainWindow::loadFileInternal(const QString &file, bool is_reload)
{
    // TODO
    if (m_compileThread)
        return;

    qDebug() << "loadFileInternal " << file;

    // just in case
    m_fileReloadTimer->stop();
    m_fileWasReload = is_reload;

    // persist settings
    QSettings s;
    s.setValue("common_options", common_options->text());
    s.setValue("qbsp_options", qbsp_options->text());
    s.setValue("vis_enabled", vis_checkbox->isChecked());
    s.setValue("light_enabled", light_checkbox->isChecked());
    s.setValue("vis_options", vis_options->text());
    s.setValue("light_options", light_options->text());
    s.setValue("nearest", nearest->isChecked());
    s.setValue("show_click_ray", show_click_ray ? show_click_ray->isChecked() : false);
    s.setValue("show_hud", show_hud ? show_hud->isChecked() : true);
    s.setValue("draw_portals", draw_portals ? draw_portals->isChecked() : false);
    s.setValue("draw_leak", draw_leak ? draw_leak->isChecked() : false);
    s.setValue("draw_lightgrid", draw_lightgrid ? draw_lightgrid->isChecked() : false);
    s.setValue("show_tris", show_tris ? show_tris->isChecked() : false);
    s.setValue("show_tris_seethrough", show_tris_seethrough ? show_tris_seethrough->isChecked() : false);
    s.setValue("vis_culling", vis_culling ? vis_culling->isChecked() : true);
    s.setValue("keep_cull_position", keep_cull_position ? keep_cull_position->isChecked() : false);
    s.setValue("keep_cull_frustum", keep_cull_frustum ? keep_cull_frustum->isChecked() : true);
    s.setValue("keep_position", keep_position ? keep_position->isChecked() : false);
    s.setValue("draw_opaque", draw_opaque ? draw_opaque->isChecked() : false);
    s.setValue("show_bmodels", show_bmodels ? show_bmodels->isChecked() : true);
    s.setValue("bspx_decoupled_lm", bspx_decoupled_lm ? bspx_decoupled_lm->isChecked() : true);
    s.setValue("bspx_normals", bspx_normals ? bspx_normals->isChecked() : true);
    if (render_lightmap_only && render_lightmap_only->isChecked())
        s.setValue("render_mode", "lightmap_only");
    else if (render_fullbright && render_fullbright->isChecked())
        s.setValue("render_mode", "fullbright");
    else if (render_normals && render_normals->isChecked())
        s.setValue("render_mode", "normals");
    else if (render_flat && render_flat->isChecked())
        s.setValue("render_mode", "flat");
    else if (render_hull0 && render_hull0->isChecked())
        s.setValue("render_mode", "hull0");
    else if (render_hull1 && render_hull1->isChecked())
        s.setValue("render_mode", "hull1");
    else if (render_hull2 && render_hull2->isChecked())
        s.setValue("render_mode", "hull2");
    else if (render_hull3 && render_hull3->isChecked())
        s.setValue("render_mode", "hull3");
    else if (render_hull4 && render_hull4->isChecked())
        s.setValue("render_mode", "hull4");
    else if (render_hull5 && render_hull5->isChecked())
        s.setValue("render_mode", "hull5");
    else
        s.setValue("render_mode", "lightmapped");

    // update title bar
    setWindowFilePath(file);
    setWindowTitle(QFileInfo(file).fileName() + " - vmt-hub");

    for (auto &edit : m_outputLogWidget->textEdits()) {
        edit->clear();
    }

    m_compileThread = QThread::create(std::bind(&MainWindow::compileMap, this, file, is_reload));
    connect(m_compileThread, &QThread::finished, this, &MainWindow::compileThreadExited);
    m_compileThread->start();
}

void MainWindow::displayCameraPositionInfo()
{
    const qvec3f point = glView->cameraPosition();
    const qvec3f forward = glView->cameraForward();

    std::string leaf_type;
    int32_t area = -1;
    {
        const auto *bsp = std::get_if<mbsp_t>(&m_bspdata.bsp);
        if (!bsp)
            return;

        const mleaf_t *leaf = BSP_FindLeafAtPoint(bsp, &bsp->dmodels[0], point);
        if (leaf) {
            auto *game = bsp->loadversion->game;
            leaf_type = game->create_contents_from_native(leaf->contents).to_string();

            area = leaf->area;
        }
    }

    std::string cpp_str = fmt::format("pos ({}) forward ({}) contents ({}) area ({})", point, forward, leaf_type, area);

    m_cameraStatus->setText(QString::fromStdString(cpp_str));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    QSettings s;
    s.setValue("window_geometry", saveGeometry());
    s.setValue("window_state", saveState());
    QMainWindow::closeEvent(event);
}

void MainWindow::displaySelectedFaceInfo(int faceIndex)
{
    if (!m_selectionStatus) {
        return;
    }

    const auto *bsp = std::get_if<mbsp_t>(&m_bspdata.bsp);
    if (!bsp || faceIndex < 0 || faceIndex >= static_cast<int>(bsp->dfaces.size())) {
        m_selectionStatus->clear();
        m_selectedFaceIndex = -1;
        m_selectedFaceInfo.clear();
        m_selectedTexture.clear();
        if (m_actionCopySelectedFace)
            m_actionCopySelectedFace->setEnabled(false);
        if (m_actionCopySelectedTexture)
            m_actionCopySelectedTexture->setEnabled(false);
        if (m_actionFocusSelectedFace)
            m_actionFocusSelectedFace->setEnabled(false);
        if (m_actionFocusSelectedLeaf)
            m_actionFocusSelectedLeaf->setEnabled(false);
        if (m_actionSoloSelectedTexture) {
            m_actionSoloSelectedTexture->setChecked(false);
            m_actionSoloSelectedTexture->setEnabled(false);
        }
        if (m_actionClearSelection)
            m_actionClearSelection->setEnabled(false);
        return;
    }

    const auto &face = bsp->dfaces.at(faceIndex);
    const std::string texname = Face_TextureName(bsp, &face);

    std::string style_list;
    for (uint8_t style : face.styles) {
        if (style == INVALID_LIGHTSTYLE_OLD) {
            continue;
        }
        if (!style_list.empty()) {
            style_list += ",";
        }
        style_list += fmt::format("{}", style);
    }
    if (style_list.empty()) {
        style_list = "none";
    }

    int model_index = 0;
    for (int mi = 0; mi < static_cast<int>(bsp->dmodels.size()); ++mi) {
        const auto &model = bsp->dmodels[mi];
        if (faceIndex >= model.firstface && faceIndex < model.firstface + model.numfaces) {
            model_index = mi;
            break;
        }
    }

    const qvec3f centroid = Face_Centroid(bsp, &face);
    const qvec3d centroid_d{centroid[0], centroid[1], centroid[2]};
    const mleaf_t *leaf = BSP_FindLeafAtPoint(bsp, &bsp->dmodels[0], centroid_d);
    int leaf_index = -1;
    int area = -1;
    if (leaf) {
        leaf_index = static_cast<int>(leaf - bsp->dleafs.data());
        area = leaf->area;
    }

    m_selectedFaceIndex = faceIndex;
    m_selectedTexture = texname;
    m_selectedFaceInfo = fmt::format("face {} tex \"{}\" styles [{}] model {} leaf {} area {}", faceIndex, texname,
        style_list, model_index, leaf_index, area);

    m_selectionStatus->setText(QString::fromStdString(m_selectedFaceInfo));

    if (m_actionCopySelectedFace)
        m_actionCopySelectedFace->setEnabled(true);
    if (m_actionCopySelectedTexture)
        m_actionCopySelectedTexture->setEnabled(true);
    if (m_actionFocusSelectedFace)
        m_actionFocusSelectedFace->setEnabled(true);
    if (m_actionFocusSelectedLeaf)
        m_actionFocusSelectedLeaf->setEnabled(true);
    if (m_actionSoloSelectedTexture)
        m_actionSoloSelectedTexture->setEnabled(true);
    if (m_actionClearSelection)
        m_actionClearSelection->setEnabled(true);

    if (m_actionSoloSelectedTexture && m_actionSoloSelectedTexture->isChecked()) {
        glView->setTextureFilter(std::optional<std::string>{m_selectedTexture});
    }
}

