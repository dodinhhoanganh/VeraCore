#pragma once

#include <QColor>
#include <QFont>
#include <QPalette>
#include <QString>
#include <QStringList>

struct DawThemeTokens {
    QString id;
    QColor canvas;
    QColor surface0;
    QColor surface1;
    QColor surface2;
    QColor surface3;
    QColor borderStrong;
    QColor border;
    QColor textPrimary;
    QColor textBright;
    QColor textSecondary;
    QColor textMuted;
    QColor disabled;
    QColor accent;
    QColor accentHover;
    QColor accentInk;
    QColor focus;
    QColor selection;
    QColor success;
    QColor successBackground;
    QColor warning;
    QColor warningBackground;
    QColor warningBorder;
    QColor danger;
    QColor dangerBackground;
    QColor info;
    QColor scrollbar;
    int hairline = 1;
    int radiusSmall = 3;
    int splitterSize = 4;
    int scrollbarSize = 12;
    int treeRowHeight = 22;
    qreal uiPointSize = 9.5;
    QStringList preferredUiFonts;
    QStringList preferredMonoFonts;
};

class DawTheme final
{
public:
    static const DawThemeTokens& professionalDark();
    static void applyPalette(QPalette& palette);
    static QString resolveStyleSheet(const QString& source,
                                     QStringList* unresolvedTokens = nullptr);
    static QFont preferredUiFont(const QStringList& availableFamilies);
    static double contrastRatio(const QColor& foreground, const QColor& background) noexcept;
    static bool validate(QStringList* errors = nullptr);
};
