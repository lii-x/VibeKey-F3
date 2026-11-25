#ifndef MACMODIFIERSTATE_H
#define MACMODIFIERSTATE_H

/* ===== 11-24: macOS 物理修饰键左右手状态机 =====
 * Qt 的 QKeyEvent::modifiers() 只有类型(Shift/Ctrl/Alt/Meta), 不区分左右,
 * 捕获右 Option / 右 Command 时只能落到默认左手位。macOS 的 flagsChanged
 * 事件(NSEvent)带 keyCode, 能精确指出是哪一个物理修饰键变化。
 *
 * 本类只做纯逻辑(不依赖 AppKit, 便于单测); AppKit 事件接入见
 * MacModifierWatcher.mm。
 *
 * 为什么不能只看 modifierFlags 判断按下/抬起:
 *   同时按住左右同类型修饰键时(如两个 Shift), 抬起任意一个的瞬间该类型的
 *   generic flag 仍然是"按下"。因此按下/抬起只能按 "keyCode 是否已在集合中"
 *   翻转; generic flag 只用于"整类释放"时清理残留(失焦/漏事件自愈)。 */
class MacModifierState
{
public:
    /* macOS 虚拟键码(kVK_*) -> HID 修饰位(与固件语义一致):
     * 0x01 LCtrl / 0x02 LShift / 0x04 LAlt / 0x08 LGui
     * 0x10 RCtrl / 0x20 RShift / 0x40 RAlt / 0x80 RGui */
    static int bitForKeyCode(unsigned short keyCode);

    /* 处理一次 flagsChanged, 返回最新位图。
     * keyCode:    NSEvent.keyCode(物理修饰键标识)
     * typeActive: 该修饰键类型当前是否仍按下(NSEventModifierFlagShift/...) */
    int update(unsigned short keyCode, bool typeActive);

    int bits() const { return m_bits; }
    void reset();

private:
    static int typeForKeyCode(unsigned short keyCode);
    void recompute();

    unsigned short m_down[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int m_count = 0;
    int m_bits = 0;
};

#endif // MACMODIFIERSTATE_H
