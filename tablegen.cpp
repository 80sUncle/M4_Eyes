// SPDX-FileCopyrightText: 2019 Phillip Burgess for Adafruit Industries
//
// SPDX-License-Identifier: MIT

//34567890123456789012345678901234567890123456789012345678901234567890123456

#include "globals.h"

// 此文件中的代码计算用于眼睛渲染的各种表格。

// 因为 3D 数学可能对我们的微控制器要求太高，
// 所以使用 2D 位移映射来伪造圆形眼球形状，类似于 Photoshop 的位移滤镜或老式的演示场景和屏幕保护程序技巧。
// 这并不是真正的 3D 旋转的准确表示，但足以欺骗普通观察者。

void calcDisplacement() {
  // 为了节省 RAM，位移映射仅为屏幕的四分之一计算，
  // 然后在渲染时沿中间水平/垂直镜像。
  // 此外，只需要计算一个轴的位移，因为眼睛形状在 X/Y 对称，
  // 只需交换轴即可查找相对轴的位移。
  if(displace = (uint8_t *)malloc((DISPLAY_SIZE/2) * (DISPLAY_SIZE/2))) {
    float    eyeRadius2 = (float)(eyeRadius * eyeRadius); // 眼睛半径的平方
    uint8_t  x, y;
    float    dx, dy, d2, d, h, a, pa;
    uint8_t *ptr = displace;
    // 位移映射在传统的“+Y 向上”笛卡尔坐标系中为第一象限计算；
    // 任何镜像或旋转都在眼睛渲染代码中处理。
    for(y=0; y<(DISPLAY_SIZE/2); y++) {
      yield(); // 定期 yield() 确保大容量存储文件系统保持活动状态
      dy  = (float)y + 0.5;
      dy *= dy; // 现在 dy^2
      for(x=0; x<(DISPLAY_SIZE/2); x++) {
        // 获取到原点的距离。像素中心位于 +0.5，这是正常的、理想的并且是设计上的 ——
        // 屏幕中心位于 (120.0,120.0) 落在像素之间，并允许数值正确的镜像。
        dx = (float)x + 0.5;
        d2 = dx * dx + dy;                 // 到原点的距离，平方
        if(d2 <= eyeRadius2) {             // 像素在眼睛区域内
          d      = sqrt(d2);               // 到原点的距离
          h      = sqrt(eyeRadius2 - d2);  // 在 d 处的眼睛半球高度
          a      = atan2(d, h);            // 从中心的角度：0 到 pi/2
          //pa     = a * eyeRadius;        // 转换为像素（不）
          pa = a / M_PI_2 * mapRadius;     // 转换为像素
          dx    /= d;                      // 归一化 2D 向量的 dx 部分
          *ptr++ = (uint8_t)(dx * pa) - x; // 舍入到像素空间（无 +0.5）
        } else {                           // 在眼睛区域外
          *ptr++ = 255;                    // 标记为超出眼睛范围
        }
      }
    }
  }
}


void calcMap(void) {
  int pixels = mapRadius * mapRadius;
  if(polarAngle = (uint8_t *)malloc(pixels * 2)) { // 为两个表分配单个内存块
    polarDist = (int8_t *)&polarAngle[pixels];     // 偏移到第二个表

    // 计算极角和距离

    float mapRadius2  = mapRadius * mapRadius;  // 半径平方
    float iRad        = screen2map(irisRadius); // 虹膜大小在极坐标映射像素中
    float irisRadius2 = iRad * iRad;            // 虹膜大小平方

    uint8_t *anglePtr = polarAngle;
    int8_t  *distPtr  = polarDist;

    // 与位移映射类似，仅计算第一象限，
    // 其他三个象限从此镜像/旋转。
    int   x, y;
    float dx, dy, dy2, d2, d, angle, xp;
    for(y=0; y<mapRadius; y++) {
      yield(); // 定期 yield() 确保大容量存储文件系统保持活动状态
      dy  = (float)y + 0.5;        // Y 到地图中心的距离
      dy2 = dy * dy;
      for(x=0; x<mapRadius; x++) {
        dx = (float)x + 0.5;       // X 到地图中心的距离
        d2 = dx * dx + dy2;        // 到地图中心的距离，平方
        if(d2 > mapRadius2) {      // 如果超过地图大小的一半，平方，
          *anglePtr++ = 0;         // 则标记为超出眼睛范围
          *distPtr++  = -128;
        } else {                   // 否则像素在眼睛区域内...
          angle  = atan2(dy, dx);  // -pi 到 +pi（第一象限中 0 到 +pi/2）
          angle  = M_PI_2 - angle; // 顺时针，0 在顶部
          angle *= 512.0 / M_PI;   // 第一象限中 0 到 <256
          *anglePtr++ = (uint8_t)angle;
          d = sqrt(d2);
          if(d2 > irisRadius2) {
            // 点在巩膜中
            d = (mapRadius - d) / (mapRadius - iRad);
            d *= 127.0;
            *distPtr++ = (int8_t)d; // 0 到 127
          } else {
            // 点在虹膜中（-dist 表示如此）
            d = (iRad - d) / iRad;
            d *= -127.0;
            *distPtr++ = (int8_t)d - 1; // -1 到 -127
          }
        }
      }
    }

    // 如果启用了狭缝瞳孔，覆盖 polarDist 映射的虹膜区域。
    if(slitPupilRadius > 0) {
      // 遍历极坐标映射的虹膜部分的每个像素...
      for(y=0; y < mapRadius; y++) {
        yield(); // 定期 yield() 确保大容量存储文件系统保持活动状态
        dy  = y + 0.5;            // 到中心的距离，Y 分量
        dy2 = dy * dy;
        for(x=0; x < mapRadius; x++) {
          dx = x + 0.5;           // 到中心点的距离，X 分量
          d2 = dx * dx + dy2;     // 到中心的距离，平方
          if(d2 <= irisRadius2) { // 如果在虹膜内...
            yield();
            xp = x + 0.5;
            // 这有点丑陋，因为它迭代计算 polarDist 值...试错法。
            // 应该可以代数简化并找到给定像素的单个 polarDist 点，
            // 但我还没有解决这个问题。
            // 这仅在启动时需要一次，不是完全灾难。
            for(int i=126; i>=0; i--) {
              float ratio = i / 128.0; // 0.0（打开）到接近 1.0（狭缝）（>= 1.0 会导致问题）
              // 根据比例在虹膜顶部和狭缝瞳孔顶部之间插值一个点
              float y1 = iRad - (iRad - slitPupilRadius) * ratio;
              // (x1 为 0，因此从下面的方程中删除)
              // 另一个点在虹膜右侧和眼睛中心之间，反比例
              float x2 = iRad * (1.0 - ratio);
              // (y2 也为零，同样处理)
              // 找到穿过上述两个点并在 Y 为 0.0 处的圆的中心 X 坐标
              float xc = (x2 * x2 - y1 * y1) / (2 * x2);
              dx = x2 - xc;       // 从圆心到右边缘的距离
              float r2 = dx * dx; // 圆心到右边缘的距离平方
              dx = xp - xc;       // ...的 X 分量
              d2 = dx * dx + dy2; // 像素到左侧 'xc' 点的距离
              if(d2 <= r2) {      // 如果点在圆内...
                polarDist[y * mapRadius + x] = (int8_t)(-1 - i); // 设置为距离 'i'
                break;
              }
            }
          }
        }
      }
    }
  }
}

// 将屏幕像素中的测量值缩放到极坐标映射像素
float screen2map(int in) {
  return atan2(in, sqrt(eyeRadius * eyeRadius - in * in)) / M_PI_2 * mapRadius;
}

// 上述的反函数
float map2screen(int in) {
  return sin((float)in / (float)mapRadius) * M_PI_2 * eyeRadius;
}
