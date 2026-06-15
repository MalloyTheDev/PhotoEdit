#include "PanelHeader.hpp"

#include "IconUtil.hpp"
#include "Theme.hpp"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QWidget>

namespace pe::app {

QWidget* makePanelHeader(const QString& title, const QString& iconName, QWidget* parent) {
    const ThemeColors& c = themeColors(currentTheme());

    auto* bar = new QWidget(parent);
    bar->setObjectName(QStringLiteral("PanelHeaderBar"));
    bar->setFixedHeight(28);

    auto* lay = new QHBoxLayout(bar);
    lay->setContentsMargins(10, 0, 8, 0);
    lay->setSpacing(7);

    if (!iconName.isEmpty()) {
        auto* icon = new QLabel(bar);
        icon->setPixmap(renderIcon(iconName, c.textDim, 14));
        lay->addWidget(icon);
    }

    auto* label = new QLabel(title, bar);  // Title Case, Photoshop-style tab label
    label->setObjectName(QStringLiteral("PanelHeaderLabel"));
    QFont f = label->font();
    f.setPointSizeF(8.0);
    f.setWeight(QFont::Medium);
    label->setFont(f);
    lay->addWidget(label);
    lay->addStretch(1);

    return bar;
}

}  // namespace pe::app
