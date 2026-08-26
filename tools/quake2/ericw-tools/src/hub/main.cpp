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
#include <QCoreApplication>
#include <QApplication>
#include <QGuiApplication>
#include <QPalette>
#include <QSettings>
#include <QSurfaceFormat>
#include <QtGlobal>

int main(int argc, char *argv[])
{
    QSettings::setDefaultFormat(QSettings::NativeFormat);

    QCoreApplication::setOrganizationName("VibeyMapTools");
    QCoreApplication::setApplicationName("vmt-hub");
    // allow non-integer monitor scaling
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

    // The default surface format must be selected before QApplication creates
    // any platform OpenGL contexts.
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(24);
    fmt.setStencilBufferSize(8);
#ifdef _DEBUG
    fmt.setOption(QSurfaceFormat::DebugContext);
#endif
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication a(argc, argv);
    a.setStyle("fusion");
    a.setPalette(QPalette(QColor(64, 64, 64)));

    MainWindow w;
    w.show();

    return a.exec();
}
