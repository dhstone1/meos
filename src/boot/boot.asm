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

PAYLOAD_SECTORS   equ 60        ; 装入 60 个扇区（30KB）给内核用
SECTORS_PER_TRACK equ 18        ; 1.44MB 软盘：每磁道 18 个扇区
HEADS             equ 2         ; 双面
PAYLOAD_ADDR      equ 0x8000    ; 内核载荷装入的物理地址

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
    mov ax, [lba]               ; 把 LBA 换算成 BIOS 需要的 柱面/磁头/扇区
    xor dx, dx
    mov cx, SECTORS_PER_TRACK
    div cx                      ; AX = 磁道号，DX = 磁道内扇区号
    inc dl
    mov si, dx                  ; SI 暂存扇区号（1..18）
    xor dx, dx
    mov cx, HEADS
    div cx                      ; AX = 柱面号，DX = 磁头号
    mov ch, al
    mov dh, dl
    mov cx, si                  ; CL = 扇区号（柱面号小于 256，高位恒为 0）
    mov dl, [boot_drive]
    mov ax, 0x0201              ; AH=02 读扇区，AL=01 读一个扇区
    int 0x13
    jc .fail
    add bx, 512                 ; 下一个扇区的目标地址
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
msg_boot   db "MeOS boot ...", 13, 10, 0
msg_fail   db "disk read error", 13, 10, 0

times 510 - ($ - $$) db 0       ; 补零到 510 字节
dw 0xAA55                       ; 引导签名