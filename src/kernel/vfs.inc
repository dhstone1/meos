; ============================================================================
;  VFS：类 Linux 的虚拟文件系统层
; ----------------------------------------------------------------------------
;  目标：让"文件"这个词脱离具体存储。命令不再直接摸 MeFS 的块链 / 位图，
;  而是走统一的 vnode + 文件描述符接口；换个文件系统、加个设备，都不动内核。
;
;  本层有四样东西（对应 Linux 的概念）：
;
;    · 挂载表  vfs_mount_tab —— 把挂载点路径映射到驱动；最长前缀匹配。
;      当前三张表： "/"    → mefs（RAM 盘，实现见 fs.inc）
;                  "/dev" → devfs（内存里的设备文件，见下面 devfs 一节）
;                  "/hd"  → ext2（硬盘上的 Linux ext2 rev0，见 ext2.inc）
;    · vnode   —— "一个节点"的通用描述 {kind, a, b, size}。
;      mefs 的 a = 数据链首块；devfs 的 a = 设备号；ext2 的 a = inode 号。
;    · fd 表   —— 打开的文件句柄 {used, drv, vnode, pos}，最多 VFS_MAX_FD 个。
;    · 驱动表  vfs_drv_tab —— 每个文件系统实现同一套函数接口：
;      open / read / write / close / readdir。vfs_open / vfs_read / vfs_write /
;      vfs_close / vfs_readdir 是唯一合法的入口，命令层只认这套 API。
;
;  所有 vfs_* 例程 pushad 保护，结果走 EAX；出错一律写 fs_err
;  （复用 fs.inc 的 FS_* 错误码和 fs_perror 打印）。
;
;  驱动接口（vfs_drv_tab 每一行 = 一个驱动，行号 = 挂载表里的驱动号）：
;
;     open    : ESI = 相对挂载点的路径（"" 或 "/xxx"），EDI = 输出 vnode(16B)
;               成功 EAX = 1；失败 EAX = 0（fs_err 给原因）
;     read    : ESI = fd 指针，EDI = 缓冲，ECX = 长度
;               返回 EAX = 实际读到的字节数（0 = 到文件尾），并推进 fd.pos
;     write   : ESI = fd 指针，EDI = 缓冲，ECX = 长度
;               返回 EAX = 实际写入字节数；不支持返回 -1
;     close   : ESI = fd 指针，返回 EAX = 0
;     readdir : ESI = fd 指针，EDI = 40B 记录（布局见 fs.inc 的 VRD_*）
;               返回 EAX = 1 = 拿到一条，0 = 没有了；fd.pos 当游标
;               （mefs/devfs = 子项序号，ext2 = 目录字节偏移）
;
;  "加一个新文件系统" = 写一个驱动 + 在 vfs_drv_tab 加一行 +
;                       给挂载表加一项，内核其余部分不碰（见 docs/内核架构.md）。
;
;  路径规则：vfs_open 先经 fs_action 把相对路径拼上 cwd 并规范化成绝对路径，
;  再按挂载点最长前缀拆分；根挂载的"相对路径"恰好就是绝对路径（以 '/' 开头），
;  非根挂载的"相对路径"从挂载点下一个字符开始（"" 表示挂载点本身）。
; ============================================================================

; ---- 常量 -------------------------------------------------------------------
VFS_MAX_FD     equ 8
VFS_MAX_MNT    equ 4
VFS_RD_BUF     equ 256

DRV_MEFS       equ 0
DRV_DEV        equ 1
DRV_EXT2       equ 2

; vnode 内部偏移（16 字节）
VN_KIND        equ 0      ; byte  0 = 文件 / 1 = 目录 / 2 = 设备
VN_A           equ 4      ; dword mefs: 数据链首块；devfs: 设备号
VN_B           equ 8      ; dword 保留（v1 恒 0）
VN_SIZE        equ 12     ; dword 文件字节数（目录 = 子项条数）

VN_FILE        equ 0
VN_DIR         equ 1
VN_DEV         equ 2

; fd 内部偏移（24 字节）
FD_USED        equ 0      ; byte  0 = 空槽
FD_DRV         equ 1      ; byte  驱动号
FD_VN          equ 4      ; vnode（16 字节，vfs_open 时从驱动拷过来）
FD_POS         equ 20     ; dword 文件位置（读写推进它）
FD_LEN         equ 24

; 驱动函数表偏移（每项一个 dword 指针）
VIA_OPEN       equ 0
VIA_READ       equ 4
VIA_WRITE      equ 8
VIA_CLOSE      equ 12
VIA_READDIR    equ 16
VIA_CREATE     equ 20             ; FS4：创建（ESI=rel，EAX=kind）；槽 0 = 不支持
VIA_LEN        equ 24

; devfs 设备号
DEV_NULL       equ 0
DEV_CONSOLE    equ 1

; ============================================================================
;  vfs_init：装配挂载表、清空 fd 表（在 fs_init 之后调用）
; ============================================================================
vfs_init:
    pushad
    ; fd 表清零
    mov  edi, vfs_fd_tab
    xor  eax, eax
    mov  ecx, VFS_MAX_FD * FD_LEN / 4
    rep  stosd
    ; 挂载表是编译期排好的（见数据段），数一遍确认有几项
    mov  dword [vfs_mount_n], 0
    xor  ecx, ecx
.count:
    cmp  ecx, VFS_MAX_MNT
    jae  .done
    cmp  dword [vfs_mount_tab + ecx * 8], 0
    je   .done
    inc  ecx
    jmp  .count
.done:
    mov  [vfs_mount_n], ecx
    mov  dword [vfs_ready], 1
    popad
    ret

; ---- fd 下标 -> fd 指针：EAX = 下标 -> EAX = 槽地址 --------------------------
;  FD_LEN 不是 2 的幂，没法写进一条 [base + reg*N] 寻址，统一走这个换算。
vfs_fd_ptr:
    imul eax, eax, FD_LEN
    add  eax, vfs_fd_tab
    ret

; ============================================================================
;  挂载匹配：ESI = 规范化绝对路径
;  -> EAX = 驱动号（-1 = 没有挂载点匹配），vfs_mm_rel = 相对路径
;  最长前缀匹配：" / " 恒中，"/dev/..." 被 "/dev" 抢走。
; ============================================================================
vfs_mount_match:
    pushad
    cld
    mov  [vfs_mm_path], esi
    mov  dword [vfs_mm_best], -1        ; 最长命中前缀长度
    mov  dword [vfs_mm_drv], -1         ; 没命中就真的是 -1，别让 0 号驱动躺枪
    xor  ebx, ebx                       ; 挂载表下标
.mnt:
    cmp  ebx, [vfs_mount_n]
    jae  .out_bad
    mov  esi, [vfs_mount_tab + ebx * 8] ; 挂载点字符串
    call str_len                        ; EAX = 挂载点长度
    mov  [vfs_mm_mlen], eax
    test eax, eax
    jz   .next
    cmp  eax, 1
    je   .hit                            ; 根挂载 "/"：绝对路径恒中，无需边界检查
    ; 前缀逐字节比较
    mov  ecx, eax
    mov  esi, [vfs_mm_path]
    mov  edi, [vfs_mount_tab + ebx * 8]
    repe cmpsb
    jnz  .next
    ; 边界：路径 == 挂载点，或紧跟着一个 '/'
    mov  al, [esi]                      ; ESI 已越过前缀
    test al, al
    jz   .hit
    cmp  al, '/'
    jne  .next
.hit:
    mov  eax, [vfs_mm_mlen]
    cmp  eax, [vfs_mm_best]
    jle  .next                          ; 不比最长更长就算了（等长取先注册的）；
                                        ; 必须有符号：best 的"无命中"哨兵是 -1，
                                        ; 无符号 jbe 会把所有真长度都比输给 0xFFFFFFFF
    mov  [vfs_mm_best], eax
    mov  [vfs_mm_drv], ebx
.next:
    inc  ebx
    jmp  .mnt
.out_bad:
    mov  eax, [vfs_mm_drv]
    mov  [DIAG_ADDR + 0x360], eax        ; [调试] 最后匹配的驱动号（0x360 段避开
    mov  eax, [vfs_mm_best]               ; fs_diag 的 0x2E0 快照区，两者曾互相覆盖）
    mov  [DIAG_ADDR + 0x364], eax        ; [调试] 最后匹配的最长前缀
    test eax, eax
    js   .no
    mov  edi, [vfs_mm_path]
    add  edi, [vfs_mm_best]             ; 相对路径起点（'/' 或 '\0'）
    mov  [vfs_mm_rel], edi
    mov  [DIAG_ADDR + 0x368], edi        ; [调试] 相对路径指针
    mov  eax, [vfs_mm_drv]               ; 返回驱动号（挂载表下标），不是前缀
                                         ; 长度！vfs_open 拿它当 vfs_drv_tab 行号
    mov  [esp + 28], eax
    popad
    ret
.no:
    mov  dword [fs_err], FS_BADPATH      ; 没有挂载点匹配——必须设错，
                                         ; 否则失败会被 fs_perror 静默吞掉
    mov  dword [DIAG_ADDR + 0x36C], 0xDEADBEEF   ; [调试] 没命中
    mov  dword [esp + 28], -1
    popad
    ret

; ============================================================================
;  vfs_open：ESI = 路径（相对/绝对都行）-> EAX = fd（>=0）或 -1
; ============================================================================
vfs_open:
    pushad
    call fs_action
    test eax, eax
    jz   .out_bad
    lea  esi, [path_buf]
    call vfs_mount_match
    test eax, eax
    js   .out_bad
    mov  [vfs_tmp_drv], eax
    mov  eax, [vfs_mm_rel]
    mov  [vfs_tmp_rel], eax
    ; 找空 fd 槽
    xor  ebx, ebx
.slot:
    cmp  ebx, VFS_MAX_FD
    jae  .out_full
    mov  eax, ebx
    call vfs_fd_ptr
    cmp  byte [eax + FD_USED], 0
    je   .have_slot
    inc  ebx
    jmp  .slot
.have_slot:
    ; 调驱动 open（所有驱动例程 pushad 保护，返回 EAX）
    mov  eax, [vfs_tmp_drv]
    imul eax, eax, VIA_LEN
    mov  eax, [vfs_drv_tab + eax + VIA_OPEN]
    mov  esi, [vfs_tmp_rel]
    mov  edi, vfs_cur_vn
    call eax
    test eax, eax
    jz   .out_bad
    ; 填 fd 槽
    mov  eax, ebx
    call vfs_fd_ptr
    mov  edi, eax
    mov  byte [edi + FD_USED], 1
    mov  al, [vfs_tmp_drv]
    mov  [edi + FD_DRV], al
    mov  dword [edi + FD_POS], 0
    mov  esi, vfs_cur_vn
    lea  edi, [edi + FD_VN]              ; vnode 拷到槽内 FD_VN（+4）位，不是槽
                                         ; 基址——拷到基址会压掉 USED/DRV，而读
                                         ; 取方全按 [slot + FD_VN + …] 取值
    mov  ecx, 16
    cld
    rep  movsb                          ; vnode 拷进 fd 槽
    mov  eax, ebx
    mov  [esp + 28], eax
    popad
    ret
.out_bad:
    ; fs_err 已由 fs_action / 驱动设好
    mov  dword [esp + 28], -1
    popad
    ret
.out_full:
    mov  dword [fs_err], FS_NOSPACE     ; 句柄用完了
    mov  dword [esp + 28], -1
    popad
    ret

; ============================================================================
;  vfs_read：EAX = fd，EDI = 缓冲，ECX = 长度 -> EAX = 读到的字节数（0 = 尾）
; ============================================================================
vfs_read:
    pushad
    cmp  eax, VFS_MAX_FD
    jae  .bad
    call vfs_fd_ptr
    mov  esi, eax
    cmp  byte [esi + FD_USED], 0
    je   .bad
    movzx eax, byte [esi + FD_DRV]
    imul eax, eax, VIA_LEN
    mov  eax, [vfs_drv_tab + eax + VIA_READ]
    call eax                            ; ESI = fd 指针，EDI/ECX 原样传入
    mov  [esp + 28], eax
    popad
    ret
.bad:
    mov  dword [esp + 28], -1
    popad
    ret

; ============================================================================
;  vfs_write：EAX = fd，EDI = 缓冲，ECX = 长度 -> EAX = 写入字节数（-1 = 不支持）
; ============================================================================
vfs_write:
    pushad
    cmp  eax, VFS_MAX_FD
    jae  .bad
    call vfs_fd_ptr
    mov  esi, eax
    cmp  byte [esi + FD_USED], 0
    je   .bad
    movzx eax, byte [esi + FD_DRV]
    imul eax, eax, VIA_LEN
    mov  eax, [vfs_drv_tab + eax + VIA_WRITE]
    call eax
    mov  [esp + 28], eax
    popad
    ret
.bad:
    mov  dword [esp + 28], -1
    popad
    ret

; ============================================================================
;  vfs_create：ESI = 路径，EAX = kind（0 文件 1 目录）→ EAX = 1/0（FS4）
;  挂载匹配后交给驱动表的 VIA_CREATE 列；槽是 0（0 = 不支持）就报
;  FS_NOTSUPP。MeFS 的创建不经这条路（命令层直接 fs_* 原语）。
; ============================================================================
vfs_create:
    pushad
    mov  [vfs_ci_kind], eax
    call fs_action
    test eax, eax
    jz   .out_bad
    lea  esi, [path_buf]
    call vfs_mount_match
    test eax, eax
    js   .out_bad                       ; fs_err 已设（FS_BADPATH）
    mov  [vfs_ci_drv], eax
    imul eax, eax, VIA_LEN
    mov  edx, [vfs_drv_tab + eax + VIA_CREATE]   ; 地址放 EDX
    test edx, edx
    jz   .supp
    mov  esi, [vfs_mm_rel]
    mov  eax, [vfs_ci_kind]
    call edx
    ; 踩坑 25：这里绝不能「call eax」——上面为传参把 EAX 重载成了 kind，
    ; 会把刚取出的函数地址撞成 0，call 0x0 执行 IVT 垃圾 = #UD → 红杠挂死。
    ; 间接调用的目标寄存器和参数装载寄存器必须分开。
    mov  [esp + 28], eax
    popad
    ret
.supp:
    mov  dword [fs_err], FS_NOTSUPP
.out_bad:
    mov  dword [esp + 28], 0
    popad
    ret

; ============================================================================
;  vfs_close：EAX = fd -> EAX = 0
; ============================================================================
vfs_close:
    pushad
    cmp  eax, VFS_MAX_FD
    jae  .bad
    call vfs_fd_ptr
    mov  esi, eax
    cmp  byte [esi + FD_USED], 0
    je   .bad
    movzx eax, byte [esi + FD_DRV]
    imul eax, eax, VIA_LEN
    mov  eax, [vfs_drv_tab + eax + VIA_CLOSE]
    call eax
    mov  eax, [esp + 28]                ; 原 EAX = fd（close 调用可能动过）
    call vfs_fd_ptr
    mov  esi, eax
    mov  byte [esi + FD_USED], 0
    mov  dword [esp + 28], 0
    popad
    ret
.bad:
    mov  dword [esp + 28], -1
    popad
    ret

; ============================================================================
;  vfs_readdir：EAX = fd，ESI = 40B 记录 -> EAX = 1（有一条）/ 0（没了）
;  错误也返回 0，但 fs_err 已设。每次进来先清 fs_err——不然上一条命令
;  的残值会让 ls 在正常到尾时误报错误。
; ============================================================================
vfs_readdir:
    pushad
    mov  [vfs_rdrec_out], esi
    cmp  eax, VFS_MAX_FD
    jae  .bad
    call vfs_fd_ptr
    mov  esi, eax
    cmp  byte [esi + FD_USED], 0
    je   .bad
    mov  dword [fs_err], 0
    movzx eax, byte [esi + FD_DRV]
    imul eax, eax, VIA_LEN
    mov  eax, [vfs_drv_tab + eax + VIA_READDIR]
    mov  edi, [vfs_rdrec_out]
    call eax
    mov  [esp + 28], eax
    popad
    ret
.bad:
    mov  dword [fs_err], FS_BADPATH
    mov  dword [esp + 28], 0
    popad
    ret

; ============================================================================
;  MeFS 驱动：把 fs.inc 的查找 / 块链接口包装成 vnode + fd
; ============================================================================

; ---- mefs_open：ESI = 相对路径（"" = 根，"/xxx" = 子树），EDI = 输出 vnode --
mefs_open:
    pushad
    mov  [vfs_tmp_vnout], edi
    cmp  byte [esi], 0
    jne  .lookup
    ; 挂载点本身：根目录
    mov  edi, [vfs_tmp_vnout]
    mov  byte [edi + VN_KIND], VN_DIR
    mov  eax, [FS_RAM_BASE + 0x14]
    mov  [edi + VN_A], eax
    mov  dword [edi + VN_B], 0
    mov  dword [edi + VN_SIZE], 0
    mov  dword [esp + 28], 1
    popad
    ret
.lookup:
    ; 根挂载拆出来的"相对路径"不带前导 '/'（如 "hello.txt"），补一个再交给
    ; fs_lookup（它只认绝对路径）。写在 path_buf 里无妨，那片本来就是临时区。
    cmp  byte [esi], '/'
    je   .got
    dec  esi
    mov  byte [esi], '/'
.got:
    call fs_lookup
    test eax, eax
    jz   .fail
    mov  edi, [vfs_tmp_vnout]
    mov  al, [fs_ent_kind]
    mov  [edi + VN_KIND], al            ; 值恰好与 VN_FILE / VN_DIR 一致
    mov  eax, [fs_ent_first]
    mov  [edi + VN_A], eax
    mov  dword [edi + VN_B], 0
    mov  eax, [fs_ent_size]
    mov  [edi + VN_SIZE], eax
    mov  dword [esp + 28], 1
    popad
    ret
.fail:
    mov  dword [esp + 28], 0
    popad
    ret

; ---- mefs_read：ESI = fd 指针，EDI = 缓冲，ECX = 长度 -> EAX = 字节数 --------
;  按 fd 当前 pos 从块链里拷贝（链上每块 BLK_CAP = 508 字节数据）。
mefs_read:
    pushad
    cld
    mov  [vfs_rd_fd], esi
    mov  [vfs_rd_dst], edi
    ; 剩余 = size - pos，本次最多读 min(剩余, ecx)
    mov  eax, [esi + FD_POS]
    mov  edx, [esi + FD_VN + VN_SIZE]
    cmp  eax, edx
    jae  .eof
    sub  edx, eax
    mov  [vfs_rd_need], edx
    cmp  edx, ecx
    jbe  .have_len
    mov  [vfs_rd_need], ecx
.have_len:
    mov  dword [vfs_rd_left], 0         ; 已读字节数
    ; 定位：pos / BLK_CAP -> EAX = 链上第几块，EDX = 块内偏移
    mov  eax, [esi + FD_POS]
    xor  edx, edx
    mov  ecx, BLK_CAP
    div  ecx
    mov  [vfs_rd_boff], edx
    mov  ecx, eax
    mov  eax, [esi + FD_VN + VN_A]      ; 首块
    mov  [vfs_rd_blk], eax
.walk:
    test ecx, ecx
    jz   .copy
    mov  eax, [vfs_rd_blk]
    test eax, eax
    jz   .trunc
    call fs_block_addr
    mov  eax, [eax]
    mov  [vfs_rd_blk], eax
    dec  ecx
    jmp  .walk
.copy:
    mov  eax, [vfs_rd_need]
    test eax, eax
    jz   .finish
    mov  eax, [vfs_rd_blk]
    test eax, eax
    jz   .trunc
    call fs_block_addr
    lea  edx, [eax + BLK_DATA]
    add  edx, [vfs_rd_boff]
    ; chunk = min(BLK_CAP - 块内偏移, need)
    mov  ecx, BLK_CAP
    sub  ecx, [vfs_rd_boff]
    mov  eax, [vfs_rd_need]
    cmp  eax, ecx
    jae  .chunk
    mov  ecx, eax
.chunk:
    push esi
    push ecx                      ; rep movsb 结束时 ECX 清零，字节数先存栈
    mov  esi, edx
    mov  edi, [vfs_rd_dst]
    rep  movsb
    pop  ecx                      ; 恢复本次块的字节数
    mov  [vfs_rd_dst], edi
    pop  esi
    add  [vfs_rd_left], ecx
    sub  [vfs_rd_need], ecx
    mov  dword [vfs_rd_boff], 0
    mov  eax, [vfs_rd_blk]
    call fs_block_addr
    mov  eax, [eax]
    mov  [vfs_rd_blk], eax
    jmp  .copy
.finish:
    mov  eax, [vfs_rd_left]
    mov  edx, [vfs_rd_fd]
    add  [edx + FD_POS], eax
    mov  [esp + 28], eax
    popad
    ret
.eof:
    mov  dword [esp + 28], 0
    popad
    ret
.trunc:
    ; 链提前到头（坏盘保护）：给已读的部分
    mov  eax, [vfs_rd_left]
    mov  edx, [vfs_rd_fd]
    add  [edx + FD_POS], eax
    mov  [esp + 28], eax
    popad
    ret

; ---- mefs_write：v1 只读盘，返回 -1 ------------------------------------------
mefs_write:
    mov  eax, -1
    ret

; ---- mefs_close ---------------------------------------------------------------
mefs_close:
    xor  eax, eax
    ret

; ---- mefs_readdir：ESI = fd 指针，EDI = 40B 记录 -> EAX = 1/0 -----------------
;  FD_POS 当"第几个非空洞孩子"用（fs_dir_first/next 的 fd 版重述）。
;  每次调用都从头走链——目录就十来项，犯不着为它维护游标状态。
;  空洞（name_len=0，删掉的记录）跳过不计，和 fs_dir_next 的语义一致。
mefs_readdir:
    pushad
    cld
    mov  [vfs_mr_fd], esi
    mov  [vfs_mr_out], edi
    mov  dword [vfs_mr_cnt], 0        ; 已数过的非空洞项
    mov  eax, [esi + FD_VN + VN_A]    ; 目录链首块
    test eax, eax
    jz   .end
    mov  [vfs_mr_blk], eax
.blk_loop:
    mov  eax, [vfs_mr_blk]
    call fs_block_addr
    mov  [vfs_mr_addr], eax           ; 块基址（链指针在 +0）
    xor  ecx, ecx                     ; 槽位
.slot_loop:
    cmp  ecx, DE_PER_BLK
    jae  .next_blk
    ; 记录地址 = 块基址 + BLK_DATA + 槽位 * DE_LEN
    mov  ebx, [vfs_mr_addr]
    lea  ebx, [ebx + BLK_DATA]
    mov  edx, ecx
    imul edx, edx, DE_LEN
    add  ebx, edx                     ; EBX = 记录地址
    cmp  byte [ebx + DE_NAME_LEN], 0
    je   .adv_slot                    ; 空洞不算数
    ; 数到 FD_POS 号了吗？
    mov  esi, [vfs_mr_fd]
    mov  eax, [esi + FD_POS]
    mov  edx, [vfs_mr_cnt]
    cmp  edx, eax
    jne  .notyet
    ; 命中：抄进 40B 输出记录
    movzx ecx, byte [ebx + DE_NAME_LEN]
    cmp  ecx, VRD_NAMEMAX             ; 坏盘防御：名字塞不进记录就截断
    jbe  .cp_ok
    mov  ecx, VRD_NAMEMAX
.cp_ok:
    mov  edi, [vfs_mr_out]
    mov  al, [ebx + DE_KIND]          ; 0/1 恰好 = VN_FILE / VN_DIR
    mov  [edi + VRD_KIND], al
    mov  eax, [ebx + DE_SIZE]
    mov  [edi + VRD_SIZE], eax
    lea  esi, [ebx + DE_NAME]
    add  edi, VRD_NAME
    push ecx                           ; rep 清 ECX（踩坑 17）
    rep  movsb
    pop  ecx
    mov  edi, [vfs_mr_out]
    mov  byte [edi + VRD_NAME + ecx], 0
    mov  esi, [vfs_mr_fd]
    inc  dword [esi + FD_POS]          ; 下一号
    mov  dword [esp + 28], 1
    popad
    ret
.notyet:
    inc  dword [vfs_mr_cnt]
.adv_slot:
    inc  ecx
    jmp  .slot_loop
.next_blk:
    mov  eax, [vfs_mr_blk]
    call fs_block_addr
    mov  eax, [eax]                   ; 链上下一块
    mov  [vfs_mr_blk], eax
    test eax, eax
    jnz  .blk_loop
.end:
    mov  dword [esp + 28], 0
    popad
    ret

; ============================================================================
;  devfs 驱动：内存里的设备文件（"一切皆文件"的最小示范）
;  固定两张表：devfs_ent（设备注册表）只读；读 /dev/null 得 0 字节、
;  读 /dev/console 得一段欢迎语、写 /dev/console 直接把字节打到屏幕。
; ============================================================================

; ---- dev_open：ESI = 相对路径，EDI = 输出 vnode ------------------------------
dev_open:
    pushad
    mov  [vfs_tmp_vnout], edi
    ; 剥开头的 '/'
.skip:
    cmp  byte [esi], '/'
    jne  .have
    inc  esi
    jmp  .skip
.have:
    cmp  byte [esi], 0
    jne  .named
    ; 挂载点本身：devfs 根目录
    mov  edi, [vfs_tmp_vnout]
    mov  byte [edi + VN_KIND], VN_DIR
    mov  dword [edi + VN_A], 0
    mov  dword [edi + VN_B], 0
    mov  dword [edi + VN_SIZE], 2
    mov  dword [esp + 28], 1
    popad
    ret
.named:
    ; 第一个组件抄进 devfs_name_buf（设备名，最多 15 字符）
    mov  edx, devfs_name_buf
    xor  ecx, ecx
.cp:
    mov  al, [esi]
    cmp  al, '/'
    je   .cp_done
    test al, al
    jz   .cp_done
    cmp  ecx, 15
    jae  .namelen
    mov  [edx + ecx], al
    inc  ecx
    inc  esi
    jmp  .cp
.cp_done:
    mov  byte [edx + ecx], 0
    cmp  byte [esi], '/'                ; 还有子路径 → devfs 没有子目录
    je   .notdir
    ; 扫设备注册表
    mov  ebx, devfs_ent
.scan:
    mov  eax, [ebx]
    test eax, eax
    jz   .noent
    mov  esi, eax
    lea  edi, [devfs_name_buf]
    call str_eq
    test eax, eax
    jnz  .match
    add  ebx, 12                        ; 每项 {名字, 长度, 设备号}
    jmp  .scan
.match:
    mov  eax, [ebx + 8]                 ; 设备号
    mov  ecx, [ebx + 4]                 ; 名字长度
    mov  edi, [vfs_tmp_vnout]
    mov  byte [edi + VN_KIND], VN_DEV
    mov  [edi + VN_A], eax
    mov  dword [edi + VN_B], 0
    cmp  eax, DEV_CONSOLE
    jne  .size_null
    mov  dword [edi + VN_SIZE], devfs_console_len
    jmp  .ok
.size_null:
    mov  dword [edi + VN_SIZE], 0
.ok:
    mov  dword [esp + 28], 1
    popad
    ret
.noent:
    mov  dword [fs_err], FS_NOENT
    mov  dword [esp + 28], 0
    popad
    ret
.notdir:
    mov  dword [fs_err], FS_NOTDIR
    mov  dword [esp + 28], 0
    popad
    ret
.namelen:
    mov  dword [fs_err], FS_NAMELEN
    mov  dword [esp + 28], 0
    popad
    ret

; ---- dev_read：ESI = fd 指针，EDI = 缓冲，ECX = 长度 -> EAX = 字节数 ----------
dev_read:
    pushad
    cld
    mov  eax, [esi + FD_VN + VN_A]
    cmp  eax, DEV_CONSOLE
    je   .console
    ; /dev/null 及其余：读不到东西
    mov  dword [esp + 28], 0
    popad
    ret
.console:
    mov  edx, [esi + FD_POS]
    cmp  edx, devfs_console_len
    jae  .eof
    mov  ebx, devfs_console_len
    sub  ebx, edx                       ; 剩余
    cmp  ebx, ecx
    jbe  .have
    mov  ebx, ecx
.have:
    push esi
    lea  esi, [devfs_console_text]
    add  esi, edx
    mov  ecx, ebx
    rep  movsb
    pop  esi
    add  [esi + FD_POS], ebx
    mov  eax, ebx
    mov  [esp + 28], eax
    popad
    ret
.eof:
    mov  dword [esp + 28], 0
    popad
    ret

; ---- dev_write：ESI = fd 指针，EDI = 缓冲，ECX = 长度 -> EAX = 字节数 ---------
;  /dev/console 的写 = 把字节打到屏幕上；/dev/null 的写 = 丢弃。
dev_write:
    pushad
    mov  eax, [esi + FD_VN + VN_A]
    cmp  eax, DEV_CONSOLE
    je   .console
    ; null：假装写成功
    mov  eax, ecx
    mov  [esp + 28], eax
    popad
    ret
.console:
    xor  ebx, ebx
.loop:
    cmp  ebx, ecx
    jae  .done
    movzx eax, byte [edi + ebx]
    cmp  al, 10
    je   .nl
    call con_putc
    jmp  .next
.nl:
    call con_crlf
.next:
    inc  ebx
    jmp  .loop
.done:
    mov  eax, ecx
    mov  [esp + 28], eax
    popad
    ret

; ---- dev_close ----------------------------------------------------------------
dev_close:
    xor  eax, eax
    ret

; ---- dev_readdir：ESI = fd 指针，EDI = 40B 记录 -> EAX = 1/0 ------------------
;  FD_POS = 设备注册表下标。设备没有空洞，顺序就是注册表顺序。
dev_readdir:
    pushad
    cld
    mov  [vfs_dr_fd], esi
    mov  [vfs_dr_out], edi
    mov  esi, [vfs_dr_fd]
    mov  eax, [esi + FD_POS]
    mov  [vfs_dr_idx], eax
    ; 注册表跳过前 idx 项
    mov  ebx, devfs_ent
    xor  ecx, ecx
.walk:
    cmp  ecx, [vfs_dr_idx]
    jae  .have
    cmp  dword [ebx], 0
    je   .end                          ; 还没数到就到表尾了
    add  ebx, 12
    inc  ecx
    jmp  .walk
.have:
    cmp  dword [ebx], 0
    je   .end
    mov  edi, [vfs_dr_out]
    mov  byte [edi + VRD_KIND], VN_DEV
    mov  eax, [ebx + 8]                ; 设备号
    cmp  eax, DEV_CONSOLE
    jne  .sz0
    mov  dword [edi + VRD_SIZE], devfs_console_len
    jmp  .cpname
.sz0:
    mov  dword [edi + VRD_SIZE], 0
.cpname:
    mov  esi, [ebx]                    ; 名字（0 结尾，设备名都很短）
    mov  edi, [vfs_dr_out]
    add  edi, VRD_NAME
    xor  ecx, ecx
.cp:
    mov  al, [esi + ecx]
    test al, al
    jz   .cpend
    cmp  ecx, VRD_NAMEMAX              ; 名字塞不进就截断（防御）
    jae  .cpend
    mov  [edi + ecx], al
    inc  ecx
    jmp  .cp
.cpend:
    mov  edi, [vfs_dr_out]
    mov  byte [edi + VRD_NAME + ecx], 0
    mov  esi, [vfs_dr_fd]
    inc  dword [esi + FD_POS]
    mov  dword [esp + 28], 1
    popad
    ret
.end:
    mov  dword [esp + 28], 0
    popad
    ret

; ============================================================================
;  vfs_cat：ESI = 路径 -> EAX = 1 成功 / 0 失败（fs_err）
;  走 fd 接口逐块读进缓冲再打到屏幕，cat 命令的唯一实现（MeFS / devfs 通吃）。
; ============================================================================
vfs_cat:
    pushad
    call vfs_open
    test eax, eax
    js   .fail
    mov  [vfs_cat_fd], eax
    ; 目录不能 cat（和 Linux 的 "Is a directory" 对应）
    call vfs_fd_ptr
    movzx eax, byte [eax + FD_VN + VN_KIND]
    cmp  eax, VN_DIR
    je   .isdir
    mov  dword [vfs_cat_total], 0
.read:
    mov  eax, [vfs_cat_fd]
    mov  edi, vfs_rdbuf
    mov  ecx, VFS_RD_BUF
    call vfs_read
    test eax, eax
    jz   .eof
    add  [vfs_cat_total], eax
    mov  [vfs_cat_n], eax
    xor  ebx, ebx
.print:
    cmp  ebx, [vfs_cat_n]
    jae  .read
    movzx eax, byte [vfs_rdbuf + ebx]
    mov  [vfs_cat_last], al
    cmp  al, 10
    je   .nl
    call con_putc
    jmp  .next
.nl:
    call con_crlf
.next:
    inc  ebx
    jmp  .print
.eof:
    ; 文件没以换行结尾就补一个回车，免得提示符粘在最后一行上
    cmp  dword [vfs_cat_total], 0
    jz   .close
    cmp  byte [vfs_cat_last], 10
    je   .close
    call con_crlf
.close:
    mov  eax, [vfs_cat_fd]
    call vfs_close
    mov  dword [esp + 28], 1
    popad
    ret
.isdir:
    mov  eax, [vfs_cat_fd]
    call vfs_close
    mov  dword [fs_err], FS_ISDIR
    mov  dword [esp + 28], 0
    popad
    ret
.fail:
    mov  dword [esp + 28], 0
    popad
    ret

; ============================================================================
;  命令层：mount / stat（一条命令一个套路：ensure → 干活 → perror → diag）
; ============================================================================

; ---- mount：打印挂载表 -------------------------------------------------------
cmd_do_mount:
    pushad
    mov  dword [fs_cmd_name], msg_fs_mount
    call fs_ensure
    test eax, eax
    jz   .done
    xor  ebx, ebx
.mnt:
    cmp  ebx, [vfs_mount_n]
    jae  .done
    mov  eax, [vfs_fsname_tab + ebx * 4]
    mov  esi, eax
    call con_puts
    mov  esi, msg_vfs_on
    call con_puts
    mov  eax, [vfs_mount_tab + ebx * 8]
    mov  esi, eax
    call con_puts
    call con_crlf
    inc  ebx
    jmp  .mnt
.done:
    call fs_diag
    popad
    ret

; ---- stat <路径>：类型 / 大小 / 所在文件系统 ----------------------------------
cmd_do_stat:
    pushad
    mov  dword [fs_cmd_name], msg_fs_stat
    call fs_ensure
    test eax, eax
    jz   .done
    test esi, esi
    jz   .usage
    call vfs_open
    test eax, eax
    js   .perr
    mov  [vfs_st_fd], eax
    ; 类型
    call vfs_fd_ptr
    movzx eax, byte [eax + FD_VN + VN_KIND]
    cmp  eax, 2
    jbe  .type_ok
    xor  eax, eax
.type_ok:
    mov  esi, [vfs_type_msgs + eax * 4]
    call con_puts
    ; 大小
    mov  esi, msg_vfs_size
    call con_puts
    mov  eax, [vfs_st_fd]
    call vfs_fd_ptr
    mov  eax, [eax + FD_VN + VN_SIZE]
    call putdec
    ; 所在文件系统
    mov  esi, msg_vfs_fs
    call con_puts
    mov  eax, [vfs_st_fd]
    call vfs_fd_ptr
    movzx eax, byte [eax + FD_DRV]
    cmp  eax, [vfs_mount_n]
    jae  .fs_unknown
    mov  eax, [vfs_fsname_tab + eax * 4]
    mov  esi, eax
    call con_puts
    jmp  .done_close
.fs_unknown:
    mov  esi, msg_vfs_unk
    call con_puts
.done_close:
    call con_crlf
    mov  eax, [vfs_st_fd]
    call vfs_close
    jmp  .done
.usage:
    mov  esi, msg_vfs_stat_usage
    call con_puts
    call con_crlf
    jmp  .done
.perr:
    call fs_perror
.done:
    call fs_diag
    popad
    ret

; ============================================================================
;  VFS 数据
; ============================================================================
vfs_ready      dd 0            ; 1 = VFS 装配好了（vfs_init）
vfs_mount_n    dd 0            ; 挂载表项数（编译期排好，vfs_init 数出来）

; 挂载表：每项 8 字节 {挂载点字符串, 保留}；驱动号 = 表下标。空指针 = 表尾。
; （vfs_mount_match / vfs_init 都用 ebx*8 步进，保留字段先留 0）
vfs_mount_tab:
    dd mnt_root, 0                 ; DRV_MEFS：根挂载 "/"
    dd mnt_dev, 0                  ; DRV_DEV ：/dev
    dd mnt_hd, 0                   ; DRV_EXT2：/hd（FS3）
    dd 0, 0, 0, 0, 0, 0            ; 空槽（加挂载点就填这里，并同步 vfs_drv_tab）

; 文件系统名字表：和挂载表一一对应（mount / stat 打印用）
vfs_fsname_tab:
    dd fsname_mefs
    dd fsname_devfs
    dd fsname_ext2
    dd 0, 0, 0, 0, 0, 0

; 驱动函数表：每行 {open, read, write, close, readdir, create}，行号 = 驱动号
vfs_drv_tab:
    dd mefs_open,  mefs_read,  mefs_write,  mefs_close,  mefs_readdir,  0            ; DRV_MEFS（MeFS 的创建走 fs_* 原语，不经驱动）
    dd dev_open,   dev_read,   dev_write,   dev_close,   dev_readdir,   0            ; DRV_DEV
    dd ext2_open,  ext2_read,  ext2_write,  ext2_close,  ext2_readdir,  ext2_create  ; DRV_EXT2

mnt_root       db "/", 0
mnt_dev        db "/dev", 0
mnt_hd         db "/hd", 0
fsname_mefs    db "mefs", 0
fsname_devfs   db "devfs", 0
fsname_ext2    db "ext2", 0

vfs_fd_tab     times VFS_MAX_FD * FD_LEN db 0
vfs_cur_vn     times 16 db 0        ; vfs_open 的 vnode 暂存
vfs_rdbuf      times VFS_RD_BUF db 0

; devfs 设备注册表：{名字指针, 名字长度, 设备号}，空指针结尾
devfs_ent:
    dd dev_name_null,    4, DEV_NULL
    dd dev_name_console, 7, DEV_CONSOLE
    dd 0

dev_name_null    db "null", 0
dev_name_console db "console", 0
devfs_console_text db "MeOS console device", 10, 0
devfs_console_len equ $ - devfs_console_text - 1

; 挂载匹配工作区
vfs_mm_path  dd 0
vfs_mm_mlen  dd 0
vfs_mm_best  dd 0
vfs_mm_drv   dd 0
vfs_mm_rel   dd 0

; vfs_create 工作区
vfs_ci_kind  dd 0
vfs_ci_drv   dd 0

; vfs_readdir 工作区（40B 记录本体是共享缓冲，命令层用完即弃）
vfs_rdrec_out dd 0
vfs_dir_rec   times VRD_LEN db 0

; mefs_readdir 工作区
vfs_mr_fd    dd 0
vfs_mr_out   dd 0
vfs_mr_blk   dd 0
vfs_mr_addr  dd 0
vfs_mr_cnt   dd 0

; dev_readdir 工作区
vfs_dr_fd    dd 0
vfs_dr_out   dd 0
vfs_dr_idx   dd 0

; vfs_open 工作区
vfs_tmp_drv  dd 0
vfs_tmp_rel  dd 0

; 驱动例程工作区
vfs_tmp_vnout dd 0
vfs_rd_fd   dd 0
vfs_rd_dst  dd 0
vfs_rd_need dd 0
vfs_rd_left dd 0
vfs_rd_boff dd 0
vfs_rd_blk  dd 0

; vfs_cat 工作区
vfs_cat_fd   dd 0
vfs_cat_n    dd 0
vfs_cat_last db 0
vfs_cat_total dd 0

; stat 命令工作区
vfs_st_fd    dd 0

devfs_name_buf times 20 db 0

; 类型名字表（stat 用，下标 = VN_KIND）
vfs_type_msgs:
    dd msg_vfs_file
    dd msg_vfs_dir
    dd msg_vfs_dev

msg_vfs_file  db "file", 0
msg_vfs_dir   db "dir", 0
msg_vfs_dev   db "dev", 0
msg_vfs_on    db " on ", 0
msg_vfs_size  db " size=", 0
msg_vfs_fs    db " fs=", 0
msg_vfs_unk   db "unknown", 0
msg_vfs_stat_usage db "usage: stat <path>", 0

cmd_mount     db "mount", 0
cmd_stat      db "stat", 0
msg_fs_mount  db "mount", 0
msg_fs_stat   db "stat", 0
