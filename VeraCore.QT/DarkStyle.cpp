/*
###############################################################################
#                                                                             #
# The MIT License                                                             #
#                                                                             #
# Copyright (C) 2017 by Juergen Skrotzky (JorgenVikingGod@gmail.com)          #
#               >> https://github.com/Jorgen-VikingGod                        #
#                                                                             #
# Sources: https://github.com/Jorgen-VikingGod/Qt-Frameless-Window-DarkStyle  #
#                                                                             #
###############################################################################
*/

#include "DarkStyle.h"
#include "DawTheme.h"

#include <QDebug>

DarkStyle::DarkStyle() : DarkStyle(styleBase()) {}

DarkStyle::DarkStyle(QStyle *style) : QProxyStyle(style) {}

QStyle *DarkStyle::styleBase(QStyle *style) const {
  static QStyle *base =
      !style ? QStyleFactory::create(QStringLiteral("Fusion")) : style;
  return base;
}

QStyle *DarkStyle::baseStyle() const { return styleBase(); }

void DarkStyle::polish(QPalette &palette) {
  DawTheme::applyPalette(palette);
}

void DarkStyle::polish(QApplication *app) {
  if (!app) return;

  QStringList themeErrors;
  if (!DawTheme::validate(&themeErrors))
    qWarning() << "VeraCore theme validation failed:" << themeErrors;

  QFile qfDarkstyle(QStringLiteral(":/darkstyle/darkstyle.qss"));
  if (qfDarkstyle.open(QIODevice::ReadOnly | QIODevice::Text)) {
    QStringList unresolved;
    const QString source = QString::fromUtf8(qfDarkstyle.readAll());
    const QString stylesheet = DawTheme::resolveStyleSheet(source, &unresolved);
    if (unresolved.isEmpty()) app->setStyleSheet(stylesheet);
    else qWarning() << "Unresolved VeraCore QSS tokens:" << unresolved;
    qfDarkstyle.close();
  }
}
