# JPEG 降采样解码（DCT-scaled decode）对照报告

测试图：4032×3024，quality 90，4:4:4 / 4:2:0 各两张（平滑渐变 + 高频纹理），
baseline、8-bit。构建：`WITH_SIMD=OFF`，`JDCT_ISLOW`（精确整数 IDCT）。

## 1. 接口

libjpeg-turbo 已内置该能力，**无需改库**，在 `jpeg_read_header()` 与
`jpeg_start_decompress()` 之间设置缩放比即可，合法分母为 1/2/4/8：

### 低层 jpeglib 接口（见 `dsdemo_jpeg.c`）

```c
jpeg_read_header(&cinfo, TRUE);
cinfo.out_color_space = JCS_RGB;
cinfo.scale_num   = 1;
cinfo.scale_denom = 8;          /* 1=全解, 2=1/2, 4=1/4, 8=1/8 */
jpeg_start_decompress(&cinfo);
/* cinfo.output_width/output_height 已是缩放后的尺寸 */
```

### TurboJPEG 高层接口（见 `dsdemo_tj.c`）

```c
tjscalingfactor sf = {1, 8};                     /* 或 {1,2}/{1,4} */
tj3SetScalingFactor(handle, sf);
int dw = TJSCALED(jpegWidth,  sf);
int dh = TJSCALED(jpegHeight, sf);
tj3Decompress8(handle, jpeg, jlen, dst, pitch, TJPF_RGB);
```

支持的全部 16 个因子可用 `tj3GetScalingFactors()` 枚举；输出尺寸用
`TJSCALED()` 宏预算（向上取整到 iMCU 边界）。

## 2. 内存对照（真实 malloc 高水位，`mallocpeak.so` 实测）

### 2a. 整文件缓冲模式（`jpeg_mem_src`，含输入 JPEG 缓冲 + 输出 RGB）

单位 KB。括号内为占全解的比例。

| 图 | 全解 | 1/2 | 1/4 | 1/8 |
|---|---:|---:|---:|---:|
| smooth 4:4:4 (JPEG 506KB) | 36346 | 9483 (26%) | 2769 (7.6%) | 1089 (3.0%) |
| smooth 4:2:0 (JPEG 327KB) | 36211 | 9326 (26%) | 2595 (7.2%) | 910 (2.5%) |
| texture 4:4:4 (JPEG 7.3MB) | 43302 | 16439 (38%) | 9725 (22%) | 8045 (19%) |
| texture 4:2:0 (JPEG 4.5MB) | 40495 | 13610 (34%) | 6879 (17%) | 5194 (13%) |

峰值构成：`输入 JPEG 缓冲 + 输出 RGB + 解码器内部缓冲`。输出 RGB 本身按
1/N² 缩减（全解 35721KB → 1/2 8930 → 1/4 2232 → 1/8 558KB）；纹理图
1/8 档的绝对值被输入 JPEG 缓冲（4.5–7.3MB）托底。

### 2b. 流式模式（`jpeg_stdio_src`，无整文件缓冲，逐行输出无全幅 RGB）

单位 KB，解码器真实工作集：

| 图 | 全解 | 1/2 | 1/4 | 1/8 |
|---|---:|---:|---:|---:|
| smooth 4:4:4 | 134 | 57 | 37 | 31 |
| smooth 4:2:0 | 180 | 81 | 42 | 32 |
| texture 4:4:4 | 134 | 57 | 37 | 31 |
| texture 4:2:0 | 180 | 81 | 42 | 32 |

baseline 流式解码的内部工作集只有几十 KB，且 1/8 档相对全解再降
76%–82%；与图像内容无关。这是预览/缩略图场景推荐的接入方式。

## 3. 逐像素对照（DCT 直出 vs 全解后缩放）

参考路径：全解 RGB 后做 box 平均（area）或最近邻（point）整数倍缩放。
指标：最大绝对差 / 平均绝对差 / 差异像素占比 / PSNR。

### 平滑图（smooth 4:4:4）

| 档位 | vs box 最大差 | 均值 | PSNR | vs point PSNR |
|---|---:|---:|---:|---:|
| 1/2 | 2 | 0.041 | 61.94 dB | 60.02 dB |
| 1/4 | 2 | 0.060 | 60.35 dB | 55.46 dB |
| 1/8 | 2 | 0.146 | 56.45 dB | 50.06 dB |

### 高频纹理图（texture 4:2:0，最差情形）

| 档位 | vs box 最大差 | 均值 | PSNR | vs point PSNR |
|---|---:|---:|---:|---:|
| 1/2 | 34 | 7.867 | 28.25 dB | 22.65 dB |
| 1/4 | 22 | 5.445 | 31.45 dB | 15.31 dB |
| 1/8 | 8 | 1.588 | 41.87 dB | 13.44 dB |

### 关于「逐像素一致」

**做不到逐像素相等，这是算法本质决定的，不是实现缺陷：**

1. DCT 降采样在 IDCT 阶段只对 8×8 块的低频系数做逆变换，等价于在
   DCT 域做抗混叠低通 + 抽取；全解后 box 平均是在像素域对 64 个重建
   像素求均值。两者数学上不等价，高频内容差异必然存在。
2. 4:2:0 图的色度上采样发生在缩放后的分辨率上，全解路径发生在全分
   辨率上，色度相位也不同（texture 4:2:0 1/2 档最大差 34 的来源）。

但数据同时说明 DCT 直出在感知质量上**不差于、多数情况下优于**朴素缩
放：它自带抗混叠，对纹理图显著好于最近邻（PSNR 高 6–28 dB），对平滑
图与 box 平均的差异仅 ±2/255（PSNR > 56 dB），肉眼不可分辨。

若业务硬性要求与某条参考路径 bit-exact，唯一办法是保持「全解 + 同一
缩放核」，无法享受 DCT 直出的内存/速度收益。建议把验收标准从逐像素
相等改为 PSNR/最大差门限（如 PSNR ≥ 40dB 或最大差 ≤ 2 用于平滑图）。

## 4. 耗时对照（ms / 次，5 次平均，4032×3024）

| 图 | 档位 | DCT 直出 | 全解 + box |
|---|---|---:|---:|
| smooth 4:4:4 | 1/2 | 19–29 | 73–93 |
| smooth 4:4:4 | 1/8 | 8–12 | 58–81 |
| texture 4:2:0 | 1/2 | 37–55 | 119–139 |
| texture 4:2:0 | 1/8 | 21–30 | 120–134 |

DCT 直出同时省掉全幅 IDCT、色度上采样和缩放遍历，1/8 档约快 4–6 倍。

## 5. 复现方式

```sh
cd build && cmake -DCMAKE_BUILD_TYPE=Release -DWITH_SIMD=OFF .. && make -j
cd ../bench
./run_bench.sh smooth_444.jpg smooth_420.jpg texture_444.jpg texture_420.jpg  # 内存
./dsbench texture_420.jpg 5        # 逐像素 + 耗时
./dsdemo_tj texture_420.jpg        # TurboJPEG 接口示例
./dsdemo_jpeg texture_420.jpg 8    # jpeglib 接口示例
```
