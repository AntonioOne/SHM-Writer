# ShmFrameTaggerOverlay 1.0

ShmFrameTaggerOverlay 用于 VTD 共享内存图像的**防重复帧**。

它作为第三个写入方，按固定频率给 VTD 写入共享内存的每一帧 RDB 图像打上递增的计数标记。这样即使 VTD 卡顿、没有输出新图像，读取方每次读到的图像也都不一样。画面中央还会显示计数的后四位数字，方便直接肉眼确认。

程序没有界面，在后台运行即可。

---

## 1. 包内容

```
ShmFrameTaggerOverlay-1.0-linux-x86_64/
├── ShmFrameTaggerOverlay        可执行程序
├── lib/                         程序自带的运行库（无需另外安装）
│   ├── libopencv_core.so.4.2
│   ├── libopencv_imgproc.so.4.2
│   └── libtbb.so.2
└── README.md                    本说明
```

程序会自动使用同目录 `lib/` 中的库，不需要设置 `LD_LIBRARY_PATH`，也不需要安装 OpenCV。
请保持 `ShmFrameTaggerOverlay` 和 `lib/` 在同一目录下。

## 2. 系统要求

| 项目 | 要求 |
|---|---|
| 架构 | x86_64 Linux |
| 系统 | Ubuntu 20.04 / 22.04 / 24.04（需要 glibc ≥ 2.29、GCC 9 及以上版本的 libstdc++） |
| 运行位置 | 与 VTD 的图像生成器（IG）**在同一台机器上**（共享内存只在本机有效） |
| 权限 | 对 VTD 图像共享内存有读写权限（VTD 默认创建的共享内存权限为 777，普通用户即可） |
| 图形界面 | 不需要 |

## 3. 快速开始

```bash
tar xzf ShmFrameTaggerOverlay-1.0-linux-x86_64.tar.gz
cd ShmFrameTaggerOverlay-1.0-linux-x86_64
./ShmFrameTaggerOverlay
```

先启动 VTD 或先启动本程序都可以。程序会自动发现并接管 key 为 `0x8100`–`0x81FF` 的所有图像共享内存；
VTD 重启、共享内存被删除或重建时也会自动重新连接。

按 `Ctrl+C` 停止。

## 4. 命令行参数

```
./ShmFrameTaggerOverlay [--rate <Hz>] [--poll-us <微秒>] [--range <起始key> <结束key>] [key ...]
```

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--rate <Hz>` | `200` | 写入频率。每写入一次计数加 1。读取方的读取频率应**不高于**此频率。 |
| `--poll-us <微秒>` | `50` | 两次写入之间检查 VTD 是否覆盖了中央数字区域的间隔。VTD 写入新帧后，程序会在其发布前把数字重新画上。值越小数字越及时，CPU 占用越高。`0` = 持续检查（占满一个 CPU 核），`-1` = 关闭检查。 |
| `--range <起始> <结束>` | `0x8100 0x81ff` | 自动接管的共享内存 key 范围。 |
| `key ...` | 无 | 只处理指定的 key，例如 `./ShmFrameTaggerOverlay 0x08130 0x08131`。 |

示例：

```bash
./ShmFrameTaggerOverlay --rate 400                 # 400 Hz 写入（推荐用于 60 Hz 以上的读取）
./ShmFrameTaggerOverlay 0x08130 0x08131            # 只处理两路
./ShmFrameTaggerOverlay --poll-us 100              # 降低 CPU 占用
```

## 5. 运行输出

```
SHM Frame Tagger + Overlay: 400 Hz, keys 0x08100-0x081ff, check every 50 us, Ctrl+C to stop
[0x08130] attached (3110836 bytes)          ← 发现并接管一路共享内存
[0x08131] attached (3110836 bytes)
[stats] 2 segment(s)  value 3974 (digits 3974)  400.0 writes/s  max late 0.30 ms  readback fail 21  stamp restores 1200  band collisions 0
[0x08131] detached                          ← 该共享内存已被删除（例如 VTD 退出）
```

每 10 秒输出一行 `[stats]`：

| 字段 | 含义 |
|---|---|
| `segment(s)` | 当前接管的共享内存数量 |
| `value` / `digits` | 当前计数值 / 画面上显示的四位数字 |
| `writes/s` | 实际写入频率 |
| `max late` | 最近一秒内定时的最大延迟 |
| `readback fail` | 写入后立即被 VTD 覆盖的次数（正常现象，下一次写入会补上） |
| `stamp restores` | VTD 写新帧覆盖了计数后，程序将其恢复的次数（约等于 VTD 的出帧数） |
| `band collisions` | 与 VTD 同时写中央区域的罕见冲突次数（正常为 0 或很少） |

## 6. 程序写入的内容

只修改每个缓冲中图像像素数据的以下两处，**不修改**共享内存头、缓冲信息以及锁/标志位（LOCK / TC / IG），也不等待、不阻塞 VTD。

**① 前 4 个像素：计数**

- 图像数据最前面 4 个像素的全部字节（RGB8 为 12 字节）组成一个**大端**无符号整数（最后一个字节为最低位）。
- 程序启动时从 0 开始，每次写入加 1。例如 `00 00 00 00 00 00 00 00 00 00 04 D2` = 1234。
- 这 4 个像素在内存中位于最前面；由于 VTD 读回的图像是自下而上存放的，它们在显示画面中位于**左下角**。

**② 画面中央：四位数字**

- 计数的后四位（`0000`–`9999`），白色数字加黑色描边，位于画面正中央 100×50 像素范围内。
- 只写数字所在的几行，并且是“VTD 原画面 + 数字”，不会留下旧数字残影，也没有底框。
- 数字的方向按上下翻转后的显示方向绘制，在常规查看器中显示为正。
- 仅支持每通道 8 位的格式（RGB8、RGBA8、8 位灰度）。其他像素格式只写计数，不画数字。

## 7. 工作原理

1. 每 0.5 秒扫描一次系统共享内存，接管范围内含有 RDB 图像的共享内存。
2. 按 `--rate` 的频率，在每个缓冲（双缓冲的两个缓冲都处理）中写入新的计数和数字。
3. 两次写入之间，每隔 `--poll-us` 检查一次：如果 VTD 写入新帧时已经完整覆盖了中央数字区域，立即把新画面作为背景重新画上数字。通常能赶在 VTD 发布这一帧之前完成，读取方读到新帧时数字已经在上面。
4. VTD 卡顿、不写新帧时，计数和数字仍按写入频率持续变化，因此读取方连续两次读到的图像不会相同。

## 8. 性能参考

测试环境：模拟 VTD 共 11 路（2 × 3840×2160 + 9 × 2560×1440，RGB8，每路 60 Hz 整帧写入），`--rate 400`。

| `--poll-us` | 读取方任意时刻读取最新缓冲时能看到数字的比例 | 本程序 CPU 占用（单核） |
|---|---|---|
| 200 | 99.1 % – 99.5 % | 10 % – 15 % |
| 100 | 99.6 % – 99.9 % | 14 % – 16 % |
| 50（默认） | 100 % | 17 % – 19 % |

VTD 降到 16 Hz、读取方以 30 Hz 读取时：连续读到相同图像 0 次，读到无数字图像 0 次（不运行本程序时，180 次读取中有 131 次相同）。

实际占用与路数、分辨率和机器性能有关。

## 9. VTD 侧要求

VTD 的 IG 需要把图像写入 key 在 `0x8100`–`0x81FF` 范围内的共享内存，例如 IG 配置中：

```xml
<RDBInterface name="MyRDBInterface" printDebugInfo="0" ignoreShmFlags="1">
    <ShmLayoutImage key="0x08130" />
</RDBInterface>
...
<Readback readbackThreadAffinity="3" synchronized="1" rdbInterface="MyRDBInterface">
    <Image outputSlot="color"/>
</Readback>
```

本程序不依赖 `ignoreShmFlags` 的设置，也不会改动缓冲标志。

## 10. 注意事项

- **同一时间只运行一个实例。** 多个实例会同时改写同一位置，计数会互相覆盖。
- 读取方的读取频率应不高于 `--rate`，否则两次读取之间可能尚未写入新计数。
- 程序会修改图像中上述两处像素。如果下游算法对图像内容敏感，处理时请忽略左下角的 4 个像素和画面中央的数字区域。
- 程序重启后计数从 0 重新开始。

### 后台运行

```bash
nohup ./ShmFrameTaggerOverlay --rate 400 > tagger.log 2>&1 &
# 停止：
kill <进程号>          # 进程号可用 pgrep ShmFrameTaggerOverlay 查看
```

### 开机自动运行（systemd 示例）

将路径和用户替换为实际值后保存为 `/etc/systemd/system/shm-frame-tagger.service`：

```ini
[Unit]
Description=VTD SHM frame tagger
After=network.target

[Service]
User=vtd
ExecStart=/opt/ShmFrameTaggerOverlay-1.0-linux-x86_64/ShmFrameTaggerOverlay --rate 400
Restart=always

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now shm-frame-tagger
journalctl -u shm-frame-tagger -f      # 查看输出
```

## 11. 常见问题

**启动时报 `GLIBC_2.29 not found` 或 `GLIBCXX_3.4.26 not found`**
系统版本过旧（例如 Ubuntu 18.04）。请在 Ubuntu 20.04 或更新版本上运行。

**启动报 `error while loading shared libraries: libopencv_core.so.4.2`**
`lib/` 目录缺失或与程序不在同一目录。请保持解压后的目录结构不变。

**启动后一直没有 `attached` 输出**
当前没有 key 在 `0x8100`–`0x81FF` 范围内的共享内存。请确认 VTD 已运行并按第 9 节配置；可用 `ipcs -m` 查看现有共享内存。

**`shmat failed: Permission denied`**
当前用户对该共享内存没有写权限。请用创建它的同一用户运行，或调整共享内存权限。

**画面中央没有数字，但计数在变化**
图像像素格式不是每通道 8 位（例如浮点或 16 位格式），或图像小于 100×50。这种情况下只写计数。
