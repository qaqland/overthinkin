// SPDX-FileCopyrightText: 2026 qaqland
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "ddeclipwinkplugin.h"
#include "clipboardmanager.h"
#include "clipboardpreviewwidget.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>

DdeClipWinkPlugin::DdeClipWinkPlugin(QObject *parent)
    : QObject(parent)
    , PluginsItemInterfaceV2()
    , m_manager(new ClipboardManager(this))
    , m_widget(new ClipboardPreviewWidget())
{
}

const QString DdeClipWinkPlugin::pluginName() const
{
    // 插件唯一标识，使用当前目录 basename：dde-clip-wink
    return QStringLiteral("dde-clip-wink");
}

const QString DdeClipWinkPlugin::pluginDisplayName() const
{
    return tr("剪贴板抖动提示");
}

void DdeClipWinkPlugin::init(PluginProxyInterface *proxyInter)
{
    // 保存 dde-tray-loader 传入的代理指针，插件生命周期内不得 delete
    m_proxyInter = proxyInter;

    // 剪贴板内容变化 → 触发图标 QQ 式闪烁（消失/出现交替约 800ms）
    connect(m_manager.data(), &ClipboardManager::contentChanged,
            m_widget.data(), &ClipboardPreviewWidget::startBlink,
            Qt::QueuedConnection);

    // 剪贴板预览文本变化 → 更新 tooltip
    connect(m_manager.data(), &ClipboardManager::previewChanged,
            m_widget.data(), &ClipboardPreviewWidget::setPreviewText,
            Qt::QueuedConnection);

    // 初始化时立即读取一次当前剪贴板，使 tooltip 初始可用
    m_manager->refresh();

    // 向任务栏注册托盘控件
    m_proxyInter->itemAdded(this, m_itemKey);
}

QWidget *DdeClipWinkPlugin::itemWidget(const QString &itemKey)
{
    // 只返回本插件注册的 item key 对应的控件
    if (itemKey == m_itemKey) {
        return m_widget.data();
    }
    return nullptr;
}

const QString DdeClipWinkPlugin::itemCommand(const QString &itemKey)
{
    Q_UNUSED(itemKey)

    // 左键点击托盘图标：弹出系统历史剪贴板面板。
    // 与 Win+V 快捷键 / 系统自带剪贴板托盘插件使用同一个官方接口：
    // org.deepin.dde.Clipboard1.Toggle（dde-clipboard UI 进程提供）。
    // 命令写法与 dde-clipboard 的 dock-clipboard-plugin 保持一致。
    // 左键点击托盘图标：弹出系统历史剪贴板面板。
    // 与 Win+V 快捷键 / 系统自带剪贴板托盘插件使用同一个官方接口：
    // org.deepin.dde.Clipboard1.Toggle（dde-clipboard UI 进程提供）。
    // 命令写法与 dde-clipboard 的 dock-clipboard-plugin 保持一致；
    // 注意 dde-tray-loader 按空格拆分命令，参数中不能带引号/分号。
    return QStringLiteral("dbus-send --session --print-reply "
                          "--dest=org.deepin.dde.Clipboard1 "
                          "/org/deepin/dde/Clipboard1 "
                          "org.deepin.dde.Clipboard1.Toggle");
}

Dock::PluginFlags DdeClipWinkPlugin::flags() const
{
    // Type_Tray：托盘区插件
    // Attribute_ForceDock：强制显示在任务栏，无需控制中心图标
    return Dock::Type_Tray | Dock::Attribute_ForceDock;
}

QString DdeClipWinkPlugin::message(const QString &msg)
{
    // 解析任务栏发来的 JSON 消息
    const QJsonObject msgObj = QJsonDocument::fromJson(msg.toUtf8()).object();
    const QString msgType = msgObj[Dock::MSG_TYPE].toString();

    // 插件属性查询：声明不需要变色龙效果，
    // 避免任务栏在鼠标 hover/press 时给托盘图标叠加变色样式
    if (msgType == Dock::MSG_PLUGIN_PROPERTY) {
        QJsonObject data;
        data[Dock::PLUGIN_PROP_NEED_CHAMELEON] = false;
        QJsonObject reply;
        reply[Dock::MSG_TYPE] = msgType;
        reply[Dock::MSG_DATA] = data;
        return QString::fromUtf8(QJsonDocument(reply).toJson());
    }

    // 可用性查询：本插件始终可用
    if (msgType == Dock::MSG_GET_SUPPORT_FLAG) {
        QJsonObject data;
        data[Dock::MSG_SUPPORT_FLAG] = true;
        QJsonObject reply;
        reply[Dock::MSG_TYPE] = msgType;
        reply[Dock::MSG_DATA] = data;
        return QString::fromUtf8(QJsonDocument(reply).toJson());
    }

    // 未识别的消息直接返回空对象
    return QStringLiteral("{}");
}

bool DdeClipWinkPlugin::pluginIsAllowDisable()
{
    // 强制显示插件，不允许在控制中心禁用
    return false;
}

bool DdeClipWinkPlugin::pluginIsDisable()
{
    // 始终处于启用状态
    return false;
}

void DdeClipWinkPlugin::displayModeChanged(const Dock::DisplayMode displayMode)
{
    Q_UNUSED(displayMode)

    // 任务栏显示模式切换时重绘图标，以适配不同尺寸/风格
    if (m_widget) {
        m_widget->update();
    }
}

void DdeClipWinkPlugin::positionChanged(const Dock::Position position)
{
    Q_UNUSED(position)

    // 任务栏位置切换（上/下/左/右）时重绘图标
    if (m_widget) {
        m_widget->update();
    }
}

void DdeClipWinkPlugin::refreshIcon(const QString &itemKey)
{
    Q_UNUSED(itemKey)

    // 系统图标主题变化时刷新
    if (m_widget) {
        m_widget->update();
    }
}
