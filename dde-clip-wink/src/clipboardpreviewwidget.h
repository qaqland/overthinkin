// SPDX-FileCopyrightText: 2026 qaqland
// SPDX-License-Identifier: LGPL-3.0-or-later

#ifndef CLIPBOARDPREVIEWWIDGET_H
#define CLIPBOARDPREVIEWWIDGET_H

#include <QWidget>
#include <QTimer>
#include <QColor>

/**
 * @brief ClipboardPreviewWidget 是任务栏托盘区的 16x16 图标控件
 *
 * 职责：
 * 1. 以固定 16x16 大小显示在系统托盘区；
 * 2. 使用 QPainter 代码绘制剪贴板图标（固定颜色 + 深色描边，
 *    不依赖调色板，鼠标悬停时颜色不会变化），无需外部 SVG/DCI 资源；
 * 3. 通过 setToolTip() 在鼠标悬停时显示剪贴板 head 预览；
 * 4. 提供 startBlink() 方法，实现 QQ 风格的“图标消失/出现”交替闪烁，
 *    总时长约 800ms 后自动恢复为可见状态。
 */
class ClipboardPreviewWidget : public QWidget
{
    Q_OBJECT

public:
    /**
     * @brief 构造托盘图标控件
     * @param parent 父控件，通常由 dde-tray-loader 框架管理
     */
    explicit ClipboardPreviewWidget(QWidget *parent = nullptr);

    /**
     * @brief 设置 tooltip 显示的预览文本
     * @param preview 剪贴板 head 预览文本
     *
     * 直接调用 QWidget::setToolTip，dde-tray-loader 在鼠标悬停时
     * 会显示该 tooltip。
     */
    void setPreviewText(const QString &preview);

    /**
     * @brief 触发图标闪烁（QQ 风格：消失/出现交替）
     * 图标先消失，随后以固定间隔交替出现/消失，约 800ms 后
     * 自动恢复为可见状态。
     */
    void startBlink();

protected:
    /**
     * @brief 绘制图标：仅当图标处于可见状态时绘制；
     *        闪烁时按定时器节奏隐藏/显示。
     */
    void paintEvent(QPaintEvent *event) override;

private slots:
    /**
     * @brief 闪烁定时器到期回调：切换图标可见性
     */
    void onBlinkTick();

private:
    /**
     * @brief 绘制剪贴板图标（固定颜色 + 深色描边，双 pass 绘制）
     * @param painter 已初始化的 QPainter
     */
    void drawIcon(QPainter *painter);

    /**
     * @brief 绘制单遍图标（描边 pass 或主体 pass）
     * @param painter 已初始化的 QPainter
     * @param color 本遍使用的颜色
     * @param penWidth 画笔宽度
     * @param fillClip 是否填充顶部夹板（描边 pass 为 true）
     */
    void paintIconPass(QPainter *painter, const QColor &color, qreal penWidth, bool fillClip);

private:
    bool m_iconVisible = true;  ///< 图标当前是否可见
    int m_blinkTicks = 0;       ///< 已完成的闪烁切换次数
    QString m_previewText;      ///< 当前 tooltip 预览文本
    QTimer *m_blinkTimer = nullptr; ///< 闪烁定时器

    static constexpr int BlinkIntervalMs = 160; ///< 闪烁切换间隔
    static constexpr int BlinkMaxTicks = 5;     ///< 总切换次数，5*160ms=800ms
};

#endif // CLIPBOARDPREVIEWWIDGET_H
