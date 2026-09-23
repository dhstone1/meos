; ============================================================================
;  MeOS · 引导扇区（BIOS 传统引导 / MBR）
; ----------------------------------------------------------------------------
;  BIOS 把磁盘第 0 个扇区加载到物理地址 0x7C00，然后跳到第一条指令。
;  这里只做三件事：
;    1. 显示一行开机信息（方便确认引导链路是通的）；
;    2. 用 BIOS 的 INT 13h 把内核载荷从第 1 个扇区起连续读到 0x8000；
;    3. 跳过去，把控制权交给内核。

;  构建：nasm -f bin src/boot/boot.asm -o build/boot.bin
;  约束：必须恰好 512 字节，最后两字节固定为 0x55 0xAA（引导签名）
; ============================================================================

BITS 16                         ; 16 位实模式
ORG 0x7C00                      ; BIOS 约定的加载地址

; 载荷容量：至少要盖住 tools/mkfloppy.py 里那个载荷的扇区数，
; 否则内核尾部（字模、数据）会被静默截断——界面上看到的怪现象就是这么来的。
; mkfloppy.py 会在载荷超容量时直接报错，两边要一起改。
PAYLOAD_SECTORS   equ 64        ; 装入 64 个扇区（32KB）给内核用
SECTORS_PER_TRACK equ 18        ; 1.44MB 软盘：每磁道 18 个扇区
HEADS             equ 2         ; 双面
PAYLOAD_ADDR      equ 0x8000    ; 内核载荷装入的物理地址
READ_RETRIES      equ 4         ; 单个扇区读失败后的重试次数

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00              ; 栈紧贴引导扇区下方，向下生长
    sti
    mov [boot_drive], dl        ; BIOS 通过 DL 告诉我们从哪个驱动器启动

    mov si, msg_boot
    call puts

    xor ah, ah                  ; 先复位驱动器，清掉可能存在的错误状态
    mov dl, [boot_drive]
    int 0x13

    xor ax, ax
    mov es, ax
    mov bx, PAYLOAD_ADDR        ; ES:BX = 0x0000:0x8000，读入目标
    mov word [lba], 1           ; 从第 1 个扇区开始读
    mov word [left], PAYLOAD_SECTORS

.load:
    ; LBA -> CHS。INT 13h 的布局是：CH = 柱面号低 8 位，CL 位 6~7 = 柱面号高位，
    ; CL 位 0~5 = 扇区号（1 起）。所以扇区号只往 CL 里写、柱面号只往 CH 里写，
    ; 千万不能用一个寄存器把整个 CX 重新装一遍——那样会把柱面号顺手抹掉，
    ; 于是永远停在第 0 柱面（LBA 0~35），往后读到的全是错数据。
    mov ax, [lba]
    xor dx, dx
    mov di, SECTORS_PER_TRACK
    div di                      ; AX = 磁道号，DX = 磁道内扇区号（0 起）
    inc dl
    mov cl, dl                  ; CL = 扇区号（1..18）
    xor dx, dx
    mov di, HEADS
    div di                      ; AX = 柱面号，DX = 磁头号
    mov ch, al                  ; CH = 柱面号低 8 位（柱面 < 256，高 2 位恒为 0）
    mov dh, dl                  ; DH = 磁头号

    mov byte [retry], READ_RETRIES
.attempt:
    mov dl, [boot_drive]
    mov ax, 0x0201              ; AH=02 读扇区，AL=01 读一个扇区
    int 0x13
    jnc .ok
    xor ah, ah                  ; 读失败：复位驱动器再试
    mov dl, [boot_drive]
    int 0x13
    dec byte [retry]
    jnz .attempt
    jmp .fail
.ok:
    add bx, 512                 ; 下一个扇区的目标地址
    jnc .adv                    ; 没跨过 64KB 边界就不用动段
    mov ax, es
    add ax, 0x1000              ; BX 从 0xFFFF 卷回 0x0000：段前进 64KB
    mov es, ax
.adv:
    inc word [lba]
    dec word [left]
    jnz .load

    mov dl, [boot_drive]        ; 把启动盘号传给内核
    jmp 0x0000:PAYLOAD_ADDR     ; 跳进内核

.fail:
    mov si, msg_fail
    call puts
    jmp halt

; ---- 用 BIOS 电传打字输出一行以 0 结尾的字符串 -------------------------------
puts:
    mov ah, 0x0E
    mov bh, 0x00                ; 页号 0
.next:
    lodsb                       ; AL = [DS:SI]，SI 自增
    test al, al
    jz .done
    int 0x10
    jmp .next
.done:
    ret

; ---- 停机：屏蔽所有可屏蔽中断后开中断空转，不用 cli+hlt ---------------------
;      纯 cli + hlt 会被 VMware 判定为「客户机禁用了 CPU」而暂停虚拟机。
halt:
    mov al, 0xFF
    out 0x21, al
    out 0xA1, al
    sti
.loop:
    hlt
    jmp .loop

boot_drive db 0
lba        dw 0
left       dw 0
retry      db 0
msg_boot   db "MeOS boot ...", 13, 10, 0
msg_fail   db "disk read error", 13, 10, 0

times 510 - ($ - $$) db 0       ; 补零到 510 字节
dw 0xAA55                       ; 引导签名