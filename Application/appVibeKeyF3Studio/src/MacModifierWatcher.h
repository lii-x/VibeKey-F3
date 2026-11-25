#ifndef MACMODIFIERWATCHER_H
#define MACMODIFIERWATCHER_H

#include <functional>

/* ===== 11-24: macOS 物理修饰键左右手追踪 =====
 * Qt 的 QKeyEvent::modifiers() 不区分左右; 用 AppKit 的本地 NSEvent 监视器
 * (NSEventMaskFlagsChanged, 只接收送到本应用的键盘事件)读取 keyCode,
 * 精确得到左/右 Ctrl、Shift、Option、Command。
 *
 * 为什么不用全局 CGEventTap / IOHIDManager:
 *   两者都需要辅助功能或输入监控授权(TCC), 对配置工具过重; 而 VibeKey 是
 *   标准 HID 键盘, 捕获组合键时本应用一定在前台, 本地监视器已经够用。
 *
 * 事件接入见 MacModifierWatcher.mm; 纯状态机见 MacModifierState。
 * 位图与固件/HID 语义一致(0x01 LCtrl ... 0x80 RGui)。 */
class MacModifierWatcher
{
public:
    /* 位图变化回调(每次物理修饰键按下/抬起各一次, 主线程)。
     * 捕获裸修饰键时用它做事件驱动的峰值/宽限收尾, 快速轻点也不会漏。 */
    using BitsCallback = std::function<void(int bits)>;

    /* 安装/卸载本地监视器(幂等; 需在 QApplication 创建之后调用) */
    static void install(BitsCallback onChange = {});
    static void uninstall();

    /* 当前物理按下的修饰键位图(捕获时优先用它, 取不到再退回 Qt 的 generic 位) */
    static int currentBits();
};

#endif // MACMODIFIERWATCHER_H
