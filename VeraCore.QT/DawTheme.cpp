#include "DawTheme.h"

#include <QHash>
#include <QRegularExpression>
#include <algorithm>
#include <array>
#include <cmath>

namespace {
QString css(const QColor& color)
{
    return color.name(QColor::HexRgb);
}

double linearChannel(int channel) noexcept
{
    const double value = std::clamp(channel / 255.0, 0.0, 1.0);
    return value <= 0.04045 ? value / 12.92
                            : std::pow((value + 0.055) / 1.055, 2.4);
}
}

const DawThemeTokens& DawTheme::professionalDark()
{
    static const DawThemeTokens tokens{
        QStringLiteral("veracore-professional-dark-v1"),
        QColor(QStringLiteral("#151816")), // canvas
        QColor(QStringLiteral("#1b1f1c")), // surface 0
        QColor(QStringLiteral("#222724")), // surface 1
        QColor(QStringLiteral("#2a302c")), // surface 2
        QColor(QStringLiteral("#343b36")), // surface 3
        QColor(QStringLiteral("#0d100e")), // strong border
        QColor(QStringLiteral("#3a433c")), // border
        QColor(QStringLiteral("#edf1ed")), // primary text
        QColor(QStringLiteral("#ffffff")), // bright text
        QColor(QStringLiteral("#bac2ba")), // secondary text
        QColor(QStringLiteral("#909991")), // muted text
        QColor(QStringLiteral("#667068")), // disabled
        QColor(QStringLiteral("#ff9f43")), // accent
        QColor(QStringLiteral("#ffb363")), // accent hover
        QColor(QStringLiteral("#17120d")), // accent ink
        QColor(QStringLiteral("#ffc078")), // focus
        QColor(QStringLiteral("#364139")), // selection
        QColor(QStringLiteral("#86d183")), // success
        QColor(QStringLiteral("#243426")), // success background
        QColor(QStringLiteral("#e0ae68")), // warning
        QColor(QStringLiteral("#382d20")), // warning background
        QColor(QStringLiteral("#815523")), // warning border
        QColor(QStringLiteral("#ef7165")), // danger
        QColor(QStringLiteral("#3a2523")), // danger background
        QColor(QStringLiteral("#62bdc8")), // info
        QColor(QStringLiteral("#59625b")), // scrollbar
        1, 3, 4, 12, 22, 9.5,
        {QStringLiteral("Bahnschrift"), QStringLiteral("Inter"),
         QStringLiteral("Noto Sans"), QStringLiteral("Segoe UI")},
        {QStringLiteral("Cascadia Mono"), QStringLiteral("Consolas"),
         QStringLiteral("DejaVu Sans Mono")}
    };
    return tokens;
}

void DawTheme::applyPalette(QPalette& palette)
{
    const DawThemeTokens& t = professionalDark();
    palette.setColor(QPalette::Window, t.surface1);
    palette.setColor(QPalette::WindowText, t.textPrimary);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, t.disabled);
    palette.setColor(QPalette::Base, t.canvas);
    palette.setColor(QPalette::AlternateBase, t.surface2);
    palette.setColor(QPalette::ToolTipBase, t.surface0);
    palette.setColor(QPalette::ToolTipText, t.textPrimary);
    palette.setColor(QPalette::Text, t.textPrimary);
    palette.setColor(QPalette::Disabled, QPalette::Text, t.disabled);
    palette.setColor(QPalette::Dark, t.canvas);
    palette.setColor(QPalette::Shadow, t.borderStrong);
    palette.setColor(QPalette::Button, t.surface2);
    palette.setColor(QPalette::ButtonText, t.textPrimary);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, t.disabled);
    palette.setColor(QPalette::BrightText, t.danger);
    palette.setColor(QPalette::Link, t.accentHover);
    palette.setColor(QPalette::Highlight, t.accent);
    palette.setColor(QPalette::Disabled, QPalette::Highlight, t.surface3);
    palette.setColor(QPalette::HighlightedText, t.accentInk);
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, t.textMuted);
}

QString DawTheme::resolveStyleSheet(const QString& source, QStringList* unresolvedTokens)
{
    const DawThemeTokens& t = professionalDark();
    const QHash<QString, QString> values{
        {"@vc-canvas", css(t.canvas)}, {"@vc-surface-0", css(t.surface0)},
        {"@vc-surface-1", css(t.surface1)}, {"@vc-surface-2", css(t.surface2)},
        {"@vc-surface-3", css(t.surface3)}, {"@vc-border-strong", css(t.borderStrong)},
        {"@vc-border", css(t.border)}, {"@vc-text-primary", css(t.textPrimary)},
        {"@vc-text-bright", css(t.textBright)}, {"@vc-text-secondary", css(t.textSecondary)},
        {"@vc-text-muted", css(t.textMuted)}, {"@vc-disabled", css(t.disabled)},
        {"@vc-accent", css(t.accent)}, {"@vc-accent-hover", css(t.accentHover)},
        {"@vc-accent-ink", css(t.accentInk)}, {"@vc-focus", css(t.focus)},
        {"@vc-selection", css(t.selection)}, {"@vc-success", css(t.success)},
        {"@vc-success-bg", css(t.successBackground)}, {"@vc-warning", css(t.warning)},
        {"@vc-warning-bg", css(t.warningBackground)}, {"@vc-warning-border", css(t.warningBorder)},
        {"@vc-danger", css(t.danger)}, {"@vc-danger-bg", css(t.dangerBackground)},
        {"@vc-info", css(t.info)}, {"@vc-scrollbar", css(t.scrollbar)},
        {"@vc-hairline", QString::number(t.hairline) + "px"},
        {"@vc-radius-small", QString::number(t.radiusSmall) + "px"},
        {"@vc-splitter-size", QString::number(t.splitterSize) + "px"},
        {"@vc-scrollbar-size", QString::number(t.scrollbarSize) + "px"},
        {"@vc-tree-row-height", QString::number(t.treeRowHeight) + "px"}
    };
    QString result = source;
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        result.replace(it.key(), it.value(), Qt::CaseSensitive);
    if (unresolvedTokens) {
        unresolvedTokens->clear();
        const QRegularExpression expression(QStringLiteral("@vc-[a-z0-9-]+"));
        auto match = expression.globalMatch(result);
        while (match.hasNext()) unresolvedTokens->push_back(match.next().captured());
        unresolvedTokens->removeDuplicates();
        unresolvedTokens->sort(Qt::CaseInsensitive);
    }
    return result;
}

QFont DawTheme::preferredUiFont(const QStringList& availableFamilies)
{
    const DawThemeTokens& t = professionalDark();
    QString family = QFont().defaultFamily();
    for (const QString& candidate : t.preferredUiFonts) {
        if (availableFamilies.contains(candidate, Qt::CaseInsensitive)) {
            family = candidate;
            break;
        }
    }
    QFont font(family);
    font.setPointSizeF(t.uiPointSize);
    font.setHintingPreference(QFont::PreferFullHinting);
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

double DawTheme::contrastRatio(const QColor& foreground, const QColor& background) noexcept
{
    auto luminance = [](const QColor& color) noexcept {
        return 0.2126 * linearChannel(color.red()) +
               0.7152 * linearChannel(color.green()) +
               0.0722 * linearChannel(color.blue());
    };
    const double left = luminance(foreground);
    const double right = luminance(background);
    return (std::max(left, right) + 0.05) / (std::min(left, right) + 0.05);
}

bool DawTheme::validate(QStringList* errors)
{
    QStringList local;
    const DawThemeTokens& t = professionalDark();
    const std::array<QColor, 26> colors{{
        t.canvas, t.surface0, t.surface1, t.surface2, t.surface3,
        t.borderStrong, t.border, t.textPrimary, t.textBright, t.textSecondary,
        t.textMuted, t.disabled, t.accent, t.accentHover, t.accentInk, t.focus,
        t.selection, t.success, t.successBackground, t.warning, t.warningBackground,
        t.warningBorder, t.danger, t.dangerBackground, t.info, t.scrollbar
    }};
    if (t.id.trimmed().isEmpty()) local.push_back(QStringLiteral("theme id is empty"));
    if (std::any_of(colors.begin(), colors.end(), [](const QColor& color) {
            return !color.isValid();
        })) local.push_back(QStringLiteral("theme contains an invalid color"));
    if (contrastRatio(t.textPrimary, t.surface1) < 7.0)
        local.push_back(QStringLiteral("primary text contrast is below 7:1"));
    if (contrastRatio(t.textSecondary, t.surface1) < 4.5)
        local.push_back(QStringLiteral("secondary text contrast is below 4.5:1"));
    if (contrastRatio(t.textMuted, t.surface1) < 4.5)
        local.push_back(QStringLiteral("muted text contrast is below 4.5:1"));
    if (contrastRatio(t.accent, t.accentInk) < 4.5)
        local.push_back(QStringLiteral("accent contrast is below 4.5:1"));
    if (t.hairline != 1 || t.radiusSmall < 2 || t.splitterSize < 3 ||
        t.scrollbarSize < 10 || t.treeRowHeight < 20 || t.uiPointSize < 9.0)
        local.push_back(QStringLiteral("density tokens are outside workstation bounds"));
    if (errors) *errors = local;
    return local.isEmpty();
}
