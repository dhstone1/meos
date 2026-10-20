; ============================================================================
;  ata.inc —— ATA/IDE PIO 轮询驱动（FS2，见 docs/Linux文件系统移植方案.md §6.1）
; ============================================================================
;  只管主盘（Primary Master，vmx 里的 ide0:0）：端口 0x1F0..0x1F7，LBA28。
;
;  中断纪律：**全程轮询，不碰 PIC**。IRQ14/15 本来就屏蔽着，sti 环境下轮询安全，
;  顺手把中断解开是把简单问题搞复杂的典型路径（方案 §11 踩坑 15）。
;
;  约定（同 fs.inc / vfs.inc）：例程自带 pushad；结果放 EAX（1 = 成功，0 = 失败）；
;  错误细节一律落 D_ATA_ERR——bit0-7 = 最后一次状态寄存器，bit8 = 超时标志。
;  ata_init 在控制台起来之前跑（pm_entry 里 fs_init/vfs_init 之后），失败不打印，
;  只写诊断块。
;
;  轮询必须有超时（方案 §11 踩坑 2）：BSY 卡死 = VM 假死，看起来像内核崩了，
;  实际是驱动在死等。超时上限取 5e6 次循环，够了（VMware 应答是毫秒级）。
; ============================================================================

; ---- 端口（寄存器文件，见 技术文档.md §12.11）--------------------------------
ATA_DATA     equ 0x1F0              ; 扇区数据（16 位 insw/outsw）
ATA_ERR      equ 0x1F1              ; 读 = 错误寄存器；写 = features
ATA_NSECT    equ 0x1F2              ; 扇区数（写 0 = 256，不是 0！）
ATA_LBA_LO   equ 0x1F3
ATA_LBA_MID  equ 0x1F4
ATA_LBA_HI   equ 0x1F5
ATA_DRIVE    equ 0x1F6              ; 0xE0 | ((lba>>24)&0x0F)：主盘 + LBA 模式
ATA_STATUS   equ 0x1F7              ; 读 = 状态；写 = 命令
ATA_ALTSTAT  equ 0x3F6              ; 备用状态（读 4 次 = 规定的 400ns 等待）

; ---- 状态位 -------------------------------------------------------------------
ATA_ST_BSY   equ 0x80               ; 忙
ATA_ST_DRQ   equ 0x08               ; 数据就绪
ATA_ST_ERR   equ 0x01               ; 出错

ATA_TIMEOUT  equ 5000000            ; 轮询上限（方案 §6.1）

; ---- 命令 ----------------------------------------------------------------------
ATA_CMD_IDENTIFY equ 0xEC
ATA_CMD_READ     equ 0x20
ATA_CMD_WRITE    equ 0x30
ATA_CMD_FLUSH    equ 0xE7

; ============================================================================
;  ata_init —— 复位总线 + IDENTIFY，解析 model / 容量
;   -> EAX = 1 找到盘；0 = 总线上没有盘（软着陆，不炸）
;  D_ATA_STAGE：0 = 未探测，1 = 复位过，2 = IDENTIFY 成功
; ============================================================================
ata_init:
    pushad
    mov  dword [DIAG_ADDR + D_ATA_STAGE], 1

    ; ---- 软复位：0x3F6 写 0x04（SRST），稍等，再写 0x00 撤销 -------------------
    mov  dx, ATA_ALTSTAT
    mov  al, 0x04
    out  dx, al
    mov  ecx, 100000                 ; 复位脉冲保持一小会儿（纯延迟循环）
.reset_wait:
    dec  ecx
    jnz  .reset_wait                 ; dec/jnz 前面有确定非零初值，不会 0 圈
    mov  al, 0x00
    out  dx, al

    call ata_wait_ready              ; 复位后等 BSY 清
    test eax, eax
    jz  .fail

    ; ---- 先探一下总线是不是空的：空总线状态口读出来是 0 或全 1 -----------------
    mov  dx, ATA_STATUS
    in   al, dx
    test al, al
    jz  .fail                        ; 悬空 = 0x00，主盘根本不存在
    cmp  al, 0xFF
    je  .fail                        ; 悬空的另一种表现

    ; ---- IDENTIFY：选主盘（0xA0），发 0xEC，读 256 字 --------------------------
    mov  dx, ATA_DRIVE
    mov  al, 0xA0
    out  dx, al
    mov  dx, ATA_STATUS
    mov  al, ATA_CMD_IDENTIFY
    out  dx, al
    call ata_wait_ready              ; 等 BSY 清（IDENTIFY 也可能直接进 DRQ）
    test eax, eax
    jz  .fail
    call ata_wait_drq                ; 等数据就绪
    test eax, eax
    jz  .fail
    mov  edi, ata_ident_buf
    call ata_pio_in_256              ; 256 字 -> ata_ident_buf
    test eax, eax
    jz  .fail

    ; ---- 解析 model（字 27..46，字节偏移 54..93：每个字存 2 个字符，
    ;      且首字符在高字节——ATA 字符串按字存、字内大端）------------------------
    mov  esi, ata_ident_buf + 54
    mov  edi, ata_model_buf
    mov  ecx, 20                     ; 20 个字 = 40 个字符
.model_loop:
    mov  al, [esi + 1]               ; 首字符
    mov  [edi], al
    inc  edi
    mov  al, [esi]                   ; 次字符
    mov  [edi], al
    inc  edi
    add  esi, 2
    dec  ecx
    jnz  .model_loop
    mov  byte [edi], 0               ; 收尾 0

    ; ---- 截掉尾巴上的空格（ATA 字符串右补空格）--------------------------------
    mov  edi, ata_model_buf + 39
    mov  ecx, 40
.trim:
    cmp  byte [edi], ' '
    jne  .trim_done
    mov  byte [edi], 0
    dec  edi
    dec  ecx
    jnz  .trim
.trim_done:

    ; ---- 解析 LBA28 容量（字 60/61，字节偏移 120）------------------------------
    mov  eax, [ata_ident_buf + 120]  ; dword = words 60|61 拼好（小端天然如此）
    mov  [ata_sectors], eax

    mov  dword [ata_present], 1
    mov  dword [DIAG_ADDR + D_ATA_STAGE], 2
    popad
    mov  eax, 1
    ret
.fail:
    mov  dword [ata_present], 0
    popad
    xor  eax, eax
    ret

; ============================================================================
;  ata_read_sectors —— EAX = LBA，ECX = 扇区数（1..256，0 视为 256），
;                      EDI = 目的缓冲 -> EAX = 1 成功
;  成功时 D_ATA_STAGE 至少 3（第一次真把数据读通了），D_ATA_LBA 记本次起始
;  LBA，D_ATA_COUNT 累加实际传输的扇区数。
;  IDENTIFY 的原始数据在 ata_ident_buf 里缓存着（ata_init 做的，只做一次）。
; ============================================================================
ata_read_sectors:
    pushad
    cmp  byte [ata_present], 1
    jne  .fail

    mov  [ata_xfer_lba], eax         ; 记下起始 LBA（诊断用）
    jecxz .count_256                 ; 0 = 256（ATA 语义）
    jmp  .count_ok
.count_256:
    mov  ecx, 256
.count_ok:
    mov  [ata_xfer_cnt], ecx

    call ata_wait_ready
    test eax, eax
    jz  .fail
    call ata_400ns                   ; 发命令前的规定等待（方案 §11 踩坑 1）

    ; ---- 填 6 个寄存器 --------------------------------------------------------
    mov  eax, [ata_xfer_lba]
    mov  dx, ATA_NSECT
    mov  al, byte [ata_xfer_cnt]     ; 256 写 0 = 256，正好同义
    out  dx, al
    mov  dx, ATA_LBA_LO
    mov  eax, [ata_xfer_lba]
    out  dx, al
    mov  dx, ATA_LBA_MID
    mov  eax, [ata_xfer_lba]
    shr  eax, 8
    out  dx, al
    mov  dx, ATA_LBA_HI
    mov  eax, [ata_xfer_lba]
    shr  eax, 16
    out  dx, al
    mov  dx, ATA_DRIVE
    mov  eax, [ata_xfer_lba]
    shr  eax, 24
    and  eax, 0x0F
    or   eax, 0xE0                   ; 主盘 + LBA 模式
    out  dx, al

    mov  dx, ATA_STATUS
    mov  al, ATA_CMD_READ
    out  dx, al

    ; ---- 每扇区：等 DRQ -> rep insw 256 字 ------------------------------------
    mov  ecx, [ata_xfer_cnt]
.sector:
    push ecx
    call ata_wait_drq
    test eax, eax
    jz  .fail_pop
    call ata_pio_in_256              ; 例程内 EDI 已被 rep insw 前进 512
    test eax, eax
    jz  .fail_pop
    pop  ecx
    dec  ecx
    jnz  .sector

    ; ---- 收账：诊断字段 -------------------------------------------------------
    mov  eax, [ata_xfer_lba]
    mov  [DIAG_ADDR + D_ATA_LBA], eax
    mov  eax, [ata_xfer_cnt]
    add  [DIAG_ADDR + D_ATA_COUNT], eax
    cmp  dword [DIAG_ADDR + D_ATA_STAGE], 3
    jae  .stage_ok
    mov  dword [DIAG_ADDR + D_ATA_STAGE], 3   ; 扇区真读通了（FS3 起超块验过再加 4）
.stage_ok:
    popad
    mov  eax, 1
    ret
.fail_pop:
    pop  ecx
.fail:
    popad
    xor  eax, eax
    ret

; ============================================================================
;  ata_write_sectors —— EAX = LBA，ECX = 扇区数，ESI = 源 -> EAX = 1 成功
;  对称的 outsw 版（FS4 才真用得上；写完发 FLUSH CACHE）。
; ============================================================================
ata_write_sectors:
    pushad
    cmp  byte [ata_present], 1
    jne  .fail

    mov  [ata_xfer_lba], eax
    jecxz .count_256
    jmp  .count_ok
.count_256:
    mov  ecx, 256
.count_ok:
    mov  [ata_xfer_cnt], ecx
    mov  [ata_xfer_src], esi

    call ata_wait_ready
    test eax, eax
    jz  .fail
    call ata_400ns

    mov  eax, [ata_xfer_lba]
    mov  dx, ATA_NSECT
    mov  al, byte [ata_xfer_cnt]
    out  dx, al
    mov  dx, ATA_LBA_LO
    mov  eax, [ata_xfer_lba]
    out  dx, al
    mov  dx, ATA_LBA_MID
    mov  eax, [ata_xfer_lba]
    shr  eax, 8
    out  dx, al
    mov  dx, ATA_LBA_HI
    mov  eax, [ata_xfer_lba]
    shr  eax, 16
    out  dx, al
    mov  dx, ATA_DRIVE
    mov  eax, [ata_xfer_lba]
    shr  eax, 24
    and  eax, 0x0F
    or   eax, 0xE0
    out  dx, al

    mov  dx, ATA_STATUS
    mov  al, ATA_CMD_WRITE
    out  dx, al

    mov  esi, [ata_xfer_src]
    mov  ecx, [ata_xfer_cnt]
.sector:
    push ecx
    call ata_wait_drq                ; 写命令：DRQ 置位表示盘准备好收数据
    test eax, eax
    jz  .fail_pop
    call ata_pio_out_256             ; 例程内 ESI 已被 rep outsw 前进 512
    test eax, eax
    jz  .fail_pop
    pop  ecx
    dec  ecx
    jnz  .sector

    call ata_wait_ready              ; 写完等盘收尾
    test eax, eax
    jz  .fail
    mov  dx, ATA_STATUS              ; FLUSH CACHE：VMware 的镜像才真落盘
    mov  al, ATA_CMD_FLUSH
    out  dx, al
    call ata_wait_ready

    mov  eax, [ata_xfer_lba]
    mov  [DIAG_ADDR + D_ATA_LBA], eax
    mov  eax, [ata_xfer_cnt]
    add  [DIAG_ADDR + D_ATA_COUNT], eax
    popad
    mov  eax, 1
    ret
.fail_pop:
    pop  ecx
.fail:
    popad
    xor  eax, eax
    ret

; ---- 等状态口：EAX=0 的 BSY 清（带超时，超时落 D_ATA_ERR bit8）------------------
;  -> EAX = 1 就绪；EAX = 0 超时
ata_wait_ready:
    pushad
    mov  ecx, ATA_TIMEOUT
.loop:
    mov  dx, ATA_ALTSTAT             ; 用备用状态口读，不会影响中断挂起位
    in   al, dx
    test al, ATA_ST_BSY
    jz   .ready
    dec  ecx
    jnz  .loop
    or   dword [DIAG_ADDR + D_ATA_ERR], 0x100      ; bit8 = 超时标志
    popad
    xor  eax, eax
    ret
.ready:
    popad
    mov  eax, 1
    ret

; ---- 等数据就绪：BSY = 0 且 DRQ = 1（同样带超时）--------------------------------
;  -> EAX = 1；EAX = 0 超时/出错
ata_wait_drq:
    pushad
    mov  ecx, ATA_TIMEOUT
.loop:
    mov  dx, ATA_ALTSTAT
    in   al, dx
    test al, ATA_ST_ERR
    jnz  .err
    test al, ATA_ST_BSY
    jnz  .next
    test al, ATA_ST_DRQ
    jnz  .ready
.next:
    dec  ecx
    jnz  .loop
    or   dword [DIAG_ADDR + D_ATA_ERR], 0x100
    popad
    xor  eax, eax
    ret
.err:
    movzx eax, al
    or   dword [DIAG_ADDR + D_ATA_ERR], eax         ; 记下出错时的状态位
    popad
    xor  eax, eax
    ret
.ready:
    popad
    mov  eax, 1
    ret

; ---- 规定的 400ns 等待：备用状态口读 4 次 ---------------------------------------
;  虚机里大概率省了也没事，真机必炸。别省（方案 §11 踩坑 1）。
ata_400ns:
    pushad
    mov  dx, ATA_ALTSTAT
    in   al, dx
    in   al, dx
    in   al, dx
    in   al, dx
    popad
    ret

; ---- 256 字 PIO 读进 [EDI]，EDI += 512（不复位 DF 就 insw 是经典坑）-------------
ata_pio_in_256:
    push ecx
    push edx
    push esi
    mov  dx, ATA_DATA
    mov  ecx, 256
    cld
    rep  insw                        ; 计数单位是字，256 字 = 512B（踩坑 3）
                                     ; EDI 由 rep 自然前进 512，绝不另加（踩坑 21）
    pop  esi
    pop  edx
    pop  ecx
    mov  eax, 1
    ret

; ---- 256 字 PIO 写出 [ESI]，ESI += 512 ------------------------------------------
ata_pio_out_256:
    push ecx
    push edx
    push edi
    mov  dx, ATA_DATA
    mov  ecx, 256
    cld
    rep  outsw                       ; ESI 由 rep 自然前进 512，绝不另加（踩坑 21）
    pop  edi
    pop  edx
    pop  ecx
    mov  eax, 1
    ret

; ============================================================================
;  cmd_do_disk —— `disk` 命令：盘在不在、型号、容量、ext2 魔数第一眼证据
;  这是 FS2 的验收面：读 LBA 2（1024B 偏移 0 处的超块），
;  偏移 0x38 处应看到 53 EF（小端 0xEF53）。
; ============================================================================
cmd_do_disk:
    pushad
    cmp  byte [ata_present], 1
    jne  .none

    mov  esi, msg_ata_hdr            ; "ata0: "
    call con_puts
    mov  esi, ata_model_buf
    call con_puts
    call con_crlf

    mov  esi, msg_ata_sect           ; "sectors: "
    call con_puts
    mov  eax, [ata_sectors]
    call putdec
    mov  esi, msg_ata_mb_a           ; " ("
    call con_puts
    mov  eax, [ata_sectors]
    shr  eax, 11                     ; /2048 = MB（512B 扇区）
    call putdec
    mov  esi, msg_ata_mb_b           ; " MB)"
    call con_puts
    call con_crlf

    ; ---- 读 LBA 2（超块所在 1KB 的前一半），看偏移 0x38 的魔数 ----------------
    mov  eax, 2
    mov  ecx, 1
    mov  edi, ata_sec_buf
    call ata_read_sectors
    test eax, eax
    jz  .read_fail

    mov  esi, msg_ata_mag            ; "lba2+0x38: "
    call con_puts
    movzx eax, byte [ata_sec_buf + 0x38]
    mov  edx, eax
    shr  eax, 4
    mov  al, [hex_digits + eax]
    call con_putc
    mov  eax, edx
    and  eax, 0x0F
    mov  al, [hex_digits + eax]
    call con_putc
    mov  al, ' '
    call con_putc
    movzx eax, byte [ata_sec_buf + 0x39]
    mov  edx, eax
    shr  eax, 4
    mov  al, [hex_digits + eax]
    call con_putc
    mov  eax, edx
    and  eax, 0x0F
    mov  al, [hex_digits + eax]
    call con_putc

    cmp  word [ata_sec_buf + 0x38], 0xEF53
    jne  .no_magic
    mov  esi, msg_ata_yesmag         ; " (ext2 magic ok)"
    call con_puts
    call con_crlf
    jmp  .done
.no_magic:
    mov  esi, msg_ata_nomag          ; " (no ext2 magic)"
    call con_puts
    call con_crlf
    jmp  .done
.read_fail:
    mov  esi, msg_ata_rdfail
    call con_puts
    call con_crlf
    jmp  .done
.none:
    mov  esi, msg_ata_none           ; "ata0: no disk"
    call con_puts
    call con_crlf
.done:
    popad
    ret

; ============================================================================
;  ata.inc 数据区（放最后，同 fs.inc 的规矩）
; ============================================================================
ata_present    dd 0                  ; 1 = 主盘在（IDENTIFY 过了）
ata_sectors   dd 0                  ; LBA28 容量（IDENTIFY 字 60/61）
ata_xfer_lba  dd 0                  ; 本轮传输的起始 LBA（内部暂存）
ata_xfer_cnt  dd 0                  ; 本轮传输的扇区数（内部暂存）
ata_xfer_src  dd 0                  ; 本轮写传输的源指针（内部暂存）

ata_ident_buf times 512 db 0        ; IDENTIFY 的 512B 原始数据
ata_sec_buf   times 512 db 0        ; 扇区级过渡缓冲（disk 命令 / ext2 半块 IO 复用）
ata_model_buf times 41 db 0         ; 型号字符串（40 字符 + 结尾 0）

msg_ata_hdr    db "ata0: ", 0
msg_ata_sect   db "sectors: ", 0
msg_ata_mb_a   db " (", 0
msg_ata_mb_b   db " MB)", 0
msg_ata_mag    db "lba2+0x38: ", 0
msg_ata_yesmag db " (ext2 magic ok)", 0
msg_ata_nomag  db " (no ext2 magic)", 0
msg_ata_none   db "ata0: no disk", 0
msg_ata_rdfail db "ata0: read failed", 0
