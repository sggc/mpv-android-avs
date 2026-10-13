# av3a-libs — AV3A 解码器（固定版本，CI 直接取用）

这里放的是**已经过验证的 AV3A(Audio Vivid / AVS3-P3) 解码器**二进制，
构建时直接复制使用，**不再从外部仓库下载**。

## 为什么 pin 在仓库里，而不是去外面下载

原先工作流从 `wuxianlin/ijkplayer-av3a` 下载预编译版（约 254K）。问题是：

* 那是**另一个构建**，和 App 里实际验证通过的不是同一个文件；
* `src/av3a_dec.c` 的逻辑是**优先 dlsym 公开 API**，找不到才回退到内部符号名
  （`Avs3AllocDecoder` / `Avs3ParseBsFrameHeader` / `Avs3Decode`）；
* 而内部符号名的**签名与公开 API 不同**（`parse_header` 是 4 参不是 6 参、
  `avs3_decode` 不收输入缓冲）—— 历史上正是这条回退路径导致过 SIGSEGV。

也就是说：如果外部那个版本缺公开 API，构建会"成功"，但打出的包里 AV3A
走的是崩溃路径，而且要到真机播放才发现。

放进仓库并**记录 SHA256** 之后：输入固定、可复现、可审计，
外部仓库变更也影响不到这里。

## 文件清单

| 文件 | 体积 | SHA256 |
|---|---|---|
| `arm64-v8a/libavs3a_decoder.so` | 355808 | `594b19602b17aadba0ae3b954e2b51005154928ed56847da76a1ed3cfa32c8ca` |
| `armeabi-v7a/libavs3a_decoder.so` | 342076 | `b5e8413a6f2a9f40cc8e401e8759d8ba7151c82fe8cbc9ac65116cf924231a3f` |
| `model.bin` | 79930 | `55d56a17dbfa22da21f2fa945ebe13824d246fe8b2669a63e0440316282ae068` |

机器可读版本见 `SHA256SUMS`，CI 会用它校验。

## 两个 ABI 都必须导出这两组符号

CI 的 "AV3A public-API check" 会逐个核对，缺失即 fail：

* **公开 API**（wrapper 首选，签名已确认）：
  `avs3_create_decoder` / `avs3_destroy_decoder` / `parse_header` / `avs3_decode`
* **内部名**（仅作存在性参考，**不要**依赖其签名）：
  `Avs3AllocDecoder` / `Avs3ParseBsFrameHeader` / `Avs3Decode` / `Avs3DecoderDestroy`

当前两份都是 4/4 + 4/4。

## model.bin 的作用

AV3A 解码器初始化时会直接 `fopen("model.bin", "rb")` 读这个神经网络模型
（相对当前工作目录）。App 侧从 assets 拷到 `filesDir` 并把工作目录切过去，
见 `com.fongmi.android.tv.App`。构建产物里它被放进 `assets/`。

## 更新方式

如果将来要换新版本：

1. 用 `tools/av3a-build/`（在 TV-release 仓库里）重新编译；
2. **在真机上验证 AV3A 能出声**（不能只靠符号检查）；
3. 覆盖这里的文件，重新生成 `SHA256SUMS`；
4. 更新本 README 里的体积与哈希。
