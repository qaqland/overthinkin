// SPDX-FileCopyrightText: 2026 qaqland
// SPDX-License-Identifier: LGPL-3.0-or-later

#ifndef DDECLIPWINKPLUGIN_H
#define DDECLIPWINKPLUGIN_H

#include "pluginsiteminterface_v2.h"

#include <QObject>
#include <QScopedPointer>

// 前向声明，保持插件头文件简洁
class ClipboardManager;
class ClipboardPreviewWidget;

/**
 * @brief DdeClipWinkPlugin dde-tray-loader 系统托盘插件
 *
 * 插件名取自当前目录 basename：dde-clip-wink。
 * 类型：Type_Tray | Attribute_ForceDock，强制显示在任务栏托盘区。
 *
 * 核心行为：
 * 1. init() 中创建 ClipboardManager 与 ClipboardPreviewWidget；
 * 2. 剪贴板变化 → 图标闪烁 800ms；
 * 3. 鼠标悬停图标时通过 setToolTip 显示最新 32 字符 head 预览。
 */
class DdeClipWinkPlugin : public QObject, public PluginsItemInterfaceV2
{
    Q_OBJECT

    // 声明实现的 Qt 插件接口
    Q_INTERFACES(PluginsItemInterfaceV2)

    // 插件元数据：api 版本 2.0.0
    Q_PLUGIN_METADATA(IID "com.deepin.dock.PluginsItemInterface_V2" FILE "dde-clip-wink.json")

public:
    explicit DdeClipWinkPlugin(QObject *parent = nullptr);

    // PluginsItemInterface 必须实现接口
    const QString pluginName() const override;
    void init(PluginProxyInterface *proxyInter) override;
    QWidget *itemWidget(const QString &itemKey) override;

    // 左键点击：弹出系统历史剪贴板面板（与 Win+V 相同）
    const QString itemCommand(const QString &itemKey) override;

    // PluginsItemInterfaceV2 必须覆写的标识
    Dock::PluginFlags flags() const override;

    // PluginsItemInterfaceV2 消息处理：声明不需要变色龙 hover 效果等
    QString message(const QString &msg) override;

    // 显示名称，用于日志/调试
    const QString pluginDisplayName() const override;

    // 禁用策略：强制显示，不允许在控制中心关闭
    bool pluginIsAllowDisable() override;
    bool pluginIsDisable() override;

    // 任务栏状态变化回调：仅转发重绘请求
    void displayModeChanged(const Dock::DisplayMode displayMode) override;
    void positionChanged(const Dock::Position position) override;
    void refreshIcon(const QString &itemKey) override;

private:
    PluginProxyInterface *m_proxyInter = nullptr;                          ///< 由 dde-tray-loader 传入，切勿 delete
    QScopedPointer<ClipboardManager> m_manager;                          ///< 剪贴板监听与预览管理
    QScopedPointer<ClipboardPreviewWidget> m_widget;                       ///< 托盘图标控件
    const QString m_itemKey = QStringLiteral("dde-clip-wink");            ///< 插件在托盘中的唯一 item key
};

#endif // DDECLIPWINKPLUGIN_H
