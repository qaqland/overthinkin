// SPDX-FileCopyrightText: 2026 qaqland
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "clipboardpreviewwidget.h"

#include <QPainter>

ClipboardPreviewWidget::ClipboardPreviewWidget(QWidget *parent)
    : QWidget(parent)
    , m_blinkTimer(new QTimer(this))
{
    // 托盘插件固定大小 16x16，与 dde-tray-loader 规范保持一致
    setFixedSize(16, 16);

    // 背景透明，避免在任务栏上产生矩形底色块
    setAttribute(Qt::WA_TranslucentBackground);

    // 闪烁定时器：固定间隔切换图标可见性
    m_blinkTimer->setInterval(BlinkIntervalMs);
    m_blinkTimer->setSingleShot(false);
    connect(m_blinkTimer, &QTimer::timeout, this, &ClipboardPreviewWidget::onBlinkTick);

    // 默认 tooltip 提示，等待第一次剪贴板变化后会被覆盖
    setToolTip(tr("剪贴板预览"));
}

void ClipboardPreviewWidget::setPreviewText(const QString &preview)
{
    m_previewText = preview;
    // 通过 setToolTip 在鼠标悬停时显示 head 预览
    setToolTip(m_previewText);
}

void ClipboardPreviewWidget::startBlink()
{
    // QQ 风格闪烁：图标先消失，再交替出现/消失
    m_blinkTicks = 0;
    m_iconVisible = false;
    update();
    m_blinkTimer->start();
}

void ClipboardPreviewWidget::onBlinkTick()
{
    ++m_blinkTicks;

    // 交替切换可见性：1 出现，2 消失，3 出现 ...
    m_iconVisible = (m_blinkTicks % 2 == 1);

    // 达到总时长后停止，并确保图标最终恢复为可见
    if (m_blinkTicks >= BlinkMaxTicks) {
        m_blinkTimer->stop();
        m_iconVisible = true;
    }

    update();
}

void ClipboardPreviewWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    // 仅绘制可见状态的图标；闪烁时消失的阶段保持透明
    if (m_iconVisible) {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        drawIcon(&painter);
    }
}

void ClipboardPreviewWidget::drawIcon(QPainter *painter)
{
    // 固定纯黑色，不随调色板/悬停状态变化。
    // 任务栏 hover 背景通常为浅色，白色图标会在浅色背景上“消失”，
    // 看起来像图标在闪烁跳动；黑色图标在浅色背景上清晰稳定。
    const QColor iconColor(0, 0, 0, 235);

    // 单遍绘制：黑色主体
    paintIconPass(painter, iconColor, 1.2, true);
}

void ClipboardPreviewWidget::paintIconPass(QPainter *painter, const QColor &color,
                                           qreal penWidth, bool fillClip)
{
    // 1. 剪贴板外框（圆角矩形）
    const QRectF boardRect(2.5, 4.5, 11.0, 10.0);
    painter->setPen(QPen(color, penWidth));
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(boardRect, 1.5, 1.5);

    // 2. 顶部夹板（小矩形）
    const QRectF clipRect(5.5, 2.5, 5.0, 2.5);
    painter->setBrush(fillClip ? color : Qt::NoBrush);
    painter->setPen(fillClip ? Qt::NoPen : QPen(color, penWidth));
    painter->drawRoundedRect(clipRect, 0.8, 0.8);

    // 3. 内部纸张（三条横线表示文字）
    const qreal lineWidth = penWidth * 0.7;
    painter->setPen(QPen(color, lineWidth));
    painter->setBrush(Qt::NoBrush);
    painter->drawLine(QPointF(4.5, 7.5), QPointF(11.5, 7.5));
    painter->drawLine(QPointF(4.5, 10.5), QPointF(11.5, 10.5));
    painter->drawLine(QPointF(4.5, 13.5), QPointF(9.5, 13.5));
}
