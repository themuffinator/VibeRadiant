#include "stats.h"

#include <QTableWidget>
#include <QVBoxLayout>
#include <QHeaderView>

#include <QtGlobal> // for qDebug()
#include <QDebug> // for QDebug

#include <common/bspfile.hh>

StatsPanel::StatsPanel(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *layout = new QVBoxLayout(this);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(2);
    QStringList labels;
    labels << QStringLiteral("stat");
    labels << QStringLiteral("count");
    m_table->setHorizontalHeaderLabels(labels);

    // make the columns fill the table
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);

    // make the m_table completely fill `this`
    layout->addWidget(m_table, 1);
    layout->setContentsMargins(0, 0, 0, 0);
}

void StatsPanel::addStat(const QString &str, qulonglong value)
{
    // add a row
    int currentRow = m_table->rowCount();
    m_table->setRowCount(currentRow + 1);

    // populate it
    auto *labelItem = new QTableWidgetItem(str);
    labelItem->setFlags(labelItem->flags() & (~Qt::ItemIsEditable));
    m_table->setItem(currentRow, 0, labelItem);

    QLocale locale(QLocale::English, QLocale::UnitedStates);

    auto *valueItem = new QTableWidgetItem(locale.toString(value));
    valueItem->setFlags(valueItem->flags() & (~Qt::ItemIsEditable));
    m_table->setItem(currentRow, 1, valueItem);
}

void StatsPanel::updateWithBSP(const mbsp_t *bsp, const bspxentries_t &entries)
{
    m_table->setRowCount(0);

    if (bsp == nullptr) {
        return;
    }

    addStat(QStringLiteral("models"), static_cast<qulonglong>(bsp->dmodels.size()));
    addStat(QStringLiteral("nodes"), static_cast<qulonglong>(bsp->dnodes.size()));
    addStat(QStringLiteral("leafs"), static_cast<qulonglong>(bsp->dleafs.size()));
    addStat(QStringLiteral("clipnodes"), static_cast<qulonglong>(bsp->dclipnodes.size()));
    addStat(QStringLiteral("planes"), static_cast<qulonglong>(bsp->dplanes.size()));
    addStat(QStringLiteral("vertexes"), static_cast<qulonglong>(bsp->dvertexes.size()));
    addStat(QStringLiteral("faces"), static_cast<qulonglong>(bsp->dfaces.size()));
    addStat(QStringLiteral("surfedges"), static_cast<qulonglong>(bsp->dsurfedges.size()));
    addStat(QStringLiteral("edges"), static_cast<qulonglong>(bsp->dedges.size()));
    addStat(QStringLiteral("leaffaces"), static_cast<qulonglong>(bsp->dleaffaces.size()));
    addStat(QStringLiteral("leafbrushes"), static_cast<qulonglong>(bsp->dleafbrushes.size()));

    addStat(QStringLiteral("areas"), static_cast<qulonglong>(bsp->dareas.size()));
    addStat(QStringLiteral("areaportals"), static_cast<qulonglong>(bsp->dareaportals.size()));

    addStat(QStringLiteral("brushes"), static_cast<qulonglong>(bsp->dbrushes.size()));
    addStat(QStringLiteral("brushsides"), static_cast<qulonglong>(bsp->dbrushsides.size()));

    addStat(QStringLiteral("texinfos"), static_cast<qulonglong>(bsp->texinfo.size()));
    addStat(QStringLiteral("textures"), static_cast<qulonglong>(bsp->dtex.textures.size()));

    addStat(QStringLiteral("visdata bytes"), static_cast<qulonglong>(bsp->dvis.bits.size()));
    addStat(QStringLiteral("lightdata bytes"), static_cast<qulonglong>(bsp->dlightdata.size()));
    addStat(QStringLiteral("entdata bytes"), static_cast<qulonglong>(bsp->dentdata.size()));

    // bspx lumps
    for (const auto &[lumpname, data] : entries) {
        addStat(QStringLiteral("%1 bytes").arg(lumpname.c_str()), static_cast<qulonglong>(data.size()));
    }
}
