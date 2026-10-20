; ============================================================================
;  ext2.inc —— Linux ext2（rev 0、1024B 块、单组）只读驱动（FS3）
; ============================================================================
;  磁上格式完全按 e2fsprogs 的 rev0 语义解析，镜像由 tools/mkext2.py 生成、
;  tools/ext2check.py 独立复核（互盲）。本驱动是 VFS 的第 2 行驱动，
;  挂在 /hd。写路径 FS4 再来（ext2_write 现在是 FS_NOTSUPP 桩）。
;
;  挂载前提：ata_init 已跑（ata_present=1）。全部例程沿用仓库约定：
;  pushad 保护、结果走 EAX、失败写 fs_err 再返回 0。
;
;  v1 支持面（越界一律 FS_NOTSUPP，别静默）：
;    · 块 1024B（s_log_block_size != 0 拒挂）
;    · 单组（组号 > 0 拒）
;    · 直接块 0..11 + 一级间接 12..267（文件上限 268 块 = 274432 B）
;    · 目录只走直接块（1 块 = 1024 B，够 8 MB 盘用）
;    · 类型只认 reg(0x8)/dir(0x4)，symlink 等报 FS_NOTSUPP
; ============================================================================

; ---- 常量 -------------------------------------------------------------------
EXT2_ROOT_INO   equ 2            ; 根 inode 固定 2
EXT2_MAGIC      equ 0xEF53
EXT2_INODE_SZ   equ 128          ; rev0 定长

; 超块字段偏移（超块在 s_first_data_block 块起，bs=1024 时 = 块 1）
SB_INODES       equ 0x00         ; s_inodes_count
SB_BLOCKS       equ 0x04         ; s_blocks_count（块号上界）
SB_FREE_BLOCKS  equ 0x0C         ; s_free_blocks_count
SB_FREE_INODES  equ 0x10         ; s_free_inodes_count
SB_FIRST_DATA   equ 0x14         ; s_first_data_block（1024B 块恒 1）
SB_LOG_BS       equ 0x18         ; s_log_block_size（v1 只认 0）
SB_BLOCKS_GRP   equ 0x20         ; s_blocks_per_group
SB_INODES_GRP   equ 0x28         ; s_inodes_per_group
SB_MAGIC        equ 0x38         ; u16
SB_STATE        equ 0x3A         ; u16 1=clean
SB_REV          equ 0x4C         ; s_rev_level（v1 只认 0）
SB_INCOMPAT     equ 0x60         ; bit1 = FILETYPE（本盘为 0，类型必须查 inode）

; 组描述符（GDT 在 first_data_block+1 块，单组取第 0 项 32B）
GD_BLOCK_BMP    equ 0x00
GD_INODE_BMP    equ 0x04
GD_INODE_TAB    equ 0x08

; inode 字段
I_MODE          equ 0x00         ; u16，高 4 位：0x8=reg 0x4=dir
I_UID           equ 0x02         ; u16（不动它，也别往这里写东西）
I_SIZE          equ 0x04         ; u32 i_size_lo —— ext2 rev0 的真偏移，0x08 是 atime
I_DTIME         equ 0x14         ; u32 i_dtime（删除时间；占用中的 inode 必须为 0）
I_LINKS         equ 0x1A         ; u16 i_links_count —— 真偏移（0x18 是 gid）
I_BLOCKS        equ 0x1C         ; u32（512B 扇区计）
I_BLOCK         equ 0x28         ; i_block[15]：0..11 直接，12 一级间接
; 布局教训（踩坑 28 补记）：rev0 inode 是 mode/uid/size/atime/ctime/mtime/dtime/
; gid/links/blocks/flags/block——曾按错误记忆把 size 挪到 0x08、links 挪到 0x02
; （那是 atime/uid 的地盘），e2fsck 报"links=0 的已删 inode"才暴露；标准核对
; 必须对着真实工具做，别凭记忆。

; 目录条目（变长；rec_len 4 对齐、>=8；inode=0 = 空洞）
ED2_INO          equ 0x00         ; u32
ED2_REC          equ 0x04         ; u16 —— movzx！惯犯字段
ED2_NAMELEN      equ 0x06         ; u8
DE_TYPE         equ 0x07         ; u8（FILETYPE 没开时不可信，一律查 inode）
ED2_NAME         equ 0x08

; readdir 输出记录（40B）的布局常量 VRD_* 定义在 fs.inc（它先被 include，
; EQU 必须先到先用），MeFS / devfs / ext2 三个驱动同构。

E2_MAX_DIRECT   equ 12           ; 直接块数
E2_MAX_L1       equ 268          ; 直接 + 一级间接（12 + 256）

; 组描述符的空闲计数（写路径要同步它们）
GD_FREE_BLOCKS  equ 0x0C         ; u16
GD_FREE_INODES  equ 0x0E         ; u16

; 单组镜像的位图块号（mkext2.py 固定布局，GD 同款）
E2_BBMP         equ 3            ; 块位图 = 块 3
E2_IBM          equ 4            ; inode 位图 = 块 4

; ============================================================================
;  ext2_init：挂载（ata_init 之后、con_init 之前调用；失败软着陆）
;  D_EXT2_STAGE：0=未动 1=超块读过 2=魔数OK 3=GD/inode表定位
;  之后的 4=目录列得动（readdir 成功时升）、5=文件读通（read 升）
; ============================================================================
ext2_init:
    pushad
    cmp  dword [ata_present], 0
    je   .done                       ; 没盘：什么都不写，留给 disk 命令报
    ; 超块 = 块 1 = 扇区 2..3。直接走 ata（ext2_read_block 的块号上界
    ; 依赖 s_blocks_count，此刻还没缓存——先有鸡还是先有蛋）。
    mov  eax, 2
    mov  ecx, 2
    mov  edi, ext2_blk_buf
    call ata_read_sectors
    test eax, eax
    jz   .done
    mov  dword [DIAG_ADDR + D_EXT2_STAGE], 1
    movzx eax, word [ext2_blk_buf + SB_MAGIC]
    cmp  eax, EXT2_MAGIC
    jne  .done                       ; 不是 ext2：软着陆，不挡启动
    ; 只认 v1 支持面：log_bs=0、rev=0
    cmp  dword [ext2_blk_buf + SB_LOG_BS], 0
    jne  .done
    cmp  dword [ext2_blk_buf + SB_REV], 0
    jne  .done
    mov  dword [DIAG_ADDR + D_EXT2_STAGE], 2
    ; 缓存超块要紧字段（sb_blocks 立即生效：此后 ext2_read_block 有上界可查）
    mov  eax, [ext2_blk_buf + SB_BLOCKS]
    mov  [ext2_sb_blocks], eax
    mov  eax, [ext2_blk_buf + SB_FREE_BLOCKS]
    mov  [ext2_sb_free], eax
    mov  [DIAG_ADDR + D_EXT2_FREE], eax
    mov  eax, [ext2_blk_buf + SB_FREE_INODES]
    mov  [ext2_sb_free_ino], eax
    mov  eax, [ext2_blk_buf + SB_INODES_GRP]
    mov  [ext2_sb_ipg], eax
    mov  eax, [ext2_blk_buf + SB_FIRST_DATA]
    mov  [ext2_first_data], eax
    ; 读组描述符（GDT 块 = first_data + 1 = 2）
    mov  eax, [ext2_first_data]
    inc  eax
    call ext2_read_block
    test eax, eax
    jz   .done
    mov  eax, [ext2_blk_buf + GD_INODE_TAB]
    mov  [ext2_gd_inotab], eax
    ; 根 inode 现场验一遍（mode 得是目录），坏了就不挂
    mov  eax, EXT2_ROOT_INO
    call ext2_read_inode
    test eax, eax
    jz   .done
    movzx eax, word [ext2_ino_buf + I_MODE]
    shr  eax, 12
    cmp  eax, 4
    jne  .done
    mov  dword [ext2_ready], 1
    mov  dword [DIAG_ADDR + D_EXT2_STAGE], 3
    mov  dword [DIAG_ADDR + D_EXT2_ERR], 0
.done:
    popad
    ret

; ---- ext2_read_block：EAX = 块号 → ext2_blk_buf，EAX = 1/0 -------------------
;  块号先对 s_blocks_count 做上界检查（坏 inode 的 i_block 值会拿去当块号用；
;  挂载前 sb_blocks=0，任何块号都过不了这道闸，init 里读超块/因此直走 ATA）。
ext2_read_block:
    pushad
    cmp  eax, [ext2_sb_blocks]
    jae  .supp
    mov  [DIAG_ADDR + D_EXT2_BLK], eax
    ; LBA = 块号 << 1（1024B 块 = 2 扇区）
    shl  eax, 1
    mov  ecx, 2
    mov  edi, ext2_blk_buf
    call ata_read_sectors
    test eax, eax
    jz   .io
    mov  dword [esp + 28], 1
    popad
    ret
.supp:
    mov  dword [fs_err], FS_NOTSUPP
    mov  dword [DIAG_ADDR + D_EXT2_ERR], FS_NOTSUPP
    mov  dword [esp + 28], 0
    popad
    ret
.io:
    mov  dword [fs_err], FS_IO
    mov  dword [DIAG_ADDR + D_EXT2_ERR], FS_IO
    mov  dword [esp + 28], 0
    popad
    ret

; ---- ext2_read_inode：EAX = inode 号 → ext2_ino_buf，EAX = 1/0 ---------------
;  一条缓存（ext2_ino_last）：cat 分段读同一文件时第二趟免读盘。
;  组 = (ino-1)/ipg（v1 单组，组号非 0 拒）；表内偏移 = 组内序 * 128。
ext2_read_inode:
    pushad
    test eax, eax
    jz   .bad                       ; inode 0 不存在
    mov  [ext2_rd_ino], eax         ; 入参存全局（后面要用它标缓存）
    cmp  eax, [ext2_ino_last]
    je   .hit
    dec  eax                        ; ino-1
    xor  edx, edx
    div  dword [ext2_sb_ipg]        ; EAX = 组号，EDX = 组内序
    test eax, eax
    jnz  .supp
    shl  edx, 7                     ; 表内偏移 = 序 * 128
    mov  [ext2_off], edx
    ; 块号 = gd_inotab + (off >> 10)；块内 = off & 0x3FF（先掩再加，别反过来）
    mov  eax, edx
    shr  eax, 10
    add  eax, [ext2_gd_inotab]
    call ext2_read_block             ; inode 所在块进 blk_buf
    test eax, eax
    jz   .io
    mov  esi, [ext2_off]
    and  esi, 0x3FF
    add  esi, ext2_blk_buf           ; ESI = 块内 inode 起点
    mov  edi, ext2_ino_buf
    mov  ecx, EXT2_INODE_SZ / 4
    cld
    rep  movsd                       ; 128B inode 落进 ino_buf
    mov  eax, [ext2_rd_ino]
    mov  [ext2_ino_last], eax
.hit:
    mov  dword [esp + 28], 1
    popad
    ret
.bad:
    mov  dword [fs_err], FS_IO
    mov  dword [DIAG_ADDR + D_EXT2_ERR], FS_IO
    mov  dword [esp + 28], 0
    popad
    ret
.supp:
    mov  dword [fs_err], FS_NOTSUPP
    mov  dword [DIAG_ADDR + D_EXT2_ERR], FS_NOTSUPP
    mov  dword [esp + 28], 0
    popad
    ret
.io:
    mov  dword [fs_err], FS_IO
    mov  dword [DIAG_ADDR + D_EXT2_ERR], FS_IO
    mov  dword [esp + 28], 0
    popad
    ret

; ---- ext2_lookup：EAX = 起点 ino，ESI = 相对路径 → EAX = 目标 ino -----------
;  逐组件走目录。".." 照走——ext2 目录里这两个条目真实存在，指向父/自身。
ext2_lookup:
    pushad
    mov  [e2lk_ino], eax
    mov  [e2lk_path], esi
.next_comp:
    mov  esi, [e2lk_path]
    cmp  byte [esi], '/'
    jne  .no_slash
    inc  esi                           ; 容忍前导斜杠（/hd 剥前缀后是 "/x"）
.no_slash:
    mov  [e2lk_path], esi
    lea  edi, [e2lk_name]
    xor  ecx, ecx                      ; 组件长度
.copy:
    mov  al, [esi]
    test al, al
    jz   .copied
    cmp  al, '/'
    je   .copied
    inc  esi
    stosb
    inc  ecx
    cmp  ecx, VRD_NAMEMAX + 1
    ja   .namelen
    jmp  .copy
.copied:
    mov  byte [edi], 0
    mov  [e2lk_path], esi
    test ecx, ecx
    jz   .check_end                    ; 空组件（尾斜杠/连斜杠）：跳过
    mov  [e2lk_nlen], ecx
    mov  eax, [e2lk_ino]
    call ext2_find_dirent              ; e2lk_name 里找 → EAX = ino
    test eax, eax
    jz   .fail                         ; fs_err 已设（FS_NOENT / FS_NOTDIR）
    mov  [e2lk_ino], eax
.check_end:
    mov  esi, [e2lk_path]            ; 解引用看指向的字符（[e2lk_path] 直接取
    cmp  byte [esi], 0               ; 的是指针低字节，永远非 0 → 死循环）
    jne  .next_comp
    mov  eax, [e2lk_ino]
    mov  [esp + 28], eax
    popad
    ret
.namelen:
    mov  dword [fs_err], FS_NAMELEN
    mov  dword [DIAG_ADDR + D_EXT2_ERR], FS_NAMELEN
    mov  dword [esp + 28], 0
    popad
    ret
.fail:
    mov  dword [esp + 28], 0
    popad
    ret

; ---- ext2_find_dirent：EAX = 目录 ino，e2lk_name/nlen → EAX = 条目 ino ------
;  只扫直接块。名字比对逐字节（避开 str_eq 的寄存器约定纠缠）。
ext2_find_dirent:
    pushad
    call ext2_read_inode
    test eax, eax
    jz   .out
    movzx eax, word [ext2_ino_buf + I_MODE]
    shr  eax, 12
    cmp  eax, 4
    jne  .notdir
    mov  eax, [ext2_ino_buf + I_SIZE]  ; 目录字节界（别信块里残留）
    mov  [e2f_size], eax
    mov  dword [e2f_pos], 0
.loop:
    mov  eax, [e2f_pos]
    cmp  eax, [e2f_size]
    jae  .noent                        ; 目录走完
    ; 块号 = i_block[pos>>10]，v1 目录只走直接块
    mov  edx, eax
    shr  edx, 10
    cmp  edx, E2_MAX_DIRECT
    jae  .supp
    mov  eax, [ext2_ino_buf + I_BLOCK + edx * 4]
    test eax, eax
    jz   .noent                        ; 目录链提前断（e2fsck 的事）
    call ext2_read_block               ; 块进 blk_buf（inode 已在 ino_buf，安全）
    test eax, eax
    jz   .out
    mov  esi, ext2_blk_buf
    mov  edx, [e2f_pos]
    and  edx, 0x3FF
    add  esi, edx                      ; ESI = 条目地址
    ; 解条目头（rec_len 是 word：movzx）
    mov  eax, [esi + ED2_INO]
    mov  [e2f_ino], eax
    movzx ecx, word [esi + ED2_REC]
    cmp  ecx, 8
    jb   .corrupt
    test ecx, 3
    jnz  .corrupt
    mov  [e2f_rec], ecx
    movzx ecx, byte [esi + ED2_NAMELEN]
    mov  [e2f_nlen], ecx
    ; 推进游标（存全局——后面读 inode 会搅 blk_buf）
    mov  edx, [e2f_pos]
    add  edx, [e2f_rec]
    mov  [e2f_pos], edx
    cmp  dword [e2f_ino], 0
    je   .loop                         ; 空洞：跳过整个 rec_len
    ; 名字长度先比（长度不等直接下一条）
    mov  eax, [e2lk_nlen]
    cmp  eax, [e2f_nlen]
    jne  .loop
    lea  edi, [e2lk_name]
    mov  edx, [e2f_pos]
    sub  edx, [e2f_rec]                ; 回到本条目起点
    and  edx, 0x3FF
    lea  esi, [ext2_blk_buf + edx + ED2_NAME]
    xor  ecx, ecx
.cmp:
    mov  al, [esi + ecx]
    cmp  al, [edi + ecx]
    jne  .loop
    inc  ecx
    cmp  ecx, [e2lk_nlen]
    jb   .cmp
    mov  eax, [e2f_ino]
    mov  [esp + 28], eax
    popad
    ret
.noent:
    mov  dword [fs_err], FS_NOENT
    jmp  .fail
.notdir:
    mov  dword [fs_err], FS_NOTDIR
    jmp  .fail
.supp:
    mov  dword [fs_err], FS_NOTSUPP
    jmp  .fail
.corrupt:
    mov  dword [fs_err], FS_IO         ; rec_len 不合规：盘坏了
.fail:
    mov  eax, [fs_err]
    mov  [DIAG_ADDR + D_EXT2_ERR], eax
    mov  dword [esp + 28], 0
    popad
    ret
.out:
    mov  dword [esp + 28], 0
    popad
    ret

; ============================================================================
;  VFS 驱动接口（第 2 行：ext2_open / read / write / close / readdir）
; ============================================================================

; ---- ext2_open：ESI = 相对路径（"" = 挂载点根），EDI = vnode 出 --------------
ext2_open:
    pushad
    cmp  dword [ext2_ready], 0
    je   .notrdy
    inc  dword [DIAG_ADDR + D_EXT2_CMD]
    cmp  byte [esi], 0
    jne  .lookup
    mov  dword [ext2_cur_ino], EXT2_ROOT_INO
    jmp  .have_ino
.lookup:
    mov  eax, EXT2_ROOT_INO
    call ext2_lookup
    test eax, eax
    jz   .fail                          ; fs_err 已设
    mov  [ext2_cur_ino], eax
.have_ino:
    mov  eax, [ext2_cur_ino]
    call ext2_read_inode
    test eax, eax
    jz   .fail
    mov  eax, [ext2_cur_ino]
    mov  [DIAG_ADDR + D_EXT2_INO], eax
    movzx eax, word [ext2_ino_buf + I_MODE]
    shr  eax, 12
    cmp  eax, 8
    je   .kind_file
    cmp  eax, 4
    je   .kind_dir
    jmp  .supp                          ; symlink 等不支持
.kind_file:
    mov  byte [edi + VN_KIND], VN_FILE
    jmp  .kind_done
.kind_dir:
    mov  byte [edi + VN_KIND], VN_DIR
.kind_done:
    mov  eax, [ext2_cur_ino]
    mov  [edi + VN_A], eax
    mov  dword [edi + VN_B], 0
    mov  eax, [ext2_ino_buf + I_SIZE]
    mov  [edi + VN_SIZE], eax
    mov  dword [esp + 28], 1
    popad
    ret
.notrdy:
    mov  dword [fs_err], FS_IO
    jmp  .fail
.supp:
    mov  dword [fs_err], FS_NOTSUPP
.fail:
    mov  eax, [fs_err]
    mov  [DIAG_ADDR + D_EXT2_ERR], eax
    mov  dword [esp + 28], 0
    popad
    ret

; ---- ext2_read：ESI = fd 槽，EDI = 缓冲，ECX = 长度 → EAX = 字节数 -----------
;  FD_POS 是字节游标。块号 = pos>>10；<12 直接、12..267 一级间接、>267 拒。
ext2_read:
    pushad
    cmp  dword [ext2_ready], 0
    je   .notrdy
    inc  dword [DIAG_ADDR + D_EXT2_CMD]
    mov  [e2rd_fd], esi
    mov  [e2rd_dst], edi
    mov  [e2rd_need], 0               ; 本次要读的字节数
    mov  dword [e2rd_got], 0
    ; 读 inode（从 vnode.a 拿 ino——别依赖 ino_last 缓存的语义归属）
    mov  eax, [esi + FD_VN + VN_A]
    mov  [ext2_cur_ino], eax
    call ext2_read_inode
    test eax, eax
    jz   .fail
    ; need = min(size - pos, 长度)
    mov  esi, [e2rd_fd]
    mov  eax, [esi + FD_VN + VN_SIZE]
    sub  eax, [esi + FD_POS]
    jbe  .eof                           ; pos >= size（0 = EOF）
    mov  [e2rd_need], eax
    cmp  eax, ecx
    jbe  .have_need
    mov  eax, ecx
    mov  [e2rd_need], eax
.have_need:
.chunk:
    mov  dword [e2rd_left], 0           ; 本块拷贝量
    mov  eax, [e2rd_need]
    test eax, eax
    jz   .finish
    mov  esi, [e2rd_fd]
    mov  edx, [esi + FD_POS]
    mov  [e2rd_pos], edx
    ; idx = pos>>10，块内 = pos & 0x3FF
    mov  eax, edx
    shr  eax, 10
    mov  [e2rd_idx], eax
    and  edx, 0x3FF
    mov  [e2rd_boff], edx
    cmp  eax, E2_MAX_L1
    jae  .supp
    cmp  eax, E2_MAX_DIRECT
    jb   .direct
    ; 一级间接：i_block[12] 指的间接块读进 ind_buf，再取第 idx-12 项
    mov  eax, [ext2_ino_buf + I_BLOCK + 12 * 4]
    shl  eax, 1
    mov  ecx, 2
    mov  edi, ext2_ind_buf
    call ata_read_sectors
    test eax, eax
    jz   .io
    mov  eax, [e2rd_idx]
    sub  eax, E2_MAX_DIRECT
    mov  eax, [ext2_ind_buf + eax * 4]
    jmp  .have_blk
.direct:
    mov  eax, [e2rd_idx]
    mov  eax, [ext2_ino_buf + I_BLOCK + eax * 4]
.have_blk:
    test eax, eax
    jz   .trunc                          ; 稀疏/截断：已读部分照给
    call ext2_read_block                ; 数据块进 blk_buf（EAX 进 = 块号，出 = 1/0）
    test eax, eax
    jz   .io
    ; chunk = min(1024 - boff, need)
    mov  ecx, 1024
    sub  ecx, [e2rd_boff]
    mov  eax, [e2rd_need]
    cmp  eax, ecx
    ja   .have_chunk          ; need > 块内剩余 → 整段给块内剩余（ecx 已是）
    mov  ecx, [e2rd_need]     ; need <= 块内剩余 → 只给 need
.have_chunk:
    mov  [e2rd_left], ecx
    mov  esi, ext2_blk_buf
    add  esi, [e2rd_boff]
    mov  edi, [e2rd_dst]
    cld
    push ecx                             ; rep 会清 ECX——踩坑 17
    rep  movsb
    pop  ecx
    mov  [e2rd_dst], edi
    mov  eax, [e2rd_pos]
    add  eax, ecx
    mov  esi, [e2rd_fd]
    mov  [esi + FD_POS], eax
    mov  eax, [e2rd_got]
    add  eax, ecx
    mov  [e2rd_got], eax
    mov  eax, [e2rd_need]
    sub  eax, ecx
    mov  [e2rd_need], eax
    jmp  .chunk
.finish:
    mov  eax, [e2rd_got]
    test eax, eax
    jz   .eof
    cmp  dword [DIAG_ADDR + D_EXT2_STAGE], 5
    jae  .eof
    mov  dword [DIAG_ADDR + D_EXT2_STAGE], 5   ; 文件真读通了
    jmp  .eof
.notrdy:
    mov  dword [fs_err], FS_IO
    jmp  .fail
.supp:
    mov  dword [fs_err], FS_NOTSUPP
    mov  dword [e2rd_got], 0
    jmp  .fail
.io:
    mov  dword [fs_err], FS_IO
    mov  dword [e2rd_got], 0
    jmp  .fail
.trunc:
    jmp  .finish
.eof:
    mov  eax, [e2rd_got]
    mov  [esp + 28], eax
    popad
    ret
.fail:
    mov  eax, [fs_err]
    mov  [DIAG_ADDR + D_EXT2_ERR], eax
    mov  eax, [e2rd_got]
    mov  [esp + 28], eax
    popad
    ret

; ---- ext2_readdir：ESI = fd 槽，EDI = 40B 记录 → EAX = 1/0 -------------------
;  FD_POS 当目录字节游标。"." / ".." 是 ext2 的磁上产物，这里吞掉——
;  MeFS 适配层不产它们，ls 的语义两边应当一致。
ext2_readdir:
    pushad
    cmp  dword [ext2_ready], 0
    je   .notrdy
    inc  dword [DIAG_ADDR + D_EXT2_CMD]
    mov  [e2rl_fd], esi
    mov  [e2rl_rec], edi
    mov  eax, [esi + FD_VN + VN_A]
    mov  [ext2_cur_ino], eax
    call ext2_read_inode
    test eax, eax
    jz   .fail
    mov  esi, [e2rl_fd]
    mov  eax, [esi + FD_VN + VN_SIZE]
    mov  [e2rl_size], eax
.loop:
    mov  esi, [e2rl_fd]
    mov  eax, [esi + FD_POS]
    cmp  eax, [e2rl_size]
    jae  .end
    mov  [e2rl_pos], eax
    mov  edx, eax
    shr  edx, 10
    cmp  edx, E2_MAX_DIRECT
    jae  .supp                            ; v1 目录只走直接块
    mov  eax, [ext2_ino_buf + I_BLOCK + edx * 4]
    test eax, eax
    jz   .end
    call ext2_read_block                  ; 目录块进 blk_buf
    test eax, eax
    jz   .io
    mov  esi, ext2_blk_buf
    mov  edx, [e2rl_pos]
    and  edx, 0x3FF
    add  esi, edx                         ; ESI = 条目地址
    mov  eax, [esi + ED2_INO]
    mov  [e2rl_ino], eax
    movzx ecx, word [esi + ED2_REC]
    cmp  ecx, 8
    jb   .corrupt
    test ecx, 3
    jnz  .corrupt
    mov  [e2rl_rec_len], ecx
    movzx ecx, byte [esi + ED2_NAMELEN]
    mov  [e2rl_nlen], ecx
    ; 先推游标（之后读 inode 会搅 blk_buf）
    mov  edx, [e2rl_pos]
    add  edx, [e2rl_rec_len]
    mov  esi, [e2rl_fd]
    mov  [esi + FD_POS], edx
    cmp  dword [e2rl_ino], 0
    je   .loop                            ; 空洞
    ; "." / ".." 吞掉（名字基址 = 块内偏移 + 8，两处都要重算——blk_buf 会被动）
    mov  edx, [e2rl_pos]
    and  edx, 0x3FF
    mov  ecx, [e2rl_nlen]
    cmp  ecx, 1
    jne  .not_dot
    cmp  byte [ext2_blk_buf + edx + ED2_NAME], '.'
    je   .loop                            ; "."
.not_dot:
    cmp  ecx, 2
    jne  .not_dotdot
    cmp  byte [ext2_blk_buf + edx + ED2_NAME], '.'
    jne  .not_dotdot
    cmp  byte [ext2_blk_buf + edx + ED2_NAME + 1], '.'
    je   .loop                            ; ".."
.not_dotdot:
    ; 名字超长 → FS_NAMELEN（MeOS 缓冲 32B）
    cmp  ecx, VRD_NAMEMAX + 1
    ja   .namelen
    ; 拷名字进记录（此时 blk_buf 还是目录块）
    lea  esi, [ext2_blk_buf + edx + ED2_NAME]
    mov  edi, [e2rl_rec]
    add  edi, VRD_NAME
    cld
    push ecx                              ; rep 清 ECX（踩坑 17）
    rep  movsb
    pop  ecx
    mov  edi, [e2rl_rec]
    mov  byte [edi + VRD_NAME + ecx], 0   ; 结尾 0
    ; 读条目 inode 定类型/大小（这步会搅 blk_buf，名字已拷走）
    mov  eax, [e2rl_ino]
    mov  [ext2_cur_ino], eax
    call ext2_read_inode
    test eax, eax
    jz   .fail
    mov  eax, [e2rl_ino]
    mov  [DIAG_ADDR + D_EXT2_INO], eax
    movzx eax, word [ext2_ino_buf + I_MODE]
    shr  eax, 12
    cmp  eax, 8
    je   .k_file
    cmp  eax, 4
    je   .k_dir
    jmp  .supp
.k_file:
    mov  edi, [e2rl_rec]
    mov  byte [edi + VRD_KIND], VN_FILE
    jmp  .k_done
.k_dir:
    mov  edi, [e2rl_rec]
    mov  byte [edi + VRD_KIND], VN_DIR
.k_done:
    mov  edi, [e2rl_rec]
    mov  eax, [ext2_ino_buf + I_SIZE]
    mov  [edi + VRD_SIZE], eax
    cmp  dword [DIAG_ADDR + D_EXT2_STAGE], 4
    jae  .ok
    mov  dword [DIAG_ADDR + D_EXT2_STAGE], 4   ; 目录列得动
.ok:
    mov  dword [esp + 28], 1
    popad
    ret
.end:
    mov  dword [esp + 28], 0
    popad
    ret
.notrdy:
    mov  dword [fs_err], FS_IO
    jmp  .fail
.namelen:
    mov  dword [fs_err], FS_NAMELEN
    jmp  .fail
.supp:
    mov  dword [fs_err], FS_NOTSUPP
    jmp  .fail
.corrupt:
    mov  dword [fs_err], FS_IO
    jmp  .fail
.io:
    mov  dword [fs_err], FS_IO
.fail:
    mov  eax, [fs_err]
    mov  [DIAG_ADDR + D_EXT2_ERR], eax
    mov  dword [esp + 28], 0
    popad
    ret

; ---- ext2_sync_super：把内存里的空闲计数写回超块和组描述符 -------------------
;  位图块在分配当口就落盘；计数集中在这里收尾（崩溃最坏丢计数，e2fsck 能修）
ext2_sync_super:
    pushad
    mov  eax, 1
    call ext2_read_block                ; 超块 = 块 1
    test eax, eax
    jz   .io
    mov  eax, [ext2_sb_free]
    mov  [ext2_blk_buf + SB_FREE_BLOCKS], eax
    mov  eax, [ext2_sb_free_ino]
    mov  [ext2_blk_buf + SB_FREE_INODES], eax
    mov  eax, 2                         ; 块 1 的首扇区 LBA
    mov  ecx, 2
    mov  esi, ext2_blk_buf
    call ata_write_sectors
    test eax, eax
    jz   .io
    mov  eax, 2
    call ext2_read_block                ; 组描述符 = 块 2
    test eax, eax
    jz   .io
    mov  eax, [ext2_sb_free]
    mov  [ext2_blk_buf + GD_FREE_BLOCKS], ax
    mov  eax, [ext2_sb_free_ino]
    mov  [ext2_blk_buf + GD_FREE_INODES], ax
    mov  eax, 4                         ; 块 2 的首扇区 LBA
    mov  ecx, 2
    mov  esi, ext2_blk_buf
    call ata_write_sectors
    test eax, eax
    jz   .io
    mov  dword [esp + 28], 1
    popad
    ret
.io:
    mov  dword [fs_err], FS_IO
    jmp  .fail
.fail:
    mov  eax, [fs_err]
    mov  [DIAG_ADDR + D_EXT2_ERR], eax
    mov  dword [esp + 28], 0
    popad
    ret

; ---- ext2_bitmap_alloc：EAX = 位图块号，ECX = 起始位，ESI = 位上限 -----------
;  找 0 位、置 1、位图块写回盘 → EAX = 位号（-1 = 满/IO 错）
ext2_bitmap_alloc:
    pushad
    mov  [e2bm_blk], eax
    mov  [e2bm_bit], ecx
    mov  [e2bm_lim], esi
    call ext2_read_block
    test eax, eax
    jz   .io
.scan:
    mov  eax, [e2bm_bit]
    cmp  eax, [e2bm_lim]
    jae  .full
    mov  ebx, eax
    shr  ebx, 3                         ; 字节下标
    movzx edx, byte [ext2_blk_buf + ebx]
    and  eax, 7                         ; 位内下标
    bt   edx, eax                       ; CF = 该位（1 = 占用）
    jnc  .set                           ; 0 位 = 空位
    inc  dword [e2bm_bit]
    jmp  .scan
.set:
    ; 置位：byte |= 1 << (bit & 7)，位图块写回
    mov  eax, [e2bm_bit]
    mov  ebx, eax
    shr  ebx, 3
    mov  edx, 1
    mov  ecx, eax
    and  ecx, 7
    shl  edx, cl
    or   [ext2_blk_buf + ebx], dl
    mov  eax, [e2bm_blk]
    shl  eax, 1                         ; 块号 × 2 = LBA
    mov  ecx, 2
    mov  esi, ext2_blk_buf
    call ata_write_sectors
    test eax, eax
    jz   .io
    mov  eax, [e2bm_bit]
    mov  [esp + 28], eax
    popad
    ret
.full:
    mov  dword [fs_err], FS_NOSPACE
    jmp  .fail
.io:
    mov  dword [fs_err], FS_IO
.fail:
    mov  eax, [fs_err]
    mov  [DIAG_ADDR + D_EXT2_ERR], eax
    mov  dword [esp + 28], -1
    popad
    ret

; ---- ext2_alloc_block → EAX = 新块号（0 = 满了/IO 错）-----------------------
;  成功时 ext2_sb_free 已减一（D_EXT2_FREE 跟着走），落盘靠 sync_super
;  标准（first_data_block=1）：块位图 bit i ↔ 块 i+1，块 0（引导区）不占位图
;  ——位图扫出的是 bit，块号 = bit+1，跟 alloc_inode 的 +1 同一个道理
ext2_alloc_block:
    pushad
    mov  eax, E2_BBMP
    mov  ecx, 0
    mov  esi, [ext2_sb_blocks]          ; 位上限（bit 8191 = padding，置 1 不会被选中）
    call ext2_bitmap_alloc
    cmp  eax, -1
    je   .fail
    inc  eax                            ; bit → 块号（bit 42 = 块 43）
    mov  [e2bm_res], eax
    dec  dword [ext2_sb_free]
    mov  eax, [ext2_sb_free]
    mov  [DIAG_ADDR + D_EXT2_FREE], eax
    mov  eax, [e2bm_res]
    mov  [esp + 28], eax
    popad
    ret
.fail:
    mov  dword [esp + 28], 0
    popad
    ret

; ---- ext2_alloc_inode → EAX = 新 inode 号（0 = 满/IO 错）--------------------
;  位 i ↔ inode i+1；从位 11 起扫（inode 1..11 是保留的）
ext2_alloc_inode:
    pushad
    mov  eax, E2_IBM
    mov  ecx, 11
    mov  esi, [ext2_sb_ipg]             ; inodes_count = 位上限（单组）
    call ext2_bitmap_alloc
    cmp  eax, -1
    je   .fail
    inc  eax
    mov  [e2bm_res], eax
    dec  dword [ext2_sb_free_ino]
    mov  eax, [e2bm_res]
    mov  [esp + 28], eax
    popad
    ret
.fail:
    mov  dword [esp + 28], 0
    popad
    ret

; ---- ext2_write_inode：EAX = inode 号（ext2_ino_buf 里是最新内容）→ 1/0 -----
ext2_write_inode:
    pushad
    cld
    mov  [e2wi_ino], eax
    test eax, eax
    jz   .bad
    dec  eax
    xor  edx, edx
    div  dword [ext2_sb_ipg]            ; eax = 组号（必须 0），edx = 组内序号
    test eax, eax
    jnz  .supp
    mov  eax, edx
    shl  eax, 7                         ; ×128 = inode 表内偏移
    mov  [e2wi_off], eax
    mov  ebx, eax
    shr  ebx, 10                        ; 表内块序号
    add  ebx, [ext2_gd_inotab]
    mov  eax, ebx
    call ext2_read_block                ; blk_buf = inode 所在块
    test eax, eax
    jz   .io
    mov  edi, ext2_blk_buf
    mov  eax, [e2wi_off]
    and  eax, 0x3FF
    add  edi, eax                       ; 块内位置
    mov  esi, ext2_ino_buf
    mov  ecx, 32
    rep  movsd                          ; 128B 拷进去
    mov  ebx, [e2wi_off]
    shr  ebx, 10
    add  ebx, [ext2_gd_inotab]
    mov  eax, ebx
    shl  eax, 1                         ; LBA
    mov  ecx, 2
    mov  esi, ext2_blk_buf
    call ata_write_sectors
    test eax, eax
    jz   .io
    ; 缓存一致性：ino_buf 现在就是这个 inode 的最新版
    mov  eax, [e2wi_ino]
    mov  [ext2_ino_last], eax
    mov  dword [esp + 28], 1
    popad
    ret
.bad:
    mov  dword [fs_err], FS_BADPATH
    jmp  .fail
.supp:
    mov  dword [fs_err], FS_NOTSUPP
    jmp  .fail
.io:
    mov  dword [fs_err], FS_IO
.fail:
    mov  eax, [fs_err]
    mov  [DIAG_ADDR + D_EXT2_ERR], eax
    mov  dword [esp + 28], 0
    popad
    ret

; ---- ext2_dir_insert：EAX = 目录 ino，EBX = 新条目 ino ----------------------
;  e2ci_name / e2ci_nlen 预先备好。EAX = 1/0
;  算法：读末块，尾条目 rec_len 有富余就劈开；不够就给目录补新块
ext2_dir_insert:
    pushad
    cld
    mov  [e2ci_dir], eax
    mov  [e2ci_ino], ebx
    call ext2_read_inode
    test eax, eax
    jz   .fail
    mov  eax, [ext2_ino_buf + I_MODE]
    and  eax, 0xF000
    cmp  eax, 0x4000
    jne  .notdir
    ; need = 8 + align4(nlen + 1)
    mov  ecx, [e2ci_nlen]
    inc  ecx
    add  ecx, 3
    and  ecx, 0xFFFFFFFC
    add  ecx, 8
    mov  [e2ci_need], ecx
    ; 目录大小为 0 → 直接补块（目录至少一块）
    mov  eax, [ext2_ino_buf + I_SIZE]
    test eax, eax
    jz   .newblock
    ; 末块号 = i_block[(size-1) >> 10]
    dec  eax
    shr  eax, 10
    lea  esi, [ext2_ino_buf + I_BLOCK]
    mov  eax, [esi + eax * 4]
    mov  [e2ci_blk], eax
    call ext2_read_block
    test eax, eax
    jz   .fail
    ; 扫到尾条目（它的 rec_len 覆盖到块尾）
    xor  esi, esi                       ; 游标
.scan:
    cmp  esi, 1024
    jae  .corrupt
    movzx ecx, word [ext2_blk_buf + esi + ED2_REC]
    test ecx, ecx
    jz   .corrupt                       ; rec_len 0 = 坏
    lea  edx, [esi + ecx]
    cmp  edx, 1024
    jb   .adv
    jne  .corrupt                       ; 越过块尾 = 坏
    jmp  .have_tail                     ; 恰好 == 1024：esi 就是尾条目
.adv:
    add  esi, ecx                       ; 没到尾：推进游标再扫（漏了这句=死循环）
    jmp  .scan
.have_tail:
    ; esi = 尾条目，ecx = 其 rec_len。实际占用 = 8 + align4(nlen+1)
    movzx eax, byte [ext2_blk_buf + esi + ED2_NAMELEN]
    inc  eax
    add  eax, 3
    and  eax, 0xFFFFFFFC
    add  eax, 8                         ; tail_used
    mov  [e2bm_res], eax                ; 借个变量存
    sub  ecx, eax                       ; 富余
    cmp  ecx, [e2ci_need]
    jb   .newblock                      ; 富余不够 → 补块
    ; 劈开：尾条目 rec_len = tail_used，新条目接在后面吃掉全部剩余
    mov  edx, [e2bm_res]
    mov  [ext2_blk_buf + esi + ED2_REC], dx
    add  esi, edx                       ; 新条目位置
    mov  eax, [e2ci_ino]
    mov  [ext2_blk_buf + esi + ED2_INO], eax
    ; 新条目 rec_len = 富余全部（ecx 自 966 行 sub 后未动），
    ; 让块保持无缝覆盖到 1024——写成 need 会留无主洞，readdir 走到
    ; 洞里就是 rec<8 的 .corrupt（cat/ls 报 i/o error 的真凶）。
    mov  [ext2_blk_buf + esi + ED2_REC], cx
    mov  eax, [e2ci_nlen]
    mov  [ext2_blk_buf + esi + ED2_NAMELEN], al
    lea  edi, [ext2_blk_buf + esi + ED2_NAME]
    mov  esi, e2ci_name
    mov  ecx, [e2ci_nlen]
    rep  movsb
    jmp  .writeblk
.newblock:
    call ext2_alloc_block
    test eax, eax
    jz   .fail                           ; 满（fs_err 已设）
    mov  [e2ci_blk], eax
    ; 新块 = 一条 rec_len = 1024 的条目
    mov  edi, ext2_blk_buf
    mov  ecx, 256
    xor  eax, eax
    rep  stosd
    mov  eax, [e2ci_ino]
    mov  [ext2_blk_buf + ED2_INO], eax
    mov  word [ext2_blk_buf + ED2_REC], 1024
    mov  eax, [e2ci_nlen]
    mov  [ext2_blk_buf + ED2_NAMELEN], al
    mov  edi, ext2_blk_buf + ED2_NAME
    mov  esi, e2ci_name
    mov  ecx, [e2ci_nlen]
    rep  movsb
    ; 目录 inode：size += 1024，末块挂上，i_blocks += 2
    mov  eax, [ext2_ino_buf + I_SIZE]
    add  eax, 1024
    mov  [ext2_ino_buf + I_SIZE], eax
    shr  eax, 10
    dec  eax                            ; 新末块序号
    mov  ebx, [e2ci_blk]
    lea  esi, [ext2_ino_buf + I_BLOCK]
    mov  [esi + eax * 4], ebx
    add  dword [ext2_ino_buf + I_BLOCKS], 2
.writeblk:
    mov  eax, [e2ci_blk]
    shl  eax, 1
    mov  ecx, 2
    mov  esi, ext2_blk_buf
    call ata_write_sectors
    test eax, eax
    jz   .io
    mov  eax, [e2ci_dir]
    call ext2_write_inode               ; 目录 inode 落盘（size/i_block 变过）
    test eax, eax
    jz   .fail
    mov  dword [esp + 28], 1
    popad
    ret
.notdir:
    mov  dword [fs_err], FS_NOTDIR
    jmp  .fail
.corrupt:
    mov  dword [fs_err], FS_IO
    jmp  .fail
.io:
    mov  dword [fs_err], FS_IO
.fail:
    mov  eax, [fs_err]
    mov  [DIAG_ADDR + D_EXT2_ERR], eax
    mov  dword [esp + 28], 0
    popad
    ret

; ---- ext2_create：ESI = 相对路径，EAX = kind（0 文件 1 目录）→ 1/0 ----------
;  驱动表 VIA_CREATE 列。文件已存在 → 截断复用（cp 覆盖语义，跨重启幂等）；
;  目录已存在 → FS_EXIST。挂进父目录 + 计数落盘。
ext2_create:
    pushad
    cld
    cmp  dword [ext2_ready], 1
    jne  .notrdy
    inc  dword [DIAG_ADDR + D_EXT2_CMD]
    mov  [e2ci_kind], eax
    ; 拷路径（只剥串首的 '/'）
    mov  edi, e2ci_path
.copy:
    lodsb
    test al, al
    jz   .copied
    cmp  al, '/'
    jne  .st
    cmp  edi, e2ci_path
    je   .copy
.st:
    stosb
    jmp  .copy
.copied:
    xor  eax, eax
    stosb
    ; 找最后一个 '/'：其后的 = 名字，之前的 = 父路径
    mov  esi, e2ci_path
    xor  edx, edx
.find:
    lodsb
    test al, al
    jz   .found
    cmp  al, '/'
    jne  .find
    lea  eax, [esi - 1]
    sub  eax, e2ci_path
    mov  edx, eax
    jmp  .find
.found:
    test edx, edx
    jz   .root_case                      ; 没斜杠 = 名字挂在根下
    lea  esi, [e2ci_path + edx + 1]
    mov  edi, e2ci_name
    xor  ecx, ecx
.ncopy:
    lodsb
    test al, al
    jz   .ndone
    stosb
    inc  ecx
    jmp  .ncopy
.ndone:
    test ecx, ecx
    jz   .noname
    mov  [e2ci_nlen], ecx
    cmp  ecx, 31
    ja   .namelen
    ; 父路径 = 斜杠之前（临时截断）
    mov  byte [e2ci_path + edx], 0
    mov  eax, EXT2_ROOT_INO
    mov  esi, e2ci_path
    call ext2_lookup
    test eax, eax
    jz   .fail                           ; fs_err 已设
    mov  [e2ci_dir], eax
    jmp  .have_parent
.root_case:
    mov  esi, e2ci_path
    mov  edi, e2ci_name
    xor  ecx, ecx
.rcopy:
    lodsb
    test al, al
    jz   .rdone
    stosb
    inc  ecx
    jmp  .rcopy
.rdone:
    test ecx, ecx
    jz   .noname
    mov  [e2ci_nlen], ecx
    cmp  ecx, 31
    ja   .namelen
    mov  dword [e2ci_dir], EXT2_ROOT_INO
.have_parent:
    ; 已存在？find_dirent 吃 e2lk_name/e2lk_nlen
    mov  edi, e2lk_name
    mov  ecx, 40
    xor  eax, eax
    rep  stosb
    mov  esi, e2ci_name
    mov  edi, e2lk_name
    mov  ecx, [e2ci_nlen]
    rep  movsb
    mov  eax, [e2ci_nlen]
    mov  [e2lk_nlen], eax
    mov  eax, [e2ci_dir]
    call ext2_find_dirent
    test eax, eax
    jnz  .exist
    ; "不存在"是 create 的正常路径，但 find_dirent 的失败尾已经把
    ; fs_err=NOENT 和 D_EXT2_ERR 镜像写上了——不清零的话验收器会把
    ; 这个预期失败当成残留错误（D_EXT2_ERR=1 的"幽灵错误"）。
    mov  dword [fs_err], FS_OK
    mov  dword [DIAG_ADDR + D_EXT2_ERR], 0
    ; 分配 inode
    call ext2_alloc_inode
    test eax, eax
    jz   .fail
    mov  [e2ci_ino], eax
    mov  edi, ext2_ino_buf
    mov  ecx, 32
    xor  eax, eax
    rep  stosd                          ; inode 清零
    cmp  dword [e2ci_kind], 0
    je   .filemode
    mov  word [ext2_ino_buf + I_MODE], 0x41ED     ; 目录 0755
    mov  word [ext2_ino_buf + I_LINKS], 2
    jmp  .mode_done
.filemode:
    mov  word [ext2_ino_buf + I_MODE], 0x81A4     ; 文件 0644
    mov  word [ext2_ino_buf + I_LINKS], 1
.mode_done:
    ; 目录：先给数据块（. .. 两条，".." 的 rec 吞掉块尾）
    cmp  dword [e2ci_kind], 0
    je   .no_dirblk
    call ext2_alloc_block
    test eax, eax
    jz   .fail
    mov  [e2ci_dblk], eax
    mov  edi, ext2_blk_buf
    mov  ecx, 256
    xor  eax, eax
    rep  stosd
    mov  eax, [e2ci_ino]
    mov  [ext2_blk_buf + ED2_INO], eax           ; "."
    mov  word [ext2_blk_buf + ED2_REC], 12
    mov  byte [ext2_blk_buf + ED2_NAMELEN], 1
    mov  byte [ext2_blk_buf + ED2_NAME], '.'
    mov  eax, [e2ci_dir]
    mov  [ext2_blk_buf + 12 + ED2_INO], eax      ; ".."
    mov  word [ext2_blk_buf + 12 + ED2_REC], 1012
    mov  byte [ext2_blk_buf + 12 + ED2_NAMELEN], 2
    mov  byte [ext2_blk_buf + 12 + ED2_NAME], '.'
    mov  byte [ext2_blk_buf + 12 + ED2_NAME + 1], '.'
    mov  eax, [e2ci_dblk]
    shl  eax, 1
    mov  ecx, 2
    mov  esi, ext2_blk_buf
    call ata_write_sectors
    test eax, eax
    jz   .io
    mov  dword [ext2_ino_buf + I_SIZE], 1024
    mov  eax, [e2ci_dblk]
    mov  [ext2_ino_buf + I_BLOCK], eax
    mov  dword [ext2_ino_buf + I_BLOCKS], 2
.no_dirblk:
    mov  eax, [e2ci_ino]
    call ext2_write_inode
    test eax, eax
    jz   .fail
    ; 挂进父目录
    mov  eax, [e2ci_dir]
    mov  ebx, [e2ci_ino]
    call ext2_dir_insert
    test eax, eax
    jz   .fail
    ; 目录还要给父目录 links + 1（子目录的 ".." 算一票）
    cmp  dword [e2ci_kind], 0
    je   .no_linkup
    mov  eax, [e2ci_dir]
    call ext2_read_inode
    test eax, eax
    jz   .fail
    inc  word [ext2_ino_buf + I_LINKS]
    mov  eax, [e2ci_dir]
    call ext2_write_inode
    test eax, eax
    jz   .fail
.no_linkup:
    call ext2_sync_super
    test eax, eax
    jz   .fail
    mov  dword [esp + 28], 1
    popad
    ret
.exist:
    mov  [e2ci_ino], eax
    cmp  dword [e2ci_kind], 0
    jne  .exist_dir
    ; 文件存在 → 截断复用（保持 inode 和块，只清 size；随后的 write 从 0 长回）
    mov  eax, [e2ci_ino]
    call ext2_read_inode
    test eax, eax
    jz   .fail
    mov  dword [ext2_ino_buf + I_SIZE], 0
    mov  eax, [e2ci_ino]
    call ext2_write_inode
    test eax, eax
    jz   .fail
    mov  dword [esp + 28], 1
    popad
    ret
.exist_dir:
    mov  dword [fs_err], FS_EXIST
    jmp  .fail
.noname:
    mov  dword [fs_err], FS_BADPATH
    jmp  .fail
.namelen:
    mov  dword [fs_err], FS_NAMELEN
    jmp  .fail
.notrdy:
    mov  dword [fs_err], FS_IO
    jmp  .fail
.io:
    mov  dword [fs_err], FS_IO
.fail:
    mov  eax, [fs_err]
    mov  [DIAG_ADDR + D_EXT2_ERR], eax
    mov  dword [esp + 28], 0
    popad
    ret

; ---- ext2_write：ESI = fd 槽，EDI = 缓冲，ECX = 长度 → EAX = 字节数 ---------
;  只走直接块（v1 文件上限 12KB）；追加语义；写通后 D_EXT2_STAGE = 6
ext2_write:
    pushad
    cld
    cmp  dword [ext2_ready], 1
    jne  .notrdy
    inc  dword [DIAG_ADDR + D_EXT2_CMD]
    mov  [e2wr_fd], esi
    mov  [e2wr_src], edi
    mov  [e2wr_len], ecx
    mov  dword [e2wr_got], 0
    cmp  byte [esi + FD_VN + VN_KIND], VN_FILE
    jne  .isdir
    mov  eax, [esi + FD_VN + VN_A]
    mov  [e2wr_ino], eax
    call ext2_read_inode
    test eax, eax
    jz   .fail
    mov  eax, [esi + FD_POS]
    mov  [e2wr_pos], eax
.loop:
    mov  eax, [e2wr_got]
    cmp  eax, [e2wr_len]
    jae  .flush
    mov  eax, [e2wr_pos]
    shr  eax, 10
    cmp  eax, E2_MAX_DIRECT
    jae  .supp                           ; 超 v1 容量
    mov  [e2wr_idx], eax
    lea  esi, [ext2_ino_buf + I_BLOCK]
    mov  ebx, [esi + eax * 4]
    test ebx, ebx
    jnz  .have_blk
    call ext2_alloc_block
    test eax, eax
    jz   .fail                           ; 满了
    mov  ebx, eax
    mov  eax, [e2wr_idx]
    lea  esi, [ext2_ino_buf + I_BLOCK]
    mov  [esi + eax * 4], ebx
    add  dword [ext2_ino_buf + I_BLOCKS], 2
.have_blk:
    mov  [e2wr_blk], ebx
    ; chunk = min(1024 - boff, 剩余)——方向别写反（踩坑 23）
    mov  eax, [e2wr_pos]
    and  eax, 0x3FF
    mov  [e2wr_boff], eax
    mov  ecx, 1024
    sub  ecx, eax                        ; ecx = 块内可用
    mov  eax, [e2wr_len]
    sub  eax, [e2wr_got]                 ; eax = 剩余
    cmp  eax, ecx
    jae  .use_avail                      ; 剩余 >= 可用 → chunk = 可用（ecx）
    mov  ecx, eax                        ; 剩余 < 可用 → chunk = 剩余
.use_avail:
    mov  [e2wr_chunk], ecx
    ; 整块 → 直接从调用方缓冲写；否则读改写
    cmp  dword [e2wr_boff], 0
    jne  .partial
    cmp  dword [e2wr_chunk], 1024
    jne  .partial
    mov  eax, [e2wr_blk]
    shl  eax, 1
    mov  ecx, 2
    mov  esi, [e2wr_src]
    add  esi, [e2wr_got]
    call ata_write_sectors
    test eax, eax
    jz   .io
    jmp  .advance
.partial:
    mov  eax, [e2wr_blk]
    call ext2_read_block
    test eax, eax
    jz   .io
    mov  edi, ext2_blk_buf
    add  edi, [e2wr_boff]
    mov  esi, [e2wr_src]
    add  esi, [e2wr_got]
    mov  ecx, [e2wr_chunk]
    rep  movsb
    mov  eax, [e2wr_blk]
    shl  eax, 1
    mov  ecx, 2
    mov  esi, ext2_blk_buf
    call ata_write_sectors
    test eax, eax
    jz   .io
.advance:
    mov  eax, [e2wr_chunk]
    add  [e2wr_pos], eax
    add  [e2wr_got], eax
    jmp  .loop
.flush:
    ; size = max(旧, pos)（截断后从 0 追加，正好得新长度）
    mov  eax, [e2wr_pos]
    cmp  eax, [ext2_ino_buf + I_SIZE]
    jbe  .size_ok
    mov  [ext2_ino_buf + I_SIZE], eax
.size_ok:
    mov  eax, [e2wr_ino]
    call ext2_write_inode
    test eax, eax
    jz   .io
    mov  esi, [e2wr_fd]
    mov  eax, [e2wr_pos]
    mov  [esi + FD_POS], eax
    mov  dword [DIAG_ADDR + D_EXT2_STAGE], 6
    call ext2_sync_super
    test eax, eax
    jz   .io
    mov  eax, [e2wr_got]
    mov  [esp + 28], eax
    popad
    ret
.isdir:
    mov  dword [fs_err], FS_ISDIR
    jmp  .fail
.supp:
    mov  dword [fs_err], FS_NOTSUPP
    jmp  .fail
.notrdy:
    mov  dword [fs_err], FS_IO
    jmp  .fail
.io:
    mov  dword [fs_err], FS_IO
.fail:
    mov  eax, [fs_err]
    mov  [DIAG_ADDR + D_EXT2_ERR], eax
    mov  dword [esp + 28], 0
    popad
    ret

; ---- ext2_close：无缓存脏页，空操作 ------------------------------------------
ext2_close:
    pushad
    mov  dword [esp + 28], 0
    popad
    ret

; ============================================================================
;  数据
; ============================================================================
ext2_ready       dd 0            ; 1 = 挂上了
ext2_sb_blocks   dd 0            ; s_blocks_count（块号上界）
ext2_sb_free     dd 0            ; s_free_blocks_count（df / D_EXT2_FREE）
ext2_sb_ipg      dd 0            ; s_inodes_per_group
ext2_first_data  dd 0            ; s_first_data_block（1024B 块恒 1）
ext2_gd_inotab   dd 0            ; 组 0 的 inode 表首块
ext2_ino_last    dd 0            ; inode 一条缓存（0 = 无）
ext2_cur_ino     dd 0            ; 本次驱动调用处理的 inode 号
ext2_rd_ino      dd 0            ; read_inode 的入参暂存（缓存打标用）
ext2_off         dd 0            ; read_inode 的表内偏移暂存

ext2_blk_buf     times 1024 db 0 ; 数据/目录块缓冲
ext2_ind_buf     times 1024 db 0 ; 一级间接块专用（与数据块并用不互踩）
ext2_ino_buf     times 128  db 0 ; 当前 inode

; ext2_lookup 工作区
e2lk_ino        dd 0
e2lk_path       dd 0
e2lk_name       times 40 db 0
e2lk_nlen       dd 0

; ext2_find_dirent 工作区
e2f_size        dd 0
e2f_pos         dd 0
e2f_ino         dd 0
e2f_rec         dd 0
e2f_nlen        dd 0

; ext2_read 工作区
e2rd_fd         dd 0
e2rd_dst        dd 0
e2rd_need       dd 0
e2rd_left       dd 0
e2rd_got        dd 0
e2rd_pos        dd 0
e2rd_idx        dd 0
e2rd_boff       dd 0

; ext2_readdir 工作区
e2rl_fd         dd 0
e2rl_rec        dd 0
e2rl_size       dd 0
e2rl_pos        dd 0
e2rl_ino        dd 0
e2rl_rec_len    dd 0
e2rl_nlen       dd 0

; ext2 写路径工作区（FS4）
ext2_sb_free_ino dd 0           ; s_free_inodes_count（缓存，sync_super 用）

e2bm_blk        dd 0            ; 位图块号
e2bm_bit        dd 0            ; 位游标 / 结果位号
e2bm_lim        dd 0            ; 位上限
e2bm_res        dd 0            ; 分配结果暂存（借给 dir_insert 存 tail_used）

e2wi_ino        dd 0            ; write_inode 目标
e2wi_off        dd 0            ; inode 表内偏移

e2ci_kind       dd 0            ; 0 = 文件 1 = 目录
e2ci_path       times 64 db 0   ; 相对路径拷贝
e2ci_name       times 40 db 0   ; 新条目名
e2ci_nlen       dd 0
e2ci_dir        dd 0            ; 父目录 ino
e2ci_ino        dd 0            ; 新 inode
e2ci_blk        dd 0            ; 目录数据块号
e2ci_need       dd 0            ; 新条目字节数
e2ci_dblk       dd 0            ; 新目录自己的数据块

e2wr_fd         dd 0
e2wr_src        dd 0
e2wr_len        dd 0
e2wr_got        dd 0
e2wr_pos        dd 0
e2wr_ino        dd 0
e2wr_idx        dd 0
e2wr_boff       dd 0
e2wr_chunk      dd 0
e2wr_blk        dd 0
