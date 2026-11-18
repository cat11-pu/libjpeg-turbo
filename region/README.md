# 按区域 + 缩放倍数解码的窄接口

在 libjpeg-turbo 的 `jpeg_crop_scanline()` / `jpeg_skip_scanlines()` 之上
封装的一个窄接口：调用方只给一个矩形和一个 DCT 缩放倍数，拿回**恰好是
目标尺寸**的紧密打包像素缓冲，不暴露任何 iMCU 对齐细节。

## 1. 接口

全部声明在 `regiondec.h`，实现在 `regiondec.c`，只依赖 libjpeg 公共 API。

```c
typedef struct {
    unsigned x, y, w, h;   /* 矩形，缩放后图像坐标系 */
    int scale_num;         /* 1..8，配合 scale_denom=8 => M/8 倍 DCT 缩放 */
    int scale_denom;       /* 固定 8 */
    int format;            /* RDEC_FMT_RGB(3 字节/像素) 或 RDEC_FMT_GRAY */
    int fancy;             /* 1=平滑上采样(默认)；0=nosmooth，边缘也逐字节一致 */
} rdec_request;

int rdec_decode_file(const char *path, const rdec_request *req,
                     unsigned char **out_pixels, size_t *out_size,
                     rdec_info *info);

int rdec_decode_mem(const unsigned char *jpeg, size_t jpeg_len,
                    const rdec_request *req,
                    unsigned char **out_pixels, size_t *out_size,
                    rdec_info *info);

int rdec_scaled_dimensions(unsigned image_width, unsigned image_height,
                           int scale_num, int scale_denom,
                           unsigned *scaled_width, unsigned *scaled_height);
```

返回码：`RDEC_OK(0)`、`RDEC_ERR_ARGUMENT`、`RDEC_ERR_FORMAT`、
`RDEC_ERR_SCALE`、`RDEC_ERR_REGION`、`RDEC_ERR_MEMORY`、`RDEC_ERR_INTERNAL`，
文字描述见 `rdec_strerror()`。

### 输出契约

- 缓冲严格为 `w * h * components` 字节，逐行紧密打包、自上而下、无行填充。
- 像素内容等价于：整图按同一倍数解码 → 取行 `[y, y+h)`、列 `[x, x+w)`。
- 内部可能解码一个更宽的 iMCU 对齐条带，但只把请求窗口 `memmove` 进输出，
  因此对齐填充永远不会串行到调用方数据里，行间也不会互相污染。
- 坐标全部是**缩放后图像坐标系**（与 `jpeg_crop_scanline` 文档一致）。

## 2. 边界语义与已知例外

libjpeg-turbo 的部分宽度解码要求左边界落在 iMCU 列上，`jpeg_crop_scanline`
会把 `x` 向左对齐、把条带加宽。本接口对调用方隐藏这一调整（`rdec_info`
里的 `actual_x/actual_w` 仅供观测），并保证返回窗口精确。

唯一的像素级例外（库文档 `doc/libjpeg.txt` 明确记载）：4:2:0 / 4:2:2 在
**fancy（平滑）色度上采样**下，当解码条带的左右边缘落在图像**内部**时，
边缘列可能把条带边缘当作整图边缘处理，导致该列与整图解码不完全相同。
实测差异**只出现在窗口的最左/最右列，内部像素从不失配**；置
`fancy = 0`（对应 `djpeg -nosmooth`）后该例外消失，所有像素逐字节一致。

垂直方向用 `jpeg_skip_scanlines`，支持任意行偏移（内部按 iMCU 行 + 部分行
读取处理），没有对应的对齐例外。

## 3. 边界用例

测试夹具 `rdec_test.c` 对每张图、每个缩放/色彩/上采样组合都跑下面 8 个
矩形（负标记表示贴边，0 表示取到边缘；固定矩形在最小 16x12 缩放图上自动
收敛到边界内）：

| 用例 | 矩形语义 |
| --- | --- |
| full-frame | 整张缩放图 |
| top-left 1px | `(0,0)` 单像素 |
| bottom-right 1px | 右下角单像素 |
| unaligned interior | 非 iMCU 对齐内部矩形 `17x11+(3,5)` |
| right-edge strip | 贴右边界竖条 |
| bottom-edge strip | 贴下边界横条 |
| odd 1-wide column | 1 像素宽竖列 |
| odd 1-tall row | 1 像素高横行 |

错误用例（全部断言具体返回码，见测试输出 `[PASS] error case ...`）：
零宽/零高 → `RDEC_ERR_ARGUMENT`；矩形越过右/下边缘、起点超出图像 →
`RDEC_ERR_REGION`；缩放分子越界（9/8）、非 DCT 分母（1/3）→
`RDEC_ERR_SCALE`；格式枚举非法 → `RDEC_ERR_ARGUMENT`。

## 4. 一致性数据

复现：

```sh
mkdir -p build && cd build
cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
cd ../region && ./run_tests.sh
```

矩阵：8 张 JPEG（灰度/4:2:0/4:2:2/4:4:0/4:4:4/渐进式 4:2:0，外加两张
512px 宽、条带边缘可落在图内的图）× 8 个缩放倍数 1/8..8/8 ×
RGB/GRAY × fancy/nosmooth × 8 个矩形。

在本机的实测汇总（`test_results.txt`）：

```
component samples compared : 12,132,048
total mismatching samples  : 734
byte-exact decode runs     : 2,032
edge-only diff runs        : 16   (全部为 fancy 上采样、且条带边缘在图内)
runs with interior diffs   : 0    (内部像素零失配)
```

- 734 个失配样本全部位于窗口边缘列；置 `fancy=0` 后失配数为 **0**，
  即 2,048 个组合全部逐字节一致。
- 与 `djpeg -crop` 交叉验证：`djpeg` 输出 iMCU 对齐后的整条带（示例中
  31 列），本接口输出请求的精确窗口（30 列）；跳过条带首列后逐字节相同，
  且本接口输出与“整图解码后裁剪”逐行 0 失配。
- 用 `-fsanitize=address,undefined` 构建库与测试跑完整矩阵，无 ASan/UBSan
  报告、无内存泄漏。

## 5. 文件与命令行示例

- `regiondec.h` / `regiondec.c`：窄接口。
- `rdec_test.c`：参考全量解码器 + 逐字节对比 + 边界/错误用例。
- `rdec.c`：示例 CLI，输出 PPM/PGM。

```sh
make
# 4/8 倍缩放，取缩放图上 30x20+(17,9) 的矩形，nosmooth 保证边缘逐字节一致
./rdec -n -s 4 -r 30x20+17+9 jpgs/wide420.jpg out.ppm
# 灰度、1/8 倍
./rdec -g -n -s 1 -r 5x3+1+1 jpgs/gray.jpg out.pgm
```
