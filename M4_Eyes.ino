// SPDX-FileCopyrightText: 2019 Phillip Burgess for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// 这是为 Adafruit MONSTER M4SK 和 HALLOWING M4 开发板设计的动画眼睛代码。
// 该代码与这些开发板的资源紧密耦合（一个或两个 ST7789 240x240 像素的 TFT 显示屏，
// 分别位于不同的 SPI 总线上，以及一个 SAMD51 微控制器），不像之前的 "Uncanny Eyes"
// 项目那样具有通用性（更适合 SAMD21 芯片或 Teensy 3.X 和 128x128 TFT 或 OLED 屏幕，
// 单 SPI 总线）。

// 重要提示：在极少数情况下，当运行此代码时同时连接到 USB，开发板可能会被“砖化”。
// 快速闪烁的状态 LED 表示文件系统已损坏。如果发生这种情况，请安装 CircuitPython
// 以重新初始化文件系统，复制您的眼睛文件（请备份！），然后重新上传此代码。
// 这种情况在高优化设置（高于 -O3）时更常见，但没有直接的因果关系。确切原因尚未找到...
// 可能是 yield() 调用不足，或者是 Arcada 库或 USB 处理代码中的某些罕见对齐问题。

// 在继续之前，让我们谈谈坐标系。从外部观察者的角度来看，这些开发板上的显示屏是
// 按列从左到右渲染的，而不是像 Uncanny Eyes 和大多数其他图形密集型代码那样按行渲染。
// 发现沿此轴进行动画处理眼睑要容易得多。通过将屏幕设置为 ROTATION 3（相对于默认
// 方向逆时针旋转 90 度），可以轻松实现“列主序”显示。这将 (0,0) 放置在显示屏的
// 左下角，+X 向上，+Y 向右 —— 因此，从概念上讲，只需交换轴，您就拥有了传统的
// 笛卡尔坐标系，三角函数按预期工作，代码在大多数地方都以这种方式“思考”。
// 由于旋转是在硬件中完成的...从显示驱动程序的角度来看，可以将这些视为“水平”“扫描线”，
// 并且眼睛是侧向绘制的，具有左右眼睑而不是上下眼睑。在这里提到这一点是因为代码中可能
// 仍然存在一些注释和/或变量，我将其称为“扫描线”，尽管在视觉/空间上这些是列。
// 我会尽力在不同地方注释局部坐标系。（由 Adafruit_ImageReader 加载的任何光栅图像
// 都以典型的 +Y = 向下顺序引用。）

// 哦，还有，“左眼”和“右眼”指的是怪物的左右眼。从观察者的角度来看，看着怪物时，
// “右眼”在左边。

#define GLOBAL_VAR
#include "globals.h"

// 适用于所有眼睛的全局状态（不是每只眼睛）：
bool     eyeInMotion = false; // 眼睛是否在移动
float    eyeOldX, eyeOldY, eyeNewX, eyeNewY; // 眼睛的旧位置和新位置
uint32_t eyeMoveStartTime = 0L; // 眼睛移动开始时间
int32_t  eyeMoveDuration  = 0L; // 眼睛移动持续时间
uint32_t lastSaccadeStop  = 0L; // 上次眼跳停止时间
int32_t  saccadeInterval  = 0L; // 眼跳间隔

// 一些草率的眼睛状态内容，一些是从旧的眼睛代码中继承的...
// 有点混乱且命名不当，稍后会清理/移动等。
uint32_t timeOfLastBlink         = 0L, // 上次眨眼时间
         timeToNextBlink         = 0L; // 下次眨眼时间
int      xPositionOverMap        = 0; // 在地图上的 X 位置
int      yPositionOverMap        = 0; // 在地图上的 Y 位置
uint8_t  eyeNum                  = 0; // 当前处理的眼睛编号
uint32_t frames                  = 0; // 帧数
uint32_t lastFrameRateReportTime = 0; // 上次帧率报告时间
uint32_t lastLightReadTime       = 0; // 上次光线读取时间
float    lastLightValue          = 0.5; // 上次光线值
double   irisValue               = 0.5; // 虹膜值
int      iPupilFactor            = 42; // 瞳孔因子
uint32_t boopSum                 = 0, // 触摸传感器总和
         boopSumFiltered         = 0; // 过滤后的触摸传感器总和
bool     booped                  = false; // 是否被触摸
int      fixate                  = 7; // 注视点
uint8_t  lightSensorFailCount    = 0; // 光线传感器失败计数

// 用于自主虹膜缩放
#define  IRIS_LEVELS 7 // 虹膜级别
float    iris_prev[IRIS_LEVELS] = { 0 }; // 前一个虹膜值
float    iris_next[IRIS_LEVELS] = { 0 }; // 下一个虹膜值
uint16_t iris_frame = 0; // 虹膜帧

// 在每个 SPI DMA 传输后调用的回调 - 设置一个标志，表示可以立即发出下一行图形。
static void dma_callback(Adafruit_ZeroDMA *dma) {
  // 可以为每个 DMA 通道分配自己的回调函数（比这种通道到眼睛的查找方式节省一些周期），
  // 但这样编写是为了扩展到所需数量的眼睛（如果移植到 Grand Central 等设备，最多每个 SERCOM 一个）。
  for(uint8_t e=0; e<NUM_EYES; e++) {
    if(dma == &eye[e].dma) {
      eye[e].dma_busy = false;
      return;
    }
  }
}

// >50MHz SPI 很有趣，但太不稳定，无法依赖
//#if F_CPU < 200000000
// #define DISPLAY_FREQ   (F_CPU / 2)
// #define DISPLAY_CLKSRC SERCOM_CLOCK_SOURCE_FCPU
//#else
 #define DISPLAY_FREQ   50000000 // 显示频率
 #define DISPLAY_CLKSRC SERCOM_CLOCK_SOURCE_100M // 显示时钟源
//#endif

SPISettings settings(DISPLAY_FREQ, MSBFIRST, SPI_MODE0); // SPI 设置

// 通过 SPI 发出一行扫描线（DISPLAY_SIZE 像素 x 16 位）所需的时间是一个已知（ish）的量。
// DMA 调度程序并不总是完全确定性的...特别是在启动时，当事情进入缓存时。
// 偶尔，某些东西（尚不清楚）会导致 SPI DMA 卡住。这种情况很容易检查...
// 代码需要定期等待 DMA 传输完成，我们可以使用 micros() 函数来确定是否花费了
// 比预期长得多的时间（使用 4 的因子 - 下面的“4000”，以允许缓存/调度余量）。
// 如果是这样，那就是我们的信号，表明可能出了问题，我们采取规避措施，重置受影响的 DMA 通道（DMAbuddy::fix()）。
#define DMA_TIMEOUT (uint32_t)((DISPLAY_SIZE * 16 * 4000) / (DISPLAY_FREQ / 1000))

// 读取触摸传感器的值
static inline uint16_t readBoop(void) {
  uint16_t counter = 0;
  pinMode(boopPin, OUTPUT);
  digitalWrite(boopPin, HIGH);
  pinMode(boopPin, INPUT);
  while(digitalRead(boopPin) && (++counter < 1000));
  return counter;
}

// 简单的错误处理程序。将消息打印到串行监视器，闪烁 LED。
void fatal(const char *message, uint16_t blinkDelay) {
  Serial.begin(9600);
  Serial.println(message);
  for(bool ledState = HIGH;; ledState = !ledState) {
    digitalWrite(LED_BUILTIN, ledState);
    delay(blinkDelay);
  }
}

#include <unistd.h> // sbrk() function

// 获取可用 RAM
uint32_t availableRAM(void) {
  char top;                      // 局部变量推入堆栈
  return &top - (char *)sbrk(0); // 堆栈顶部减去堆的末尾
}

// SETUP 函数 - 在程序启动时调用一次 ---------------------------

void setup() {
  if(!arcada.arcadaBegin())     fatal("Arcada 初始化失败！", 100); // 初始化 Arcada
#if defined(USE_TINYUSB)
  if(!arcada.filesysBeginMSD()) fatal("未找到文件系统！", 250); // 初始化文件系统
#else
  if(!arcada.filesysBegin())    fatal("未找到文件系统！", 250); // 初始化文件系统
#endif

  user_setup(); // 用户自定义设置

  arcada.displayBegin(); // 初始化显示

  // 尽快关闭背光，屏幕初始化并清除后会打开
  arcada.setBacklight(0);

  DISPLAY_SIZE = min(ARCADA_TFT_WIDTH, ARCADA_TFT_HEIGHT); // 显示尺寸

  Serial.begin(115200); // 初始化串口
  //while(!Serial) yield();

  Serial.printf("启动时可用 RAM: %d\n", availableRAM()); // 打印可用 RAM
  Serial.printf("启动时可用闪存: %d\n", arcada.availableFlash()); // 打印可用闪存
  yield(); // 定期 yield() 确保大容量存储文件系统保持活动状态

  // 还没有文件选择器。在此期间，您可以通过在启动时按住三个边缘按钮之一来覆盖默认的
  // 配置文件（加载 config1.eye、config2.eye 或 config3.eye）。执行此操作时请保持手指
  // 远离鼻子触摸传感器...它在启动时会自校准。
  // 在启动画面之前执行此操作，因此不需要长时间按住。
  char *filename = (char *)"config.eye";
 
  uint32_t buttonState = arcada.readButtons(); // 读取按钮状态
  if((buttonState & ARCADA_BUTTONMASK_UP) && arcada.exists("config1.eye")) {
    filename = (char *)"config1.eye"; // 如果按住上按钮且存在 config1.eye，则加载 config1.eye
  } else if((buttonState & ARCADA_BUTTONMASK_A) && arcada.exists("config2.eye")) {
    filename = (char *)"config2.eye"; // 如果按住 A 按钮且存在 config2.eye，则加载 config2.eye
  } else if((buttonState & ARCADA_BUTTONMASK_DOWN) && arcada.exists("config3.eye")) {
    filename = (char *)"config3.eye"; // 如果按住下按钮且存在 config3.eye，则加载 config3.eye
  }

  yield();
  // 初始化显示屏
#if (NUM_EYES > 1)
  eye[0].display = arcada._display; // 左眼显示屏
  eye[1].display = arcada.display2; // 右眼显示屏
#else
  eye[0].display = arcada.display; // 单眼显示屏
#endif

  // 初始化 DMA
  yield();
  uint8_t e;
  for(e=0; e<NUM_EYES; e++) {
#if (ARCADA_TFT_WIDTH != 160) && (ARCADA_TFT_HEIGHT != 128) // 160x128 是 ST7735，无法处理
    eye[e].spi->setClockSource(DISPLAY_CLKSRC); // 加速 SPI！
#endif
    eye[e].display->fillScreen(0); // 清屏
    eye[e].dma.allocate(); // 分配 DMA
    eye[e].dma.setTrigger(eye[e].spi->getDMAC_ID_TX()); // 设置 DMA 触发
    eye[e].dma.setAction(DMA_TRIGGER_ACTON_BEAT); // 设置 DMA 动作
    eye[e].dptr = eye[e].dma.addDescriptor(NULL, NULL, 42, DMA_BEAT_SIZE_BYTE, false, false); // 添加 DMA 描述符
    eye[e].dma.setCallback(dma_callback); // 设置 DMA 回调
    eye[e].dma.setPriority(DMA_PRIORITY_0); // 设置 DMA 优先级
    uint32_t spi_data_reg = (uint32_t)eye[e].spi->getDataRegister(); // 获取 SPI 数据寄存器地址
    for(int i=0; i<2; i++) {   // 对于每 2 行...
      for(int j=0; j<NUM_DESCRIPTORS; j++) { // 对于每行的每个描述符...
        eye[e].column[i].descriptor[j].BTCTRL.bit.VALID    = true; // 设置描述符有效
        eye[e].column[i].descriptor[j].BTCTRL.bit.EVOSEL   = DMA_EVENT_OUTPUT_DISABLE; // 禁用事件输出
        eye[e].column[i].descriptor[j].BTCTRL.bit.BLOCKACT = DMA_BLOCK_ACTION_NOACT; // 无块动作
        eye[e].column[i].descriptor[j].BTCTRL.bit.BEATSIZE = DMA_BEAT_SIZE_BYTE; // 字节大小
        eye[e].column[i].descriptor[j].BTCTRL.bit.DSTINC   = 0; // 目标地址不递增
        eye[e].column[i].descriptor[j].BTCTRL.bit.STEPSEL  = DMA_STEPSEL_SRC; // 源地址步进
        eye[e].column[i].descriptor[j].BTCTRL.bit.STEPSIZE = DMA_ADDRESS_INCREMENT_STEP_SIZE_1; // 步进大小为 1
        eye[e].column[i].descriptor[j].DSTADDR.reg         = spi_data_reg; // 目标地址为 SPI 数据寄存器
      }
    }
    eye[e].colNum       = DISPLAY_SIZE; // 强制初始回绕到第一列
    eye[e].colIdx       = 0;
    eye[e].dma_busy     = false;
    eye[e].column_ready = false;
    eye[e].dmaStartTime = 0;

    // 可以在配置文件中覆盖的默认设置
    eye[e].pupilColor        = 0x0000; // 瞳孔颜色
    eye[e].backColor         = 0xFFFF; // 背景颜色
    eye[e].iris.color        = 0xFF01; // 虹膜颜色
    eye[e].iris.data         = NULL; // 虹膜数据
    eye[e].iris.filename     = NULL; // 虹膜文件名
    eye[e].iris.startAngle   = (e & 1) ? 512 : 0; // 交替眼睛旋转 180 度
    eye[e].iris.angle        = eye[e].iris.startAngle; // 虹膜角度
    eye[e].iris.mirror       = 0; // 虹膜镜像
    eye[e].iris.spin         = 0.0; // 虹膜旋转
    eye[e].iris.iSpin        = 0; // 虹膜旋转增量
    eye[e].sclera.color      = 0xFFFF; // 巩膜颜色
    eye[e].sclera.data       = NULL; // 巩膜数据
    eye[e].sclera.filename   = NULL; // 巩膜文件名
    eye[e].sclera.startAngle = (e & 1) ? 512 : 0; // 交替眼睛旋转 180 度
    eye[e].sclera.angle      = eye[e].sclera.startAngle; // 巩膜角度
    eye[e].sclera.mirror     = 0; // 巩膜镜像
    eye[e].sclera.spin       = 0.0; // 巩膜旋转
    eye[e].sclera.iSpin      = 0; // 巩膜旋转增量
    eye[e].rotation          = 3; // 旋转

    // Uncanny eyes 遗留内容，目前很混乱：
    eye[e].blink.state = NOBLINK; // 眨眼状态
    eye[e].blinkFactor = 0.0; // 眨眼因子
  }

  // 启动画面（如果文件存在）---------------------------------------

  yield();
  uint32_t startTime, elapsed;
  if (showSplashScreen) {
    showSplashScreen = ((arcada.drawBMP((char *)"/splash.bmp",
                         0, 0, eye[0].display)) == IMAGE_SUCCESS); // 显示启动画面
    if (showSplashScreen) { // 加载成功？
      Serial.println("显示启动画面");
      if (NUM_EYES > 1) {   // 在另一只眼睛上加载，忽略状态
        yield();
        arcada.drawBMP((char *)"/splash.bmp", 0, 0, eye[1].display);
      }
      // 在 1/2 秒内逐渐增加背光
      startTime = millis();
      while ((elapsed = (millis() - startTime)) <= 500) {
        yield();
        arcada.setBacklight(255 * elapsed / 500);
      }
      arcada.setBacklight(255); // 最大亮度
      startTime = millis();     // 记录当前时间以便稍后保持背光
    }
  }

  // 如果没有启动画面，或加载失败，则提前打开背光，以便用户得到一些反馈，
  // 表明电路板没有锁定，只是在思考。
  if (!showSplashScreen) arcada.setBacklight(255);

  // 加载配置文件 -----------------------------------------------

  loadConfig(filename);

  // 加载眼睑和纹理贴图 -----------------------------------------

  // 在加载纹理贴图时遇到内存碎片问题。这些图像仅临时占用 RAM ——
  // 它们被复制到内部闪存中，然后释放。然而，某些东西阻止了释放的内存
  // 恢复到连续块。例如，如果加载了大约 50% RAM 的纹理图像/复制/释放，
  // 随后加载更大的纹理（或尝试分配更大的极坐标查找数组）会失败，因为 RAM
  // 被分成两个段。我已经仔细检查了这段代码、Adafruit_ImageReader 和 Adafruit_GFX，
  // 它们似乎以与分配相反的顺序释放 RAM（这应该可以避免碎片），但我可能忽略了
  // 那里的某些东西或其他库中的额外分配 —— 可能是文件系统和/或大容量存储代码。
  // 所以，这里是肮脏的解决方法...
  // Adafruit_ImageReader 提供了一个 bmpDimensions() 函数，用于确定图像的像素大小
  // 而无需实际加载它。我们可以使用它来估计加载图像所需的 RAM，然后分配一个
  // “助推器座位”，使后续的图像加载发生在更高的内存中，而碎片部分则稍微超出。
  // 当图像和助推器都被释放时，应该会恢复一个大块的连续内存，将碎片留在高内存中。
  // 但不要太高，我们需要为堆栈留出一些 RAM 以在此程序的整个生命周期内运行
  // 并处理小的堆分配。

  uint32_t maxRam = availableRAM() - stackReserve; // 最大可用 RAM

  // 加载眼睛的纹理贴图
  uint8_t e2;
  for(e=0; e<NUM_EYES; e++) { // 对于每只眼睛...
    yield();
    for(e2=0; e2<e; e2++) {    // 与每只之前的眼睛比较...
      // 如果两只眼睛有相同的虹膜文件名...
      if((eye[e].iris.filename && eye[e2].iris.filename) &&
         (!strcmp(eye[e].iris.filename, eye[e2].iris.filename))) {
        // 那么眼睛 'e' 可以共享 'e2' 的虹膜图形
        // 旋转和镜像保持独立，只共享图像
        eye[e].iris.data   = eye[e2].iris.data;
        eye[e].iris.width  = eye[e2].iris.width;
        eye[e].iris.height = eye[e2].iris.height;
        break;
      }
    }
    if((!e) || (e2 >= e)) { // 如果是第一只眼睛，或未找到匹配项...
      // 如果未指定虹膜文件名，或文件加载失败...
      if((eye[e].iris.filename == NULL) || (loadTexture(eye[e].iris.filename,
        &eye[e].iris.data, &eye[e].iris.width, &eye[e].iris.height,
        maxRam) != IMAGE_SUCCESS)) {
        // 将虹膜数据指向颜色变量并将图像大小设置为 1px
        eye[e].iris.data  = &eye[e].iris.color;
        eye[e].iris.width = eye[e].iris.height = 1;
      }
      // 助推器座位的想法仍然不总是有效，
      // 高内存中有东西泄漏。每次加载纹理时，
      // 逐渐缩小助推器座位的大小。唉。
      maxRam -= 20;
    }
    // 重复巩膜...
    for(e2=0; e2<e; e2++) {    // 与每只之前的眼睛比较...
      // 如果两只眼睛有相同的巩膜文件名...
      if((eye[e].sclera.filename && eye[e2].sclera.filename) &&
         (!strcmp(eye[e].sclera.filename, eye[e2].sclera.filename))) {
        // 那么眼睛 'e' 可以共享 'e2' 的巩膜图形
        // 旋转和镜像保持独立，只共享图像
        eye[e].sclera.data   = eye[e2].sclera.data;
        eye[e].sclera.width  = eye[e2].sclera.width;
        eye[e].sclera.height = eye[e2].sclera.height;
        break;
      }
    }
    if((!e) || (e2 >= e)) { // 如果是第一只眼睛，或未找到匹配项...
      // 如果未指定巩膜文件名，或文件加载失败...
      if((eye[e].sclera.filename == NULL) || (loadTexture(eye[e].sclera.filename,
        &eye[e].sclera.data, &eye[e].sclera.width, &eye[e].sclera.height,
        maxRam) != IMAGE_SUCCESS)) {
        // 将巩膜数据指向颜色变量并将图像大小设置为 1px
        eye[e].sclera.data  = &eye[e].sclera.color;
        eye[e].sclera.width = eye[e].sclera.height = 1;
      }
      maxRam -= 20; // 参见上面的注释
    }
  }

  // 加载眼睑图形。
  yield();
  ImageReturnCode status;

  status = loadEyelid(upperEyelidFilename ?
    upperEyelidFilename : (char *)"upper.bmp",
    upperClosed, upperOpen, DISPLAY_SIZE-1, maxRam); // 加载上眼睑

  status = loadEyelid(lowerEyelidFilename ?
    lowerEyelidFilename : (char *)"lower.bmp",
    lowerOpen, lowerClosed, 0, maxRam); // 加载下眼睑

  // 不再需要文件名...
  for(e=0; e<NUM_EYES; e++) {
    if(eye[e].sclera.filename) free(eye[e].sclera.filename);
    if(eye[e].iris.filename)   free(eye[e].iris.filename);
  }
  if(lowerEyelidFilename) free(lowerEyelidFilename);
  if(upperEyelidFilename) free(upperEyelidFilename);

  // 此时调用 availableRAM() 将返回接近 reserveSpace 的值，表明可用 RAM 很少...
  // 但该函数实际上只是返回堆和堆栈之间的空间，我们已经在上面确定了堆顶是某种海市蜃楼。
  // 大块分配仍然可以在较低的堆中进行！

  calcMap(); // 计算地图
  calcDisplacement(); // 计算位移
  Serial.printf("可用 RAM: %d\n", availableRAM()); // 打印可用 RAM

  randomSeed(SysTick->VAL + analogRead(A2)); // 随机种子
  eyeOldX = eyeNewX = eyeOldY = eyeNewY = mapRadius; // 从中心开始
  for(e=0; e<NUM_EYES; e++) { // 对于每只眼睛...
    eye[e].display->setRotation(eye[e].rotation); // 设置旋转
    eye[e].eyeX = eyeOldX; // 设置初始位置
    eye[e].eyeY = eyeOldY;
  }

  if (showSplashScreen) { // 上面加载了图像？
    // 保持背光开启最多 2 秒（减去其他初始化时间）
    if ((elapsed = (millis() - startTime)) < 2000) {
      delay(2000 - elapsed);
    }
    // 在 1/2 秒内逐渐降低背光
    startTime = millis();
    while ((elapsed = (millis() - startTime)) <= 500) {
      yield();
      arcada.setBacklight(255 - (255 * elapsed / 500));
    }
    arcada.setBacklight(0); // 关闭背光
    for(e=0; e<NUM_EYES; e++) {
      eye[e].display->fillScreen(0); // 清屏
    }
  }

#if defined(ADAFRUIT_MONSTER_M4SK_EXPRESS)
  if(voiceOn) { // 如果启用语音
    if(!voiceSetup((waveform > 0))) { // 初始化语音
      Serial.println("语音初始化失败，继续无语音");
      voiceOn = false;
    } else {
      voiceGain(gain); // 设置语音增益
      currentPitch = voicePitch(currentPitch); // 设置语音音高
      if(waveform) voiceMod(modulate, waveform); // 设置语音调制
      arcada.enableSpeaker(true); // 启用扬声器
    }
  }
#endif

  arcada.setBacklight(255); // 背光重新打开，即将显示图形

  yield();
  if(boopPin >= 0) { // 如果启用了触摸传感器
    boopThreshold = 0;
    for(int i=0; i<DISPLAY_SIZE; i++) {
      boopThreshold += readBoop(); // 读取触摸传感器值
    }
    boopThreshold = boopThreshold * 110 / 100; // 10% 余量
  }

  lastLightReadTime = micros() + 2000000; // 延迟初始光线读取
}


// LOOP 函数 - 重复调用直到断电 -----------------------

/*
此代码中的 loop() 函数是一个奇怪的动物，与早期的 "Uncanny Eyes" 眼睛项目有些不同。
在之前的项目中，我们这样做：

  for(每只眼睛) {
    * 为一帧动画进行位置计算等 *
    for(每行) {
      * 绘制一行像素 *
    }
  }

这个新代码“从内到外”工作，更像这样：

  for(每列) {
    if(显示的第一列) {
      * 为一帧动画进行位置计算等 *
    }
    * 绘制一列像素 *
  }

这样做的原因是 A) 与旧项目相比，我们有大量的像素要绘制（几乎是 4 倍），
B) 每个屏幕现在都在自己的 SPI 总线上...数据可以并发发出...因此，与其在 while() 循环中
等待每行传输完成（只是浪费周期），代码寻找机会处理其他眼睛（眼睛更新不一定同步；
每个眼睛可以根据当前的特定复杂性以独立的帧速率运行）。
*/

// loop() 函数处理一只眼睛的一列...

void loop() {
  if(++eyeNum >= NUM_EYES) eyeNum = 0; // 循环处理眼睛...

  uint8_t  x = eye[eyeNum].colNum;
  uint32_t t = micros();

  // 如果此眼睛的下一列尚未渲染...
  if(!eye[eyeNum].column_ready) {
    if(!x) { // 如果是第一列...

      // 每帧眼睛动画逻辑发生在这里 -------------------

      // 眼睛移动
      float eyeX, eyeY;
      if(moveEyesRandomly) {
        int32_t dt = t - eyeMoveStartTime;      // 自上次眼睛事件以来的微秒数
        if(eyeInMotion) {                       // 眼睛当前在移动？
          if(dt >= eyeMoveDuration) {           // 时间到了？ 到达目的地。
            eyeInMotion = false;                // 停止移动
            // “移动”持续时间暂时变为保持持续时间...
            // 通常为 35 毫秒到 1 秒，但不要超过 gazeMax 设置
            uint32_t limit = min(1000000, gazeMax);
            eyeMoveDuration = random(35000, limit); // 微眼跳之间的时间
            if(!saccadeInterval) {              // 当“大”眼跳完成时清除
              lastSaccadeStop = t;              // 眼跳停止时间
              saccadeInterval = random(eyeMoveDuration, gazeMax); // 下次在 30 毫秒到 3 秒内
            }
            // 同样，“移动”开始时间变为“停止”开始时间...
            eyeMoveStartTime = t;               // 保存事件时间
            eyeX = eyeOldX = eyeNewX;           // 保存位置
            eyeY = eyeOldY = eyeNewY;
          } else { // 移动时间尚未完全过去 —— 插值位置
            float e  = (float)dt / float(eyeMoveDuration); // 0.0 到 1.0 在移动期间
            e = 3 * e * e - 2 * e * e * e; // 缓动函数：3*e^2-2*e^3 0.0 到 1.0
            eyeX = eyeOldX + (eyeNewX - eyeOldX) * e; // 插值 X
            eyeY = eyeOldY + (eyeNewY - eyeOldY) * e; // 和 Y
          }
        } else {                       // 眼睛当前停止
          eyeX = eyeOldX;
          eyeY = eyeOldY;
          if(dt > eyeMoveDuration) {   // 时间到了？ 开始新移动。
            if((t - lastSaccadeStop) > saccadeInterval) { // 是时候进行“大”眼跳了
              // r 是眼睛可以在 X 和 Y 中移动的半径，从中心 (0,0) 开始。
              float r = ((float)mapDiameter - (float)DISPLAY_SIZE * M_PI_2) * 0.75;
              eyeNewX = random(-r, r);
              float h = sqrt(r * r - eyeNewX * eyeNewX);
              eyeNewY = random(-h, h);
              // 设置此移动的持续时间，并开始移动。
              eyeMoveDuration = random(83000, 166000); // ~1/12 - ~1/6 秒
              saccadeInterval = 0; // 当此眼跳停止时计算下一个间隔
            } else { // 微眼跳
              // r 是可能的移动半径，约为全眼跳大小的 1/10。
              // 我们不费心剪裁，因为如果它稍微偏离一点，
              // 那也没关系，它会在下次全眼跳时被放回范围内。
              float r = (float)mapDiameter - (float)DISPLAY_SIZE * M_PI_2;
              r *= 0.07;
              float dx = random(-r, r);
              eyeNewX = eyeX - mapRadius + dx;
              float h = sqrt(r * r - dx * dx);
              eyeNewY = eyeY - mapRadius + random(-h, h);
              eyeMoveDuration = random(7000, 25000); // 7-25 毫秒微眼跳
            }
            eyeNewX += mapRadius;    // 将新点转换为地图空间
            eyeNewY += mapRadius;
            eyeMoveStartTime = t;    // 保存移动的初始时间
            eyeInMotion      = true; // 在下一帧开始移动
          }
        }
      } else {
        // 允许用户代码控制眼睛位置（例如 IR 传感器、操纵杆等）
        float r = ((float)mapDiameter - (float)DISPLAY_SIZE * M_PI_2) * 0.9;
        eyeX = mapRadius + eyeTargetX * r;
        eyeY = mapRadius + eyeTargetY * r;
      }

      // 眼睛注视（稍微交叉）—— 数量经过过滤以应对触摸
      int nufix = booped ? 90 : 7;
      fixate = ((fixate * 15) + nufix) / 16;
      // 将眼睛位置保存到此眼睛的结构中，以便在整个渲染过程中保持一致
      if(eyeNum & 1) eyeX += fixate; // 眼睛稍微向中心汇聚
      else           eyeX -= fixate;
      eye[eyeNum].eyeX = eyeX;
      eye[eyeNum].eyeY = eyeY;

      // pupilFactor? irisValue? 待办事项：选择一个名称并坚持使用
      eye[eyeNum].pupilFactor = irisValue;
      // 还要注意 - irisValue 在此函数的末尾为下一帧计算
      // （因为必须在没有 SPI 通信到左眼时读取传感器）

      // 类似于上面的自主眼睛移动 —— 眨眼开始时间
      // 和持续时间是随机的（在范围内）。
      if((t - timeOfLastBlink) >= timeToNextBlink) { // 开始新眨眼？
        timeOfLastBlink = t;
        uint32_t blinkDuration = random(36000, 72000); // ~1/28 - ~1/14 秒
        // 为两只眼睛设置持续时间（如果尚未眨眼）
        for(uint8_t e=0; e<NUM_EYES; e++) {
          if(eye[e].blink.state == NOBLINK) {
            eye[e].blink.state     = ENBLINK;
            eye[e].blink.startTime = t;
            eye[e].blink.duration  = blinkDuration;
          }
        }
        timeToNextBlink = blinkDuration * 3 + random(4000000);
      }

      float uq, lq; // 这里有很多草率的临时变量，抱歉
      if(tracking) {
        // 眼睑自然“跟踪”瞳孔（自动上下移动）
        int ix = (int)map2screen(mapRadius - eye[eyeNum].eyeX) + (DISPLAY_SIZE/2), // 瞳孔位置
            iy = (int)map2screen(mapRadius - eye[eyeNum].eyeY) + (DISPLAY_SIZE/2); // 在屏幕上
        iy += irisRadius * trackFactor;
        if(eyeNum & 1) ix = DISPLAY_SIZE - 1 - ix; // 右眼翻转
        if(iy > upperOpen[ix]) {
          uq = 1.0;
        } else if(iy < upperClosed[ix]) {
          uq = 0.0;
        } else {
          uq = (float)(iy - upperClosed[ix]) / (float)(upperOpen[ix] - upperClosed[ix]);
        }
        if(booped) {
          uq = 0.9;
          lq = 0.7;
        } else {
          lq = 1.0 - uq;
        }
      } else {
        // 如果没有跟踪，眼睛在不眨眼时完全睁开
        uq = 1.0;
        lq = 1.0;
      }
      // 稍微抑制眼睑运动
      // 保存每只眼睛的上眼睑和下眼睑因子，
      // 它们需要在帧之间保持一致
      eye[eyeNum].upperLidFactor = (eye[eyeNum].upperLidFactor * 0.6) + (uq * 0.4);
      eye[eyeNum].lowerLidFactor = (eye[eyeNum].lowerLidFactor * 0.6) + (lq * 0.4);

      // 处理眨眼
      if(eye[eyeNum].blink.state) { // 眼睛当前在眨眼？
        // 检查当前眨眼状态时间是否已过
        if((t - eye[eyeNum].blink.startTime) >= eye[eyeNum].blink.duration) {
          if(++eye[eyeNum].blink.state > DEBLINK) { // 眨眼完成？
            eye[eyeNum].blink.state = NOBLINK;      // 不再眨眼
            eye[eyeNum].blinkFactor = 0.0;
          } else { // 从 ENBLINK 模式进入 DEBLINK 模式
            eye[eyeNum].blink.duration *= 2; // DEBLINK 是 ENBLINK 速度的 1/2
            eye[eyeNum].blink.startTime = t;
            eye[eyeNum].blinkFactor = 1.0;
          }
        } else {
          eye[eyeNum].blinkFactor = (float)(t - eye[eyeNum].blink.startTime) / (float)eye[eyeNum].blink.duration;
          if(eye[eyeNum].blink.state == DEBLINK) eye[eyeNum].blinkFactor = 1.0 - eye[eyeNum].blinkFactor;
        }
      }

      // 定期报告帧率。实际上是“绘制的眼球总数”。
      // 如果有两只眼睛，两个屏幕的总体刷新率大约是此值的一半。
      frames++;
      if(((t - lastFrameRateReportTime) >= 1000000) && t) { // 每秒一次。
        Serial.println((frames * 1000) / (t / 1000));
        lastFrameRateReportTime = t;
      }

      // 每帧（眼睛 #0）重置 boopSum...
      if((eyeNum == 0) && (boopPin >= 0)) {
        boopSumFiltered = ((boopSumFiltered * 3) + boopSum) / 4;
        if(boopSumFiltered > boopThreshold) {
          if(!booped) {
            Serial.println("BOOP!");
          }
          booped = true;
        } else {
          booped = false;
        }
        boopSum = 0;
      }

      float mins = (float)millis() / 60000.0;
      if(eye[eyeNum].iris.iSpin) {
        // 旋转每帧固定量（眼睛可能失去同步，但“马车轮”技巧有效）
        eye[eyeNum].iris.angle   += eye[eyeNum].iris.iSpin;
      } else {
        // 保持旋转动画的时间一致（眼睛保持同步，没有“马车轮”效果）
        eye[eyeNum].iris.angle    = (int)((float)eye[eyeNum].iris.startAngle   + eye[eyeNum].iris.spin   * mins + 0.5);
      }
      if(eye[eyeNum].sclera.iSpin) {
        eye[eyeNum].sclera.angle += eye[eyeNum].sclera.iSpin;
      } else {
        eye[eyeNum].sclera.angle  = (int)((float)eye[eyeNum].sclera.startAngle + eye[eyeNum].sclera.spin * mins + 0.5);
      }

      // 结束每帧眼睛动画 ----------------------------------

    } // 结束第一行检查

    // 每列渲染 ------------------------------------------------

    // 这些应该是局部变量，
    // 但动画变得超级卡顿，怎么回事？
    xPositionOverMap = (int)(eye[eyeNum].eyeX - (DISPLAY_SIZE/2.0));
    yPositionOverMap = (int)(eye[eyeNum].eyeY - (DISPLAY_SIZE/2.0));

    // 这些在帧之间是恒定的，可以存储在眼睛结构中
    float upperLidFactor = (1.0 - eye[eyeNum].blinkFactor) * eye[eyeNum].upperLidFactor,
          lowerLidFactor = (1.0 - eye[eyeNum].blinkFactor) * eye[eyeNum].lowerLidFactor;
    iPupilFactor = (int)((float)eye[eyeNum].iris.height * 256 * (1.0 / eye[eyeNum].pupilFactor));

    int y1, y2;
    int lidColumn = (eyeNum & 1) ? (DISPLAY_SIZE - 1 - x) : x; // 左眼反转眼睑列

    DmacDescriptor *d = &eye[eyeNum].column[eye[eyeNum].colIdx].descriptor[0];

    if(upperOpen[lidColumn] == 255) {
      // 此行没有眼睑数据；眼睑图像小于屏幕。
      // 太好了！制作一整行空白，无需渲染：
      d->BTCTRL.bit.SRCINC = 0;
      d->BTCNT.reg         = DISPLAY_SIZE * 2;
      d->SRCADDR.reg       = (uint32_t)&eyelidIndex;
      d->DESCADDR.reg      = 0; // 无链接描述符
    } else {
      y1 = lowerClosed[lidColumn] + (int)(0.5 + lowerLidFactor *
        (float)((int)lowerOpen[lidColumn] - (int)lowerClosed[lidColumn]));
      y2 = upperClosed[lidColumn] + (int)(0.5 + upperLidFactor *
        (float)((int)upperOpen[lidColumn] - (int)upperClosed[lidColumn]));
      if(y1 > DISPLAY_SIZE-1)    y1 = DISPLAY_SIZE-1; // 如果 lidfactor 超出通常的 0.0 到 1.0 范围，则剪裁结果
      else if(y1 < 0) y1 = 0;   // 
      if(y2 > DISPLAY_SIZE-1)    y2 = DISPLAY_SIZE-1;
      else if(y2 < 0) y2 = 0;
      if(y1 >= y2) {
        // 眼睑完全或部分闭合，足以使此行没有像素需要渲染。如上所述，制作“空白”。
        d->BTCTRL.bit.SRCINC = 0;
        d->BTCNT.reg         = DISPLAY_SIZE * 2;
        d->SRCADDR.reg       = (uint32_t)&eyelidIndex;
        d->DESCADDR.reg      = 0; // 无链接描述符
      } else {
        // 如果单眼，根据需要动态构建描述符列表，
        // 否则使用单个描述符并完全缓冲每行。
#if NUM_DESCRIPTORS > 1
        DmacDescriptor *next;
        int             renderlen;
        if(y1 > 0) { // 除非在图像顶部，否则执行上眼睑
          d->BTCTRL.bit.SRCINC = 0;
          d->BTCNT.reg         = y1 * 2;
          d->SRCADDR.reg       = (uint32_t)&eyelidIndex;
          next                 = &eye[eyeNum].column[eye[eyeNum].colIdx].descriptor[1];
          d->DESCADDR.reg      = (uint32_t)next; // 链接到下一个描述符
          d                    = next;           // 前进到下一个描述符
        }
        // 将渲染部分列
        renderlen            = y2 - y1 + 1;
        d->BTCTRL.bit.SRCINC = 1;
        d->BTCNT.reg         = renderlen * 2;
        d->SRCADDR.reg       = (uint32_t)eye[eyeNum].column[eye[eyeNum].colIdx].renderBuf + renderlen * 2; // 指向数据末尾！
#else
        // 将渲染整列；DISPLAY_SIZE 像素，将源指向 renderBuf 的末尾并启用源递增。
        d->BTCTRL.bit.SRCINC = 1;
        d->BTCNT.reg         = DISPLAY_SIZE * 2;
        d->SRCADDR.reg       = (uint32_t)eye[eyeNum].column[eye[eyeNum].colIdx].renderBuf + DISPLAY_SIZE * 2;
        d->DESCADDR.reg      = 0; // 无链接描述符
#endif
        // 将列 'x' 渲染到眼睛的下一个可用 renderBuf 中
        uint16_t *ptr = eye[eyeNum].column[eye[eyeNum].colIdx].renderBuf;
        int xx = xPositionOverMap + x;
        int y;

#if NUM_DESCRIPTORS == 1
        // 如果需要，渲染下眼睑
        for(y=0; y<y1; y++) *ptr++ = eyelidColor;
#else
        y = y1;
#endif

        // tablegen.cpp 解释了一些位移映射技巧。
        uint8_t *displaceX, *displaceY;
        int8_t   xmul; // X 位移的符号：+1 或 -1
        int      doff; // 位移数组中的偏移量
        if(x < (DISPLAY_SIZE/2)) {  // 屏幕的左半部分（象限 2, 3）
          displaceX = &displace[ (DISPLAY_SIZE/2 - 1) - x       ];
          displaceY = &displace[((DISPLAY_SIZE/2 - 1) - x) * (DISPLAY_SIZE/2)];
          xmul      = -1; // X 位移始终为负
        } else {       // 屏幕的右半部分（象限 1, 4）
          displaceX = &displace[ x - (DISPLAY_SIZE/2)       ];
          displaceY = &displace[(x - (DISPLAY_SIZE/2)) * (DISPLAY_SIZE/2)];
          xmul      =  1; // X 位移始终为正
        }

        for(; y<=y2; y++) { // 对于此列中每只睁开的眼睛的每个像素...
          int yy = yPositionOverMap + y;
          int dx, dy;

          if(y < (DISPLAY_SIZE/2)) { // 屏幕的下半部分（象限 3, 4）
            doff = (DISPLAY_SIZE/2 - 1) - y;
            dy   = -displaceY[doff];
          } else {      // 屏幕的上半部分（象限 1, 2）
            doff = y - (DISPLAY_SIZE/2);
            dy   =  displaceY[doff];
          }
          dx = displaceX[doff * (DISPLAY_SIZE/2)];
          if(dx < 255) {      // 在眼球区域内
            dx *= xmul;       // 如果在象限 2 或 3 中，翻转 x 偏移的符号
            int mx = xx + dx; // 极角/距离地图坐标
            int my = yy + dy;
            if((mx >= 0) && (mx < mapDiameter) && (my >= 0) && (my < mapDiameter)) {
              // 在极角/距离地图内
              int angle, dist, moff;
              if(my >= mapRadius) {
                if(mx >= mapRadius) { // 象限 1
                  // 直接使用角度和距离
                  mx   -= mapRadius;
                  my   -= mapRadius;
                  moff  = my * mapRadius + mx; // 地图数组中的偏移量
                  angle = polarAngle[moff];
                  dist  = polarDist[moff];
                } else {                // 象限 2
                  // 将角度旋转 90 度（顺时针 270 度；768）
                  // 在 X 轴上镜像距离
                  mx    = mapRadius - 1 - mx;
                  my   -= mapRadius;
                  angle = polarAngle[mx * mapRadius + my] + 768;
                  dist  = polarDist[ my * mapRadius + mx];
                }
              } else {
                if(mx < mapRadius) {  // 象限 3
                  // 将角度旋转 180 度
                  // 在 X 和 Y 轴上镜像距离
                  mx    = mapRadius - 1 - mx;
                  my    = mapRadius - 1 - my;
                  moff  = my * mapRadius + mx;
                  angle = polarAngle[moff] + 512;
                  dist  = polarDist[ moff];
                } else {                // 象限 4
                  // 将角度旋转 270 度（顺时针 90 度；256）
                  // 在 Y 轴上镜像距离
                  mx   -= mapRadius;
                  my    = mapRadius - 1 - my;
                  angle = polarAngle[mx * mapRadius + my] + 256;
                  dist  = polarDist[ my * mapRadius + mx];
                }
              }
              // 将角度/距离转换为纹理贴图坐标
              if(dist >= 0) { // 巩膜
                angle = ((angle + eye[eyeNum].sclera.angle) & 1023) ^ eye[eyeNum].sclera.mirror;
                int tx = angle * eye[eyeNum].sclera.width  / 1024; // 纹理贴图 x/y
                int ty = dist  * eye[eyeNum].sclera.height / 128;
                *ptr++ = eye[eyeNum].sclera.data[ty * eye[eyeNum].sclera.width + tx];
              } else if(dist > -128) { // 虹膜或瞳孔
                int ty = dist * iPupilFactor / -32768;
                if(ty >= eye[eyeNum].iris.height) { // 瞳孔
                  *ptr++ = eye[eyeNum].pupilColor;
                } else { // 虹膜
                  angle = ((angle + eye[eyeNum].iris.angle) & 1023) ^ eye[eyeNum].iris.mirror;
                  int tx = angle * eye[eyeNum].iris.width / 1024;
                  *ptr++ = eye[eyeNum].iris.data[ty * eye[eyeNum].iris.width + tx];
                }
              } else {
                *ptr++ = eye[eyeNum].backColor; // 眼睛背面
              }
            } else {
              *ptr++ = eye[eyeNum].backColor; // 超出地图，使用眼睛背面颜色
            }
          } else { // 超出眼球区域
            *ptr++ = eyelidColor;
          }
        }

#if NUM_DESCRIPTORS == 1
        // 如果需要，渲染上眼睑
        for(; y<DISPLAY_SIZE; y++) *ptr++ = eyelidColor;
#else
        if(y2 >= (DISPLAY_SIZE-1)) {
          // 无第三个描述符；关闭它
          d->DESCADDR.reg      = 0;
        } else {
          next                 = &eye[eyeNum].column[eye[eyeNum].colIdx].descriptor[(y1 > 0) ? 2 : 1];
          d->DESCADDR.reg      = (uint32_t)next; // 链接到下一个描述符
          d                    = next; // 递增描述符
          d->BTCTRL.bit.SRCINC = 0;
          d->BTCNT.reg         = ((DISPLAY_SIZE-1) - y2) * 2;
          d->SRCADDR.reg       = (uint32_t)&eyelidIndex;
          d->DESCADDR.reg      = 0; // 描述符列表结束
        }
#endif
      }
    }
    eye[eyeNum].column_ready = true; // 行已渲染！
  }

  // 如果此眼睛的 DMA 当前繁忙，不要阻塞，尝试下一只眼睛...
  if(eye[eyeNum].dma_busy) {
    if((micros() - eye[eyeNum].dmaStartTime) < DMA_TIMEOUT) return;
    // 如果我们到达代码中的这一点，SPI DMA 传输花费的时间明显长于预期，
    // 并且可能已卡住（请参阅 DMAbuddy.h 文件中的注释和此代码中 DMA_TIMEOUT 声明上方的注释）。
    // 采取行动！
    // digitalWrite(13, HIGH);
    Serial.printf("眼睛 #%d 卡住，重置 DMA 通道...\n", eyeNum);
    eye[eyeNum].dma.fix();
    // 如果这被证明是不够的，我们仍然有核选项，
    // 即完全从头重新启动草图，尽管这会在启动期间使动画停滞几秒钟。
    // 除非 fix() 函数无法修复，否则不要启用此行！
    //NVIC_SystemReset();
  }

  // 此时，上述检查确认列已准备好且 DMA 空闲
  if(!x) { // 如果是第一列...
    // 结束先前的 SPI 事务...
    digitalWrite(eye[eyeNum].cs, HIGH); // 取消选择
    eye[eyeNum].spi->endTransaction();
    // 初始化新的 SPI 事务和地址窗口...
    eye[eyeNum].spi->beginTransaction(settings);
    digitalWrite(eye[eyeNum].cs, LOW);  // 芯片选择
    eye[eyeNum].display->setAddrWindow((eye[eyeNum].display->width() - DISPLAY_SIZE) / 2, (eye[eyeNum].display->height() - DISPLAY_SIZE) / 2, DISPLAY_SIZE, DISPLAY_SIZE);
    delayMicroseconds(1);
    digitalWrite(eye[eyeNum].dc, HIGH); // 数据模式
    if(eyeNum == (NUM_EYES-1)) {
      // 处理瞳孔缩放
      if(lightSensorPin >= 0) {
        // 读取光线传感器，但不要太频繁（Seesaw 不喜欢这样）
        #define LIGHT_INTERVAL (1000000 / 10) // 10 Hz，不要频繁轮询 Seesaw
        if((t - lastLightReadTime) >= LIGHT_INTERVAL) {
          // 有趣的事实：眼睛对光线有“共同反应” —— 即使刺激另一只眼睛，两只瞳孔都会反应。
          // 这意味着我们可以为两只眼睛使用单个光线传感器。此注释与代码无关。
          uint16_t rawReading = arcada.readLightSensor();
          if(rawReading <= 1023) {
            if(rawReading < lightSensorMin)      rawReading = lightSensorMin; // 钳制光线传感器范围
            else if(rawReading > lightSensorMax) rawReading = lightSensorMax; // 在可用范围内
            float v = (float)(rawReading - lightSensorMin) / (float)(lightSensorMax - lightSensorMin); // 0.0 to 1.0
            v = pow(v, lightSensorCurve);
            lastLightValue    = irisMin + v * irisRange;
            lastLightReadTime = t;
            lightSensorFailCount = 0;
          } else { // I2C 错误
            if(++lightSensorFailCount >= 25) { // 如果连续多次错误...
              lightSensorPin = -1; // 停止使用光线传感器
            } else {
              lastLightReadTime = t - LIGHT_INTERVAL + 30000; // 30 毫秒后重试
            } }
        }
        irisValue = (irisValue * 0.97) + (lastLightValue * 0.03); // 过滤响应以获得平滑反应
      } else {
        // 不响应光线。使用自主虹膜和分形细分
        float n, sum = 0.5;
        for(uint16_t i=0; i<IRIS_LEVELS; i++) { // 0,1,2,3,...
          uint16_t iexp  = 1 << (i+1);          // 2,4,8,16,...
          uint16_t imask = (iexp - 1);          // 2^i-1 (1,3,7,15,...)
          uint16_t ibits = iris_frame & imask;  // 0 到 mask
          if(ibits) {
            float weight = (float)ibits / (float)iexp; // 0.0 到 <1.0
            n            = iris_prev[i] * (1.0 - weight) + iris_next[i] * weight;
          } else {
            n            = iris_next[i];
            iris_prev[i] = iris_next[i];
            iris_next[i] = -0.5 + ((float)random(1000) / 999.0); // -0.5 到 +0.5
          }
          iexp = 1 << (IRIS_LEVELS - i); // ...8,4,2,1
          sum += n / (float)iexp;
        }
        irisValue = irisMin + (sum * irisRange); // 0.0-1.0 -> 虹膜最小/最大
        if((++iris_frame) >= (1 << IRIS_LEVELS)) iris_frame = 0;
      }
#if defined(ADAFRUIT_MONSTER_M4SK_EXPRESS)
      if(voiceOn) {
        // 读取按钮，改变音高
        arcada.readButtons();
        uint32_t buttonState = arcada.justPressedButtons();
        if(       buttonState & ARCADA_BUTTONMASK_UP) {
          currentPitch *= 1.05;
        } else if(buttonState & ARCADA_BUTTONMASK_A) {
          currentPitch = defaultPitch;
        } else if(buttonState & ARCADA_BUTTONMASK_DOWN) {
          currentPitch *= 0.95;
        }
        if(buttonState & (ARCADA_BUTTONMASK_UP | ARCADA_BUTTONMASK_A | ARCADA_BUTTONMASK_DOWN)) {
          currentPitch = voicePitch(currentPitch);
          if(waveform) voiceMod(modulate, waveform);
          Serial.print("语音音高: ");
          Serial.println(currentPitch);
        }
      }
#endif
      user_loop();
    }
  } // 结束第一列检查

  // 必须在没有 SPI 通信穿过鼻子时读取触摸传感器！
  if((eyeNum == (NUM_EYES-1)) && (boopPin >= 0)) {
    boopSum += readBoop();
  }

  memcpy(eye[eyeNum].dptr, &eye[eyeNum].column[eye[eyeNum].colIdx].descriptor[0], sizeof(DmacDescriptor));
  eye[eyeNum].dma_busy       = true;
  eye[eyeNum].dma.startJob();
  eye[eyeNum].dmaStartTime   = micros();
  if(++eye[eyeNum].colNum >= DISPLAY_SIZE) { // 如果最后一行已发送...
    eye[eyeNum].colNum      = 0;    // 回绕到开头
  }
  eye[eyeNum].colIdx       ^= 1;    // 交替 0/1 行结构
  eye[eyeNum].column_ready = false; // 可以渲染下一行
}
