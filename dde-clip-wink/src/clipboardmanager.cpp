// SPDX-FileCopyrightText: 2026 qaqland
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "clipboardmanager.h"

#include <QApplication>
#include <QClipboard>
#include <QDBusConnection>
#include <QDataStream>
#include <QDebug>

ClipboardManager::ClipboardManager(QObject *parent)
    : QObject(parent)
    , m_clipboard(QApplication::clipboard())
    , m_debounceTimer(new QTimer(this))
{
    // 单发定时器：dataComing 触发后等待 100ms，若期间再次触发则重新计时
    m_debounceTimer->setSingleShot(true);
    m_debounceTimer->setInterval(DebounceMs);
    connect(m_debounceTimer, &QTimer::timeout, this, &ClipboardManager::onDebounceTimeout);

    // 注意：不要监听 QClipboard::dataChanged！
    // 实测鼠标悬停托盘图标时，dock 会读取剪贴板导致 X11 剪贴板所有权短暂变化，
    // 插件进程的 QClipboard::dataChanged 会被假触发，造成图标误闪烁。
    // 因此唯一触发源是 DDE 剪贴板守护进程的 dataComing 信号。
    if (!connectToClipboardDaemon()) {
        qWarning() << "[dde-clip-wink] failed to connect to" << DaemonService << DaemonSignal;
    } else {
        qInfo() << "[dde-clip-wink] connected to clipboard daemon DBus signal";
    }
}

ClipboardManager::~ClipboardManager() = default;

QString ClipboardManager::previewText() const
{
    return m_previewText;
}

void ClipboardManager::refresh()
{
    onDebounceTimeout();
}

void ClipboardManager::onDataComing(const QByteArray &data)
{
    // 唯一触发源：解析守护进程负载，得到最新剪贴板条目
    ClipboardItemData info;
    if (!parseItemData(data, info)) {
        // 解析失败则退回 QClipboard 读取
        m_pendingText = m_clipboard->text();
    } else {
        m_pendingText = itemText(info);
    }
    m_hasPendingData = true;
    m_debounceTimer->start();
}

bool ClipboardManager::connectToClipboardDaemon()
{
    // 连接 session bus 上守护进程广播的信号，订阅剪贴板内容变化
    return QDBusConnection::sessionBus().connect(
        QString::fromLatin1(DaemonService),
        QString::fromLatin1(DaemonPath),
        QString::fromLatin1(DaemonInterface),
        QString::fromLatin1(DaemonSignal),
        this,
        SLOT(onDataComing(QByteArray)));
}

void ClipboardManager::onDebounceTimeout()
{
    // 优先使用 dataComing 负载中的文本，其次退回系统剪贴板
    QString rawText;
    if (m_hasPendingData) {
        rawText = m_pendingText;
        m_hasPendingData = false;
    } else {
        rawText = m_clipboard->text();
    }

    // 生成 head 预览
    const QString newPreview = buildPreview(rawText);

    // 只有预览文本发生变化时才更新 tooltip，避免无意义重绘
    if (newPreview != m_previewText) {
        m_previewText = newPreview;
        emit previewChanged(m_previewText);
    }

    // 每次数据变化都通知图标闪烁（题目要求：检测到变化即闪烁 800ms）
    emit contentChanged();
}

bool ClipboardManager::parseItemData(const QByteArray &data, ClipboardItemData &out)
{
    QDataStream stream(data);
    // 与 dde-clipboard 守护进程 Info2Buf() 保持一致的流版本
    stream.setVersion(QDataStream::Qt_5_11);

    stream >> out.formatMap >> out.type >> out.urls >> out.hasImage;
    if (stream.status() != QDataStream::Ok)
        return false;

    if (out.hasImage) {
        stream >> out.variantImage >> out.pixSize;
        if (stream.status() != QDataStream::Ok)
            return false;
    }

    stream >> out.enable >> out.text >> out.textSize >> out.createTime >> out.iconBuf;
    return stream.status() == QDataStream::Ok;
}

QString ClipboardManager::itemText(const ClipboardItemData &info)
{
    // 文本类型：直接使用守护进程给出的文本（已截断至 4096 字符）
    if (info.type == 1) { // Text
        return info.text;
    }

    // 非文本类型：给出简短的文字描述
    if (info.type == 2 && info.hasImage) { // Image
        return tr("剪贴板图片 %1x%2").arg(info.pixSize.width()).arg(info.pixSize.height());
    }
    if (info.type == 3) { // File
        return tr("剪贴板文件 %1 个").arg(info.urls.size());
    }
    return tr("剪贴板内容不含文本");
}

QString ClipboardManager::buildPreview(const QString &text)
{
    if (text.isEmpty()) {
        return QObject::tr("剪贴板为空或不含文本");
    }

    // 把换行等空白统一替换为空格，保证单行 tooltip 显示整齐
    QString simplified = text;
    simplified.replace(QLatin1Char('\n'), QLatin1Char(' '));
    simplified.replace(QLatin1Char('\r'), QLatin1Char(' '));
    simplified.replace(QLatin1Char('\t'), QLatin1Char(' '));

    // 多个连续空格合并为一个
    while (simplified.contains(QStringLiteral("  "))) {
        simplified.replace(QStringLiteral("  "), QStringLiteral(" "));
    }

    // 截断至 32 字符，超出部分用省略号表示
    if (simplified.length() > PreviewMaxLength) {
        return simplified.left(PreviewMaxLength).trimmed() + QStringLiteral("...");
    }

    return simplified.trimmed();
}
