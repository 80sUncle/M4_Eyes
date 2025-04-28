// SPDX-FileCopyrightText: 2019 Phillip Burgess for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// 非常罕见的情况下，Adafruit_ZeroDMA 中的 jobStatus 成员会失去同步，
// 导致屏幕 DMA 更新锁定。这是一个临时的解决方案。
// jobStatus 是 Adafruit_ZeroDMA 的一个受保护成员，我们无法在 sketch 中直接重置它，
// 但子类可以。因此，我们有一个最小的子类，带有一个 DMA 通道开关和 jobStatus 重置函数。

class DMAbuddy : public Adafruit_ZeroDMA { // 继承自 Adafruit_ZeroDMA
 public:
  // 当检测到 DMA 停顿时调用此函数：
  void fix(void) {
    // 我们实际上只是通过关闭再打开通道来修复它...
    DMAC->Channel[channel].CHCTRLA.bit.ENABLE = 0; // 禁用通道
    DMAC->Channel[channel].CHCTRLA.bit.ENABLE = 1; // 启用通道
    jobStatus = DMA_STATUS_OK; // 恢复正常！
  }
};