#include "MacModifierWatcher.h"
#include "MacModifierState.h"

#import <Cocoa/Cocoa.h>

/* ===== 11-24: macOS 物理修饰键左右手追踪(AppKit 接入层) =====
 * addLocalMonitorForEventsMatchingMask 只接收送到本应用的事件, 不触发任何
 * TCC 授权弹窗; 捕获组合键时本应用在前台, 恰好能满足。回调在主线程执行,
 * 且先于 Qt 处理该事件, 所以 keyDown 到达 KeyConfigManager 时状态已更新。
 * 返回原事件 -> 不吞事件, 不影响正常输入。 */

namespace {

MacModifierState g_state;
MacModifierWatcher::BitsCallback g_callback;
id g_monitor = nil;
id g_resignObserver = nil;
id g_activeObserver = nil;

/* keyCode -> 该修饰键类型的 generic flag(用于判断类型是否仍按下) */
NSEventModifierFlags genericFlagForKeyCode(unsigned short keyCode)
{
    switch (keyCode) {
    case 0x3B: case 0x3E: return NSEventModifierFlagControl;  // L/R Control
    case 0x38: case 0x3C: return NSEventModifierFlagShift;    // L/R Shift
    case 0x3A: case 0x3D: return NSEventModifierFlagOption;   // L/R Option
    case 0x37: case 0x36: return NSEventModifierFlagCommand;  // L/R Command
    default:   return 0;                                      // CapsLock/Fn 等
    }
}

void resetState()
{
    g_state.reset();
}

}   // namespace

void MacModifierWatcher::install(BitsCallback onChange)
{
    g_callback = std::move(onChange);
    if (g_monitor)
        return;

    g_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskFlagsChanged
                                                     handler:^NSEvent *(NSEvent *event) {
        const unsigned short keyCode = event.keyCode;
        const NSEventModifierFlags flags =
                event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
        const NSEventModifierFlags typeFlag = genericFlagForKeyCode(keyCode);
        if (typeFlag != 0) {
            const int before = g_state.bits();
            g_state.update(keyCode, (flags & typeFlag) != 0);
            const int after = g_state.bits();
            if (after != before && g_callback)
                g_callback(after);
        }
        return event;
    }];

    /* 失焦期间收不到修饰键释放事件, 清空避免位粘滞;
     * 回到前台时也清空 —— 捕获总是从一次点击开始, 不依赖 app 非活跃期间的状态。 */
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    g_resignObserver = [center addObserverForName:NSApplicationDidResignActiveNotification
                                           object:nil
                                            queue:[NSOperationQueue mainQueue]
                                       usingBlock:^(NSNotification *__unused note) {
        resetState();
    }];
    g_activeObserver = [center addObserverForName:NSApplicationDidBecomeActiveNotification
                                           object:nil
                                            queue:[NSOperationQueue mainQueue]
                                       usingBlock:^(NSNotification *__unused note) {
        resetState();
    }];
}

void MacModifierWatcher::uninstall()
{
    if (g_monitor) {
        [NSEvent removeMonitor:g_monitor];
        g_monitor = nil;
    }
    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    if (g_resignObserver) {
        [center removeObserver:g_resignObserver];
        g_resignObserver = nil;
    }
    if (g_activeObserver) {
        [center removeObserver:g_activeObserver];
        g_activeObserver = nil;
    }
    g_callback = nullptr;
    resetState();
}

int MacModifierWatcher::currentBits()
{
    return g_state.bits();
}
