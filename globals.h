// SPDX-FileCopyrightText: 2019 Phillip Burgess for Adafruit Industries
//
// SPDX-License-Identifier: MIT

//34567890123456789012345678901234567890123456789012345678901234567890123456

//#include "Adafruit_Arcada.h"
#include "DMAbuddy.h" // DMA 问题修复类

#if defined(GLOBAL_VAR) // 仅在 .ino 文件中定义
  #define GLOBAL_INIT(X) = (X)
  #define INIT_EYESTRUCTS
#else
  #define GLOBAL_VAR extern
  #define GLOBAL_INIT(X)
#endif

#if defined(ARCADA_LEFTTFT_SPI) // MONSTER M4SK 或自定义 Arcada 设置
  #define NUM_EYES 2
  // MONSTER M4SK 光线传感器默认不启用。
  // 在配置中使用 "lightSensor : 102"
#else
  #define NUM_EYES 1
#endif

// 全局变量 --------------------------------------------------------

GLOBAL_VAR Adafruit_Arcada arcada;

GLOBAL_VAR bool      showSplashScreen    GLOBAL_INIT(true);   // 设置为 false 以禁用启动画面

#define MAX_DISPLAY_SIZE 240
GLOBAL_VAR int       DISPLAY_SIZE        GLOBAL_INIT(240);    // 假设显示为 240x240
GLOBAL_VAR uint32_t  stackReserve        GLOBAL_INIT(5192);   // 参见图像加载代码
GLOBAL_VAR int       eyeRadius           GLOBAL_INIT(0);      // 0 = 在 loadConfig() 中使用默认值
GLOBAL_VAR int       eyeDiameter;                             // 稍后根据 eyeRadius 计算
GLOBAL_VAR int       irisRadius          GLOBAL_INIT(60);     // 屏幕像素中的近似大小
GLOBAL_VAR int       slitPupilRadius     GLOBAL_INIT(0);      // 0 = 圆形瞳孔
GLOBAL_VAR uint8_t   eyelidIndex         GLOBAL_INIT(0x00);   // 从表中：learn.adafruit.com/assets/61921
GLOBAL_VAR uint16_t  eyelidColor         GLOBAL_INIT(0x0000); // 将 eyelidIndex 扩展为 16 位
// mapRadius 是极坐标到矩形地图的一个象限的大小，
// 以像素为单位。为了覆盖眼睛的前半球，这应该至少为
// (eyeRadius * Pi / 2) —— 但是，为了提供一些超出前半球的覆盖，
// 'coverage' 的值决定了这个地图围绕眼睛的覆盖范围。
// 0.0 = 无覆盖，0.5 = 前半球，1.0 = 整个球体。不要将其设置为 1.0 ——
// 眼睛的远背面实际上永远不会被看到，因为我们使用的是位移映射技巧，
// 而不是实际旋转球体，加上生成的地图将占用大量 RAM，可能超过我们拥有的 RAM。
// 这里的默认值 0.6 提供了一个良好的覆盖范围和 RAM 之间的平衡，
// 偶尔会看到眼睛背面的新月形颜色（巩膜纹理地图可以设计为与之混合）。
// eyeRadius 在 loadConfig() 中计算为 eyeRadius * Pi * coverage ——
// 如果 eyeRadius 为 125，coverage 为 0.6，mapRadius 将为 236 像素，
// 生成的极角/距离地图将总共占用约 111K RAM。
GLOBAL_VAR float     coverage            GLOBAL_INIT(0.6);
GLOBAL_VAR int       mapRadius;          // 在 loadConfig() 中计算
GLOBAL_VAR int       mapDiameter;        // 在 loadConfig() 中计算
GLOBAL_VAR uint8_t  *displace            GLOBAL_INIT(NULL);
GLOBAL_VAR uint8_t  *polarAngle          GLOBAL_INIT(NULL);
GLOBAL_VAR int8_t   *polarDist           GLOBAL_INIT(NULL);
GLOBAL_VAR uint8_t   upperOpen[MAX_DISPLAY_SIZE];
GLOBAL_VAR uint8_t   upperClosed[MAX_DISPLAY_SIZE];
GLOBAL_VAR uint8_t   lowerOpen[MAX_DISPLAY_SIZE];
GLOBAL_VAR uint8_t   lowerClosed[MAX_DISPLAY_SIZE];
GLOBAL_VAR char     *upperEyelidFilename GLOBAL_INIT(NULL);
GLOBAL_VAR char     *lowerEyelidFilename GLOBAL_INIT(NULL);
GLOBAL_VAR uint16_t  lightSensorMin      GLOBAL_INIT(0);
GLOBAL_VAR uint16_t  lightSensorMax      GLOBAL_INIT(1023);
GLOBAL_VAR float     lightSensorCurve    GLOBAL_INIT(1.0);
GLOBAL_VAR float     irisMin             GLOBAL_INIT(0.45);
GLOBAL_VAR float     irisRange           GLOBAL_INIT(0.35);
GLOBAL_VAR bool      tracking            GLOBAL_INIT(true);
GLOBAL_VAR float     trackFactor         GLOBAL_INIT(0.5);
GLOBAL_VAR uint32_t  gazeMax             GLOBAL_INIT(3000000); // 主要眼睛运动的最大等待时间（微秒）

// 随机眼睛运动：由基础项目提供，但可由用户代码覆盖。
GLOBAL_VAR bool      moveEyesRandomly    GLOBAL_INIT(true);   // 设置为 false 以禁用随机眼睛运动，让用户代码控制
GLOBAL_VAR float     eyeTargetX          GLOBAL_INIT(0.0);  // 然后在 user_loop 中连续设置这些值。
GLOBAL_VAR float     eyeTargetY          GLOBAL_INIT(0.0);  // 范围为 -1.0 到 +1.0。

// 引脚定义将在此处

GLOBAL_VAR int8_t    lightSensorPin      GLOBAL_INIT(-1);
GLOBAL_VAR int8_t    blinkPin            GLOBAL_INIT(-1); // 手动双眼眨眼引脚（-1 = 无）

#if defined(ADAFRUIT_MONSTER_M4SK_EXPRESS)
  GLOBAL_VAR int8_t  boopPin             GLOBAL_INIT(A2);
#else
  GLOBAL_VAR int8_t  boopPin             GLOBAL_INIT(-1);
#endif
GLOBAL_VAR uint32_t  boopThreshold       GLOBAL_INIT(17500);

#if defined(ADAFRUIT_MONSTER_M4SK_EXPRESS)
GLOBAL_VAR bool      voiceOn             GLOBAL_INIT(false);
GLOBAL_VAR float     currentPitch        GLOBAL_INIT(1.0);
GLOBAL_VAR float     defaultPitch        GLOBAL_INIT(1.0);
GLOBAL_VAR float     gain                GLOBAL_INIT(1.0);
GLOBAL_VAR uint8_t   waveform            GLOBAL_INIT(0);
GLOBAL_VAR uint32_t  modulate            GLOBAL_INIT(30); // Dalek 音高
#endif

// 眼睛相关结构 --------------------------------------------------

// 眼睛是按列渲染的，使用 DMA 在计算下一列时发出一列数据，
// 在两个列结构之间交替（无论如何，缓冲整个 240x240 屏幕的 RAM 几乎不够）。
// 每个正在渲染/发出的列使用 1 到 3 个链接的 DMA 描述符，
// 通常包含：1) 眼睑区域“下方”的背景像素，
// 2) 眼睛内部的渲染像素（绘制在 renderBuf[] 扫描线缓冲区中，
// 分配为 240 像素以匹配屏幕大小，尽管通常只使用一部分），
// 和 3) 眼睑区域“上方”的更多背景像素。
#if NUM_EYES > 1
  #define NUM_DESCRIPTORS 1 // 参见下面的注释
#else
  #define NUM_DESCRIPTORS 3
#endif
  // 重要提示：原始计划（如上所述，使用动态描述符列表）被硅片错误（记录在 SAMD51 勘误表中）
  // 破坏了，当在多个通道上使用链接描述符时。目前的解决方法是跳过眼睑优化，
  // 完全缓冲/渲染每一行，使用单个描述符。这对于单眼不是问题（因为只有一个通道），
  // 我们仍然可以在 HalloWing M4 上使用此技巧。
typedef struct {
  uint16_t       renderBuf[MAX_DISPLAY_SIZE]; // 像素缓冲区
  DmacDescriptor descriptor[NUM_DESCRIPTORS]; // DMA 描述符列表
} columnStruct;

// 使用简单的状态机来控制眼睛的眨眼/眨眼：
#define NOBLINK 0       // 当前未进行眨眼
#define ENBLINK 1       // 眼睑正在闭合
#define DEBLINK 2       // 眼睑正在打开
typedef struct {
  uint8_t  state;       // NOBLINK/ENBLINK/DEBLINK
  uint32_t duration;    // 眨眼状态的持续时间（微秒）
  uint32_t startTime;   // 上次状态更改的时间（微秒）
} eyeBlink;

// 虹膜和巩膜纹理地图的数据
typedef struct {
  char     *filename;
  float     spin;       // RPM * 1024.0
  uint16_t  color;
  uint16_t *data;
  uint16_t  width;
  uint16_t  height;
  uint16_t  startAngle; // 初始旋转 0-1023 逆时针
  uint16_t  angle;      // 当前旋转 0-1023 逆时针
  uint16_t  mirror;     // 0 = 正常，1023 = 翻转 X 轴
  uint16_t  iSpin;      // 每帧固定整数旋转，覆盖 'spin' 值
} texture;

// 每只眼睛使用以下结构。每只眼睛必须位于其自己的 SPI 总线上，
// 具有独立的控制线（与 Uncanny Eyes 代码不同，后者它们轮流使用一个总线）。
// 两个如上所述的列结构，然后是大量 DMA 细节和动画状态数据。
typedef struct {
  // 这些值在下面的表中初始化：
  const char      *name;         // 用于加载每只眼睛的配置
  SPIClass        *spi;          // 指向相应 SPI 对象的指针
  int8_t           cs;           // CS 引脚 #
  int8_t           dc;           // DC 引脚 #
  int8_t           rst;          // RST 引脚 #（-1 如果使用 Seesaw）
  int8_t           winkPin;      // 手动眼睛眨眼控制（-1 = 无）
  // 其余值在代码中初始化：
  columnStruct     column[2];    // 交替列结构 A/B
  Adafruit_SPITFT *display;      // 指向显示对象的指针
  DMAbuddy         dma;          // 带有 fix() 函数的 DMA 通道对象
  DmacDescriptor  *dptr;         // DMA 通道描述符指针
  uint32_t         dmaStartTime; // 用于 DMA 超时处理程序
  uint8_t          colNum;       // 列计数器（0-239）
  uint8_t          colIdx;       // 交替 0/1 索引到 column[] 数组
  bool             dma_busy;     // true = DMA 传输正在进行
  bool             column_ready; // true = 下一列已渲染
  uint16_t         pupilColor;   // 16 位 565 RGB，大端格式
  uint16_t         backColor;    // 16 位 565 RGB，大端格式
  texture          iris;         // 虹膜纹理地图
  texture          sclera;       // 巩膜纹理地图
  uint8_t          rotation;     // 屏幕旋转（GFX 库）

  // 从 Uncanny Eyes 代码继承的内容。现在需要独立于每只眼睛，
  // 因为我们逐行交替绘制两只眼睛，而不是完整绘制每只眼睛。
  // 这可能会稍作清理，但目前...
  eyeBlink blink;
  float    eyeX, eyeY;  // 保存每只眼睛以避免撕裂
  float    pupilFactor; // 同上
  float    blinkFactor;
  float    upperLidFactor, lowerLidFactor;
} eyeStruct;

#ifdef INIT_EYESTRUCTS
  eyeStruct eye[NUM_EYES] = {
  #if (NUM_EYES > 1)
    // name     spi  cs  dc rst wink
    { "right", &ARCADA_TFT_SPI , ARCADA_TFT_CS,  ARCADA_TFT_DC, ARCADA_TFT_RST, -1 },
    { "left" , &ARCADA_LEFTTFT_SPI, ARCADA_LEFTTFT_CS, ARCADA_LEFTTFT_DC, ARCADA_LEFTTFT_RST, -1 } };
  #else
    {  NULL  , &ARCADA_TFT_SPI, ARCADA_TFT_CS, ARCADA_TFT_DC, ARCADA_TFT_RST, -1 } };
  #endif
#else
  extern eyeStruct eye[];
#endif

// 函数原型 -----------------------------------------------------

// file.cpp 中的函数
extern int             file_setup(bool msc=true);
extern void            handle_filesystem_change();
// 当文件系统内容更改时设置为 true。
// 最初设置为 true，以便程序以“更改”任务开始。
extern bool            filesystem_change_flag GLOBAL_INIT(true);
extern void            loadConfig(char *filename);
extern ImageReturnCode loadEyelid(char *filename, uint8_t *minArray, uint8_t *maxArray, uint8_t init, uint32_t maxRam);
extern ImageReturnCode loadTexture(char *filename, uint16_t **data, uint16_t *width, uint16_t *height, uint32_t maxRam);

// memory.cpp 中的函数
extern uint32_t        availableRAM(void);
extern uint32_t        availableNVM(void);
extern uint8_t        *writeDataToFlash(uint8_t *src, uint32_t len);

// pdmvoice.cpp 中的函数
#if defined(ADAFRUIT_MONSTER_M4SK_EXPRESS)
extern bool              voiceSetup(bool modEnable);
extern float             voicePitch(float p);
extern void              voiceGain(float g);
extern void              voiceMod(uint32_t freq, uint8_t waveform);
extern volatile uint16_t voiceLastReading;
#endif // ADAFRUIT_MONSTER_M4SK_EXPRESS

// tablegen.cpp 中的函数
extern void            calcDisplacement(void);
extern void            calcMap(void);
extern float           screen2map(int in);
extern float           map2screen(int in);

// user.cpp 中的函数
extern void            user_setup(void);
extern void            user_loop(void);