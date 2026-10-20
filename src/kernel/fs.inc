; ============================================================================
;  MeFS：RAM 文件系统 + 类 Linux 终端命令
; ----------------------------------------------------------------------------
;  协议与 tools/mkramfs.py 严格一致（两边是同一份布局的两次实现）：
;
;    · 块 512 字节，首 4 字节 = 链上下一块（0 = 结尾），后 508 字节 = 数据
;    · 块 0 超块：魔数"MEFS"/版本/块大小/总块数/空闲块数/根目录块
;    · 块 1..8 分配位图（每块盖 4096 个块），块 9 起是数据
;    · 目录记录 42 字节：kind(0 文件/1 目录) name_len name(32) first(4) size(4)
;      name_len == 0 表示空洞（删掉的记录）；目录 size = 子项条数
;
;  fs_init 把 ramfs.inc 里的快照拷到 0x00100000（共 16MB 的 RAM 区），
;  之后所有块操作都直接按「块号 x 512 + 基址」寻址，跟快照里的块号一一对应。
;
;  路径解析统一规则：相对路径先拼上 cwd 再整条规范化（去多余斜杠、"."，
;  ".." 退一层），最终得到绝对路径存进 path_buf，根目录就是 "/"。
;
;  终端字模只有 0x20..0x7E，所以人机对话（命令、文件名、提示）一律 ASCII。
; ============================================================================

FS_BLOCK_SIZE    equ 512
FS_RAM_BASE      equ 0x00100000
FS_RAM_SIZE      equ 16 * 1024 * 1024
FS_BLOCKS        equ FS_RAM_SIZE / FS_BLOCK_SIZE        ; 32768
FS_BITMAP_BLOCK0 equ 1
FS_BITMAP_BLOCKS equ (FS_BLOCKS / 8) / FS_BLOCK_SIZE    ; 8
FS_MAGIC         equ 0x5346454D                         ; "MEFS"

BLK_NEXT    equ 0x00
BLK_DATA    equ 0x04
BLK_CAP     equ FS_BLOCK_SIZE - BLK_DATA                ; 508

DE_KIND     equ 0x00
DE_NAME_LEN equ 0x01
DE_NAME     equ 0x02
DE_FIRST    equ 0x22
DE_SIZE     equ 0x26
DE_LEN      equ 42
DE_PER_BLK  equ BLK_CAP / DE_LEN                        ; 12
MAX_NAME    equ 31

KIND_FILE   equ 0
KIND_DIR    equ 1

; 错误码
FS_OK       equ 0
FS_NOENT    equ 1
FS_NOTDIR   equ 2
FS_ISDIR    equ 3
FS_EXIST    equ 4
FS_NOSPACE  equ 5
FS_NAMELEN  equ 6
FS_NOTEMPTY equ 7
FS_BADPATH  equ 8
FS_IO       equ 9            ; 坏盘 / 传输失败（FS3 起，ext2 驱动用）
FS_NOTSUPP  equ 10           ; 格式超出 v1 支持面（ext2 驱动用）

; VFS readdir 输出记录（40 字节，ls 枚举目录的统一载荷）。驱动写它、
; 命令层读它；MeFS / devfs / ext2 三个驱动同构。定义在 fs.inc 是因为
; 本文件先于 vfs.inc / ext2.inc 被 include，EQU 必须先到先用。
VRD_KIND    equ 0            ; byte  = VN_FILE / VN_DIR / VN_DEV 的值
VRD_SIZE    equ 4            ; dword 字节数（目录 = 语义各驱动自定）
VRD_NAME    equ 8            ; 31 字节名字 + 结尾 0
VRD_LEN     equ 40
VRD_NAMEMAX equ 31

; ============================================================================
;  基础块操作
; ============================================================================

; ---- 块号 -> 线性地址：EAX = 块号 -> EAX = 地址 ------------------------------
fs_block_addr:
    imul eax, eax, FS_BLOCK_SIZE
    add  eax, FS_RAM_BASE
    ret

; ---- 设置位图某一位：EAX = 块号 ----------------------------------------------
fs_set_bit:
    push ebx
    push ecx
    push edx
    mov  ebx, eax                       ; blk
    mov  ecx, ebx
    and  ecx, 7                         ; 位
    mov  edx, 1
    shl  edx, cl
    shr  ebx, 3                         ; 总字节偏移 = blk / 8
    mov  eax, ebx
    shr  eax, 9                         ; 落在第几块位图
    add  eax, FS_BITMAP_BLOCK0
    call fs_block_addr
    and  ebx, FS_BLOCK_SIZE - 1         ; 块内字节（位图块 512 字节装 4096 位）
    add  eax, ebx
    or   [eax], dl
    pop  edx
    pop  ecx
    pop  ebx
    ret

; ---- 清掉位图某一位：EAX = 块号 ----------------------------------------------
fs_clear_bit:
    push ebx
    push ecx
    push edx
    mov  ebx, eax
    mov  ecx, ebx
    and  ecx, 7
    mov  edx, 1
    shl  edx, cl
    not  dl
    shr  ebx, 3
    mov  eax, ebx
    shr  eax, 9
    add  eax, FS_BITMAP_BLOCK0
    call fs_block_addr
    and  ebx, FS_BLOCK_SIZE - 1
    add  eax, ebx
    and  [eax], dl
    pop  edx
    pop  ecx
    pop  ebx
    ret

; ---- 分配一个空闲块 -> EAX = 块号（0 = 满了）-------------------------------
fs_alloc_block:
    push ebx
    push ecx
    push edx
    push esi
    mov  ebx, 0                         ; 已扫过的块数（供拼块号）
    mov  ecx, FS_BITMAP_BLOCK0
.bm_blk:
    mov  eax, ecx
    call fs_block_addr
    mov  esi, eax
    xor  edx, edx                       ; 位图块内字节
.by:
    cmp  edx, FS_BLOCK_SIZE
    jae  .bm_next
    movzx eax, byte [esi + edx]
    cmp  al, 0xFF
    jne  .have
    inc  edx
    jmp  .by
.bm_next:
    add  ebx, FS_BLOCK_SIZE * 8         ; 一块位图盖 4096 个块
    inc  ecx
    cmp  ecx, FS_BITMAP_BLOCK0 + FS_BITMAP_BLOCKS
    jb   .bm_blk
    mov  dword [fs_err], FS_NOSPACE     ; 全满
    xor  eax, eax
    jmp  .out
.have:
    xor  ecx, ecx                       ; 找字节里最低的 0 位
.bit:
    test al, 1
    jz   .got
    shr  al, 1
    inc  ecx
    jmp  .bit
.got:
    mov  eax, edx
    shl  eax, 3
    add  eax, ecx
    add  eax, ebx                       ; 块号
    push eax
    call fs_set_bit
    mov  dword [fs_err], FS_OK
    pop  eax
.out:
    pop  esi
    pop  edx
    pop  ecx
    pop  ebx
    ret

; ---- 释放整条链：EAX = 首块 ------------------------------------------------
fs_free_chain:
    push ebx
    push esi
    mov  esi, eax
.free:
    test esi, esi
    jz   .done
    mov  ebx, esi
    mov  eax, ebx
    call fs_block_addr
    mov  esi, [eax]                     ; 先记下下一个，再清位
    mov  eax, ebx
    call fs_clear_bit
    jmp  .free
.done:
    pop  esi
    pop  ebx
    ret

; ---- 找链尾：EAX = 首块 -> EAX = 尾块 --------------------------------------
fs_chain_tail:
    test eax, eax
    jz   .done
.loop:
    push eax
    call fs_block_addr
    mov  eax, [eax]
    mov  ecx, eax
    pop  eax
    test ecx, ecx
    jz   .done
    mov  eax, ecx
    jmp  .loop
.done:
    ret

; ---- 从内存拷内容建一条链：ESI = 源, ECX = 长度 -> EAX = 首块 ----------------
;  长度 0 返回 0（空文件/空链）；中途没块了把已分配的整条还回去。
;  会改动 fs_err。调用方别指望 fs_tmp0..5 原样保留。
fs_alloc_chain_from:
    pushad
    test ecx, ecx
    jz   .zero
    mov  [fs_tmp0], esi
    mov  [fs_tmp1], ecx
    mov  dword [fs_tmp2], 0             ; 首块
    mov  dword [fs_tmp3], 0             ; 上一块
.loop:
    mov  eax, [fs_tmp1]
    test eax, eax
    jz   .done
    call fs_alloc_block
    test eax, eax
    jz   .fail
    mov  [fs_tmp4], eax
    cmp  dword [fs_tmp3], 0
    jne  .link
    mov  eax, [fs_tmp4]
    mov  [fs_tmp2], eax
    jmp  .copy
.link:
    mov  eax, [fs_tmp3]
    call fs_block_addr
    mov  edx, [fs_tmp4]
    mov  [eax], edx
.copy:
    mov  eax, [fs_tmp4]
    call fs_block_addr
    lea  edi, [eax + BLK_DATA]
    mov  ecx, [fs_tmp1]
    cmp  ecx, BLK_CAP
    jbe  .n
    mov  ecx, BLK_CAP
.n:
    mov  [fs_tmp5], ecx
    mov  esi, [fs_tmp0]
    rep  movsb
    mov  ecx, [fs_tmp5]
    add  [fs_tmp0], ecx
    sub  [fs_tmp1], ecx
    mov  eax, [fs_tmp4]
    mov  [fs_tmp3], eax
    jmp  .loop
.zero:
    mov  dword [fs_tmp2], 0
    jmp  .done
.fail:
    mov  eax, [fs_tmp2]
    call fs_free_chain
    mov  dword [fs_tmp2], 0
.done:
    mov  eax, [fs_tmp2]
    mov  [esp + 28], eax
    popad
    ret

; ============================================================================
;  目录记录
; ============================================================================

; ---- 字符串长度：ESI -> EAX ------------------------------------------------
str_len:
    push esi
    xor  eax, eax
.loop:
    cmp  byte [esi], 0
    je   .done
    inc  esi
    inc  eax
    jmp  .loop
.done:
    pop  esi
    ret

; ---- 把 fs_ent_* 打包成一条记录，写进 fs_rec_buf ---------------------------
fs_build_rec:
    pushad
    mov  eax, fs_rec_buf
    mov  bl, [fs_ent_kind]
    mov  [eax + DE_KIND], bl
    mov  bl, [fs_ent_name_len]
    mov  [eax + DE_NAME_LEN], bl
    movzx ecx, byte [fs_ent_name_len]
    mov  esi, fs_ent_name
    lea  edi, [eax + DE_NAME]
    rep  movsb
    mov  ecx, [fs_ent_first]
    mov  [eax + DE_FIRST], ecx
    mov  ecx, [fs_ent_size]
    mov  [eax + DE_SIZE], ecx
    popad
    ret

; ---- 把一条记录读进 fs_ent_*：EAX = 记录地址 --------------------------------
fs_read_rec:
    pushad
    mov  edx, eax
    movzx eax, byte [edx + DE_KIND]
    mov  [fs_ent_kind], al
    movzx eax, byte [edx + DE_NAME_LEN]
    mov  [fs_ent_name_len], al
    movzx ecx, al
    mov  esi, edx
    add  esi, DE_NAME
    mov  edi, fs_ent_name
    rep  movsb
    mov  byte [edi], 0
    mov  eax, [edx + DE_FIRST]
    mov  [fs_ent_first], eax
    mov  eax, [edx + DE_SIZE]
    mov  [fs_ent_size], eax
    popad
    ret

; ---- 在目录链里找一条记录：EAX = 目录块, ESI = 名字 -------------------------
;  命中 -> EAX = 1，fs_dirent_addr + fs_ent_* 填好；没命中 -> EAX = 0。
;  会改动 fs_err / fs_tmp0..4（本函数私有）。
fs_dir_find:
    pushad
    mov  [fs_tmp0], eax                 ; 目录块
    mov  [fs_tmp1], esi                 ; 名字
.loop:
    mov  eax, [fs_tmp0]
    test eax, eax
    jz   .notfound
    call fs_block_addr
    mov  ebx, eax                       ; 块地址
    xor  ecx, ecx                       ; 槽位
.slot:
    mov  [fs_tmp4], ecx                 ; 槽号存档：repe cmpsb 会把 ECX 当计数器踩掉
    imul edx, ecx, DE_LEN
    lea  edx, [ebx + edx + BLK_DATA]
    mov  [fs_tmp2], edx
    movzx eax, byte [edx + DE_NAME_LEN]
    mov  [fs_tmp3], eax
    test eax, eax
    jz   .slot_next                     ; 空洞
    mov  esi, [fs_tmp1]
    call str_len
    cmp  eax, [fs_tmp3]
    jne  .slot_next
    mov  ecx, [fs_tmp3]
    mov  esi, [fs_tmp1]
    mov  edi, [fs_tmp2]
    add  edi, DE_NAME
    repe cmpsb
    jnz  .slot_next
    ; 命中
    mov  eax, [fs_tmp2]
    mov  [fs_dirent_addr], eax
    call fs_read_rec
    mov  dword [fs_err], FS_OK
    mov  dword [esp + 28], 1
    popad
    ret
.slot_next:
    mov  ecx, [fs_tmp4]                 ; 取回真正的槽号（比较过的槽 ECX 已不是槽号）
    inc  ecx
    cmp  ecx, DE_PER_BLK
    jb   .slot
    mov  eax, [fs_tmp0]
    call fs_block_addr
    mov  eax, [eax]                     ; 链上下一块
    mov  [fs_tmp0], eax
    xor  ecx, ecx
    jmp  .loop
.notfound:
    mov  dword [esp + 28], 0
    popad
    ret

; ---- 在目录链里追加 fs_rec_buf 里的记录：EAX = 目录块 ------------------------
;  找到第一个空洞就填，全满则补一块链上去。目录约定至少有一块（fs_create 保证），
;  万一传 0 只能报 BADPATH——父记录连不回写（踩过：空目录 first=0 引发连环炸）。
fs_dir_append:
    pushad
    mov  [fs_tmp0], eax                 ; 当前块
    mov  dword [fs_tmp1], 0             ; 链尾块
.loop:
    mov  eax, [fs_tmp0]
    test eax, eax
    jz   .new_block
    mov  [fs_tmp1], eax
    call fs_block_addr
    mov  ebx, eax
    xor  ecx, ecx
.slot:
    imul edx, ecx, DE_LEN
    lea  edx, [ebx + edx + BLK_DATA]
    cmp  byte [edx + DE_NAME_LEN], 0
    je   .write
    inc  ecx
    cmp  ecx, DE_PER_BLK
    jb   .slot
    mov  eax, [fs_tmp0]
    call fs_block_addr
    mov  eax, [eax]
    mov  [fs_tmp0], eax
    jmp  .loop
.new_block:
    cmp  dword [fs_tmp1], 0
    je   .bad_empty                     ; 空目录链，招呼不了
    call fs_alloc_block
    test eax, eax
    jz   .fail
    mov  [fs_tmp2], eax                 ; 新块
    mov  eax, [fs_tmp1]                 ; 链尾块
    call fs_block_addr
    mov  edx, [fs_tmp2]
    mov  [eax], edx                     ; 链尾.next = 新块
    mov  eax, [fs_tmp2]                 ; 新块地址
    call fs_block_addr
    add  eax, BLK_DATA
    mov  edi, eax
    mov  esi, fs_rec_buf
    mov  ecx, DE_LEN
    rep  movsb
    mov  dword [fs_err], FS_OK
    mov  dword [esp + 28], 1
    popad
    ret
.write:
    mov  esi, fs_rec_buf
    mov  edi, edx
    mov  ecx, DE_LEN
    rep  movsb
    mov  dword [fs_err], FS_OK
    mov  dword [esp + 28], 1
    popad
    ret
.bad_empty:
    mov  dword [fs_err], FS_BADPATH
    mov  dword [esp + 28], 0
    popad
    ret
.fail:
    mov  dword [fs_err], FS_NOSPACE
    mov  dword [esp + 28], 0
    popad
    ret

; ---- 目录迭代器：EAX = 目录块，从 -1 槽开始 ---------------------------------
fs_dir_first:
    mov  [fs_iter_blk], eax
    mov  dword [fs_iter_slot], -1
; 回落到 fs_dir_next
; ---- 取下一条目录记录 -> EAX = 1：fs_ent_* / fs_dirent_addr；0 = 枚举完 ------
fs_dir_next:
    pushad
.next_blk:
    mov  eax, [fs_iter_blk]
    test eax, eax
    jz   .done0
    call fs_block_addr
    mov  ebx, eax
    mov  ecx, [fs_iter_slot]
    inc  ecx
.slot:
    cmp  ecx, DE_PER_BLK
    jae  .advance
    imul edx, ecx, DE_LEN
    lea  edx, [ebx + edx + BLK_DATA]
    cmp  byte [edx + DE_NAME_LEN], 0
    je   .skip
    mov  [fs_iter_slot], ecx
    mov  [fs_dirent_addr], edx
    mov  eax, edx
    call fs_read_rec
    mov  dword [esp + 28], 1
    popad
    ret
.skip:
    inc  ecx
    jmp  .slot
.advance:
    mov  eax, [fs_iter_blk]
    call fs_block_addr
    mov  eax, [eax]
    mov  [fs_iter_blk], eax
    mov  dword [fs_iter_slot], -1
    jmp  .next_blk
.done0:
    mov  dword [esp + 28], 0
    popad
    ret

; ============================================================================
;  路径解析与查找
; ============================================================================

; ---- 规范化路径：ESI = 原始路径 -> path_buf（绝对、无尾斜杠，根 = "/"） -----
;  相对路径先拼 cwd；"." 丢弃、".." 退一层、多余的 '/' 合并。
;  EAX = 1 成功 / 0 失败（fs_err 给出原因）。会改动 fs_err。
fs_action:
    pushad
    mov  [fs_path_pos], esi
    ; 清空 path_buf（256 字节）
    mov  edi, path_buf
    xor  eax, eax
    mov  ecx, 64
    rep  stosd
    ; 组装：绝对直接用，相对拼 cwd + '/'（edi 重新锚定到 path_buf！）
    mov  edi, path_buf
    mov  esi, [fs_path_pos]
    cmp  byte [esi], '/'
    je   .raw
    mov  esi, cwd_buf
.copy_cwd:
    mov  al, [esi]
    test al, al
    jz   .cwd_done
    mov  [edi], al
    inc  esi
    inc  edi
    jmp  .copy_cwd
.cwd_done:
    mov  byte [edi], '/'
    inc  edi
    mov  esi, [fs_path_pos]
.raw:
    mov  al, [esi]
    test al, al
    jz   .parse
    mov  [edi], al
    inc  esi
    inc  edi
    jmp  .raw
.parse:
    mov  dword [fs_stk_len], 0
    xor  esi, esi                       ; 读指针
.comp:
    ; 跳过 '/'
.skip_slash:
    cmp  byte [path_buf + esi], '/'
    jne  .have
    inc  esi
    jmp  .skip_slash
.have:
    cmp  byte [path_buf + esi], 0
    je   .rebuild
    mov  edx, esi                       ; 组件起点
.len:
    cmp  byte [path_buf + esi], '/'
    je   .len_done
    cmp  byte [path_buf + esi], 0
    je   .len_done
    inc  esi
    jmp  .len
.len_done:
    mov  ecx, esi
    sub  ecx, edx                       ; 组件长度
    ; "." 丢弃
    cmp  ecx, 1
    jne  .not_dot
    cmp  byte [path_buf + edx], '.'
    je   .comp
.not_dot:
    ; ".." 弹栈
    cmp  ecx, 2
    jne  .push
    cmp  word [path_buf + edx], 0x2E2E
    jne  .push
    cmp  dword [fs_stk_len], 0
    jz   .comp                         ; 根目录顶上的 ".." 原地踏步
    dec  dword [fs_stk_len]
    jmp  .comp
.push:
    cmp  ecx, MAX_NAME
    ja   .namelen
    mov  eax, [fs_stk_len]
    cmp  eax, 16
    jae  .deep
    mov  [fs_stk_off + eax * 4], edx
    mov  [fs_stk_off + eax * 4 + 64], ecx
    inc  dword [fs_stk_len]
    jmp  .comp
.rebuild:
    mov  edi, path_buf
    mov  byte [edi], '/'
    inc  edi
    xor  ecx, ecx                       ; 栈下标
.rloop:
    cmp  ecx, [fs_stk_len]
    jae  .rebuild_done
    mov  esi, [fs_stk_off + ecx * 4]
    mov  ebx, [fs_stk_off + ecx * 4 + 64]
    xor  edx, edx
.cp:
    cmp  edx, ebx
    jae  .cp_done
    mov  al, [path_buf + esi + edx]
    mov  [edi], al
    inc  edi
    inc  edx
    jmp  .cp
.cp_done:
    mov  byte [edi], '/'
    inc  edi
    inc  ecx
    jmp  .rloop
.rebuild_done:
    cmp  edi, path_buf + 1
    je   .root                        ; 只有 "/"
    dec  edi
    mov  byte [edi], 0
    jmp  .ok
.root:
    mov  byte [edi], 0
.ok:
    mov  dword [fs_err], FS_OK
    mov  dword [esp + 28], 1
    popad
    ret
.namelen:
    mov  dword [fs_err], FS_NAMELEN
    mov  dword [esp + 28], 0
    popad
    ret
.deep:
    mov  dword [fs_err], FS_BADPATH
    mov  dword [esp + 28], 0
    popad
    ret

; ---- 按绝对路径找目标：ESI = 规范化绝对路径 ---------------------------------
;  命中 -> EAX = 1，fs_ent_* / fs_dirent_addr 填好；否则 EAX = 0，fs_err 给原因。
;  用寄存器持有循环状态（ebx = 当前目录块，edi = 剩余路径），
;  因为 fs_dir_find 会改写 fs_tmp0..3，碰不得。
fs_lookup:
    pushad
    mov  edi, esi                       ; 剩余路径
    mov  eax, [FS_RAM_BASE + 0x14]
    mov  ebx, eax                       ; 当前目录块（根）
    ; 先消费开头斜杠
.skip0:
    mov  al, [edi]
    cmp  al, '/'
    jne  .check
    inc  edi
    jmp  .skip0
.check:
    mov  al, [edi]
    test al, al
    jz   .root                          ; 路径就是 "/"
.comp_loop:
    ; 拷一个组件到 name_buf
    lea  esi, [name_buf]
    xor  edx, edx
.comp_copy:
    mov  al, [edi]
    cmp  al, '/'
    je   .comp_end
    test al, al
    jz   .comp_end
    cmp  edx, MAX_NAME
    jae  .namelen
    mov  [esi + edx], al
    inc  edi
    inc  edx
    jmp  .comp_copy
.comp_end:
    mov  byte [esi + edx], 0
    ; 判断还有没有下一个组件
.adv:
    mov  al, [edi]
    cmp  al, '/'
    jne  .has_more
    inc  edi
    jmp  .adv
.has_more:
    mov  al, [edi]
    test al, al
    jz   .last
    ; 中间组件：必须是目录
    mov  eax, ebx
    lea  esi, [name_buf]
    call fs_dir_find
    test eax, eax
    jz   .noent
    cmp  byte [fs_ent_kind], KIND_DIR
    jne  .notdir
    mov  eax, [fs_ent_first]
    mov  ebx, eax
    jmp  .comp_loop
.last:
    mov  eax, ebx
    lea  esi, [name_buf]
    call fs_dir_find
    test eax, eax
    jz   .noent
    mov  dword [esp + 28], 1
    popad
    ret
.root:
    mov  byte [fs_ent_kind], KIND_DIR
    mov  byte [fs_ent_name_len], 1
    mov  byte [fs_ent_name], '/'
    mov  byte [fs_ent_name + 1], 0
    mov  eax, [FS_RAM_BASE + 0x14]
    mov  [fs_ent_first], eax
    mov  dword [fs_ent_size], 0
    mov  dword [fs_dirent_addr], 0
    mov  dword [fs_err], FS_OK
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

; ---- 拆分「父目录 + 末组件」：ESI = 路径 -------------------------------------
;  成功 -> EAX = 1，fs_cur_dir = 父目录块，fs_cur_rec = 父在它爹里的记录地址，
;  name_buf = 末组件名。失败 -> EAX = 0（fs_err）。
fs_resolve_parent:
    pushad
    call fs_action
    push eax                              ; 保存 fs_action 结果
    mov  dword [DIAG_ADDR + 0x340], 51
    ; [调试] fs_action 结果快照
    mov  edi, DIAG_ADDR + 0x350
    mov  esi, path_buf
    mov  ecx, 8
    rep  movsb
    mov  eax, [fs_stk_len]
    mov  [DIAG_ADDR + 0x358], eax
    pop  eax
    test eax, eax
    jz   .out0
    ; 找到 path_buf 的尾巴
    xor  edi, edi
.blen:
    cmp  byte [path_buf + edi], 0
    je   .blen_done
    inc  edi
    jmp  .blen
.blen_done:
    cmp  edi, 1
    jbe  .root_err                      ; 根目录没有父
    mov  esi, edi
.back:
    dec  esi
    cmp  byte [path_buf + esi], '/'
    je   .found_slash
    test esi, esi
    jz   .root_err
    jmp  .back
.found_slash:
    mov  ecx, edi
    sub  ecx, esi
    dec  ecx                            ; 末组件长度
    cmp  ecx, MAX_NAME
    ja   .namelen
    mov  edx, esi
    inc  edx
    lea  edi, [name_buf]
    mov  eax, 0
.cp:
    cmp  eax, ecx
    jae  .cp_done
    mov  bl, [path_buf + edx + eax]
    mov  [edi + eax], bl
    inc  eax
    jmp  .cp
.cp_done:
    mov  byte [edi + eax], 0
    ; 截断 path_buf 成父目录路径
    mov  byte [path_buf + esi], 0
    mov  dword [DIAG_ADDR + 0x340], 52
    lea  esi, [path_buf]
    call fs_lookup
    mov  dword [DIAG_ADDR + 0x340], 53
    test eax, eax
    jz   .out0
    cmp  byte [fs_ent_kind], KIND_DIR
    jne  .notdir
    mov  eax, [fs_ent_first]
    mov  [fs_cur_dir], eax
    mov  eax, [fs_dirent_addr]
    mov  [fs_cur_rec], eax
    mov  dword [fs_err], FS_OK
    mov  dword [esp + 28], 1
    popad
    ret
.root_err:
    mov  dword [DIAG_ADDR + 0x340], 43
    mov  dword [fs_err], FS_BADPATH
.out0:
    mov  dword [esp + 28], 0
    popad
    ret
.namelen:
    mov  dword [fs_err], FS_NAMELEN
    mov  dword [esp + 28], 0
    popad
    ret
.notdir:
    mov  dword [fs_err], FS_NOTDIR
    mov  dword [esp + 28], 0
    popad
    ret
; ============================================================================
;  增删改查
; ============================================================================

; ---- 在当前目录创建条目：EAX = kind（0 文件 / 1 目录）-----------------------
;  前置：fs_resolve_parent 已成功（fs_cur_dir = 父目录块，name_buf = 名字）。
fs_create:
    pushad
    mov  [fs_cr_kind], eax
    ; 重名检查
    mov  eax, [fs_cur_dir]
    lea  esi, [name_buf]
    call fs_dir_find
    test eax, eax
    jz   .free_name
    mov  dword [fs_err], FS_EXIST
    jmp  .out0
.free_name:
    lea  esi, [name_buf]
    call str_len
    test eax, eax
    jz   .badname
    mov  [fs_ent_name_len], al
    ; 名字抄进 fs_ent_name（fs_build_rec 的输入）
    lea  esi, [name_buf]
    lea  edi, [fs_ent_name]
    movzx ecx, byte [fs_ent_name_len]
    rep  movsb
    mov  byte [edi], 0
    mov  al, [fs_cr_kind]
    mov  [fs_ent_kind], al
    mov  dword [fs_ent_first], 0
    mov  dword [fs_ent_size], 0
    ; 目录必须现分一块——fs_dir_append 指着「目录至少一块」才会走
    cmp  byte [fs_ent_kind], KIND_DIR
    jne  .have_first
    call fs_alloc_block
    test eax, eax
    jz   .out0                          ; fs_err 由 fs_alloc_block 设好
    mov  [fs_ent_first], eax
.have_first:
    call fs_build_rec
    mov  eax, [fs_cur_dir]
    call fs_dir_append
    test eax, eax
    jz   .out0
    ; 父目录的 size（子项数）加一；根目录没记录可改
    mov  eax, [fs_cur_rec]
    test eax, eax
    jz   .ok
    inc  dword [eax + DE_SIZE]
.ok:
    mov  dword [fs_err], FS_OK
    mov  dword [esp + 28], 1
    popad
    ret
.badname:
    mov  dword [DIAG_ADDR + 0x340], 44
    mov  dword [fs_err], FS_BADPATH
.out0:
    mov  dword [esp + 28], 0
    popad
    ret

; ---- 删除当前目录里的条目（文件或空目录）：fs_cur_dir + name_buf ------------
fs_delete:
    pushad
    mov  eax, [fs_cur_dir]
    lea  esi, [name_buf]
    call fs_dir_find
    test eax, eax
    jz   .noent
    cmp  byte [fs_ent_kind], KIND_DIR
    jne  .is_file
    cmp  dword [fs_ent_size], 0
    jne  .notempty
.is_file:
    mov  eax, [fs_dirent_addr]
    mov  [fs_del_addr], eax
    mov  al, [fs_ent_kind]
    mov  [fs_del_kind], al
    mov  eax, [fs_ent_first]
    mov  [fs_del_first], eax
    ; 先摘记录（变空洞），再还块
    mov  eax, [fs_del_addr]
    mov  byte [eax + DE_NAME_LEN], 0
    mov  eax, [fs_del_first]
    call fs_free_chain
    ; 父 size 减一
    mov  eax, [fs_cur_rec]
    test eax, eax
    jz   .nosize
    cmp  dword [eax + DE_SIZE], 0
    jbe  .nosize
    dec  dword [eax + DE_SIZE]
.nosize:
    mov  dword [fs_err], FS_OK
    mov  dword [esp + 28], 1
    popad
    ret
.noent:
    mov  dword [fs_err], FS_NOENT
    mov  dword [esp + 28], 0
    popad
    ret
.notempty:
    mov  dword [fs_err], FS_NOTEMPTY
    mov  dword [esp + 28], 0
    popad
    ret

; ---- 把文件链读进内存：EAX = 首块, ECX = size, ESI = 目标 -------------------
;  返回 EAX = 实际读到的字节数。调用方别保留 fs_tmp0..4。
fs_read_file:
    pushad
    mov  [fs_tmp0], eax                 ; 当前块
    mov  [fs_tmp1], ecx                 ; 剩余字节
    mov  [fs_tmp2], esi                 ; 目标
    mov  dword [fs_tmp3], 0             ; 已读字节
.loop:
    mov  eax, [fs_tmp1]
    test eax, eax
    jz   .done
    mov  eax, [fs_tmp0]
    test eax, eax
    jz   .done
    call fs_block_addr
    lea  esi, [eax + BLK_DATA]
    mov  edi, [fs_tmp2]
    mov  ecx, [fs_tmp1]
    cmp  ecx, BLK_CAP
    jbe  .n
    mov  ecx, BLK_CAP
.n:
    mov  [fs_tmp4], ecx
    rep  movsb
    mov  ecx, [fs_tmp4]
    add  [fs_tmp3], ecx
    sub  [fs_tmp1], ecx
    add  [fs_tmp2], ecx
    mov  eax, [fs_tmp0]
    call fs_block_addr
    mov  eax, [eax]
    mov  [fs_tmp0], eax
    jmp  .loop
.done:
    mov  eax, [fs_tmp3]
    mov  [esp + 28], eax
    popad
    ret

; ---- 把文件直接吐到屏幕上：EAX = 首块, ECX = size ----------------------------
fs_cat_file:
    pushad
    mov  [fs_tmp0], eax
    mov  [fs_tmp1], ecx
.loop:
    mov  eax, [fs_tmp1]
    test eax, eax
    jz   .done
    mov  eax, [fs_tmp0]
    test eax, eax
    jz   .done
    call fs_block_addr
    lea  esi, [eax + BLK_DATA]
    mov  ecx, [fs_tmp1]
    cmp  ecx, BLK_CAP
    jbe  .n
    mov  ecx, BLK_CAP
.n:
    mov  [fs_tmp4], ecx
.print:
    test ecx, ecx
    jz   .chunk_done
    movzx eax, byte [esi]
    inc  esi
    cmp  eax, 10                        ; 文件里的 \n 就是终端里的回车
    je   .nl
    call con_putc
    jmp  .pnext
.nl:
    call con_crlf
.pnext:
    dec  ecx
    jmp  .print
.chunk_done:
    mov  ecx, [fs_tmp4]
    sub  [fs_tmp1], ecx
    mov  eax, [fs_tmp0]
    call fs_block_addr
    mov  eax, [eax]
    mov  [fs_tmp0], eax
    jmp  .loop
.done:
    popad
    ret

; ---- 拷贝：ESI = 源路径, EDI = 目标路径 -------------------------------------
;  文件整条拷过去；目录拒绝（终端才 80 列宽，递归交互就没意思了）。
;  用 fs_cp_* 这组私有全局撑过 fs_dir_find / fs_alloc_block 的临时区。
fs_copy:
    pushad
    mov  [fs_cp_src], esi
    mov  [fs_cp_dst], edi
    ; ---- 源 ----
    mov  esi, [fs_cp_src]
    call fs_resolve_parent
    test eax, eax
    jz   .fail0
    mov  eax, [fs_cur_dir]
    lea  esi, [name_buf]
    call fs_dir_find
    test eax, eax
    jz   .noent
    cmp  byte [fs_ent_kind], KIND_DIR
    je   .isdir
    mov  eax, [fs_ent_first]
    mov  [fs_cp_first], eax
    mov  eax, [fs_ent_size]
    mov  [fs_cp_size], eax
    ; ---- 目标 ----
    mov  esi, [fs_cp_dst]
    call fs_resolve_parent
    test eax, eax
    jz   .fail0
    mov  eax, [fs_cur_dir]
    lea  esi, [name_buf]
    call fs_dir_find
    test eax, eax
    jnz  .exist
    ; ---- 逐块搬内容 ----
    mov  dword [fs_cp_new_first], 0
    mov  dword [fs_cp_new_last], 0
    mov  eax, [fs_cp_first]
    mov  [fs_cp_cur], eax
.copy_loop:
    mov  eax, [fs_cp_cur]
    test eax, eax
    jz   .copy_done
    call fs_alloc_block
    test eax, eax
    jz   .copy_fail
    mov  [fs_cp_blk], eax
    cmp  dword [fs_cp_new_first], 0
    jne  .link
    mov  eax, [fs_cp_blk]
    mov  [fs_cp_new_first], eax
    jmp  .data
.link:
    mov  eax, [fs_cp_new_last]
    call fs_block_addr
    mov  edx, [fs_cp_blk]
    mov  [eax], edx
.data:
    mov  eax, [fs_cp_blk]
    call fs_block_addr
    lea  edi, [eax + BLK_DATA]
    mov  eax, [fs_cp_cur]
    call fs_block_addr
    lea  esi, [eax + BLK_DATA]
    mov  ecx, BLK_CAP
    rep  movsb
    mov  eax, [fs_cp_blk]
    mov  [fs_cp_new_last], eax
    mov  eax, [fs_cp_cur]
    call fs_block_addr
    mov  eax, [eax]
    mov  [fs_cp_cur], eax
    jmp  .copy_loop
.copy_done:
    mov  byte [fs_ent_kind], KIND_FILE
    mov  eax, [fs_cp_size]
    mov  [fs_ent_size], eax
    mov  eax, [fs_cp_new_first]
    mov  [fs_ent_first], eax
    lea  esi, [name_buf]
    call str_len
    mov  [fs_ent_name_len], al
    ; 名字抄进 fs_ent_name（fs_build_rec 的输入）
    lea  esi, [name_buf]
    lea  edi, [fs_ent_name]
    movzx ecx, byte [fs_ent_name_len]
    rep  movsb
    mov  byte [edi], 0
    call fs_build_rec
    mov  eax, [fs_cur_dir]
    call fs_dir_append
    test eax, eax
    jz   .append_fail
    mov  eax, [fs_cur_rec]
    test eax, eax
    jz   .ok
    inc  dword [eax + DE_SIZE]
    jmp  .ok
.copy_fail:
    mov  eax, [fs_cp_new_first]
    call fs_free_chain
    mov  dword [fs_err], FS_NOSPACE
    jmp  .fail0
.append_fail:
    mov  eax, [fs_cp_new_first]
    call fs_free_chain
    jmp  .fail0
.noent:
    mov  dword [fs_err], FS_NOENT
    jmp  .fail0
.isdir:
    mov  dword [fs_err], FS_ISDIR
    jmp  .fail0
.exist:
    mov  dword [fs_err], FS_EXIST
.fail0:
    mov  dword [esp + 28], 0
    popad
    ret
.ok:
    mov  dword [fs_err], FS_OK
    mov  dword [esp + 28], 1
    popad
    ret

; ---- cp 的 /hd 版本：源 = MeFS，目标 = ext2（FS4）---------------------------
;  ESI = 源（原始参数），EDI = 目标（已规范化的路径串）→ EAX = 1/0
;  目标串先搬进 fs_ce_buf（后面 MeFS 查找会踩 path_buf）。
;  v1 上限：单文件 4KB、路径 63 字符——超了就报错，不做流式。
fs_copy_to_ext2:
    pushad
    cld
    mov  [fs_ce_src], esi
    mov  [fs_ce_dst], edi
    xor  ecx, ecx
.dcopy:
    mov  al, [edi]
    mov  [fs_ce_buf + ecx], al
    inc  edi
    test al, al
    jz   .dcopied
    inc  ecx
    cmp  ecx, 63
    ja   .namelen
    jmp  .dcopy
.dcopied:
    mov  [fs_ce_len], ecx
    ; ---- 源：MeFS ----
    mov  esi, [fs_ce_src]
    call fs_path_lookup
    test eax, eax
    jz   .fail_err                       ; fs_err 已设
    cmp  byte [fs_ent_kind], KIND_FILE   ; kind 是 db！按 dword 读会把
    jne  .isdir                          ; 名字前两字节也卷进来（踩坑 24）
    mov  eax, [fs_ent_size]
    cmp  eax, FS_CP_CAP
    ja   .toobig
    mov  [fs_ce_len], eax
    test eax, eax
    jz   .have_data                      ; 空文件：建了不写
    mov  esi, fs_cp_buf
    mov  eax, [fs_ent_first]
    mov  ecx, [fs_ce_len]
    call fs_read_file
    cmp  eax, [fs_ce_len]
    jne  .io
.have_data:
    ; ---- 目标：建（已存在 = 截断）→ 开 → 写 → 关 ----
    mov  esi, fs_ce_buf
    mov  eax, KIND_FILE
    call vfs_create
    test eax, eax
    jz   .fail_err
    mov  esi, fs_ce_buf
    call vfs_open
    test eax, eax
    js   .fail_err
    mov  [fs_ce_fd], eax
    mov  edi, fs_cp_buf
    mov  ecx, [fs_ce_len]
    call vfs_write
    cmp  eax, [fs_ce_len]
    jne  .io
    mov  eax, [fs_ce_fd]
    call vfs_close
    mov  dword [esp + 28], 1
    popad
    ret
.isdir:
    mov  dword [fs_err], FS_ISDIR
    jmp  .fail
.toobig:
    mov  dword [fs_err], FS_NOTSUPP
    jmp  .fail
.namelen:
    mov  dword [fs_err], FS_NAMELEN
    jmp  .fail
.io:
    mov  dword [fs_err], FS_IO
.fail:
.fail_err:
    mov  dword [esp + 28], 0
    popad
    ret

; ---- 移动 / 改名：ESI = 旧路径, EDI = 新路径 --------------------------------
fs_rename:
    pushad
    mov  [fs_mv_src], esi
    mov  [fs_mv_dst], edi
    mov  dword [fs_step], 71
    ; ---- 旧条目 ----
    mov  esi, [fs_mv_src]
    call fs_resolve_parent
    mov  dword [fs_step], 72
    test eax, eax
    jz   .fail0
    mov  eax, [fs_cur_rec]
    mov  [fs_mv_pdir], eax              ; 旧父的记录地址
    mov  eax, [fs_cur_dir]
    lea  esi, [name_buf]
    call fs_dir_find
    mov  dword [fs_step], 73
    test eax, eax
    jz   .noent
    mov  eax, [fs_dirent_addr]
    mov  [fs_mv_addr], eax
    mov  al, [fs_ent_kind]
    mov  [fs_mv_kind], al
    mov  eax, [fs_ent_first]
    mov  [fs_mv_first], eax
    mov  eax, [fs_ent_size]
    mov  [fs_mv_size], eax
    ; 旧名字抄到 name2_buf，后面再解析新路径会盖掉 name_buf
    lea  esi, [name_buf]
    lea  edi, [name2_buf]
    mov  ecx, 40
.cp_name:
    mov  al, [esi]
    mov  [edi], al
    test al, al
    jz   .cp_done
    inc  esi
    inc  edi
    jmp  .cp_name
.cp_done:
    ; ---- 新位置 ----
    mov  esi, [fs_mv_dst]
    call fs_resolve_parent
    mov  dword [fs_step], 74
    test eax, eax
    jz   .fail0
    mov  eax, [fs_cur_dir]
    lea  esi, [name_buf]
    call fs_dir_find
    mov  dword [fs_step], 75
    test eax, eax
    jnz  .exist
    ; ---- 在新父里写同一条链的新记录 ----
    mov  al, [fs_mv_kind]
    mov  [fs_ent_kind], al
    mov  eax, [fs_mv_first]
    mov  [fs_ent_first], eax
    mov  eax, [fs_mv_size]
    mov  [fs_ent_size], eax
    lea  esi, [name_buf]
    call str_len
    mov  [fs_ent_name_len], al
    ; 名字抄进 fs_ent_name（fs_build_rec 的输入）
    lea  esi, [name_buf]
    lea  edi, [fs_ent_name]
    movzx ecx, byte [fs_ent_name_len]
    rep  movsb
    mov  byte [edi], 0
    call fs_build_rec
    mov  eax, [fs_cur_dir]
    call fs_dir_append
    test eax, eax
    jz   .fail0
    ; ---- 摘掉旧记录 ----
    mov  eax, [fs_mv_addr]
    mov  byte [eax + DE_NAME_LEN], 0
    ; ---- 两边父目录的 size 各微调（同父则净 0，跳过）----
    mov  eax, [fs_cur_rec]              ; 新父记录地址
    cmp  eax, [fs_mv_pdir]
    je   .ok
    mov  edx, [fs_mv_pdir]
    test edx, edx
    jz   .new_only
    cmp  dword [edx + DE_SIZE], 0
    jbe  .new_only
    dec  dword [edx + DE_SIZE]
.new_only:
    test eax, eax
    jz   .ok
    inc  dword [eax + DE_SIZE]
.ok:
    mov  dword [fs_err], FS_OK
    mov  dword [esp + 28], 1
    popad
    ret
.noent:
    mov  dword [fs_err], FS_NOENT
    jmp  .fail0
.exist:
    mov  dword [fs_err], FS_EXIST
.fail0:
    mov  dword [esp + 28], 0
    popad
    ret

; ---- 挂载：快照拷进 RAM 区并核对超块 ----------------------------------------
fs_init:
    pushad
    mov  dword [fs_ready], 0
    ; 快照进 RAM
    mov  esi, ramfs_snapshot
    mov  edi, FS_RAM_BASE
    mov  ecx, ramfs_snapshot_len
    rep  movsb
    ; 超块核对
    mov  eax, [FS_RAM_BASE]
    cmp  eax, FS_MAGIC
    jne  .bad
    mov  eax, [FS_RAM_BASE + 0x14]      ; 根目录块
    test eax, eax
    jz   .bad
    mov  [fs_cur_dir], eax
    mov  dword [fs_cur_rec], 0
    mov  dword [cwd_buf], '/'
    mov  byte [cwd_buf + 1], 0
    mov  dword [fs_ready], 1
    mov  dword [fs_err], FS_OK
    ; 诊断块：进度 / 空闲数 / 前 384 块的位图 / 错码 / 命令计数
    mov  dword [DIAG_ADDR + D_FS_STAGE], 2
    mov  eax, [FS_RAM_BASE + 0x10]
    mov  [DIAG_ADDR + D_FS_FREE], eax
    mov  esi, FS_RAM_BASE + FS_BLOCK_SIZE
    mov  edi, DIAG_ADDR + D_FS_BITMAP
    mov  ecx, 12
    rep  movsd
    mov  dword [DIAG_ADDR + D_FS_ERR], 0
    mov  dword [DIAG_ADDR + D_FS_CMD], 0
    jmp  .out
.bad:
    mov  dword [DIAG_ADDR + D_FS_STAGE], 0
    mov  dword [fs_err], FS_BADPATH
.out:
    popad
    ret

; ---- 把 cwd / 错码 / 命令计数抄进诊断块 -------------------------------------
fs_diag:
    pushad
    cmp  dword [fs_ready], 0
    jz   .done
    ; [调试] 抄最近一次命令的名字到 0x626C（0x268 是 D_EXT2_CMD 的地盘，
    ; 早期调试字段占过那里，两者叠写曾把命令名伪装成调用计数）
    mov  esi, [fs_cmd_name]
    test esi, esi
    jz   .skip_name
    mov  edi, DIAG_ADDR + 0x26C
    xor  ecx, ecx
.name_loop:
    cmp  ecx, 31
    jae  .name_done
    mov  al, [esi]
    mov  [edi], al
    inc  esi
    inc  edi
    inc  ecx
    test al, al
    jnz  .name_loop
.name_done:
.skip_name:
    ; cwd 固定 64 字节区
    mov  esi, cwd_buf
    mov  edi, DIAG_ADDR + D_FS_CWD
    mov  ecx, 0
.cp:
    cmp  ecx, 63
    jae  .cwd_done
    mov  al, [esi]
    mov  [edi], al
    inc  esi
    inc  edi
    inc  ecx
    test al, al
    jnz  .cp
.cwd_done:
    mov  eax, [fs_err]
    mov  [DIAG_ADDR + D_FS_ERR], eax
    inc  dword [DIAG_ADDR + D_FS_CMD]
    ; [调试] 环形记录（序号, 错码, step, 最近find块/名）到 0x6700，8 槽 x 24 字节
    mov  eax, [DIAG_ADDR + D_FS_CMD]
    dec  eax
    and  eax, 7
    imul eax, eax, 24
    lea  edi, [DIAG_ADDR + 0x500 + eax]
    mov  ecx, [DIAG_ADDR + D_FS_CMD]
    dec  ecx
    mov  [edi], ecx
    mov  edx, [fs_err]
    mov  [edi + 4], edx
    mov  edx, [fs_step]
    mov  [edi + 8], edx
    mov  edx, [DIAG_ADDR + 0x400]
    mov  [edi + 12], edx
    mov  edx, [DIAG_ADDR + 0x408]
    mov  [edi + 16], edx
    mov  dword [fs_step], 0
    ; [调试] 中间状态快照到 0x64E0：path_buf/name_buf/stk_len/ent_*
    lea  esi, [path_buf]
    mov  edi, DIAG_ADDR + 0x2E0
    mov  ecx, 32
    rep  movsb
    lea  esi, [name_buf]
    mov  edi, DIAG_ADDR + 0x300
    mov  ecx, 32
    rep  movsb
    mov  eax, [fs_stk_len]
    mov  [DIAG_ADDR + 0x320], eax
    movzx eax, byte [fs_ent_kind]
    mov  [DIAG_ADDR + 0x324], eax
    mov  eax, [fs_ent_first]
    mov  [DIAG_ADDR + 0x328], eax
    mov  eax, [fs_cur_dir]
    mov  [DIAG_ADDR + 0x32C], eax
.done:
    popad
    ret

; ---- 打印 fs_err 对应的原因：前缀用 fs_cmd_name，打完自动清零 ----------------
fs_perror:
    pushad
    mov  eax, [esp + 32]                    ; 返回地址
    mov  [DIAG_ADDR + 0x2D0], eax           ; [调试] 错误来自哪个调用点
    mov  eax, [fs_err]
    test eax, eax
    jz   .done
    cmp  eax, 9
    jae  .done
    mov  esi, [fs_cmd_name]
    test esi, esi
    jz   .noname
    call con_puts
    mov  al, ':'
    call con_putc
    mov  al, ' '
    call con_putc
.noname:
    mov  eax, [fs_err]
    mov  esi, [fs_err_msgs + eax * 4]
    call con_puts
    call con_crlf
    ; mov  dword [fs_err], FS_OK          ; [调试] 先不清零，看 D_FS_ERR 留痕
.done:
    popad
    ret

; ============================================================================
;  命令层：把上面的原语接成终端命令（ls cat touch mkdir rm pwd cd cp mv df）
; ============================================================================
;  每条命令同一个套路：
;     fs_ensure（没挂载就提示退出）
;     → 解析路径 / 参数
;     → 调原语（失败时 fs_err 有原因）
;     → fs_perror（打印即消费）
;     → fs_diag（把 cwd / 错码 / 命令计数抄进诊断块）

; ---- 挂载检查：返回 EAX = 1 已就绪 / 0 未就绪（已打印提示）------------------
fs_ensure:
    cmp  dword [fs_ready], 0
    jne  .ok
    mov  esi, msg_fs_notready
    call con_puts
    call con_crlf
    xor  eax, eax
    ret
.ok:
    mov  eax, 1
    ret

; ---- 跳过行首空格：ESI -> 第一个非空格字符 ----------------------------------
fs_skip_spaces:
.sp:
    cmp  byte [esi], ' '
    jne  .done
    inc  esi
    jmp  .sp
.done:
    ret

; ---- 拆两个参数：ESI = "a b" -> EAX=1 时 ESI=a、EDI=b（两个参数字符串）--------
;  参数 1 的结尾空格会被改写成 0，两个参数都指向 line_buf 内部：
;  fs_copy / fs_rename 只保存指针，解析过程不动它们，所以安全。
fs_split_two:
    push ebx
    push edx
    call fs_skip_spaces
    mov  edx, esi                       ; 参数 1 起点
    cmp  byte [esi], 0
    je   .bad
.next:
    mov  al, [esi]
    test al, al
    jz   .bad
    cmp  al, ' '
    je   .split
    inc  esi
    jmp  .next
.split:
    mov  byte [esi], 0                  ; 参数 1 在第一个空格处截断
    inc  esi
    call fs_skip_spaces
    cmp  byte [esi], 0
    je   .bad
    mov  ebx, esi                       ; 参数 2 起点
    mov  esi, edx
    mov  edi, ebx
    mov  eax, 1
    pop  edx
    pop  ebx
    ret
.bad:
    xor  eax, eax
    pop  edx
    pop  ebx
    ret

; ---- 解析并查找一条路径：ESI = 原始路径 -> EAX = 1 命中（fs_ent_* 填好）------
;  失败 -> EAX = 0，fs_err 有原因（FS_BADPATH / FS_NOENT / FS_NOTDIR ...）。
fs_path_lookup:
    push esi
    call fs_action
    mov  dword [DIAG_ADDR + 0x340], 56
    test eax, eax
    jz   .out
    lea  esi, [path_buf]
    call fs_lookup
    mov  dword [DIAG_ADDR + 0x340], 57
.out:
    pop  esi
    ret

; ---- pwd：打印当前目录 -------------------------------------------------------
cmd_do_pwd:
    pushad
    mov  dword [fs_cmd_name], msg_fs_pwd
    call fs_ensure
    test eax, eax
    jz   .done
    mov  esi, cwd_buf
    call con_puts
    call con_crlf
.done:
    call fs_diag
    popad
    ret

; ---- cd [路径]：无参与 "cd /" 都回根目录 ------------------------------------
cmd_do_cd:
    pushad
    mov  dword [fs_cmd_name], msg_fs_cd
    call fs_ensure
    test eax, eax
    jz   .done
    test esi, esi
    jnz  .path
    ; 无参数：回根目录
    mov  dword [cwd_buf], '/'           ; '/' + 0 + 0 + 0
    mov  eax, [FS_RAM_BASE + 0x14]      ; 超块里记的根目录块
    mov  [fs_cur_dir], eax
    mov  dword [fs_cur_rec], 0
    mov  dword [fs_err], FS_OK
    jmp  .done
.path:
    call fs_path_lookup
    test eax, eax
    jz   .perr
    cmp  byte [fs_ent_kind], KIND_DIR
    je   .isdir
    mov  dword [fs_err], FS_NOTDIR
    jmp  .perr
.isdir:
    ; 把规范化好的绝对路径抄进 cwd_buf（64 字节区，路径解析不保证短于 64）
    lea  esi, [path_buf]
    lea  edi, [cwd_buf]
    xor  ecx, ecx
.cp:
    mov  al, [esi]
    mov  [edi], al
    inc  esi
    inc  edi
    inc  ecx
    test al, al
    jz   .cpend
    cmp  ecx, 63
    jae  .cpend
    jmp  .cp
.cpend:
    mov  eax, [fs_ent_first]
    mov  [fs_cur_dir], eax
    mov  eax, [fs_dirent_addr]
    mov  [fs_cur_rec], eax
    mov  dword [fs_err], FS_OK
    jmp  .done
.perr:
    call fs_perror
.done:
    call fs_diag
    popad
    ret

; ---- ls [路径]：缺省列当前目录；目录名后跟一个 '/' ---------------------------
;  FS3 起走 VFS 统一枚举（vfs_readdir）：MeFS / devfs / ext2 一条路，
;  /dev 不再特判（vfs_is_devpath / vfs_list_dev 退役）。
cmd_do_ls:
    pushad
    mov  dword [fs_cmd_name], msg_fs_ls
    call fs_ensure
    test eax, eax
    jz   .done
    test esi, esi
    jnz  .path
    lea  esi, [cwd_buf]                ; 缺省 = 当前目录（fs_action 自会拼绝对路径）
.path:
    call vfs_open
    test eax, eax
    js   .perr
    mov  [fs_ls_fd], eax
    call vfs_fd_ptr
    cmp  byte [eax + FD_VN + VN_KIND], VN_DIR
    jne  .notdir
.loop:
    mov  eax, [fs_ls_fd]
    mov  esi, vfs_dir_rec
    call vfs_readdir
    test eax, eax
    jz   .endloop
    mov  esi, vfs_dir_rec + VRD_NAME
    call con_puts
    cmp  byte [vfs_dir_rec + VRD_KIND], VN_DIR
    jne  .nl
    mov  al, '/'
    call con_putc
.nl:
    call con_crlf
    jmp  .loop
.endloop:
    cmp  dword [fs_err], 0             ; 枚举中途出错才会非 0（正常到尾是 0）
    jne  .perr
    mov  eax, [fs_ls_fd]
    call vfs_close
    jmp  .done
.notdir:
    mov  dword [fs_err], FS_NOTDIR
    mov  eax, [fs_ls_fd]
    call vfs_close
    jmp  .perr
.perr:
    call fs_perror
.done:
    call fs_diag
    popad
    ret

; ---- cat <路径>：把文件内容原样吐到屏幕 -------------------------------------
;  走 VFS 的 fd 接口（vfs_cat 在 vfs.inc），MeFS 和 /dev 下的设备文件通吃。
cmd_do_cat:
    pushad
    mov  dword [fs_cmd_name], msg_fs_cat
    call fs_ensure
    test eax, eax
    jz   .done
    test esi, esi
    jz   .usage
    call vfs_cat
    test eax, eax
    jnz  .done
    call fs_perror
    jmp  .done
.usage:
    mov  esi, msg_fs_cat_usage
    call con_puts
    call con_crlf
    jmp  .done
.done:
    call fs_diag
    popad
    ret

; ---- touch <路径>：没有就建空文件，有就什么都不做（本系统没有 mtime）--------
cmd_do_touch:
    pushad
    mov  dword [fs_cmd_name], msg_fs_touch
    call fs_ensure
    test eax, eax
    jz   .done
    ; ---- FS4：目标在 /hd 下 → ext2 ----
    mov  [fs_tmp0], esi
    call fs_action                       ; path_buf = 规范化（ESI 由 pushad 保住）
    test eax, eax
    jz   .perr
    lea  esi, [path_buf]
    call vfs_mount_match
    cmp  eax, DRV_EXT2
    jne  .meos
    lea  esi, [path_buf]
    call vfs_open
    test eax, eax
    jns  .hd_have                        ; 已存在（文件或目录）= 无事可做
    lea  esi, [path_buf]
    mov  eax, KIND_FILE
    call vfs_create                      ; 不存在 → 建空文件
    test eax, eax
    jz   .perr
.hd_have:
    jmp  .done
.meos:
    mov  esi, [fs_tmp0]
    call fs_resolve_parent
    test eax, eax
    jz   .perr
    ; 已存在 = 无事可做
    mov  eax, [fs_cur_dir]
    lea  esi, [name_buf]
    call fs_dir_find
    test eax, eax
    jnz  .done
    mov  eax, KIND_FILE
    call fs_create
    test eax, eax
    jz   .perr
    jmp  .done
.perr:
    call fs_perror
.done:
    call fs_diag
    popad
    ret

; ---- mkdir <路径> ------------------------------------------------------------
cmd_do_mkdir:
    pushad
    mov  dword [fs_cmd_name], msg_fs_mkdir
    call fs_ensure
    test eax, eax
    jz   .done
    ; ---- FS4：目标在 /hd 下 → ext2 ----
    mov  [fs_tmp0], esi
    call fs_action
    test eax, eax
    jz   .perr
    lea  esi, [path_buf]
    call vfs_mount_match
    cmp  eax, DRV_EXT2
    jne  .meos
    lea  esi, [path_buf]
    mov  eax, KIND_DIR
    call vfs_create                      ; 已存在 → FS_EXIST → .perr（语义正好）
    test eax, eax
    jz   .perr
    jmp  .done
.meos:
    mov  esi, [fs_tmp0]
    call fs_resolve_parent
    test eax, eax
    jz   .perr
    mov  eax, KIND_DIR
    call fs_create
    test eax, eax
    jz   .perr
    jmp  .done
.perr:
    call fs_perror
.done:
    call fs_diag
    popad
    ret

; ---- rm <路径>：删文件或（空的）目录 -----------------------------------------
cmd_do_rm:
    pushad
    mov  dword [fs_cmd_name], msg_fs_rm
    call fs_ensure
    test eax, eax
    jz   .done
    test esi, esi
    jz   .usage
    ; ---- FS4：目标在 /hd 下 → 显式不支持（ext2 删除是 FS5）----
    mov  [fs_tmp0], esi
    call fs_action
    test eax, eax
    jz   .perr
    lea  esi, [path_buf]
    call vfs_mount_match
    cmp  eax, DRV_EXT2
    jne  .meos
    mov  dword [fs_err], FS_NOTSUPP
    jmp  .perr
.meos:
    mov  esi, [fs_tmp0]
    call fs_resolve_parent
    test eax, eax
    jz   .perr
    call fs_delete
    test eax, eax
    jz   .perr
    jmp  .done
.usage:
    mov  esi, msg_fs_rm_usage
    call con_puts
    call con_crlf
    jmp  .done
.perr:
    call fs_perror
.done:
    call fs_diag
    popad
    ret

; ---- cp <源> <目标>：文件拷贝（目录拒绝，见 fs_copy 注释）--------------------
cmd_do_cp:
    pushad
    mov  dword [fs_cmd_name], msg_fs_cp
    call fs_ensure
    test eax, eax
    jz   .done
    call fs_split_two
    test eax, eax
    jz   .usage
    ; ---- FS4：目标在 /hd 下 → ext2 拷贝（源仍取 MeFS）----
    ;  split 后 ESI = 源、EDI = 目标
    mov  [fs_tmp0], esi
    mov  esi, edi
    call fs_action                       ; path_buf = 目标的规范化
    test eax, eax
    jz   .perr
    lea  esi, [path_buf]
    call vfs_mount_match
    cmp  eax, DRV_EXT2
    jne  .meos
    mov  esi, [fs_tmp0]
    lea  edi, [path_buf]
    call fs_copy_to_ext2
    test eax, eax
    jz   .perr
    jmp  .done
.meos:
    mov  esi, [fs_tmp0]
    call fs_copy
    test eax, eax
    jz   .perr
    jmp  .done
.usage:
    mov  esi, msg_fs_cp_usage
    call con_puts
    call con_crlf
    jmp  .done
.perr:
    call fs_perror
.done:
    call fs_diag
    popad
    ret

; ---- mv <源> <目标>：移动 / 改名 ---------------------------------------------
cmd_do_mv:
    pushad
    mov  dword [fs_cmd_name], msg_fs_mv
    mov  dword [fs_step], 70
    call fs_ensure
    test eax, eax
    jz   .done
    call fs_split_two
    test eax, eax
    jz   .usage
    call fs_rename
    test eax, eax
    jz   .perr
    jmp  .done
.usage:
    mov  esi, msg_fs_mv_usage
    call con_puts
    call con_crlf
    jmp  .done
.perr:
    call fs_perror
.done:
    call fs_diag
    popad
    ret

; ---- df：超块信息 ------------------------------------------------------------
;  MeFS 一行 + ext2 一行（盘挂上才打；ext2 只读，free 恒为挂载时的值）。
cmd_do_df:
    pushad
    mov  dword [fs_cmd_name], msg_fs_df
    call fs_ensure
    test eax, eax
    jz   .done
    mov  esi, msg_fs_df_a
    call con_puts
    mov  eax, [FS_RAM_BASE + 0x0C]      ; 总块数
    call putdec
    mov  esi, msg_fs_df_b
    call con_puts
    mov  eax, [FS_RAM_BASE + 0x08]      ; 块大小
    call putdec
    mov  esi, msg_fs_df_c
    call con_puts
    mov  eax, [FS_RAM_BASE + 0x10]      ; 空闲块数
    call putdec
    mov  esi, msg_fs_df_d
    call con_puts
    mov  eax, [FS_RAM_BASE + 0x14]      ; 根目录块
    call putdec
    call con_crlf
    cmp  dword [DIAG_ADDR + D_EXT2_STAGE], 3
    jb   .done                          ; ext2 没挂上就不打这行
    mov  esi, msg_fs_df_e
    call con_puts
    mov  eax, [ext2_sb_blocks]
    call putdec
    mov  esi, msg_fs_df_f
    call con_puts
    mov  eax, [DIAG_ADDR + D_EXT2_FREE]
    call putdec
    mov  esi, msg_fs_df_g
    call con_puts
    call con_crlf
.done:
    call fs_diag
    popad
    ret

; ============================================================================
;  fs 全局数据（fs.inc 的所有例程共享；定义得放在最后，代码在前）
; ============================================================================
fs_ready      dd 0            ; 1 = MeFS 挂载好了
fs_cur_dir    dd 0            ; 当前目录块
fs_cur_rec    dd 0            ; 当前目录在父目录里的记录地址（根为 0）
fs_err        dd 0            ; 最近一次错误码（FS_*）
fs_cmd_name   dd 0            ; fs_perror 打印错误时用的命令名前缀
fs_dirent_addr dd 0           ; fs_dir_find / fs_dir_next 命中时的记录地址

cwd_buf       times 64 db 0   ; 当前目录（绝对路径，ASCII，0 结尾）
path_buf      times 256 db 0  ; 规范化后的绝对路径
name_buf      times 40 db 0   ; 路径解析用的组件缓冲
name2_buf     times 40 db 0   ; fs_rename 存旧名字用
fs_rec_buf    times DE_LEN db 0   ; 打包记录用

fs_ent_kind     db 0
fs_ent_name_len db 0
fs_ent_name     times 40 db 0
fs_ent_first    dd 0
fs_ent_size     dd 0

fs_tmp0    dd 0
fs_tmp1    dd 0
fs_tmp2    dd 0
fs_tmp3    dd 0
fs_tmp4    dd 0
fs_tmp5    dd 0

fs_stk_len dd 0
fs_stk_off times 32 dd 0     ; 前 16 个 dword = 组件偏移，后 16 个 = 组件长度

fs_iter_blk  dd 0
fs_iter_slot dd 0
fs_path_pos  dd 0            ; fs_action 存原始路径指针用

fs_step      dd 0            ; [调试] 最近一次 fs 操作步骤号
fs_cr_kind   db 0
fs_del_addr  dd 0
fs_del_kind  db 0
fs_del_first dd 0

fs_cp_src       dd 0
fs_cp_dst       dd 0
fs_cp_first     dd 0
fs_cp_size      dd 0
fs_cp_new_first dd 0
fs_cp_new_last  dd 0
fs_cp_cur       dd 0
fs_cp_blk       dd 0

fs_mv_src   dd 0
fs_mv_dst   dd 0
fs_mv_pdir  dd 0
fs_mv_addr  dd 0
fs_mv_kind  db 0
fs_mv_first dd 0
fs_mv_size  dd 0

; ---- 错误信息表：下标 = 错误码（0 号不会打印）-------------------------------
fs_err_msgs:
    dd msg_e_0
    dd msg_e_noent
    dd msg_e_notdir
    dd msg_e_isdir
    dd msg_e_exist
    dd msg_e_nospace
    dd msg_e_namelen
    dd msg_e_notempty
    dd msg_e_badpath
    dd msg_e_io
    dd msg_e_notsupp

msg_e_0        db "ok", 0               ; 占位
msg_e_noent    db "no such file or directory", 0
msg_e_notdir   db "not a directory", 0
msg_e_isdir    db "is a directory", 0
msg_e_exist    db "already exists", 0
msg_e_nospace  db "no space left on device", 0
msg_e_namelen  db "name too long", 0
msg_e_notempty db "directory not empty", 0
msg_e_badpath  db "invalid path", 0
msg_e_io       db "i/o error", 0
msg_e_notsupp  db "not supported", 0

msg_fs_notready  db "filesystem not mounted", 0
msg_fs_pwd       db "pwd", 0
msg_fs_ls        db "ls", 0
msg_fs_cd        db "cd", 0
msg_fs_cat       db "cat", 0
msg_fs_touch     db "touch", 0
msg_fs_mkdir     db "mkdir", 0
msg_fs_rm        db "rm", 0
msg_fs_cp        db "cp", 0
msg_fs_mv        db "mv", 0
msg_fs_df        db "df", 0
msg_fs_cat_usage db "usage: cat <path>", 0
msg_fs_rm_usage  db "usage: rm <path>", 0
msg_fs_cp_usage  db "usage: cp <src> <dst>", 0
msg_fs_mv_usage  db "usage: mv <src> <dst>", 0
msg_fs_df_a db "MeFS ramdisk: ", 0
msg_fs_df_b db " blocks x ", 0
msg_fs_df_c db "B, ", 0
msg_fs_df_d db " free, root block ", 0
msg_fs_df_e db "ext2 hard disk: ", 0
msg_fs_df_f db " blocks x 1024B, ", 0
msg_fs_df_g db " free", 0

fs_ls_fd    dd 0                 ; ls 枚举期间攥着的目录句柄

; fs_copy_to_ext2 工作区（FS4）
FS_CP_CAP    equ 4096            ; v1：cp 进 ext2 的单文件上限
fs_ce_src    dd 0                ; 源参数指针
fs_ce_dst    dd 0                ; 目标串指针（搬运前）
fs_ce_len    dd 0                ; 字节数
fs_ce_fd     dd 0                ; 目标 fd
fs_ce_buf    times 64 db 0       ; 目标路径串的私人拷贝
fs_cp_buf    times FS_CP_CAP db 0 ; 拷贝中转缓冲
