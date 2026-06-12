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

DarkStyle::DarkStyle() : DarkStyle(styleBase()) {}

DarkStyle::DarkStyle(QStyle *style) : QProxyStyle(style) {}

QStyle *DarkStyle::styleBase(QStyle *style) const {
  static QStyle *base =
      !style ? QStyleFactory::create(QStringLiteral("Fusion")) : style;
  return base;
}

QStyle *DarkStyle::baseStyle() const { return styleBase(); }

void DarkStyle::polish(QPalette &palette) {
  // Flat dark surfaces (Ableton) with an orange accent (FL Studio).
  palette.setColor(QPalette::Window, QColor(37, 37, 37));
  palette.setColor(QPalette::WindowText, QColor(230, 230, 230));
  palette.setColor(QPalette::Disabled, QPalette::WindowText,
                   QColor(120, 120, 120));
  palette.setColor(QPalette::Base, QColor(26, 26, 26));
  palette.setColor(QPalette::AlternateBase, QColor(45, 45, 45));
  palette.setColor(QPalette::ToolTipBase, QColor(26, 26, 26));
  palette.setColor(QPalette::ToolTipText, QColor(230, 230, 230));
  palette.setColor(QPalette::Text, QColor(230, 230, 230));
  palette.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 120, 120));
  palette.setColor(QPalette::Dark, QColor(22, 22, 22));
  palette.setColor(QPalette::Shadow, QColor(10, 10, 10));
  palette.setColor(QPalette::Button, QColor(45, 45, 45));
  palette.setColor(QPalette::ButtonText, QColor(230, 230, 230));
  palette.setColor(QPalette::Disabled, QPalette::ButtonText,
                   QColor(120, 120, 120));
  palette.setColor(QPalette::BrightText, Qt::red);
  palette.setColor(QPalette::Link, QColor(255, 160, 64));
  palette.setColor(QPalette::Highlight, QColor(255, 144, 32));
  palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor(80, 80, 80));
  palette.setColor(QPalette::HighlightedText, QColor(20, 20, 20));
  palette.setColor(QPalette::Disabled, QPalette::HighlightedText,
                   QColor(127, 127, 127));
}

void DarkStyle::polish(QApplication *app) {
  if (!app) return;

  // increase font size for better reading,
  // setPointSize was reduced from +2 because when applied this way in Qt5, the
  // font is larger than intended for some reason
  QFont defaultFont = QApplication::font();
  defaultFont.setPointSize(defaultFont.pointSize());
  app->setFont(defaultFont);

  // loadstylesheet
  QFile qfDarkstyle(QStringLiteral(":/darkstyle/darkstyle.qss"));
  if (qfDarkstyle.open(QIODevice::ReadOnly | QIODevice::Text)) {
    // set stylesheet
    QString qsStylesheet = QString::fromLatin1(qfDarkstyle.readAll());
    app->setStyleSheet(qsStylesheet);
    qfDarkstyle.close();
  }
}
