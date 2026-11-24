#include "MacModifierState.h"

/* macOS 虚拟键码与键盘布局无关(HIToolbox/Events.h 的 kVK_* 常量),
 * 这里直接写数值并标注常量名, 保持本文件为纯 C++。 */
int MacModifierState::bitForKeyCode(unsigned short keyCode)
{
    switch (keyCode) {
    case 0x3B: return 0x01;   // kVK_Control      -> LCtrl
    case 0x38: return 0x02;   // kVK_Shift        -> LShift
    case 0x3A: return 0x04;   // kVK_Option       -> LAlt
    case 0x37: return 0x08;   // kVK_Command      -> LGui(Command)
    case 0x3E: return 0x10;   // kVK_RightControl -> RCtrl
    case 0x3C: return 0x20;   // kVK_RightShift   -> RShift
    case 0x3D: return 0x40;   // kVK_RightOption  -> RAlt
    case 0x36: return 0x80;   // kVK_RightCommand -> RGui(Command)
    default:   return 0;      // CapsLock(0x39)/Fn(0x3F) 等不参与快捷键
    }
}

/* 修饰键类型(用于"整类释放"清理残留): 1=Control 2=Shift 3=Option 4=Command */
int MacModifierState::typeForKeyCode(unsigned short keyCode)
{
    switch (keyCode) {
    case 0x3B: case 0x3E: return 1;
    case 0x38: case 0x3C: return 2;
    case 0x3A: case 0x3D: return 3;
    case 0x37: case 0x36: return 4;
    default: return 0;
    }
}

int MacModifierState::update(unsigned short keyCode, bool typeActive)
{
    const int type = typeForKeyCode(keyCode);
    if (type == 0)
        return m_bits;

    int idx = -1;
    for (int i = 0; i < m_count; ++i) {
        if (m_down[i] == keyCode) {
            idx = i;
            break;
        }
    }

    if (idx >= 0) {
        /* 本键已在集合中 -> 本次是抬起(同类另一只手仍按住时 typeActive 仍为真) */
        m_down[idx] = m_down[--m_count];
    } else if (typeActive) {
        /* 类型仍按下、本键不在集合 -> 本键刚按下(如先按左 Shift 再按右 Shift) */
        if (m_count < int(sizeof(m_down) / sizeof(m_down[0])))
            m_down[m_count++] = keyCode;
    }
    /* 本键不在集合且类型已释放: 失焦期间漏了按下事件, 忽略 */

    /* 整类释放: 清掉同类残留位, 避免漏事件导致粘滞 */
    if (!typeActive) {
        for (int i = 0; i < m_count; ) {
            if (typeForKeyCode(m_down[i]) == type)
                m_down[i] = m_down[--m_count];
            else
                ++i;
        }
    }

    recompute();
    return m_bits;
}

void MacModifierState::recompute()
{
    int bits = 0;
    for (int i = 0; i < m_count; ++i)
        bits |= bitForKeyCode(m_down[i]);
    m_bits = bits;
}

void MacModifierState::reset()
{
    m_count = 0;
    m_bits = 0;
}
