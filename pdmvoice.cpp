// SPDX-FileCopyrightText: 2019 Phillip Burgess for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// 基本的语音变换代码。此版本特定于使用 PDM 麦克风的 Adafruit MONSTER M4SK 板。

#if defined(ADAFRUIT_MONSTER_M4SK_EXPRESS)

#include "globals.h"
#include <SPI.h>
#include <Adafruit_ZeroPDMSPI.h>

#define MIN_PITCH_HZ   65    // 最小音高频率
#define MAX_PITCH_HZ 1600    // 最大音高频率
#define TYP_PITCH_HZ  175    // 典型音高频率

static void  voiceOutCallback(void); // 语音输出回调函数
static float actualPlaybackRate;     // 实际播放速率

// PDM 麦克风允许 1.0 到 3.25 MHz 的最大时钟（典型值为 2.4 MHz）。
// SPI 原生最大值为 24 MHz，因此可用速度为 12、6、3 MHz。
#define SPI_BITRATE 3000000  // SPI 比特率
// 3 MHz / 32 位 = 93,750 Hz 中断频率
// 2 次中断/样本 = 46,875 Hz 音频采样率
const float sampleRate = (float)SPI_BITRATE / 64.0;
// sampleRate 是浮点数，以防因子变化导致不能整除。
// 它不会随时间变化，只有 playbackRate 会变化。

// 尽管 SPI 库现在有一个选项可以在运行时获取 SPI 对象的 SERCOM 编号，
// 但中断处理程序必须在编译时声明...所以无论如何都需要提前知道 SERCOM #。
#define PDM_SPI            SPI2    // PDM 麦克风 SPI 外设
#define PDM_SERCOM_HANDLER SERCOM3_0_Handler

Adafruit_ZeroPDMSPI pdmspi(&PDM_SPI); // PDM SPI 对象

static float          playbackRate     = sampleRate; // 播放速率
static uint16_t      *recBuf           = NULL;       // 录音缓冲区
// recBuf 当前分配（在 voiceSetup() 中）用于两个最低音高的完整周期。
// 目前它并不真正需要这么大，但如果将来添加音高检测，这将变得更有用。
// 麦克风的 46,875 采样率，65 Hz 最低音高 -> 2884 字节。
static const uint16_t recBufSize       = (uint16_t)(sampleRate / (float)MIN_PITCH_HZ * 2.0 + 0.5); // 录音缓冲区大小
static int16_t        recIndex         = 0;          // 录音索引
static int16_t        playbackIndex    = 0;          // 播放索引

volatile uint16_t     voiceLastReading = 32768;      // 最后读取的语音值
volatile uint16_t     voiceMin         = 32768;      // 语音最小值
volatile uint16_t     voiceMax         = 32768;      // 语音最大值

#define MOD_MIN 20 // 最低支持的调制频率（越低 = 使用更多 RAM）
static uint8_t        modWave          = 0;     // 调制波形类型（无、正弦、方波、三角波、锯齿波）
static uint8_t       *modBuf           = NULL;  // 调制波形缓冲区
static uint32_t       modIndex         = 0;     // 当前在 modBuf 中的位置
static uint32_t       modLen           = 0;     // 基于 modFreq 的 modBuf 当前使用量

// 直接从录音循环缓冲区播放会产生可听见的咔嗒声，因为波形很少在缓冲区的开始和结束处对齐。
// 所以我们做的是，当播放索引可能超过或低于录音索引时，将播放索引向前或向后移动一定量，
// 并在短时间内从当前读取值插值到跳转后的读取值。在理想情况下，这个“一定量”应该是当前语音音高的一个波长...
// 但是...由于目前没有音高检测，我们使用一个固定的中间值：TYP_PITCH_HZ，默认为 175，
// 略低于典型女性说话音高范围，略高于典型男性说话音高范围。这在唱歌时就不适用了，
// 当然年轻人的说话音高会更高，这只是一个粗略的近似值。
static const uint16_t jump      = (int)(sampleRate / (float)TYP_PITCH_HZ + 0.5); // 跳转量
static const uint16_t interp    = jump / 4; // 插值时间 = 1/4 波形
static bool           jumping   = false;    // 是否正在跳转
static uint16_t       jumpCount = 1;        // 跳转计数
static int16_t        jumpThreshold;        // 跳转阈值
static int16_t        playbackIndexJumped;  // 跳转后的播放索引
static uint16_t       nextOut   = 2048;     // 下一个输出值

float voicePitch(float p); // 设置音高

// 启动音高变换（无参数）----------------------------------------

bool voiceSetup(bool modEnable) {

  // 为音频分配循环缓冲区
  if(NULL == (recBuf = (uint16_t *)malloc(recBufSize * sizeof(uint16_t)))) {
    return false; // 失败
  }

  // 如果启用，为语音调制分配缓冲区
  if(modEnable) {
    // 250 来自 voicePitch() 中的最小周期
    modBuf = (uint8_t *)malloc((int)(48000000.0 / 250.0 / MOD_MIN + 0.5));
    // 如果 malloc 失败，程序将继续运行，但没有调制
  }

  pdmspi.begin(sampleRate);  // 设置 PDM 麦克风
  analogWriteResolution(12); // 设置模拟输出
  voicePitch(1.0);           // 设置定时器间隔

  return true; // 成功
}

// 设置音高 ---------------------------------------------------------------

// 设置音高调整，数值越高 = 音高越高。0 < 音高 < 无穷大
// 0.5 = 频率减半（降低一个八度）
// 1.0 = 正常播放
// 2.0 = 频率加倍（提高一个八度）
// 可用的音高调整范围取决于各种硬件因素（SPI 速度、定时器/计数器分辨率等），
// 并且将返回实际应用的音高调整（在应用约束之后）。
float voicePitch(float p) {
  float   desiredPlaybackRate = sampleRate * p;
  // 裁剪到合理范围
  if(desiredPlaybackRate < 19200)       desiredPlaybackRate = 19200;  // ~0.41X
  else if(desiredPlaybackRate > 192000) desiredPlaybackRate = 192000; // ~4.1X
  arcada.timerCallback(desiredPlaybackRate, voiceOutCallback);
  // 在此假设 Arcada 将使用 1:1 预分频：
  int32_t period = (int32_t)(48000000.0 / desiredPlaybackRate);
  actualPlaybackRate = 48000000.0 / (float)period;
  p = (actualPlaybackRate / sampleRate); // 新音高
  jumpThreshold = (int)(jump * p + 0.5);
  return p;
}

// 设置增益 ----------------------------------------------------------------

void voiceGain(float g) {
  pdmspi.setMicGain(g); // 处理自己的裁剪
}

// 设置调制 ----------------------------------------------------------

// 这需要在调用 voicePitch() 之后调用 —— 调制表目前不会自动重新生成。也许以后会改变。

void voiceMod(uint32_t freq, uint8_t waveform) {
  if(modBuf) { // 如果没有分配调制缓冲区，则忽略
    if(freq < MOD_MIN) freq = MOD_MIN;
    modLen = (uint32_t)(actualPlaybackRate / freq + 0.5);
    if(modLen   < 2) modLen   = 2;
    if(waveform > 4) waveform = 4;
    modWave = waveform;
    yield();
    switch(waveform) {
     case 0: // 无
      break;
     case 1: // 方波
      memset(modBuf, 255, modLen / 2);
      memset(&modBuf[modLen / 2], 0, modLen - modLen / 2);
      break;
     case 2: // 正弦波
      for(uint32_t i=0; i<modLen; i++) {
        modBuf[i] = (int)((sin(M_PI * 2.0 * (float)i / (float)modLen) + 1.0) * 0.5 * 255.0 + 0.5);
      }
      break;
     case 3: // 三角波
      for(uint32_t i=0; i<modLen; i++) {
        modBuf[i] = (int)(fabs(0.5 - (float)i / (float)modLen) * 2.0 * 255.0 + 0.5);
      }
      break;
     case 4: // 锯齿波（递增）
      for(uint32_t i=0; i<modLen; i++) {
        modBuf[i] = (int)((float)i / (float)(modLen - 1) * 255.0 + 0.5);
      }
      break;
    }
  }
}

// 中断处理程序 ------------------------------------------------------

void PDM_SERCOM_HANDLER(void) {
  uint16_t micReading = 0;
  if(pdmspi.decimateFilterWord(&micReading, true)) {
    // 所以，理论是，将来可以在这里添加一些基本的音高检测，
    // 这可以用来改善播放中断中的接缝过渡（可能还有其他事情，
    // 比如动态调整播放速率以实现单调和其他效果）。
    // 实际上，语音上的可用音高检测结果是音频处理中几乎无法解决的问题之一...
    // 如果你在想“哦，只需计算零交叉”“只需使用 FFT”，
    // 这真的不是那么简单，相信我，我已经阅读了所有相关内容，语音波形很复杂。
    // 这里有一些“可能足够好的近似值，适用于一个粗糙的微控制器项目”的代码，
    // 但现在为了在合理的时间内将一些不坏的东西交到人们手中，它被删除了。
    if(++recIndex >= recBufSize) recIndex = 0;
    recBuf[recIndex] = micReading;

    // 外部代码可以使用 voiceLastReading 的值，如果你想做一个近似的实时波形显示，
    // 或者基于麦克风输入的动态增益调整，或其他东西。
    // 这不会给你录音缓冲区中的每一个样本，按顺序一个接一个...
    // 它只是在你轮询它之前存储的最后一件事，但可能仍然有一些用途。
    voiceLastReading = micReading;

    // 同样，用户代码可以 extern 这些变量并监控峰峰值范围。
    // 它们在语音代码中永远不会被重置，用户代码有责任定期将两者重置为 32768。
    if(micReading < voiceMin)      voiceMin = micReading;
    else if(micReading > voiceMax) voiceMax = micReading;
  }
}

static void voiceOutCallback(void) {

  // 调制是在输出上完成的（而不是在输入上），因为对调制的输入进行音高变换会导致奇怪的波形不连续性。
  // 这确实需要在每次音高变化时重新计算调制表。
  if(modWave) {
    nextOut = (((int32_t)nextOut - 2048) * (modBuf[modIndex] + 1) / 256) + 2048;
    if(++modIndex >= modLen) modIndex = 0;
  }

  // 尽快进行模拟写入，以确保输出时间一致
  analogWrite(A0, nextOut);
  analogWrite(A1, nextOut);
  // 然后我们可以花任意时间处理下一个周期...

  if(++playbackIndex >= recBufSize) playbackIndex = 0;

  if(jumping) {
    // 波形混合过渡正在进行中
    uint32_t w1 = 65536UL * jumpCount / jump, // 将 playbackIndexJumped 向上斜坡（14 位）
             w2 = 65536UL - w1;               // 将 playbackIndex 向下斜坡（14 位）
    nextOut = (recBuf[playbackIndexJumped] * w1 + recBuf[playbackIndex] * w2) >> 20; // 28 位结果 -> 12 位
    if(++jumpCount >= jump) {
      playbackIndex = playbackIndexJumped;
      jumpCount     = 1;
      jumping       = false;
    } else {
      if(++playbackIndexJumped >= recBufSize) playbackIndexJumped = 0;
    }
  } else {
    nextOut = recBuf[playbackIndex] >> 4; // 16 -> 12 位
    if(playbackRate >= sampleRate) { // 加速
      // 播放可能会超过录音，需要定期后退
      int16_t dist = (recIndex >= playbackIndex) ?
        (recIndex - playbackIndex) : (recBufSize - (playbackIndex - recIndex));
      if(dist <= jumpThreshold) {
        playbackIndexJumped = playbackIndex - jump;
        if(playbackIndexJumped < 0) playbackIndexJumped += recBufSize;
        jumping             = true;
      }
    } else { // 减速
      // 播放可能会低于录音，需要定期前进
      int16_t dist = (playbackIndex >= recIndex) ?
        (playbackIndex - recIndex) : (recBufSize - 1 - (recIndex - playbackIndex));
      if(dist <= jumpThreshold) {
        playbackIndexJumped = (playbackIndex + jump) % recBufSize;
        jumping             = true;
      }
    }
  }
}

#endif // ADAFRUIT_MONSTER_M4SK_EXPRESS
