// SPDX-FileCopyrightText: 2026 qaqland
// SPDX-License-Identifier: LGPL-3.0-or-later

#ifndef CLIPBOARDMANAGER_H
#define CLIPBOARDMANAGER_H

#include <QObject>
#include <QTimer>
#include <QString>
#include <QDateTime>
#include <QMap>
#include <QUrl>
#include <QVariant>
#include <QSize>
#include <QByteArray>

QT_BEGIN_NAMESPACE
class QClipboard;
QT_END_NAMESPACE

/**
 * @brief 与 dde-clipboard 守护进程 dataComing 信号负载对应的数据结构
 *
 * 序列化格式见 dde-clipboard 源码 Info2Buf()：
 * QDataStream（Qt_5_11 版本）依次写入：
 * formatMap、type、urls、hasImage、[variantImage、pixSize]、enable、text、textSize、createTime、iconBuf
 */
struct ClipboardItemData
{
    QMap<QString, QByteArray> formatMap; ///< 剪贴板全部 MIME 格式数据
    int type = 0;                        ///< 数据类型：0 未知，1 文本，2 图片，3 文件
    QList<QUrl> urls;                    ///< 文件类型时的文件 URL 列表
    bool hasImage = false;               ///< 是否包含图片数据
    QVariant variantImage;               ///< 图片数据（缩略图）
    QSize pixSize;                       ///< 图片尺寸
    bool enable = false;                 ///< 是否启用
    QString text;                        ///< 文本内容（守护进程已截断至 4096 字符）
    int textSize = 0;                    ///< 文本原始长度
    QDateTime createTime;                ///< 创建时间
    QByteArray iconBuf;                  ///< 文件图标数据
};

/**
 * @brief ClipboardManager 封装系统剪贴板访问
 *
 * 职责：
 * 1. 监听 DDE 剪贴板守护进程的 DBus 信号
 *    org.deepin.dde.ClipboardLoader1.dataComing 作为唯一触发源，
 *    并从负载中直接解析出文本预览（避免插件进程读不到 X11 剪贴板的问题）；
 * 2. 不监听 QClipboard::dataChanged：鼠标悬停托盘图标时 dock 会读取剪贴板，
 *    造成 X11 剪贴板所有权短暂变化，QClipboard::dataChanged 会被假触发
 *    导致图标误闪烁；
 * 3. 通过短时定时器做去抖，避免快速连续复制导致频繁闪烁；
 * 4. 从最新文本内容中提取前 32 个字符作为 head 预览；
 * 5. 发出 contentChanged() 通知图标闪烁，发出 previewChanged() 通知 tooltip 更新。
 */
class ClipboardManager : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief 构造剪贴板管理器
     * @param parent 父对象，通常为插件实例
     */
    explicit ClipboardManager(QObject *parent = nullptr);
    ~ClipboardManager() override;

    /**
     * @brief 当前缓存的预览文本
     * @return 已经处理过的 head 预览字符串
     */
    QString previewText() const;

    /**
     * @brief 手动刷新一次剪贴板内容
     * 在插件初始化完成后调用一次，使 tooltip 初始即有内容。
     */
    void refresh();

signals:
    /**
     * @brief 剪贴板内容发生变化（去抖后）
     * 用于触发托盘图标 800ms 闪烁。
     */
    void contentChanged();

    /**
     * @brief 剪贴板预览文本发生变化
     * @param preview 处理后的 head 预览文本
     */
    void previewChanged(const QString &preview);

private slots:
    /**
     * @brief 去抖定时器到期后真正读取剪贴板
     */
    void onDebounceTimeout();

    /**
     * @brief DDE 剪贴板守护进程 dataComing 信号回调（唯一触发源）
     * @param data 守护进程序列化的 ItemInfo 数据
     */
    void onDataComing(const QByteArray &data);

private:
    /**
     * @brief 解析守护进程 dataComing 信号负载
     * @param data 原始字节
     * @param out 解析结果
     * @return 解析成功返回 true
     */
    static bool parseItemData(const QByteArray &data, ClipboardItemData &out);

    /**
     * @brief 从 ItemInfo 构造预览文本
     * @param info 解析出的剪贴板条目
     * @return 用于 tooltip 的原始预览文本
     */
    static QString itemText(const ClipboardItemData &info);

    /**
     * @brief 从原始文本构造预览文本
     * @param text 原始剪贴板文本
     * @return 截断至 32 字符、替换换行、超出加省略号的预览文本
     */
    static QString buildPreview(const QString &text);

    /**
     * @brief 建立到 DDE 剪贴板守护进程的 DBus 信号连接
     * @return 连接成功返回 true
     */
    bool connectToClipboardDaemon();

private:
    QClipboard *m_clipboard = nullptr;  ///< Qt 系统剪贴板对象
    QTimer *m_debounceTimer = nullptr;  ///< 去抖定时器，默认 100ms
    QString m_previewText;              ///< 当前缓存的预览文本
    QString m_pendingText;              ///< 最近一次 dataComing 负载解析出的文本
    bool m_hasPendingData = false;      ///< 是否有待处理的 dataComing 数据

    /// DDE 剪贴板守护进程 DBus 信息
    static constexpr const char *DaemonService = "org.deepin.dde.ClipboardLoader1";
    static constexpr const char *DaemonPath = "/org/deepin/dde/ClipboardLoader1";
    static constexpr const char *DaemonInterface = "org.deepin.dde.ClipboardLoader1";
    static constexpr const char *DaemonSignal = "dataComing";

    static constexpr int PreviewMaxLength = 32; ///< 用户要求的 head 预览长度
    static constexpr int DebounceMs = 100;      ///< 去抖时长，避免重复触发闪烁
};

#endif // CLIPBOARDMANAGER_H
