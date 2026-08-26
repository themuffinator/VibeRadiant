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

#pragma once

#include <QMainWindow>
#include <QVBoxLayout>

#include <common/bspfile.hh>
#include <common/settings.hh>

class GLView;
class QFileSystemWatcher;
class QLineEdit;
class QCheckBox;
class QTextEdit;
class StatsPanel;
class QLabel;
class QRadioButton;

enum class ETLogTab
{
    TAB_hub,
    TAB_BSP,
    TAB_VIS,
    TAB_LIGHT,

    TAB_TOTAL
};

class ETLogWidget : public QTabWidget
{
    Q_OBJECT

public:
    static constexpr const char *logTabNames[(size_t)ETLogTab::TAB_TOTAL] = {"hub", "bsp", "vis", "light"};

    explicit ETLogWidget(QWidget *parent = nullptr);
    ~ETLogWidget() { }

    QTextEdit *textEdit(ETLogTab i) { return m_textEdits[(size_t)i]; }
    const QTextEdit *textEdit(ETLogTab i) const { return m_textEdits[(size_t)i]; }

    auto &textEdits() { return m_textEdits; }

private:
    std::array<QTextEdit *, std::size(logTabNames)> m_textEdits;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

private:
    QFileSystemWatcher *m_watcher = nullptr;
    std::unique_ptr<QTimer> m_fileReloadTimer;
    bool m_fileWasReload = false;
    QString m_mapFile;
    bspdata_t m_bspdata;
    std::vector<uint8_t> m_litdata;
    std::vector<uint32_t> m_hdr_litdata;
    settings::common_settings render_settings;
    qint64 m_fileSize = -1;
    ETLogTab m_activeLogTab = ETLogTab::TAB_hub;
    QThread *m_compileThread = nullptr;
    QLabel *m_cameraStatus = nullptr;
    QLabel *m_selectionStatus = nullptr;
    int m_selectedFaceIndex = -1;
    std::string m_selectedFaceInfo;
    std::string m_selectedTexture;
    QAction *m_actionSoloSelectedTexture = nullptr;
    QAction *m_actionCopySelectedFace = nullptr;
    QAction *m_actionCopySelectedTexture = nullptr;
    QAction *m_actionFocusSelectedFace = nullptr;
    QAction *m_actionFocusSelectedLeaf = nullptr;
    QAction *m_actionClearSelection = nullptr;

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    void resetSettings();
    void setupSettingsMenu();
    void createPropertiesSidebar();
    void createOutputLog();
    void createStatsSidebar();
    void hub_log_callback(logging::flag flags, const char *str);
    void hub_percent_callback(std::optional<uint32_t> percent, std::optional<duration> elapsed);
    void logWidgetSetText(ETLogTab tab, const std::string &str);
    void createStatusBar();
    void updateRecentsSubmenu(const QStringList &recents);
    void updateCameraBookmarksSubmenu();
    void setupMenu();
    void fileOpen();
    void takeScreenshot();
    void fileReloadTimerExpired();
    int compileMap(const QString &file, bool is_reload);
    void compileThreadExited();
    bspdata_t QbspVisLight_Common(const fs::path &name, std::vector<std::string> extra_common_args,
        std::vector<std::string> extra_qbsp_args, std::vector<std::string> extra_vis_args,
        std::vector<std::string> extra_light_args, bool run_vis, bool run_light);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void reload();
    void loadFile(const QString &file);
    void loadFileInternal(const QString &file, bool is_reload);
    void displayCameraPositionInfo();
    void displaySelectedFaceInfo(int faceIndex);

private:
    GLView *glView = nullptr;
    StatsPanel *stats_panel = nullptr;

    QCheckBox *vis_checkbox = nullptr;
    QCheckBox *light_checkbox = nullptr;
    QCheckBox *nearest = nullptr;
    QCheckBox *bspx_decoupled_lm = nullptr;
    QCheckBox *bspx_normals = nullptr;
    QCheckBox *show_click_ray = nullptr;
    QCheckBox *show_hud = nullptr;
    QCheckBox *draw_portals = nullptr;
    QCheckBox *draw_leak = nullptr;
    QCheckBox *draw_lightgrid = nullptr;
    QCheckBox *show_tris = nullptr;
    QCheckBox *show_tris_seethrough = nullptr;
    QCheckBox *vis_culling = nullptr;
    QCheckBox *keep_position = nullptr;
    QCheckBox *keep_cull_frustum = nullptr;
    QCheckBox *keep_cull_position = nullptr;
    QCheckBox *draw_opaque = nullptr;
    QCheckBox *show_bmodels = nullptr;

    QRadioButton *render_lightmapped = nullptr;
    QRadioButton *render_lightmap_only = nullptr;
    QRadioButton *render_fullbright = nullptr;
    QRadioButton *render_normals = nullptr;
    QRadioButton *render_flat = nullptr;
    QRadioButton *render_hull0 = nullptr;
    QRadioButton *render_hull1 = nullptr;
    QRadioButton *render_hull2 = nullptr;
    QRadioButton *render_hull3 = nullptr;
    QRadioButton *render_hull4 = nullptr;
    QRadioButton *render_hull5 = nullptr;

    QLineEdit *common_options = nullptr;
    QLineEdit *qbsp_options = nullptr;
    QLineEdit *vis_options = nullptr;
    QLineEdit *light_options = nullptr;
    QVBoxLayout *lightstyles = nullptr;

    QMenu *viewMenu = nullptr;
    QMenu *settingsMenu = nullptr;
    QMenu *cameraBookmarksMenu = nullptr;
    QMenu *openRecentMenu = nullptr;

    ETLogWidget *m_outputLogWidget = nullptr;
};

