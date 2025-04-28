// SPDX-FileCopyrightText: 2019 Phillip Burgess for Adafruit Industries
//
// SPDX-License-Identifier: MIT

//34567890123456789012345678901234567890123456789012345678901234567890123456

#define ARDUINOJSON_ENABLE_COMMENTS 1
#include <ArduinoJson.h>          // JSON 配置文件函数
#include "globals.h"

extern Adafruit_Arcada arcada;

// 配置文件处理 ---------------------------------------------

// 此函数从 JSON 配置文件中解码整数值，支持多种格式...
// 例如，"foo" 可以指定为：
// "foo" : 42                         - 作为有符号十进制整数
// "foo" : "0x42"                     - 正十六进制整数
// "foo" : "0xF800"                   - 16 位 RGB 颜色
// "foo" : [ 255, 0, 0 ]              - 使用 0-255 的整数的 RGB 颜色
// "foo" : [ "0xFF", "0x00", "0x00" ] - 使用十六进制的 RGB
// "foo" : [ 1.0, 0.0, 0.0 ]          - 使用浮点数的 RGB
// 24 位 RGB 颜色将被缩减为 16 位格式。
// 此函数返回的 16 位颜色为大端格式。
// 十六进制值必须加引号 —— JSON 只能处理字符串形式的十六进制。
// 这并不是万无一失的！它处理了许多格式良好的（和一些不太格式良好的）数字，
// 但不是所有可以想象的情况，并且会猜测什么是 RGB 颜色，什么不是。
// 尽我所能，JSON 很挑剔，有时候人们只需要把它弄好。
static int32_t dwim(JsonVariant v, int32_t def = 0) { // "Do What I Mean"
  if(v.is<int>()) {                      // 如果是整数...
    return v;                            // ...直接返回值
  } else if(v.is<float>()) {             // 如果是浮点数...
    return (int)(v.as<float>() + 0.5);   // ...返回四舍五入的整数
  } else if(v.is<const char*>()) {             // 如果是字符串...
    if((strlen(v) == 6) && !strncasecmp(v, "0x", 2)) { // 4 位十六进制？
      uint16_t rgb = strtol(v, NULL, 0); // 可能是 16 位 RGB 颜色，
      return __builtin_bswap16(rgb);     // 转换为大端格式
    } else {
      return strtol(v, NULL, 0);         // 其他整数/十六进制/八进制
    }
  } else if(v.is<JsonArray>()) {         // 如果是数组...
    if(v.size() >= 3) {                  // ...并且至少有 3 个元素...
      long cc[3];                        // ...解析 RGB 颜色分量...
      for(uint8_t i=0; i<3; i++) {       // 处理整数/十六进制/八进制/浮点数...
        if(v[i].is<int>()) {
          cc[i] = v[i].as<int>();
        } else if(v[i].is<float>()) {
          cc[i] = (int)(v[i].as<float>() * 255.999);
        } else if(v[i].is<const char*>()) {
          cc[i] = strtol(v[i], NULL, 0);
        }
        if(cc[i] > 255)    cc[i] = 255;  // 裁剪到 8 位范围
        else if(cc[i] < 0) cc[i] = 0;
      }
      uint16_t rgb = ((cc[0] & 0xF8) << 8) | // 将 24 位 RGB 缩减
                     ((cc[1] & 0xFC) << 3) | // 为 16 位
                     ( cc[2]         >> 3);
      return __builtin_bswap16(rgb);         // 并返回大端格式
    } else {                             // 一些意外的数组
      if(v[0].is<int>()) {               // 返回第一个元素
        return v[0];                     // 作为简单整数，
      } else {
        return strtol(v[0], NULL, 0);    // 或整数/十六进制/八进制
      }
    }
  } else {                               // 未在文档中找到
    return def;                          // ...返回默认值
  }
}

/*
static void getFilename(JsonVariant v, char **ptr) {
  if(*ptr) {          // 如果字符串已分配，
    free(*ptr);       // 删除旧值...
    *ptr = NULL;
  }
  if(v.is<const char*>()) {
    *ptr = strdup(v); // 复制字符串，保存
  }
}
*/

void loadConfig(char *filename) {
  File    file;
  uint8_t rotation = 3;

  if(file = arcada.open(filename, FILE_READ)) {
    StaticJsonDocument<2048> doc;

    yield();
    DeserializationError error = deserializeJson(doc, file);
    yield();
    if(error) {
      Serial.println("配置文件错误，使用默认设置");
      Serial.println(error.c_str());
    } else {
      uint8_t e;

      // 适用于两只眼睛或全局程序配置的值...
      stackReserve    = dwim(doc["stackReserve"], stackReserve),
      eyeRadius       = dwim(doc["eyeRadius"]);
      eyelidIndex     = dwim(doc["eyelidIndex"]);
      irisRadius      = dwim(doc["irisRadius"]);
      slitPupilRadius = dwim(doc["slitPupilRadius"]);
      gazeMax         = dwim(doc["gazeMax"], gazeMax);
      JsonVariant v;
      v = doc["coverage"];
      if(v.is<int>() || v.is<float>()) coverage = v.as<float>();
      v = doc["upperEyelid"];
      if(v.is<const char*>())    upperEyelidFilename = strdup(v);
      v = doc["lowerEyelid"];
      if(v.is<const char*>())    lowerEyelidFilename = strdup(v);

      lightSensorMin   = doc["lightSensorMin"] | lightSensorMin;
      lightSensorMax   = doc["lightSensorMax"] | lightSensorMax;
      if(lightSensorMin > 1023)   lightSensorMin = 1023;
      else if(lightSensorMin < 0) lightSensorMin = 0;
      if(lightSensorMax > 1023)   lightSensorMax = 1023;
      else if(lightSensorMax < 0) lightSensorMax = 0;
      if(lightSensorMin > lightSensorMax) {
        uint16_t  temp = lightSensorMin;
        lightSensorMin = lightSensorMax;
        lightSensorMax = temp;
      }
      lightSensorCurve = doc["lightSensorCurve"] | lightSensorCurve;
      if(lightSensorCurve < 0.01) lightSensorCurve = 0.01;

      // 瞳孔大小在代码中的表示方式与设置文件中的表示方式不同。
      // 将其表示为 "pupilMin"（瞳孔大小的最小比例，从 0.0 到 1.0）
      // 和 pupilMax（瞳孔大小的最大比例）似乎更容易理解。
      // 但在代码中，它实际上表示为 irisMin（上述 pupilMax 的倒数）
      // 和 irisRange（添加到 irisMin 的量，产生 pupilMin 的倒数）。
      float pMax = doc["pupilMax"] | (1.0 - irisMin),
            pMin = doc["pupilMin"] | (1.0 - (irisMin + irisRange));
      if(pMin > 1.0)      pMin = 1.0;
      else if(pMin < 0.0) pMin = 0.0;
      if(pMax > 1.0)      pMax = 1.0;
      else if(pMax < 0.0) pMax = 0.0;
      if(pMin > pMax) {
        float temp = pMin;
        pMin = pMax;
        pMax = temp;
      }
      irisMin   = (1.0 - pMax);
      irisRange = (pMax - pMin);

      lightSensorPin = doc["lightSensor"]   | lightSensorPin;
      boopPin        = doc["boopSensor"]    | boopPin;
// 在启动时计算，现在不从文件中读取
//      boopThreshold  = doc["boopThreshold"] | boopThreshold;

      // 可以每只眼睛不同但具有共同默认值的值...
      uint16_t    pupilColor   = dwim(doc["pupilColor"] , eye[0].pupilColor),
                  backColor    = dwim(doc["backColor"]  , eye[0].backColor),
                  irisColor    = dwim(doc["irisColor"]  , eye[0].iris.color),
                  scleraColor  = dwim(doc["scleraColor"], eye[0].sclera.color),
                  irisMirror   = 0,
                  scleraMirror = 0,
                  irisAngle    = 0,
                  scleraAngle  = 0,
                  irisiSpin    = 0,
                  scleraiSpin  = 0;
      float       irisSpin     = 0.0,
                  scleraSpin   = 0.0;
      JsonVariant iristv       = doc["irisTexture"],
                  scleratv     = doc["scleraTexture"];

      rotation  = doc["rotate"] | rotation; // 屏幕旋转（GFX 库）
      rotation &= 3;

      v = doc["tracking"];
      if(v.is<bool>()) tracking = v.as<bool>();
      v = doc["squint"];
      if(v.is<float>()) {
        trackFactor = 1.0 - v.as<float>();
        if(trackFactor < 0.0)      trackFactor = 0.0;
        else if(trackFactor > 1.0) trackFactor = 1.0;
      }

      // 将顺时针整数（0-1023）或浮点数（0.0-1.0）值转换为内部使用的逆时针整数：
      v = doc["irisSpin"];
      if(v.is<float>()) irisSpin   = v.as<float>() * -1024.0;
      v = doc["scleraSpin"];
      if(v.is<float>()) scleraSpin = v.as<float>() * -1024.0;
      v = doc["irisiSpin"];
      if(v.is<int>()) irisiSpin    = v.as<int>();
      v = doc["scleraiSpin"];
      if(v.is<int>()) scleraiSpin  = v.as<int>();
      v = doc["irisMirror"];
      if(v.is<bool>() || v.is<int>()) irisMirror   = v ? 1023 : 0;
      v = doc["scleraMirror"];
      if(v.is<bool>() || v.is<int>()) scleraMirror = v ? 1023 : 0;
      for(e=0; e<NUM_EYES; e++) {
        eye[e].pupilColor    = pupilColor;
        eye[e].backColor     = backColor;
        eye[e].iris.color    = irisColor;
        eye[e].sclera.color  = scleraColor;
        // 全局设置的 irisAngle 和 scleraAngle 每次都会被读取，
        // 因为如果没有全局设置，每只眼睛都有不同的默认值。
        // 如果全局设置，则首先覆盖...
        v = doc["irisAngle"];
        if(v.is<int>())        irisAngle   = 1023 - (v.as<int>() & 1023);
        else if(v.is<float>()) irisAngle   = 1023 - ((int)(v.as<float>() * 1024.0) & 1023);
        else                   irisAngle   = eye[e].iris.angle;
        eye[e].iris.angle    = eye[e].iris.startAngle   = irisAngle;
        v = doc["scleraAngle"];
        if(v.is<int>())        scleraAngle = 1023 - (v.as<int>() & 1023);
        else if(v.is<float>()) scleraAngle = 1023 - ((int)(v.as<float>() * 1024.0) & 1023);
        else                   scleraAngle = eye[e].sclera.angle;
        eye[e].sclera.angle  = eye[e].sclera.startAngle = scleraAngle;
        eye[e].iris.mirror   = irisMirror;
        eye[e].sclera.mirror = scleraMirror;
        eye[e].iris.spin     = irisSpin;
        eye[e].sclera.spin   = scleraSpin;
        eye[e].iris.iSpin    = irisiSpin;
        eye[e].sclera.iSpin  = scleraiSpin;
        // iris 和 sclera 文件名是为每只眼睛 strdup 的，而不是共享一个公共指针，
        // 原因是在覆盖其中一个时，尝试正确处理 free/strdup 会变得非常混乱。
        // 所以这会浪费一点 RAM，但只是文件名的大小，并且只在初始化期间。NBD。
        if(iristv.is<const char*>())   eye[e].iris.filename   = strdup(iristv);
        if(scleratv.is<const char*>()) eye[e].sclera.filename = strdup(scleratv);
        eye[e].rotation = rotation; // 可能会在每只眼睛的代码中被覆盖
      }

#if NUM_EYES > 1
      // 处理任何不同的每只眼睛设置...
      // 并非所有内容都可以每只眼睛配置。颜色和纹理内容可以。
      // 其他内容如虹膜大小或瞳孔形状则不行，原因是没有足够的 RAM 来存储两只眼睛的极角/距离表。
      for(uint8_t e=0; e<NUM_EYES; e++) {
        eye[e].pupilColor    = dwim(doc[eye[e].name]["pupilColor"]  , eye[e].pupilColor);
        eye[e].backColor     = dwim(doc[eye[e].name]["backColor"]   , eye[e].backColor);
        eye[e].iris.color    = dwim(doc[eye[e].name]["irisColor"]   , eye[e].iris.color);
        eye[e].sclera.color  = dwim(doc[eye[e].name]["scleraColor"] , eye[e].sclera.color);
        v = doc[eye[e].name]["irisAngle"];
        if(v.is<int>())        eye[e].iris.angle   = 1023 - (v.as<int>()   & 1023);
        else if(v.is<float>()) eye[e].iris.angle   = 1023 - ((int)(v.as<float>()  * 1024.0) & 1023);
        eye[e].iris.startAngle = eye[e].iris.angle;
        v = doc[eye[e].name]["scleraAngle"];
        if(v.is<int>())        eye[e].sclera.angle = 1023 - (v.as<int>() & 1023);
        else if(v.is<float>()) eye[e].sclera.angle = 1023 - ((int)(v.as<float>() * 1024.0) & 1023);
        eye[e].sclera.startAngle = eye[e].sclera.angle;
        v = doc[eye[e].name]["irisSpin"];
        if(v.is<float>()) eye[e].iris.spin   = v.as<float>() * -1024.0;
        v = doc[eye[e].name]["scleraSpin"];
        if(v.is<float>()) eye[e].sclera.spin = v.as<float>() * -1024.0;
        v = doc[eye[e].name]["irisiSpin"];
        if(v.is<int>()) eye[e].iris.iSpin   = v.as<int>();
        v = doc[eye[e].name]["scleraiSpin"];
        if(v.is<int>()) eye[e].sclera.iSpin = v.as<int>();
        v = doc[eye[e].name]["irisMirror"];
        if(v.is<bool>() || v.is<int>()) eye[e].iris.mirror   = v ? 1023 : 0;
        v = doc[eye[e].name]["scleraMirror"];
        if(v.is<bool>() || v.is<int>()) eye[e].sclera.mirror = v ? 1023 : 0;
        v = doc[eye[e].name]["irisTexture"];
        if(v.is<const char*>()) {                           // 指定了每只眼睛的虹膜纹理？
          if(eye[e].iris.filename) free(eye[e].iris.filename); // 删除旧名称（如果有）
          eye[e].iris.filename = strdup(v);                    // 保存新名称
        }
        v = doc[eye[e].name]["scleraTexture"]; // 同上，巩膜
        if(v.is<const char*>()) {
          if(eye[e].sclera.filename) free(eye[e].sclera.filename);
          eye[e].sclera.filename = strdup(v);
        }
        eye[e].rotation  = doc[eye[e].name]["rotate"] | rotation;
        eye[e].rotation &= 3;
      }
#endif
#if defined(ADAFRUIT_MONSTER_M4SK_EXPRESS)
      v = doc["voice"];
      if(v.is<bool>()) voiceOn = v.as<bool>();
      currentPitch = defaultPitch = doc["pitch"] | defaultPitch;
      gain = doc["gain"] | gain;
      modulate = doc["modulate"] | modulate;
      v = doc["waveform"];
      if(v.is<const char*>()) { // 如果是字符串...
        if(!strncasecmp(     v, "sq", 2)) waveform = 1;
        else if(!strncasecmp(v, "si", 2)) waveform = 2;
        else if(!strncasecmp(v, "t" , 1)) waveform = 3;
        else if(!strncasecmp(v, "sa", 2)) waveform = 4;
        else                              waveform = 0;
      }
#endif // ADAFRUIT_MONSTER_M4SK_EXPRESS
    }
    file.close();
  } else {
    Serial.println("无法打开配置文件，使用默认设置");
  }
  // 如果配置文件丢失或错误，初始化默认值 ----------

  // 一些默认值在 globals.h 中初始化（因为无法检查这些无效输入），
  // 其他值在此处初始化，如果有明显的标志（例如值为 0 表示“使用默认值”）。

  // 默认眼睛大小设置为略大于屏幕。这是故意的，因为位移效果在其极端情况下看起来最差...
  // 这允许瞳孔移动到显示器的边缘，同时保持与位移限制的几个像素距离。
  if(!eyeRadius) eyeRadius = DISPLAY_SIZE/2 + 5;
  else           eyeRadius = abs(eyeRadius);
  eyeDiameter  = eyeRadius * 2;
  eyelidIndex &= 0xFF;      // 从表中：learn.adafruit.com/assets/61921
  eyelidColor  = eyelidIndex * 0x0101; // 将 eyelidIndex 扩展为 16 位 RGB

  if(!irisRadius) irisRadius = DISPLAY_SIZE/4; // 屏幕像素中的大小
  else            irisRadius = abs(irisRadius);
  slitPupilRadius = abs(slitPupilRadius);
  if(slitPupilRadius > irisRadius) slitPupilRadius = irisRadius;

  if(coverage < 0.0)      coverage = 0.0;
  else if(coverage > 1.0) coverage = 1.0;
  mapRadius   = (int)(eyeRadius * M_PI * coverage + 0.5);
  mapDiameter = mapRadius * 2;
}

// 眼睑和纹理贴图文件处理 ------------------------------------

// 加载一个眼睑，将位图转换为 2 个数组（每列的最小值、最大值）。
// 传入文件名、目标数组（最小值、最大值，各 240 个元素）。
ImageReturnCode loadEyelid(char *filename,
  uint8_t *minArray, uint8_t *maxArray, uint8_t init, uint32_t maxRam) {
  Adafruit_Image  image; // 图像对象在堆栈上，像素数据在堆上
  int32_t         w, h;
  uint32_t        tempBytes;
  uint8_t        *tempPtr = NULL;
  ImageReturnCode status;
  Adafruit_ImageReader *reader;

  yield();
  reader = arcada.getImageReader();
  if (!reader) {
     return IMAGE_ERR_FILE_NOT_FOUND;
  }

  memset(minArray, init, DISPLAY_SIZE); // 用初始值填充眼睑数组以
  memset(maxArray, init, DISPLAY_SIZE); // 标记“此列没有眼睑数据”

  // 这是 m4eyes.ino 中描述的“助推器座位”
  yield();
  if(reader->bmpDimensions(filename, &w, &h) == IMAGE_SUCCESS) {
    tempBytes = ((w + 7) / 8) * h; // 位图大小（字节）
    if (maxRam > tempBytes) {
      if((tempPtr = (uint8_t *)malloc(maxRam - tempBytes)) != NULL) {
        // 对 tempPtr 进行一些引用，否则优化器会删除分配！
        tempPtr[0] = 0;
      }
    }
    // 不要在此处嵌套图像读取情况。如果找到碎片问题的罪魁祸首，
    // 这段代码（和后面的 free() 块）很容易删除。
  }

  yield();
  if((status = reader->loadBMP(filename, image)) == IMAGE_SUCCESS) {
    if(image.getFormat() == IMAGE_1) { // 必须是 1 位图像
      uint16_t *palette = image.getPalette();
      uint8_t   white = (!palette || (palette[1] > palette[0]));
      int       x, y, ix, iy, sx1, sx2, sy1, sy2;
      // 将眼睑图像居中/裁剪到屏幕...
      sx1 = (DISPLAY_SIZE - image.width()) / 2;  // 最左边的像素，屏幕空间
      sy1 = (DISPLAY_SIZE - image.height()) / 2; // 最上面的像素，屏幕空间
      sx2 = sx1 + image.width() - 1;    // 最右边的像素，屏幕空间
      sy2 = sy1 + image.height() - 1;   // 最下面的像素，屏幕空间
      ix  = -sx1;                       // 最左边的像素，图像空间
      iy  = -sy1;                       // 最上面的像素，图像空间
      if(sx1 <   0) sx1 =   0;          // 图像比屏幕宽
      if(sy1 <   0) sy1 =   0;          // 图像比屏幕高
      if(sx2 > (DISPLAY_SIZE-1)) sx2 = DISPLAY_SIZE - 1; // 图像比屏幕宽
      if(sy2 > (DISPLAY_SIZE-1)) sy2 = DISPLAY_SIZE - 1; // 图像比屏幕高
      if(ix   <   0) ix   =   0;        // 图像比屏幕窄
      if(iy   <   0) iy   =   0;        // 图像比屏幕短

      GFXcanvas1 *canvas = (GFXcanvas1 *)image.getCanvas();
      uint8_t    *buffer = canvas->getBuffer();
      int         bytesPerLine = (image.width() + 7) / 8;
      for(x=sx1; x <= sx2; x++, ix++) { // 对于每列...
        yield();
        // 获取图像缓冲区的初始指针
        uint8_t *ptr  = &buffer[iy * bytesPerLine + ix / 8];
        uint8_t  mask = 0x80 >> (ix & 7); // 列掩码
        uint8_t  wbit = white ? mask : 0; // 白色像素的位值
        int      miny = 255, maxy;
        for(y=sy1; y<=sy2; y++, ptr += bytesPerLine) {
          if((*ptr & mask) == wbit) { // 像素是否设置？
            if(miny == 255) miny = y; // 如果是第一个设置的像素，设置 miny
            maxy = y;                 // 如果有设置的像素，设置 max
          }
        }
        if(miny != 255) {
          // 由于稍后使用的坐标系（屏幕旋转），
          // 在存储之前翻转 min/max 和 Y 坐标...
          maxArray[x] = DISPLAY_SIZE - 1 - miny;
          minArray[x] = DISPLAY_SIZE - 1 - maxy;
        }
      }
    } else {
      status = IMAGE_ERR_FORMAT; // 不要直接返回，需要释放...
    }
  }

  if(tempPtr) {
    free(tempPtr);
  }
  // 图像析构函数将处理该对象数据的释放

  return status;
}

ImageReturnCode loadTexture(char *filename, uint16_t **data,
  uint16_t *width, uint16_t *height, uint32_t maxRam) {
  Adafruit_Image  image; // 图像对象在堆栈上，像素数据在堆上
  int32_t         w, h;
  uint32_t        tempBytes;
  uint8_t        *tempPtr = NULL;
  ImageReturnCode status;
  Adafruit_ImageReader *reader;

  yield();
  reader = arcada.getImageReader();
  if (!reader) {
     return IMAGE_ERR_FILE_NOT_FOUND;
  }
  
  // 这是 m4eyes.ino 中描述的“助推器座位”
  if(reader->bmpDimensions(filename, &w, &h) == IMAGE_SUCCESS) {
    tempBytes = w * h * 2; // 图像大小（字节）（转换为 16bpp）
    if (maxRam > tempBytes) {
      if((tempPtr = (uint8_t *)malloc(maxRam - tempBytes)) != NULL) {
        // 对 tempPtr 进行一些引用，否则优化器会删除分配！
        tempPtr[0] = 0;
      }
    }
    // 不要在此处嵌套图像读取情况。如果找到碎片问题的罪魁祸首，
    // 这段代码（和后面的 free() 块）很容易删除。
  }

  yield();
  if((status = reader->loadBMP(filename, image)) == IMAGE_SUCCESS) {
    if(image.getFormat() == IMAGE_16) { // 必须是 16 位图像
      Serial.println("纹理已加载！");
      GFXcanvas16 *canvas = (GFXcanvas16 *)image.getCanvas();
      canvas->byteSwap(); // 匹配屏幕的字节序以进行直接 DMA 传输
      *width  = image.width();
      *height = image.height();
      *data = (uint16_t *)arcada.writeDataToFlash((uint8_t *)canvas->getBuffer(),
        (int)*width * (int)*height * 2);
    } else {
      status = IMAGE_ERR_FORMAT; // 不要直接返回，需要释放...
    }
  }

  if(tempPtr) {
    free(tempPtr);
  }
  // 图像析构函数将处理该对象数据的释放

  return status;
}