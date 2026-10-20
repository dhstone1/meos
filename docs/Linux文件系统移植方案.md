# Linux 文件系统移植方案（ext2 + ATA）

> 状态：方案（未实施）。本文档回答"把 Linux 文件系统移植进 MeOS"怎么做、做到什么程度、
> 分几步走、每步怎么验收。实施过程中每完成一个阶段，回头把该阶段的"实际落点"更新进
> 《技术文档.md》；本文档只做蓝图与决策记录。
>
> 阅读顺序建议：§1（目标）→ §2（现状，务必先看，文档与代码有漂移）→ §4（决策）→
> §10（阶段计划）。§5–§9 是实施期的规格手册。

---

## 1. 目标与"移植"的确切含义

### 1.1 用户需求

> "移植 linux 文件系统内核到本系统"

字面上把 Linux 内核源码搬进来是做不到的，也不是本方案要做的事。先把这个话说死，
避免后面所有讨论都建立在错误预期上：

- Linux 的 fs 代码是 C，依赖 slab/kmalloc、锁、页缓存、block 层、make/ELF 工具链；
  MeOS 是 34 KB 的纯 NASM 扁平二进制，**没有 C 工具链**（《技术文档.md》§13.5 把
  "C 工具链"单列为构建体系的独立里程碑）、没有堆、没有内核线程。
- 所以"移植"在这里的含义是两条，也是本方案唯二承诺的东西：
  1. **磁盘格式移植**：MeOS 能原样读写 Linux 的 ext2 文件系统镜像——
     和 `mke2fs`/`e2fsck`/Linux 互相认盘。这是"Linux 文件系统"的本体。
  2. **架构移植**：文件访问路径按 Linux 的分层（VFS 挂载表 → 驱动 → 块设备）组织。
     这一半其实已经开始做了——`src/kernel/vfs.inc` 已经是这套形状，本方案把它补完。

### 1.2 目标（做完 FS4 后成立）

- MeOS 从一块 IDE 虚拟硬盘上挂载 ext2 文件系统（挂载点 `/hd`）；
- `ls / cat / stat / cd` 能跨挂载点操作 `/hd` 下的文件；
- 能往 `/hd` 里 `cp / mkdir / touch`，关机后再开机数据还在（持久化）；
- 镜像拿到 Linux 上（或 WSL）`e2fsck -fn` 报 clean，`mount -t ext2` 直接可用；
  反向，`mke2fs` 出来的小镜像 MeOS 也能读。

### 1.3 非目标（明确不做）

| 不做 | 理由 |
| --- | --- |
| ext3/ext4（日志、extent、HTree 目录） | 复杂度对玩具内核是数量级错配，且 ext2 是 Linux 家族里最后一个小到能用汇编写完的成员 |
| dentry cache / inode cache / 页缓存 | 无内存管理子系统，逐次现读，留 1 条 inode 缓存当面子工程 |
| 运行时 `mount` 命令（探测+挂载任意盘） | 挂载表编译期写死够用；运行时挂载留给未来 |
| 符号链接、特殊文件、权限检查 | 单用户 toy OS，mode 字段存了但不解释 |
| 崩溃一致性保证（日志） | 无日志，用写序把损坏窗口缩到最小，详见 D7 |
| FAT32 / ISO9660 | 用户点名 Linux；FAT 簇链语义离 Linux inode 模型更远 |

### 1.4 Linux 概念 → MeOS 落点映射表

这张表是"架构移植"的验收清单——右边这列每一格都该有对应代码：

| Linux | MeOS 落点 | 状态 |
| --- | --- | --- |
| `struct super_block` | `ext2.inc` 的 `ext2_sb_*` 缓存（只存用到的字段） | 本方案 FS3 |
| `struct inode`（读） | `ext2_ino_buf` + `ext2_ino_last` 一条缓存 | 本方案 FS3 |
| `struct inode`（分配） | `ext2_alloc_inode`（inode 位图顺序扫描） | 本方案 FS4 |
| 路径走查 / dentry lookup | `ext2_lookup` 逐组件线性扫目录（无 dcache） | 本方案 FS3 |
| `struct file` / `file_operations` | `vfs_fd_tab` + `vfs_drv_tab` 每行一套函数指针 | **已存在**（vfs.inc） |
| `mount` / `struct vfsmount` | `vfs_mount_tab` 每行 {挂载点, 驱动号}（编译期） | **已存在**，加一行 |
| `readdir` / `getdents` | `VIA_READDIR` 驱动槽位 + `vfs_readdir` | 本方案 FS3 |
| `struct block_device` / gendisk | `ata.inc`，写死主盘 0x1F0，无盘表 | 本方案 FS2 |
| buffer_head / 页缓存 | `ext2_blk_buf` 单块缓冲，write-through | 本方案 FS3/FS4 |
| ext2 块分配器 | `ext2_alloc_block`（块位图扫描 + 超块/GD 计数回写） | 本方案 FS4 |
| 文件系统类型注册 (`register_filesystem`) | `vfs_drv_tab` 加行 = 注册 | 模式**已存在** |
| ext2 journal | 无 | 非目标 |

---

## 2. 现状盘点（代码即真相）

**《技术文档.md》落后于代码，本文档以代码现状为准。** 实施前先做一轮对账（阶段 FS0）。
下面是盘点结果，附证据。

### 2.1 已经有的资产（比文档声称的多）

- **VFS 层已存在**：`src/kernel/vfs.inc`（914 行）——挂载表、最长前缀匹配
  （`vfs_mount_match`）、vnode（16 B）、fd 表（`VFS_MAX_FD equ 8`）、驱动函数表
  （`vfs_drv_tab`，每行 {open, read, write, close}，行号 = 驱动号）。vfs.inc 头注释
  原话："加一个新文件系统 = 写一个驱动 + 在 vfs_drv_tab 加一行 + 给挂载表加一项，
  内核其余部分不碰"。**本方案正是这句话的执行。**
- **MeFS 已存在**：`src/kernel/fs.inc`（1883 行）RAM 文件系统，16 MB @ 0x00100000，
  512 B 块链 + 位图，构建期快照 `ramfs.inc` 由 `tools/mkramfs.py` 烤进载荷。
  当前快照 8192 B，含 `/hello.txt`、`/etc/motd` 等。
- **devfs 已存在**：`/dev/null`、`/dev/console`（只读固定文本）。
- **挂载表现状**（vfs.inc:829-843）：

  ```nasm
  vfs_mount_tab:
      dd mnt_root, 0                 ; 下标 0 = DRV_MEFS："/"
      dd mnt_dev, 0                  ; 下标 1 = DRV_DEV ："/dev"
      dd 0, 0, 0, 0, 0, 0            ; 空槽（VFS_MAX_MNT equ 4，已用 2）
  vfs_fsname_tab: dd fsname_mefs, fsname_devfs, 0,0,0,0
  vfs_drv_tab:
      dd mefs_open, mefs_read, mefs_write, mefs_close   ; 行 0
      dd dev_open,  dev_read,  dev_write,  dev_close    ; 行 1
  ```

  每行 8 字节 = {挂载点字符串指针, 保留}，**驱动号 = 行下标**。加 ext2 = 第 2 行。
- **命令层**：`ls cat touch mkdir rm pwd cd cp mv df`（MeFS 直连）+ `mount stat`
  （走 VFS）。`cat` 已经完全走 VFS（`vfs_cat` → `vfs_open/read/close`），
  `ls` 对 `/dev` 有一个分支特例。
- **初始化顺序**（kernel.asm:412-446）：`fs_init` → `vfs_init` 都在 `con_init`
  **之前**——文件系统初始化失败时控制台还没起来，只能靠诊断块说话。
  本方案沿用这个约束：ATA/ext2 初始化失败必须**软着陆**（写 D_ 字段、照常开机）。

### 2.2 文档漂移清单（FS0 已修的账）

> FS0 处置完毕（2026-10-16）：下表保留作历史记录，右列是当时的处置方案，
> 所有行已落地——§15 改写为"设计记录/未实施"、checkdoc 不再读 llm.inc、
> PAYLOAD_SECTORS 统一为 128、需求.md M3 改"部分完成"、技术文档补了
> §8.3/§12.8、selftest 数字 232 已写对。

| 漂移 | 证据 | 处置 |
| --- | --- | --- |
| `技术文档.md` §15 描述 llm 命令 + `src/kernel/llm.inc` | 磁盘上**没有** llm.inc，kernel.asm 里 grep `llm` 零命中 | §15 标注"未实施/已移除"或补回实现，二选一，FS0 定 |
| `checkdoc.py` 一跑就崩 | `FileNotFoundError: src/kernel/llm.inc`（cached() 读它），exit=1 | §15 修好后自然恢复；FS0 验收 = checkdoc exit 0 |
| checkdoc.py 断言 `PAYLOAD_SECTORS=128` | boot.asm 实际 `equ 96` | 改 checkdoc 或改 boot.asm，以未来预算为准（见 §8.4） |
| `需求.md` M3"简单文件系统：未开始" | fs.inc/vfs.inc/ramfs 都在，命令都通了 | 需求.md 状态改"部分完成（MeFS+VFS），ext2 见本方案" |
| `技术文档.md` 无 fs.inc/vfs.inc 的任何章节 | §8.3 命令表、§12 例程索引都停在 echo/net | FS0 补 §8.3/§12，后续每阶段跟着改 |
| `boot.asm:17` 注释"M5 起载荷里多了 MeFS" | 全库没有 M4/M5 的定义出处 | 顺手把里程碑编号的出处写清（本文档用 FS0..FS5 独立编号，避免撞车） |
| `技术文档.md` §15 说 llm 回放缓冲等 selftest 相关 43 字节 | kernel.asm:1728 `selftest_len equ 232` | §13/§15 数字一并刷新 |

**这套仓库的铁律是"doc stale is worse than no doc"（checkdoc.py 头注释），
所以 FS0 不做任何功能，专门把账对平。**

### 2.3 为什么现在做正合适

VFS 的驱动接口已经就位（open/read/write/close + 行号即驱动号），挂载表还有空槽，
`cat`/`stat` 天生走 VFS——**ext2 落地 = 新增两个 .inc + 挂载表加一行 + 一块盘**，
内核其余部分几乎不动。这正是当初把 VFS 先行铺好的回报。

---

## 3. 总体架构

### 3.1 分层图（加粗 = 本方案新增）

```
shell 命令层（kernel.asm: ls cat stat mount df cp mkdir touch rm … disk）
    │  老命令渐渐改走 VFS，不再直摸 MeFS
    ▼
VFS（vfs.inc）：挂载表 → 最长前缀匹配 → 驱动号
    fd 表 / vnode / **vfs_readdir（新增）**
    ├── 行 0 mefs  驱动（fs.inc，RAM 快照，读写已通）
    ├── 行 1 devfs 驱动（vfs.inc 内嵌，只读）
    └── 行 2 **ext2 驱动（ext2.inc，新增）**
              │  只认：块号 → 块数据；inode 号 → inode 结构
              ▼
        **块设备层（ata.inc，新增）**：ATA PIO LBA28，主盘 0x1F0，轮询无中断
              │  sector = 512 B，`rep insw/outsw` 搬运
              ▼
        **hd.vmdk / hd-flat.vmdk（VMware 两文件 flat 虚盘）**
              ext2 镜像由 tools/mkext2.py 生成，Linux/e2fsck 可校验
```

读路径：`cat /hd/hello.txt` → `vfs_open`（fs_action 规范化路径 + 匹配 `/hd`）
→ `ext2_open("/hello.txt")`（根 inode 2 逐组件查找）→ fd → `ext2_read`
（FD_POS → 块号/块内偏移 → `ext2_read_block` → `ata_read_sectors`）。

### 3.2 文件清单

| 文件 | 类型 | 内容 | 阶段 |
| --- | --- | --- | --- |
| `src/kernel/ata.inc` | 新增 | ATA PIO 驱动 + `ata_init` | FS2 |
| `src/kernel/ext2.inc` | 新增 | ext2 驱动（只读 → 读写） | FS3/FS4 |
| `src/kernel/vfs.inc` | 修改 | 驱动表加第 5 列 readdir、挂载表第 2 行、`vfs_readdir` | FS3 |
| `src/kernel/kernel.asm` | 修改 | include、初始化调用、`disk` 命令、`ls` 重写、selftest 扩展 | FS2-4 |
| `src/kernel/fs.inc` | 修改 | fs_err 加 2 个码（FS_IO/FS_NOTSUPP）、mefs readdir 适配 | FS3 |
| `tools/mkext2.py` | 新增 | ext2 镜像 + VMDK 生成器（含自检） | FS1 |
| `tools/ext2check.py` | 新增 | 独立 ext2 解析校验器（构建期 + 关机后校验写回） | FS1/FS4 |
| `tools/verify_ext2.py` | 新增 | .vmem 断言 D_ATA_*/D_EXT2_*（挂进 selftest 流程） | FS2/FS3 |
| `src/ext2root/` | 新增 | 烤进硬盘的内容树（hello.txt、etc/motd…） | FS1 |
| `vm/meos.vmx` | 修改 | 加 `ide0:0` 硬盘（hd.vmdk） | FS1 |
| `build.ps1` | 修改 | 新步骤 [2.5/6] mkext2.py；产物复制；-Clean | FS1 |
| `docs/技术文档.md` | 修改 | §5 诊断块新字段、§8.3/§12 新命令与例程、§15 对账 | FS0 起持续 |

---

## 4. 关键决策

每条决策都带被否决的备选——没有备选的决策是拍脑袋。

**D1 文件系统选 ext2 rev 0，不选 ext3/ext4，不选 FAT32。**
- ext4：extent 树、HTree 目录校验、flex_bg——驱动复杂度直接 x10，玩具内核吞不下；
  且只读还要解析 journal 才能安全挂载。
- FAT32：宿主 Windows 生成/检查方便，但簇链 + FAT 表语义与 Linux 的 inode/位图模型
  完全异构，"移植 Linux 文件系统"就名不副实了。
- ext2 rev 0：结构全部定长、文档（内核头文件 + e2fsprogs）详尽、e2fsck 可以当
  裁决者；128 B inode、线性目录，用汇编写完是苦役但不是奇迹。
- rev 0 而非 rev 1：inode 尺寸等字段 rev 1 才有，rev 0 全部隐含固定（inode 128 B、
  first_ino 11），解析分支最少。rev 1 兼容放 FS5。

**D2 存储介质 = 真虚拟硬盘 + ATA PIO，不做 RAM 快照、不动 BIOS、不动启动器。**
- PM 之后没有 BIOS（§13.5 钦点的坑），INT 13h 不可用——文档自己已经指路
  "磁盘访问需要自己的 ATA PIO 驱动（端口 0x1F0..0x1F7）"。本方案照办。
- 备选 A"ext2 镜像像 ramfs.inc 一样烧进载荷"：8 MB 镜像 vs 载荷上限 48 KB，
  软盘 1.44 MB 也装不下；且"持久化"直接没戏。否决。
- 备选 B"挂第二张软盘/用 El Torito 追加扇区"：要改 boot.asm 装载循环、改 mkfloppy
  布局，为一个可避免的改动引入回归面。否决——硬盘是独立设备，零启动器改动。
- 附带收益：这盘同时是将来交换区/用户数据的落点，一步到位。

**D3 虚盘格式 = 两文件 flat VMDK，由 `tools/mkext2.py` 直接生成。**
- VMware 的 flat VMDK = 一个文本描述符（`hd.vmdk`）+ 一个裸字节文件
  （`hd-flat.vmdk`），后者就是逐扇区原样映射——宿主侧生成/校验都不需要任何
  VMware 工具，ext2check.py 直接读 flat 文件。
- 备选"monolithicSparse"（压缩粒度表）：二进制格式复杂，无收益。否决。
- 备选"hd.img + plainhardDisk"：新版 Workstation 对 plainhardDisk 支持不明，
  不赌。否决。

**D4 镜像起步规格 = 8 MB、1024 B 块、单块组、256 inode、rev 0；但驱动只信超块，不信布局。**
- 1024 B 块 = 单块组 8192 块 = 位图恰好 1 块，扇区换算左移 1 位就完事；
  256 inode × 128 B = 32 KB inode 表。这是"一个驱动分支都不用走"的最大规模。
- **关键纪律：所有结构位置（GDT 块、位图块、inode 表块、根 inode）都必须从
  超块/组描述符现读现算，禁止硬编码 261 之类的数字**——这样 `mke2fs` 生成的
  镜像（布局细节不同）才能直接挂（§9 互操作验收）。我们控制生成器只为自检方便，
  不为驱动省事。

**D5 VFS 驱动接口加第 5 个槽位 `VIA_READDIR`，不另开旁门。**
- 现状缺口：驱动只有 open/read/write/close，**列不了目录**——`ls` 至今直摸
  MeFS 的 `fs_dir_first/next`，对 `/dev` 走特例分支。ext2 的目录枚举必须进驱动接口，
  否则每加一个 fs 就多一个 if。
- 方案：`vfs_drv_tab` 每行 4 → 5 个指针（VIA_READDIR equ 16，VIA_LEN 16→20）；
  语义 = Linux 的 readdir：`esi=fd 指针, edi=输出记录`，EAX=1 有条目 / 0 遍历完，
  游标复用 fd 的 FD_POS（目录字节偏移）。mefs/devfs/ext2 三个驱动各实现一份。
- `ls` 整体重写为 `vfs_open → 循环 vfs_readdir → 打印`，`/dev` 特例分支删除
  （devfs 的 readdir 由注册表给出）。这是"架构移植"落到实处的一步。
- 备选"目录也能 read，返回原始结构"：把磁盘格式泄漏进命令层，且 MeFS 42 B 记录
  和 ext2 变长记录没法统一。否决。

**D6 路径与 cwd：FS3 只支持绝对路径访问 `/hd`；FS4 才上收 cwd 到 VFS。**
- 现状：cwd 是 MeFS 的（`cwd_buf`/`fs_cur_dir`/`fs_cur_rec`），`fs_action` 用它
  拼相对路径。`cd /hd/x` 若放行，后续相对路径会被 MeFS 的 cwd 解释，错得悄无声息。
- FS3：`cd` 拒绝进入非 MeFS 路径（`fs_path_lookup` 查不到自然报 FS_NOENT，
  行为正确无需新代码）；`cat / stat / ls` 用绝对路径走 `/hd`，全部可用。
- FS4：引入 `vfs_cwd`（纯字符串，VFS 层），`fs_action` 改用它；`cd` 用
  `vfs_open` 验证目标。MeFS 的三个 cwd 变量退化为驱动私有。这是本方案里
  最伤筋动骨的一次重构，单独排在写支持之后，避免两座桥一起修。
  **实施记录（2026-10-21）：这条重构最终没做**——FS4 落地时 cwd 仍留在
  fs.inc（`cwd_buf` + `fs_action`），`cd` 走 MeFS 的 `fs_path_lookup`，
  进 `/hd` 自然报 FS_NOENT（即上面 FS3 段的"行为正确无需新代码"状态，
  无静默错路由）；`/hd` 全部访问用绝对路径，§9.1 FS4 验收面不含 cd，
  全绿不受影响。vfs_cwd 上收连同 `cd /hd` 支持**留给 FS5**，理由是
  D6 自己说的"伤筋动骨、单独排"——验收全绿的系统上为无验收项的重构
  重开工程风险，违反本方案的顺序纪律。

**D7 写支持 write-through + 固定写序（数据 → 位图 → inode → 目录条目 → 超块/GD），无日志。**
- 每次 cp/mkdir/touch 立即落盘（无缓存可脏）。崩溃时最坏丢一个刚建的文件或
  多一个泄漏的位图位，`e2fsck` 可修——这是 ext2 无日志设计的原装语义，
  我们连这个语义都一并移植了（顺序即元数据策略）。
- `ata_write_sectors`（outsw）与读对称；写后等 BSY 清才算完成，不做 FLUSH CACHE
  （虚盘写穿）。
- 备选"先做写缓冲再批量落盘"：崩溃面更大、代码更多。否决。

**D8 诊断先行：每个阶段动之前先定 D_ 字段和阶段值。**
- ATA 与 ext2 初始化都发生在 `con_init` 之前（沿用现有顺序），失败时屏幕上
  什么都没有——必须能从 .vmem 里读出"卡在哪一步"（D_ATA_STAGE/D_EXT2_STAGE），
  这是这个仓库调试的立身之本（§14 哲学：diag 块 + 宿主机读 .vmem）。

---

## 5. 磁盘与镜像规格

### 5.1 VMDK 描述符（`tools/mkext2.py` 产出，全文样例）

```
# Disk DescriptorFile
version=1
CID=fffffffe
parentCID=ffffffff
createType="monolithicFlat"

# Extent description
RW 16384 FLAT "hd-flat.vmdk" 0

# The Disk Data Base
#DDB

ddb.adapterType = "ide"
ddb.geometry.cylinders = "17"
ddb.geometry.heads = "16"
ddb.geometry.sectors = "63"
ddb.toolsVersion = "0"
```

- 8 MB = 16384 扇区；几何 17×16×63 = 17136 ≥ 16384（IDE 用 LBA 翻译，几何只需覆盖）。
- **createType 必须是 `monolithicFlat`**（描述符 + 单个 -flat.vmdk）。本方案初稿
  写的 twoFileMaxExtent 不是合法类型，VMware 报 "Unknown disk create type"
  直接拒挂——FS1 实测踩到的坑，已改。
- **RW 扇区数必须和 hd-flat.vmdk 的字节数严格相等**（差一字节 VMware 拒挂，
  见 §11 踩坑 5）。
- `vm/meos.vmx` 追加（hd*.vmdk 由 build.ps1 直接生成到 `build\`，vmx 被复制到
  build\ 后相对路径解析到同目录——**没有 vm/ 复制这回事**）：

  ```
  ide0:0.present = "TRUE"
  ide0:0.fileName = "hd.vmdk"
  ide0:0.mode = "persistent"
  ide0:0.startConnected = "TRUE"
  ```

  必须是 ide0:0（主盘 = 端口 0x1F0、drive 位 0）；挂从盘驱动选盘就错（§11 踩坑 16）。

### 5.2 ext2 镜像布局（mkext2.py 的产出形态，驱动不依赖）

8 MB / 1024 B 块 / 单组 / 256 inode / rev 0：

| 块号 | 内容 |
| --- | --- |
| 0 | 引导块占位（bs=1024 的 `first_data_block=1` 传统，标已用） |
| 1 | 超块（字节偏移 1024，即扇区 2–3） |
| 2 | 组描述符表（单组 = 1 项 32 B） |
| 3 | 块位图（8192 位 = 1 块） |
| 4 | inode 位图（256 位 = 32 B） |
| 5..36 | inode 表（256 × 128 B = 32 块） |
| 37 | 根目录数据（inode 2 的 i_block[0]） |
| 38+ | 文件数据 |

内容树 `src/ext2root/`：`hello.txt`、`readme.txt`、`etc/motd`（与 ramfs 相同的
字符纪律：只允许可打印 ASCII 0x20..0x7E + 换行，终端字模只有这些——生成器复用
mkramfs 的 `check_text`）。目录条目 `.`/`..`/普通项按 e2fsprogs 的排法：
`.` rec_len=12、`..` rec_len=12、末条目 rec_len 吃满到块尾。

### 5.3 驱动用到的结构字段（偏移表，写驱动时的唯一参考）

**超块**（块 `first_data_block` 起的 1024 B；bs=1024 时在块 1）：

| 偏移 | 类型 | 字段 | 驱动用途 |
| --- | --- | --- | --- |
| 0x00 | u32 | s_inodes_count | 校验 |
| 0x04 | u32 | s_blocks_count | 块号合法性上界 |
| 0x0C | u32 | s_free_blocks_count | df/写回 |
| 0x10 | u32 | s_free_inodes_count | 写回 |
| 0x14 | u32 | s_first_data_block | bs=1024 → 1，否则 0（GDT 块 = 它 +1） |
| 0x18 | u32 | s_log_block_size | 块字节 = 1024<<此值（v1 只支持 0，否则 FS_NOTSUPP） |
| 0x20 | u32 | s_blocks_per_group | v1 只支持单组，越界 FS_NOTSUPP |
| 0x28 | u32 | s_inodes_per_group | inode→组/序号换算 |
| 0x38 | u16 | **s_magic = 0xEF53** | 挂载判据 |
| 0x3A | u16 | s_state | 1=clean（打印用） |
| 0x4C | u32 | s_rev_level | 0 才继续；1 → FS_NOTSUPP（FS5 再说） |
| 0x60 | u32 | s_feature_incompat | bit1 FILETYPE：目录条目 type 字段是否可信 |

**组描述符**（32 B，块 `first_data_block+1` 起的表，单组取第 0 项）：

| 偏移 | 字段 |
| --- | --- |
| 0x00 u32 | bg_block_bitmap |
| 0x04 u32 | bg_inode_bitmap |
| 0x08 u32 | bg_inode_table |
| 0x0C u16 | bg_free_blocks_count（写回） |
| 0x0E u16 | bg_free_inodes_count（写回） |
| 0x10 u16 | bg_used_dirs_count（mkdir 时 +1） |

**inode**（128 B，rev 0 定长）：

| 偏移 | 字段 | 用途 |
| --- | --- | --- |
| 0x00 u16 | i_mode | 高 4 位类型：0x8=reg、0x4=dir（kind 映射） |
| 0x04 u32 | i_size | 文件字节数 |
| 0x1A u16 | i_links_count | rm/新建时维护 |
| 0x1C u32 | i_blocks | 512 B 扇区计数（新建时 = 块数×2） |
| 0x28 | i_block[15] | 0..11 直接块；[12] 一级间接；[13]/[14] 双/三级（v1 不走） |

**目录条目**（变长，rec_len 4 对齐且 ≥8；inode=0 = 已删空洞）：

| 偏移 | 字段 |
| --- | --- |
| 0x00 u32 | inode |
| 0x04 u16 | rec_len（**movzx！word 惯犯**） |
| 0x06 u8 | name_len（v1 上限 31，超了 FS_NAMELEN——MeOS 缓冲只有 32 B） |
| 0x07 u8 | file_type（FILETYPE 特性启用才可信：1=reg 2=dir；否则查 inode mode） |
| 0x08.. | name（不加 NUL） |

**换算**：块 b 的字节偏移 = b << 10；扇区 = b << 1；`ext2_read_block(EAX=b)` =
`ata_read_sectors(LBA=b*2, 2, ext2_blk_buf)`。根 inode 固定 = 2；保留 inode
0/1/10（0 坏、1 bad blocks、10 resize，实际可用从 11 起——分配器从 11 扫）。

---

## 6. 内核模块规格

统一沿用本仓库调用约定：**pushad 保护、结果走 EAX、错误写 `fs_err`**。
新增错误码（fs.inc 常量 + `fs_err_msgs` 表同步加两项，注意表是按索引对位的）：

```nasm
FS_IO        equ 9      ; "i/o error"   —— ATA 超时/错误、结构越界
FS_NOTSUPP   equ 10     ; "not supported" —— 双间接/rev1/4096 块等超纲操作
```

### 6.1 `src/kernel/ata.inc`（FS2，估 ~350 行）

端口表（全部常量放文件头，写进《技术文档.md》§12）：

```nasm
ATA_DATA      equ 0x1F0   ; 读写扇区数据（16 位 insw/outsw）
ATA_ERR       equ 0x1F1   ; 读=错误寄存器；写=features
ATA_NSECT     equ 0x1F2   ; 扇区数（0 = 256！）
ATA_LBA_LO    equ 0x1F3
ATA_LBA_MID   equ 0x1F4
ATA_LBA_HI    equ 0x1F5
ATA_DRIVE     equ 0x1F6   ; 0xE0 | ((lba>>24)&0x0F)：主盘 + LBA 模式
ATA_STATUS    equ 0x1F7   ; 读=状态；写=命令（0x20 读 / 0x30 写 / 0xEC IDENTIFY）
ATA_ALTSTAT   equ 0x3F6   ; 备用状态（读 4 次 = 400ns 规定等待）；写=设备控制（复位）
```

例程（对外入口只列这三个 + 命令）：

| 例程 | 入参 → 结果 | 说明 |
| --- | --- | --- |
| `ata_init` | → EAX=1 找到盘 | 复位（0x3F6 写 0x04 再写 0x00）→ 浮动总线检查（状态 0/0xFF = 没盘）→ IDENTIFY → 解析 model（词 27..46，每词两字符、首字符在高字节）/ LBA28 容量（词 120）缓存进 ata_ident_buf / ata_sectors；D_ATA_STAGE 走到 2。**IDENTIFY 已并入 init，不单设 ata_identify 例程**（2026-10-20 实施时合并，免去独立入口的参数传递） |
| `ata_read_sectors` | EAX=LBA, ECX=扇区数(0 表示 256), EDI=目的 → EAX=1 | 全协议：等 BSY 清 → 400ns → 填 6 个寄存器 → 0x20 → 每扇区等 DRQ → `cld; rep insw` 256 字；成功记 D_ATA_LBA/D_ATA_COUNT、D_ATA_STAGE 升到 3 |
| `ata_write_sectors` | EAX=LBA, ECX, ESI=源 → EAX=1 | 对称的 outsw 版 + 收尾 FLUSH 0xE7（FS4） |
| `cmd_do_disk` | —— | `disk` 命令（FS_CMD0 挂进 .fs_cmds）：打 model / `sectors: N (M MB)`（MB=N>>11）/ LBA2 的 `lba2+0x38: 53 EF (ext2 magic ok)`；无盘打 `ata0: no disk` |

内部例程 `ata_wait_ready`（BSY 清，带 ~5×10⁶ 次循环上限，超时置 D_ATA_ERR bit8
并返回 0——**轮询必须有超时，否则 VM 假死**）、`ata_wait_drq`（BSY=0 且 DRQ=1）、
`ata_400ns`（读 0x3F6 四次）。

中断纪律：**保持轮询，不动 PIC**。IRQ14/15 本来就屏蔽着，`sti` 环境下轮询安全；
不要顺手解开（§11 踩坑 15）。

### 6.2 `src/kernel/ext2.inc`（FS3 只读 ~700 行 + FS4 写 ~600 行）

数据区（注意：flat binary 里这些占**文件字节**，进载荷预算）：

```nasm
ext2_blk_buf   times 1024 db 0   ; 块缓冲（读块/间接块复用需小心，见下）
ext2_ind_buf   times 1024 db 0   ; 一级间接块专用（数据块和间接块同时要用）
ext2_ino_buf   times 128  db 0   ; 当前 inode
ata_sec_buf    times 512  db 0   ; 超块/位图等半块读写的过渡缓冲
ext2_ino_last  dd 0              ; inode 一条缓存（命中免读）
ext2_sb_blocks dd 0  ...         ; 超块缓存字段（见 §5.3 用途列）
```

例程清单：

| 例程 | 入参 → 结果 | 阶段 |
| --- | --- | --- |
| `ext2_init` | → 初始化 | `ata_init` → 读块 1 → 验 0xEF53 → 读 GD0 → 缓存 → D_EXT2_STAGE=3；任一步失败软着陆返回 |
| `ext2_read_inode` | EAX=ino → ext2_ino_buf | 组 = (ino-1)/s_inodes_per_group（v1 必 0）；表内偏移 = (ino-1)%×128 → 块 = bg_inode_table + off>>10 → 读两扇区再按 0x3FF 偏移拷 128 B；`ext2_ino_last` 命中直通 |
| `ext2_lookup` | EAX=起始 ino, ESI=相对路径 → EAX=目标 ino | 逐组件：`ext2_read_inode` → 按块扫 i_block[0..11] → 逐条目比对（**rec_len movzx**、inode=0 跳过、name_len>31 → FS_NAMELEN） |
| `ext2_open` | ESI=rel 路径, EDI=vnode（驱动接口） | "" → 根 inode 2；否则 `ext2_lookup`；vnode：kind←i_mode>>12 映射 VN_FILE/VN_DIR，a=ino，size=i_size |
| `ext2_read` | ESI=fd, EDI=buf, ECX=len → EAX=字节数 | FD_POS → (块号 idx = pos>>10, 块内 = pos&0x3FF)；idx<12 直取；12..267 走 `ext2_ind_buf`（i_block[12] 一级间接）；≥268 → FS_NOTSUPP（v1 文件上限 274432 B，够用并写进文档） |
| `ext2_readdir` | ESI=fd, EDI=40B 记录 → EAX=1/0 | FD_POS 当目录字节游标；跳 inode=0；查 inode 补 kind/size；记录布局 = {u8 kind, u8 pad, u32 size, char name[32]}（和 MeFS 的 readdir 输出同构，ls 只认这个） |
| `ext2_write` | ESI=fd, EDI=buf, ECX=len → EAX=字节数 | FS4。见下面写流程 |
| `ext2_close` | → EAX=0 | 空操作（无缓存脏页） |
| `ext2_alloc_block` | → EAX=块号或 FS_NOSPACE | FS4。读块位图 → 从 11 起扫 0 位 → 置位 → `ata_write_sectors` 写回位图块 → GD/sb free 计数 -- → 回写块 1（超块）和块 2（GD） |
| `ext2_alloc_inode` | → EAX=ino | FS4。同套路，扫 inode 位图 |
| `ext2_dir_insert` | EAX=父 ino, ESI=名字, EBX=新 ino | FS4。见 §6.2.1 |
| `ext2_bmap_set` | EAX=inode, ECX=idx, EDX=块号 | FS4。直接块写 i_block[idx]；>11 → FS_NOTSUPP（写路径不碰间接块，v1 上限 12 KB/新文件，文档写明） |

**FS4 写流程（cp /hd/x 为例，顺序即 D7 的元数据写序）**：
1. `ext2_alloc_inode` + 填 inode（mode=0x81A4，links=1，size=0，时间 = ticks/1000）
   → 写回 inode 所在扇区；
2. 循环：`ext2_alloc_block` → 数据写 `ata_write_sectors` → `ext2_bmap_set`；
   每满一块更新 size 并写回 inode；
3. `ext2_dir_insert` 父目录加条目；
4. 完成：D_EXT2_FREE 更新。
   崩在任何一步，最坏 = 位图泄漏一块（e2fsck 可修），符合 D7。

**§6.2.1 目录插入算法**（Linux ext2 的原版套路）：
- 读父目录最后一个数据块；块内最后一个条目的 rec_len 吃满到块尾；
- `need = 8 + (name_len+3 & ~3)`；若 `rec_len - 8 - (name_len+3 & ~3) ≥ need`：
  末条目 rec_len 收缩为其实际长度，尾部放新条目、rec_len = 剩余全部；
- 放不下：`ext2_alloc_block` 扩目录（新块整块做新条目的 rec_len=1024），
  i_block[] 顺延、size += 1024；
- 生成器（mkext2.py）用**同一算法**造初始目录，自检时两边互验——同一段逻辑
  两处实现，错不了太远。

### 6.3 `src/kernel/vfs.inc` 变更（FS3）

```nasm
VIA_READDIR    equ 16     ; 新驱动槽位
VIA_LEN        equ 20     ; 16 → 20
DRV_EXT2       equ 2      ; 新驱动号（= 挂载表行下标）

vfs_mount_tab:
    dd mnt_root, 0                 ; 行 0 mefs   "/"
    dd mnt_dev, 0                  ; 行 1 devfs  "/dev"
    dd mnt_hd, 0                   ; 行 2 ext2   "/hd"   ← 新增
    dd 0, 0                        ; 空槽到 VFS_MAX_MNT=4
vfs_fsname_tab: dd fsname_mefs, fsname_devfs, fsname_ext2, 0
vfs_drv_tab:
    dd mefs_open, mefs_read, mefs_write, mefs_close, mefs_readdir   ; 行 0
    dd dev_open,  dev_read,  dev_write,  dev_close,  dev_readdir    ; 行 1
    dd ext2_open, ext2_read, ext2_write, ext2_close, ext2_readdir   ; 行 2
```

- `vfs_readdir(EAX=fd, EDI=记录)`：按 FD_DRV 取第 5 列指针调用；
  `mefs_readdir` 把 fd 的 vnode.a 当目录链首块接 `fs_dir_first/fs_dir_next`
  （迭代器游标就放 FD_POS，fs_dir_* 的全局 fs_iter_blk/slot 留给兼容，逐步退役）；
  `dev_readdir` 遍历 `devfs_ent`。
- `mount` 命令输出自动多一行（`mefs on /  devfs on /dev  ext2 on /hd`）。
- `stat /hd/x`、`cat /hd/x` **一行不改**就走通——它们已经是纯 VFS 客户端。

### 6.4 `src/kernel/kernel.asm` 变更

- `%include "ata.inc"`、`%include "ext2.inc"` 追加在 `vfs.inc` 之后。
- `pm_entry`（412-446 区间）：`call vfs_init` 之后插 `call ata_init`、
  `call ext2_init`——都在 `con_init` 之前，失败只写 D_（D8）。
- 新命令 `disk`（FS_CMD 宏照抄现有套路）：IDENTIFY 摘要（model 字符串、
  LBA 扇区数）+ 读扇区 2 偏移 0x30..0x40 的 hex（0x38 处应见 `53 EF`——
  超块魔数 0xEF53 的小端序，这就是"盘上真是 ext2"的第一眼证据）+ 打印 D_ATA_*。
- `ls` 重写为 vfs_readdir 版（§D5），`/dev` 特例删除；
- `df` 加 `/hd` 行（读 D_EXT2_FREE）；
- FS4：`cp` 目标 `/hd/...` 分支到 ext2 写路径；`touch/mkdir` 同理；
  `rm` 只做 MeFS（ext2 的 unlink 留 FS5）；
- `msg_help` 更新加 `disk`；`selftest_data` 追加
  `ls /hd`、`cat /hd/hello.txt`、`stat /hd/hello.txt`（FS4 再追加
  `cp /hello.txt /hd/c1.txt`、`cat /hd/c1.txt`）的扫描码，
  `selftest_len equ 232` 同步加长（格式：set-1 扫描码字节流，0x00 = 空档拍，
  见 kernel.asm:1729 注释与既有条目）。

---

## 7. 诊断块新增字段（kernel.asm D_ 常量区 + 《技术文档.md》§5 同步）

接在现有 D_FS_CWD(0x204，64 B) 之后、fs 命令环形记录（DIAG+0x500）之前的空档：

```nasm
D_ATA_STAGE   equ 0x244   ; dword 0=未探测 1=复位过 2=IDENTIFY 成功 3=扇区读通 4=超块验过
D_ATA_ERR     equ 0x248   ; dword 0=无错；否则状态寄存器 bit0-7 + bit8=超时标志
D_ATA_LBA     equ 0x24C   ; dword 最近一次读写的 LBA
D_ATA_COUNT   equ 0x250   ; dword 累计读出扇区数
D_EXT2_STAGE  equ 0x254   ; dword 0=未初始化 1=超块读过 2=魔数OK 3=GD/inode表定位
                          ;       4=根目录列得动 5=文件读通 6=写通
D_EXT2_ERR    equ 0x258   ; dword 最近一次 fs_err 镜像
D_EXT2_INO    equ 0x25C   ; dword 最近访问 inode 号
D_EXT2_BLK    equ 0x260   ; dword 最近访问块号
D_EXT2_FREE   equ 0x264   ; dword 超块空闲块数（df/验收对账）
D_EXT2_CMD    equ 0x268   ; dword 进入 ext2 驱动的命令计数
```

占到 0x26C，距离 0x500 的环形记录还有充足余量。**动 vmdiag.py 的字段表时
连带 verify_ext2.py 一起写**——诊断字段先落地、代码后动（D8）。

---

## 8. 构建管线变更

### 8.1 build.ps1

- 步骤 [2.5/6]：`python tools/mkext2.py` → `build/hd-ext2.img`、`build/hd.vmdk`、
  `build/hd-flat.vmdk`（自检失败 = SystemExit，构建中断，学 mkramfs），随后
  `ext2check.py` 复核（fail = throw）。
- 步骤 [6/6] 仍只复制 `vm/meos.vmx` → `build\`：盘三件套本来就生成在 `build\`，
  vmx 的相对路径直接解析（**方案初稿写的 "hd*.vmdk 复制到 vm/" 是错的，实测
  纠正**）。
- `-Clean`：`build\` 整目录删除已覆盖 hd*（VM 运行时删除失败 → 现有
  "-Clean 在 VM 运行时失败"的行为一致，报错即可）。

### 8.2 tools/mkext2.py（~250 行）

- 输入 `src/ext2root/`；输出三件套（§5.1/§5.2）。
- inode 初值：根 mode=0x41ED、文件 0x81A4，links 文件=1/目录=2+子目录数，
  时间戳 0；`i_blocks` = 块数×2。
- 自检（不通过就退出，学 mkramfs 的三面对账）：
  1. 独立重放整棵树（用另一个解析方向走回去，比内容）；
  2. 位图逐位对账：置位集合 == {0,1,2,3,4,5..36,37,数据块...}；
  3. 所有 rec_len 4 对齐、≥8、目录块内无缝隙无越界；
  4. 超块/GD 的 free 计数 == 8192-已用。
- `--from-mke2fs <file>` 模式：不生成，只把现成镜像包成 VMDK（互操作验收用，
  §9）。

### 8.3 tools/ext2check.py / verify_ext2.py

- `ext2check.py`：独立解析器（**不 import mkext2 的任何函数**，互盲才有意义），
  读 flat 文件 → 超块/GD/位图/inode → 递归走树 → 与 `src/ext2root/` 比对 +
  free 计数核对。用法两处：构建后跑一次；VM 关机后对 `build/hd-flat.vmdk` 再跑
  （验证 FS4 写回真的落了盘——VM 运行期文件被锁，必须关机后跑，§11 踩坑 6）。
- `verify_ext2.py`：读 .vmem 断言 D_ATA_STAGE≥3、D_EXT2_STAGE≥5、
  D_EXT2_FREE == ext2check 算出的值。挂在 verify_selftest 流程后面，
  vmtest.ps1 的调用点同步。

### 8.4 载荷预算（盯紧，flat binary 没有 BSS 空洞）

> FS0 已把上限从 96 扇区提到 128：boot.asm:17 与 checkdoc.py 断言现已一致
> （都 = 128），下表按新预算重算。真正的天花板不是软盘（1.44 MB 充裕）而是
> 内存布局：载荷终点 0x8000+64KB = 0x18000 必须低于 `NET_BASE`（0x20000，
> e1000 收发环）——再往上加扇区前先挪 NET_BASE。

| 项 | 字节 |
| --- | --- |
| 现在的 payload.bin | 34 403 |
| 上限（PAYLOAD_SECTORS=128 × 512） | 65 536 |
| 当前余量 | 31 133 |
| ata.inc 代码（估） | ~1 500 |
| ext2 只读（估） | ~3 000 |
| ext2 写（估） | ~3 000 |
| 新命令 + vfs 变更（估） | ~1 500 |
| **新数据缓冲（ext2_blk/ind 1024+1024, ino 128, sec 512, ident 512, 杂项）** | **~3 500** |
| 合计新增（估） | ~12 500 |
| 预计剩余 | ~18 600（宽裕） |

再超了怎么办：`PAYLOAD_SECTORS` 128 → 160（80 KB），先确认 0x8000+80KB
仍低于当时的 NET_BASE。**同步点只有两处**：src/boot/boot.asm:17 和
tools/checkdoc.py 的 CONSTS——mkfloppy.py 没有 LOADER_SECTORS 常量
（它只补齐 512 倍数并卡 1.44 MB，不会静默截断载荷）。
预算超 60% 就先升上限再写代码。

---

## 9. 验证与验收

### 9.1 每阶段验收表

| 阶段 | 验收（全绿才进下一阶段） |
| --- | --- |
| **FS0 对账** ✅ 2026-10-16 | checkdoc.py exit 0（57 项 0 问题）；build + verify_selftest + vmtest.ps1 全绿（回归基准）；技术文档 §8.3/§12/§15、需求.md 状态刷新 |
| **FS1 盘与镜像** ✅ 2026-10-16 | ext2check.py 绿（34 项）；VMware 开机能起（`DISK: OPEN ide0:0 …hd.vmdk 永久 R[]` 无报错，内核无盘逻辑、系统照常——BIOS 忽略硬盘正常启动）；可选：`wsl e2fsck -fn build/hd-ext2.img` 报 clean（未验） |
| **FS2 ATA** ✅ 2026-10-20 | `disk` 命令出 model/容量/扇区 2 偏移 0x38 处的魔数 `53 EF`；.vmem：D_ATA_STAGE=3（verify_ext2.py PASS：STAGE=3/ERR=0/LBA=2/COUNT=1）；verify_selftest 25 行像素全对（16861=16861）；拔盘场景（vmx 临时 ide0:0.present=FALSE）软着陆：VM 活、D_STAGE=6、D_CMD_NUM=21、D_ATA_STAGE=1、disk 打 no disk。排障中修了 4 个真 bug：selftest 的 d 扫描码、mount_match 的 -1 哨兵配无符号 jbe（恒败）、mount_match 返回前缀长度而非驱动号、mefs_read 的 rep-后-ECX-清零记账（见技术文档 §14 踩坑 17-20） |
| **FS3 只读** ✅ 2026-10-20 | selftest 回放 `ls /hd` `cat /hd/hello.txt` `stat /hd/hello.txt`：verify_selftest 25 行像素全对（19458=19458，`/hd` 期望输出由回放器自己的 `_e2_walk` 走查模型生成）；verify_ext2.py PASS（STAGE=5/ERR=0/INO=13/FREE=8150=镜像真值）；`mount` 打印 ext2 on /hd；`cat /hd/hello.txt` 输出 == 宿主文件字节（像素级）、`stat` 的 size=315。排障中修了 3 个真 bug：ata_read_sectors 每扇区指针双重前进（第二扇区砸进隔壁变量，块缓冲 +512 永远是零洞）、fs.inc [调试] 名字段与 D_EXT2_CMD 撞址（"29548 次调用"假象）、ext2_read 的 chunk min/max 比较写反（整块 1024B 读出、cat 多空行）（见技术文档 §14 踩坑 21-23） |
| **FS4 写** ✅ 2026-10-21 | selftest 回放 `cp /hello.txt /hd/c1.txt` `cat /hd/c1.txt` `ls /hd`：verify_selftest 25 行像素全对（19792=19792，e2root 可变树模拟 cp 副作用，ls /hd 含 lost+found）；verify_ext2.py 活断言 PASS（STAGE=6/ERR=0/INO=16/FREE=8148=真值8149-1/D_EXVEC=0）；**关机后 `verify_ext2.py disk` PASS**（c1.txt ino=16/mode/links/size=73/内容、块位图 bit42（=块43）、inode 位图 bit15、超块 free、根目录游走 end==1024 无洞）；**e2fsck 1.47.3 -fn 第三方审计双 clean**（全新镜像和运行时写过的盘都 EXIT=0，含 lost+found/padding 配件）；**二次开机（不重建同盘）重放整条流，像素 19792=19792 幂等**（截断复用不新分配，FREE 停在 8148）。排障中修了 4 个真 bug + 一批标准对齐：vfs_create 的 call 目标寄存器被参数装载撞掉（call 0 → #UD → idt 存根挂死，加 idt 向量记录器才确诊）、selftest_len equ 差 1（post-stream 整屏审计永不触发）、dir_insert 新条目 rec 写成 need 留无主洞（readdir 撞 corrupt）；外加"预期 NOENT 残留 D_EXT2_ERR"的幽灵错误清理。标准对齐的教训很深（踩坑 28）：块位图是 **bit i ↔ 块 i+1**（first_data_block=1 移位，位号别直接当块号）；而 inode 偏移 i_size@0x04/i_links@0x1A 本来就对，"凭记忆对齐"改成 0x08/0x02 反而是把对的改错（工具链全绿也掩盖不了，e2fsck 一跑就露馅，最终整体回滚） |

### 9.2 互操作验收（有 WSL/Linux 环境时跑，没条件就标"未验"）

- **出**（✅ 已验 2026-10-21）：拿 FS4 写过的 `hd-flat.vmdk` 喂
  `wsl -d docker-desktop e2fsck 1.47.3 -fn` → **0 报错 EXIT=0**
  （16/256 files, 44/8192 blocks——运行时写的 c1.txt 与分配全被 Linux
  原生 fsck 认账）。`mount` 挂载读文件这条没条件（WSL 无 root 挂载权），
  但 e2fsck 深检通过等价于格式合法的强证明。
- **进**（未做，留给 FS5+）：`mke2fs -t ext2 -b 1024 …` → `mkext2.py
  --from-mke2fs` → MeOS `ls /hd`。当前 mkext2 无此入口；驱动端解析的是
  rev0 布局本身，mkext2 自产盘与 mke2fs 盘的超块/GD/位图约定已逐项
  对齐 e2fsck 口径。
- 出的这条过了，"格式移植"才算真正交货；进的那条过不了，说明驱动在
  偷懒硬编码（D4 纪律）。

### 9.3 回归

每阶段收尾必跑：`build.ps1` → `verify_selftest.py`（含新增回放）→ `vmtest.ps1` →
`checkdoc.py`。任何一步红，先修再前进——这套纪律是这个仓库能走到今天的原因。

---

## 10. 阶段计划

| 阶段 | 主题 | 产出 | 规模（估） |
| --- | --- | --- | --- |
| **FS0** ✅ 2026-10-16 | 文档对账 | §2.2 漂移清单一项项修平 | 0 代码，纯文档 |
| **FS1** ✅ 2026-10-16 | 盘与镜像（纯宿主侧） | mkext2.py、ext2check.py、vmx 挂盘、build.ps1 步骤 | ~300 行 Python，0 行 asm |
| **FS2** ✅ 2026-10-20 | ATA PIO | ata.inc、`disk` 命令、D_ATA_* | ~350 行 asm + ~40 行 kernel |
| **FS3** ✅ 2026-10-20 | ext2 只读 + readdir | ext2.inc(读)、vfs 5 列驱动表、`/hd` 挂载、ls 重写、selftest 扩展 | ~750 行 asm |
| **FS4** ✅ 2026-10-21 | ext2 写 | ext2 写路径（create/write/位图/dir_insert/sync_super）、cp/touch/mkdir 跨挂载、VIA_CREATE 第 6 列、e2fsck 标准对齐 | ~650 行 asm |
| **FS5** | （未来） | vfs_cwd 上收 + `cd /hd`（D6 留尾）、双间接块、rev 1 / 4096 块 / 多组、symlink、unlink、运行时 mount、df 跨 fs、mkext2 `--from-mke2fs`（§9.2 "进"方向） | 不承诺 |

顺序不能乱：FS1 完全不碰内核（风险零，先把"裁判"备好——ext2check 和 e2fsck
是后面所有阶段的地面真值）；FS2 有了 `disk` 命令就能人肉看盘；FS3 读通了
FS4 的 bug 才有地方对照（读不对先别写）。

---

## 11. 风险与踩坑清单（新坑按此防，老坑按《技术文档.md》§14 防）

1. **ATA 400ns 等待想省掉**：虚机里大概率没事，真机必炸。老老实实读 4 次 0x3F6。
2. **BSY/DRQ 轮询不带超时**：死循环 = VM 假死，还以为内核崩了。必须有循环上限，
   超时落 D_ATA_ERR bit8。
3. **`rep insw` 的计数单位是字**：256 字 = 512 B；且 `insw` 前必须 `cld`
   （DF 的方向没人替你保证）。
4. **扇区数寄存器写 0 表示 256**，不是 0：别把"读 1 扇区"写成 0。
5. **VMDK 描述符的 RW 扇区数与 flat 文件字节数差一个字节都挂不了盘**：
   mkext2.py 自检里显式断言 `sectors*512 == len(flat)`。
6. **hd-flat.vmdk 在 VM 运行期被锁**：ext2check 只能关机后跑；-Clean 在 VM
   运行时本来就会失败，行为一致。
7. **bs=1024 的 `first_data_block=1` 陷阱**：块号从 0 数、数据从块 1 起；
   块 b 的扇区 = b×2（块 1 在扇区 2–3，不是扇区 1）。GDT 块 = first_data_block+1。
8. **rev 0 没有 s_inode_size 字段**（0x58 是 rev 1 的）：rev0 一律 128 B。
   先看 s_rev_level 再解释超块。
9. **rec_len 是 u16**：`movzx`——本仓库 word 硬伤第二案（§14 第一案是 D_ROWPIX）。
10. **rec_len 必须 4 对齐且 ≥8**：插入算法里两个断言都放生成器和驱动两侧，
    算错 = 目录结构烂，只有 e2fsck 救得回来。
11. **ext2 名字最长 255，MeOS 缓冲只有 32 B**：name_len > 31 一律 FS_NAMELEN，
    别截断（截断的名字 = 静默数据错）。
12. **没实现的路径返回 FS_NOTSUPP，别硬读**：双间接、rev1、4096 块、多组。
    硬读出来的是垃圾，垃圾比错误码危险得多。
13. **新缓冲占文件字节**（flat binary 无 BSS）：预算表 §8.4 逼到 60% 就先升
    PAYLOAD_SECTORS（同步两处：boot.asm / checkdoc.py；mkfloppy.py 无此常量）。
14. **诊断块新字段（0x244..0x26C）必须写进《技术文档.md》§5 + checkdoc.py**：
    不写 = 自己破坏自己的断言器。
15. **PIC 不动，IRQ14/15 保持屏蔽**：轮询 PIO 与 `sti` 共存是安全的，
    别画蛇添足解屏蔽。
16. **虚盘必须挂 ide0:0（主盘）**：驱动写死 0x1F0 + drive 位 0；挂从盘
    （ide0:1）选盘字节就错。
17. **保留 inode**：0（坏块表头）、1、10（resize）不用，分配从 11 起；
    根固定 inode 2。
18. **VMware 依旧丢合成按键**：所有屏显验收靠 selftest_data 回放（0x00=空档拍），
    人肉敲键盘不算验收。

---

## 12. 与《技术文档.md》的同步义务

本方案实施 = 每阶段动三处文档，缺一即"doc stale is worse than no doc"：

1. 《技术文档.md》§5 诊断块表：新增 D_ATA_*/D_EXT2_* 行；
2. 《技术文档.md》§8.3 命令表 + §12 例程索引：ata.inc / ext2.inc 例程、
   `disk` 命令、驱动表 5 列结构；
3. checkdoc.py：把新常量（ATA_*、EXT2_*、D_ATA_*、D_EXT2_*、PAYLOAD_SECTORS
   若变）加进 CONSTS / 偏移表断言。

本文档本身在阶段完成时把"估"字改成实数、把规格表跟实现对一遍——
方案文档只在实施前是蓝图，实施后是决策记录。
