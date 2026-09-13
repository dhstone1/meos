; ============================================================================
;  MeOS · 内核
; ----------------------------------------------------------------------------
;  由引导扇区从物理地址 0x8000 装入，分两段执行：
;
;    一、16 位实模式（把机器准备好）
;        1. 打开 A20 地址线，否则访问不到 1MB 以上的显存；
;        2. 用 VBE 遍历可用图形模式，挑一个带线性帧缓冲的直接色模式，
;           顺手把帧缓冲地址 / 行跨度 / 分辨率 / 位深抄进参数块（0x6000）；
;        3. 校验参数块，任何一项不像样就退回文本模式报错停机；
;        4. 建好 GDT，打开 CR0 的保护模式位。
;
;    二、32 位保护模式（真正干活）
;        5. 建一张兜底的 IDT——保护模式下没有 IDT，任何异常都会三重故障，
;           VMware 会把整台虚拟机复位，排查起来极其难受；
;        6. 采一份硬件参数写进诊断块（0x6200），再清屏、居中画字；
;        7. 屏蔽 8259 后开中断空转，CPU 真正歇下来，画面保持不动。
;
;  诊断块：.vmx 运行期间 VMware 会把客户机物理内存镜像到 build\*.vmem，
;  于是「把中间结果写进 0x6200」就等于有了一条输出通道，比盯着黑屏猜快得多。
;  看诊断块：python tools/vmdiag.py
;
;  构建：nasm -f bin -I src/kernel src/kernel/kernel.asm -o build/payload.bin
; ============================================================================

BITS 16
ORG 0x8000

FONT_STEP        equ FONT_GLYPH_W + 4     ; 字间距 = 字宽 + 4 像素空隙
FONT_GAP         equ FONT_STEP - FONT_GLYPH_W

VBE_INFO_ADDR    equ 0x5000        ; VbeInfoBlock 缓冲区（512 字节）
VBE_MODE_ADDR    equ 0x5200        ; VbeModeInfoBlock 缓冲区（256 字节）
PARAM_ADDR       equ 0x6000        ; 交给 32 位内核的参数块
DIAG_ADDR        equ 0x6200        ; 诊断块，从 .vmem 里读
IDT_ADDR         equ 0x7000        ; 32 位内核的 IDT（256 项 x 8 字节）
KERNEL_STACK_TOP equ 0x7C000       ; 内核栈顶

; 参数块内部偏移（16 位阶段写入、32 位阶段读取，两边必须一致）
P_FB        equ 0x00               ; dword 帧缓冲物理地址
P_PITCH     equ 0x04               ; word  每行字节数
P_WIDTH     equ 0x06               ; word  水平分辨率
P_HEIGHT    equ 0x08               ; word  垂直分辨率
P_BPP       equ 0x0A               ; byte  每像素位数
P_MAGIC     equ 0x0C               ; dword 校验值，证明参数确实被写过
P_PIXBYTES  equ 0x10               ; dword 每像素字节数（2 / 3 / 4）

PARAM_MAGIC equ 0x534F454D         ; "MEOS" 的小端表示

FONT_TEXT_W     equ FONT_CHAR_NUM * FONT_STEP - FONT_GAP   ; 整句话的像素宽度

; 诊断块内部偏移
D_STAGE     equ 0x00               ; dword 执行到第几步（1=IDT 2=清屏 3=画字 4=停机）
D_CX        equ 0x04               ; dword 文字左上角 X
D_CY        equ 0x08               ; dword 文字左上角 Y
D_SVGA_ID   equ 0x0C               ; dword SVGA_REG_ID
D_FBSTART   equ 0x10               ; dword SVGA_REG_FB_START
D_FBOFFSET  equ 0x14               ; dword SVGA_REG_FB_OFFSET
D_BPL       equ 0x18               ; dword SVGA_REG_BYTES_PER_LINE
D_SW        equ 0x1C               ; dword SVGA_REG_WIDTH
D_SH        equ 0x20               ; dword SVGA_REG_HEIGHT
D_SBPP      equ 0x24               ; dword SVGA_REG_BITS_PER_PIXEL
D_SENABLE   equ 0x28               ; dword SVGA_REG_ENABLE
D_PROBE_FB  equ 0x2C               ; dword 在参数块 FB+0x1000 处读写回测结果
D_PROBE_ALT equ 0x30               ; dword 在 SVGA_REG_FB_START+0x1000 处读写回测结果
D_BARPIX    equ 0x34               ; dword 保留
D_TEXTPIX   equ 0x38               ; dword 文字包围盒里读回的白像素数
D_MSGBOX    equ 0x3C               ; dword 低 16 位 = 文字左上角 X，高 16 位 = Y

; ============================================================================
;  一、16 位实模式
; ============================================================================
kernel_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    call enable_a20
    call vbe_pick_mode                 ; 选模式，并把参数写进参数块
    call check_params                  ; 参数不像样就别切模式了
    call vbe_set_mode                  ; 真正切到图形模式
    jmp enter_protected_mode

; ---- 打开 A20：先问 BIOS，再用 Fast A20 兜底 ---------------------------------
;  端口 0x92：bit0 = 快速复位（0 才是不复位），bit1 = A20 门控。
;  所以是 or 上 bit1、and 掉 bit0，一个都不能写反。
enable_a20:
    mov ax, 0x2401
    int 0x15
    in  al, 0x92
    test al, 0x02
    jnz .done
    and al, 0xFE
    or  al, 0x02
    out 0x92, al
.done:
    ret

; ---- 遍历 VBE 模式列表，挑一个最合适的 ---------------------------------------
;  评分规则：先看色深（32 位 > 24 位），再看是不是 800x600 / 640x480 这种常用分辨率。
;  一旦刷新最高分，就把该模式的参数抄进参数块——切模式之后不再回头查询，
;  少一次 BIOS 调用就少一个变量。
vbe_pick_mode:
    xor ax, ax
    mov es, ax
    mov di, VBE_INFO_ADDR
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .fail
    cmp dword [es:VBE_INFO_ADDR], 0x41534556    ; 签名必须是 "VESA"
    jne .fail

    mov ax, [es:VBE_INFO_ADDR + 0x10]           ; VideoModePtr 段
    mov [mode_seg], ax
    mov ax, [es:VBE_INFO_ADDR + 0x0E]           ; VideoModePtr 偏移
    mov [mode_off], ax

    mov word [best_mode], 0xFFFF
    mov word [best_score], 0

.next:
    mov ax, [mode_seg]
    mov fs, ax
    mov si, [mode_off]
    mov cx, [fs:si]                             ; 取一个模式号
    add word [mode_off], 2
    cmp cx, 0xFFFF                              ; 0xFFFF 表示列表结束
    je .done

    xor ax, ax
    mov es, ax
    mov di, VBE_MODE_ADDR
    push cx
    mov ax, 0x4F01                              ; 取该模式的详细信息
    int 0x10
    pop cx
    cmp ax, 0x004F
    jne .next

    mov ax, [es:VBE_MODE_ADDR + 0x00]           ; ModeAttributes
    test ax, 0x0001                             ; bit0  该模式可用
    jz .next
    test ax, 0x0010                             ; bit4  是图形模式
    jz .next
    test ax, 0x0080                             ; bit7  支持线性帧缓冲
    jz .next
    cmp byte [es:VBE_MODE_ADDR + 0x1B], 6       ; MemoryModel = 6 直接色
    jne .next
    mov ax, [es:VBE_MODE_ADDR + 0x12]           ; XResolution
    cmp ax, 640
    jb .next
    mov ax, [es:VBE_MODE_ADDR + 0x14]           ; YResolution
    cmp ax, 480
    jb .next

    mov bl, [es:VBE_MODE_ADDR + 0x19]           ; BitsPerPixel
    xor dx, dx
    cmp bl, 32
    jne .try24
    mov dx, 1000
    jmp .score_res
.try24:
    cmp bl, 24
    jne .next
    mov dx, 500

.score_res:
    mov ax, [es:VBE_MODE_ADDR + 0x12]
    cmp ax, 800
    jne .check640
    cmp word [es:VBE_MODE_ADDR + 0x14], 600
    jne .check640
    add dx, 100
    jmp .compare
.check640:
    cmp ax, 640
    jne .compare
    cmp word [es:VBE_MODE_ADDR + 0x14], 480
    jne .compare
    add dx, 50
.compare:
    cmp dx, [best_score]
    jbe .next
    mov [best_score], dx
    mov [best_mode], cx
    call save_mode_params
    jmp .next

.done:
    cmp word [best_mode], 0xFFFF
    je .fail
    ret
.fail:
    mov si, msg_vbe_fail
    call puts
    jmp halt

; ---- 把选中的模式参数抄进参数块 -----------------------------------------------
save_mode_params:
    xor ax, ax
    mov es, ax
    mov eax, [es:VBE_MODE_ADDR + 0x28]          ; PhysBasePtr 帧缓冲物理地址
    mov [es:PARAM_ADDR + P_FB], eax
    mov ax, [es:VBE_MODE_ADDR + 0x10]           ; BytesPerScanLine
    mov [es:PARAM_ADDR + P_PITCH], ax
    mov ax, [es:VBE_MODE_ADDR + 0x12]           ; XResolution
    mov [es:PARAM_ADDR + P_WIDTH], ax
    mov ax, [es:VBE_MODE_ADDR + 0x14]           ; YResolution
    mov [es:PARAM_ADDR + P_HEIGHT], ax
    mov al, [es:VBE_MODE_ADDR + 0x19]           ; BitsPerPixel
    mov [es:PARAM_ADDR + P_BPP], al
    movzx eax, al
    shr eax, 3                                  ; 每像素字节数 = 位深 / 8
    mov [es:PARAM_ADDR + P_PIXBYTES], eax
    mov dword [es:PARAM_ADDR + P_MAGIC], PARAM_MAGIC
    ret

; ---- 参数体检：宁可什么都不画，也不能拿着垃圾参数去刷内存 ---------------------
check_params:
    xor ax, ax
    mov es, ax
    cmp dword [es:PARAM_ADDR + P_MAGIC], PARAM_MAGIC
    jne .fail
    mov eax, [es:PARAM_ADDR + P_FB]
    test eax, eax
    jz .fail
    movzx eax, word [es:PARAM_ADDR + P_PITCH]
    test eax, eax
    jz .fail
    cmp eax, 8192
    ja .fail
    movzx ecx, word [es:PARAM_ADDR + P_WIDTH]
    cmp ecx, 320
    jb .fail
    cmp ecx, 4096
    ja .fail
    movzx edx, word [es:PARAM_ADDR + P_HEIGHT]
    cmp edx, 200
    jb .fail
    cmp edx, 4096
    ja .fail
    mov ebx, [es:PARAM_ADDR + P_PIXBYTES]
    cmp ebx, 2
    jb .fail
    cmp ebx, 4
    ja .fail
    imul ecx, ebx                               ; 行跨度至少得装得下一行像素
    cmp eax, ecx
    jb .fail
    ret
.fail:
    call dump_params
    jmp halt

; ---- 设置选中的 VBE 模式，bit14 要求 BIOS 用线性帧缓冲 ------------------------
vbe_set_mode:
    mov bx, [best_mode]
    or  bx, 0x4000
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .fail
    ret
.fail:
    mov si, msg_set_fail
    call puts
    jmp halt

; ---- 建 GDT，切到 32 位保护模式 -----------------------------------------------
enter_protected_mode:
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or  eax, 1                                  ; CR0.PE = 1
    mov cr0, eax
    jmp 0x08:pm_entry                           ; 远跳转，清空流水线并换段基址

; ---- 16 位辅助函数 ------------------------------------------------------------
; 用 BIOS 电传打字输出一条以 0 结尾的字符串，SI 指向字符串
puts:
    mov ah, 0x0E
    mov bh, 0x00
.next:
    lodsb
    test al, al
    jz .done
    int 0x10
    jmp .next
.done:
    ret

; 把 EAX 以 8 位十六进制打印出来，参数不对时用来定位
puthex32:
    pusha
    mov cx, 8
.next:
    rol eax, 4
    mov bx, ax
    and bx, 0x0F
    mov dl, [hex_digits + bx]
    mov ah, 0x0E
    mov al, dl
    int 0x10
    dec cx
    jnz .next
    popa
    ret

dump_params:
    xor ax, ax
    mov es, ax
    mov si, msg_bad_head
    call puts
    mov si, msg_lbl_fb
    call puts
    mov eax, [es:PARAM_ADDR + P_FB]
    call puthex32
    mov si, msg_lbl_pitch
    call puts
    movzx eax, word [es:PARAM_ADDR + P_PITCH]
    call puthex32
    mov si, msg_lbl_wh
    call puts
    movzx eax, word [es:PARAM_ADDR + P_WIDTH]
    call puthex32
    mov si, msg_sep
    call puts
    movzx eax, word [es:PARAM_ADDR + P_HEIGHT]
    call puthex32
    mov si, msg_lbl_pix
    call puts
    mov eax, [es:PARAM_ADDR + P_PIXBYTES]
    call puthex32
    mov si, msg_crlf
    call puts
    ret

; 停机：不能只写 cli + hlt，那样 VMware 会判定「客户机操作系统已禁用 CPU」。
; 先把 8259 的中断全屏蔽，再 sti，CPU 才能安安静静地歇在 hlt 上。
halt:
    mov al, 0xFF
    out 0x21, al
    out 0xA1, al
    sti
.loop:
    hlt
    jmp .loop

; ============================================================================
;  二、32 位保护模式
; ============================================================================
BITS 32
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, KERNEL_STACK_TOP

    mov dword [DIAG_ADDR + D_STAGE], 0
    call setup_idt
    mov dword [DIAG_ADDR + D_STAGE], 1
    call probe_hardware
    mov dword [DIAG_ADDR + D_STAGE], 2
    call clear_screen
    mov dword [DIAG_ADDR + D_STAGE], 3
    call draw_message
    mov dword [DIAG_ADDR + D_STAGE], 4
    call verify_framebuffer             ; 把显存读回来，确认到底画上没有
    mov dword [DIAG_ADDR + D_STAGE], 5
    jmp halt_pm

; ---- 读 SVGA 寄存器：EAX = 寄存器号 -> EAX = 值 --------------------------------
;  VMware 的 SVGA 用 0x1070（索引）/ 0x1071（值）这一对 32 位端口。读它们是安全的，
;  正好用来交叉验证 VBE 报出来的 PhysBasePtr 到底是不是真正的帧缓冲地址。
svga_read:
    mov dx, 0x1070
    out dx, eax
    mov dx, 0x1071
    in  eax, dx
    ret

; ---- 采一份硬件参数，写进诊断块，事后从 .vmem 里读出来 ------------------------
probe_hardware:
    pushad
    xor eax, eax                        ; SVGA_REG_ID
    call svga_read
    mov [DIAG_ADDR + D_SVGA_ID], eax
    mov eax, 13                         ; SVGA_REG_FB_START
    call svga_read
    mov [DIAG_ADDR + D_FBSTART], eax
    mov eax, 14                         ; SVGA_REG_FB_OFFSET
    call svga_read
    mov [DIAG_ADDR + D_FBOFFSET], eax
    mov eax, 12                         ; SVGA_REG_BYTES_PER_LINE
    call svga_read
    mov [DIAG_ADDR + D_BPL], eax
    mov eax, 2                          ; SVGA_REG_WIDTH
    call svga_read
    mov [DIAG_ADDR + D_SW], eax
    mov eax, 3                          ; SVGA_REG_HEIGHT
    call svga_read
    mov [DIAG_ADDR + D_SH], eax
    mov eax, 7                          ; SVGA_REG_BITS_PER_PIXEL
    call svga_read
    mov [DIAG_ADDR + D_SBPP], eax
    mov eax, 1                          ; SVGA_REG_ENABLE
    call svga_read
    mov [DIAG_ADDR + D_SENABLE], eax

    mov edi, [PARAM_ADDR + P_FB]        ; VBE 说的帧缓冲地址
    call probe_write
    mov [DIAG_ADDR + D_PROBE_FB], eax
    mov edi, [DIAG_ADDR + D_FBSTART]    ; SVGA 说的帧缓冲地址
    call probe_write
    mov [DIAG_ADDR + D_PROBE_ALT], eax
    popad
    ret

; ---- 在 EDI + 0x1000 处写一个花样再读回，确认这块地址到底是不是可写显存 --------
;  返回：EAX = 读回来的值（等于 0x5A5AA5A5 说明是能读写的内存）
probe_write:
    push ebx
    pushfd
    mov ebx, [edi + 0x1000]             ; 先存原值
    mov dword [edi + 0x1000], 0x5A5AA5A5
    mov eax, [edi + 0x1000]
    mov [edi + 0x1000], ebx             ; 还原
    popfd
    pop ebx
    ret

; ---- 装一张兜底 IDT：256 个门全指向同一个处理程序 -----------------------------
;  保护模式下异常若没有 IDT 兜着，就会三重故障，VMware 直接把虚拟机复位，
;  现象和「虚拟机自己重启了」一模一样，非常难查。这里先让它至少能停在原地。
setup_idt:
    mov edi, IDT_ADDR
    mov edx, idt_stub                           ; 全部指向同一个处理程序
    mov ecx, 256
.fill:
    mov [edi + 0], dx                           ; 偏移低 16 位
    mov word [edi + 2], 0x0008                  ; 代码段选择子
    mov byte [edi + 4], 0x00
    mov byte [edi + 5], 0x8E                    ; P=1, DPL=0, 32 位中断门
    shr edx, 16
    mov [edi + 6], dx                           ; 偏移高 16 位
    shl edx, 16
    add edi, 8
    dec ecx
    jnz .fill
    lidt [idt_descriptor]
    ret

; 异常处理程序：在屏幕顶上刷一条红杠再停住，一眼就能看出是「CPU 出事了」
; 而不是「画面没画出来」。32 位色下字节序是 B,G,R,X，红要写在第 3 个字节。
idt_stub:
    cli
    mov edi, [PARAM_ADDR + P_FB]
    movzx eax, word [PARAM_ADDR + P_PITCH]
    shl eax, 3                                  ; 前 8 行像素
    shr eax, 2
    mov ecx, eax
    mov eax, 0x00FF0000
    cld
    rep stosd
.hang:
    hlt
    jmp .hang

; ---- 用黑色填满整块帧缓冲 -----------------------------------------------------
clear_screen:
    cld
    mov edi, [PARAM_ADDR + P_FB]
    movzx eax, word [PARAM_ADDR + P_PITCH]
    movzx ecx, word [PARAM_ADDR + P_HEIGHT]
    imul ecx, eax                               ; 总字节数 = 行跨度 x 行数
    cmp ecx, 16 * 1024 * 1024                   ; 兜底上限，参数再离谱也只刷 16MB
    jbe .ok
    mov ecx, 16 * 1024 * 1024
.ok:
    shr ecx, 2
    xor eax, eax
    rep stosd
    ret

; ---- 把整句话水平、垂直居中画出来 ---------------------------------------------
draw_message:
    movzx eax, word [PARAM_ADDR + P_WIDTH]
    mov ecx, FONT_TEXT_W
    cmp eax, ecx                                ; 屏幕比整句话还窄就贴左边，别算出负数
    ja .fit
    xor eax, eax
    jmp .store
.fit:
    sub eax, ecx
    shr eax, 1
.store:
    mov [cursor_x], eax

    movzx eax, word [PARAM_ADDR + P_HEIGHT]
    sub eax, FONT_GLYPH_H
    shr eax, 1
    mov [cursor_y], eax

    mov eax, [cursor_x]
    mov [DIAG_ADDR + D_CX], eax
    mov [msg_x0], eax
    push eax
    mov eax, [cursor_y]
    mov [DIAG_ADDR + D_CY], eax
    mov [msg_y0], eax
    shl eax, 16
    pop edx
    and edx, 0x0000FFFF
    or  eax, edx
    mov [DIAG_ADDR + D_MSGBOX], eax

    mov esi, font_bitmap
    mov ebp, FONT_CHAR_NUM
.char_loop:
    call draw_glyph
    add esi, FONT_GLYPH_LEN
    add dword [cursor_x], FONT_STEP
    dec ebp
    jnz .char_loop
    ret

; ---- 画一个字：ESI 指向字模，位置取自 cursor_x / cursor_y ----------------------
draw_glyph:
    pushad
    mov edi, [PARAM_ADDR + P_FB]
    movzx eax, word [PARAM_ADDR + P_PITCH]
    mov ecx, [cursor_y]
    imul ecx, eax
    add edi, ecx                                ; 先加上纵向偏移
    mov ecx, [PARAM_ADDR + P_PIXBYTES]
    mov eax, [cursor_x]
    imul eax, ecx
    add edi, eax                                ; 再加上横向偏移

    mov ebp, FONT_GLYPH_H
.row_loop:
    mov ebx, FONT_ROW_BYTES
    mov edx, edi                                ; EDX = 本行的写入指针
.byte_loop:
    movzx eax, byte [esi]                       ; 取本行的一个字节
    inc esi
    mov ecx, 8                                  ; 一个字节 = 8 个像素
.bit_loop:
    shl al, 1                                   ; 必须用 8 位移位！进位取的是 AL 的 bit7；
                                                ; 写成 shl eax,1 的话进位永远来自 bit31，
                                                ; 而 EAX 只有 8 位有效数据，结果一个像素都不会画。
    jnc .advance
    cmp dword [PARAM_ADDR + P_PIXBYTES], 4
    jne .px24
    mov dword [edx], 0x00FFFFFF                 ; 32 位色：白
    jmp .advance
.px24:
    mov byte [edx + 0], 0xFF                    ; 24 位色：BGR 三字节全白
    mov byte [edx + 1], 0xFF
    mov byte [edx + 2], 0xFF
.advance:
    add edx, [PARAM_ADDR + P_PIXBYTES]
    dec ecx
    jnz .bit_loop
    dec ebx
    jnz .byte_loop

    movzx eax, word [PARAM_ADDR + P_PITCH]
    add edi, eax                                ; 换到下一行
    dec ebp
    jnz .row_loop
    popad
    ret

; ---- 显存自检：把刚写进去的东西读回来数一遍 -------------------------------------
;  这一招能一刀切开「没画上去」和「画上去了但没显示」两种故障：
;  数出来有白像素就说明显存里的画是对的，问题只可能在显示刷新那一段。
verify_framebuffer:
    pushad
    mov edi, [PARAM_ADDR + P_FB]                ; 文字包围盒
    movzx eax, word [PARAM_ADDR + P_PITCH]
    mov ecx, [msg_y0]
    imul ecx, eax
    add edi, ecx
    mov eax, [msg_x0]
    shl eax, 2
    add edi, eax
    mov ecx, FONT_GLYPH_H
    xor ebx, ebx
.txt_row:
    mov edx, FONT_TEXT_W
.txt_col:
    cmp dword [edi], 0x00FFFFFF
    jne .txt_next
    inc ebx
.txt_next:
    add edi, 4
    dec edx
    jnz .txt_col
    movzx eax, word [PARAM_ADDR + P_PITCH]
    sub edi, FONT_TEXT_W * 4
    add edi, eax
    dec ecx
    jnz .txt_row
    mov [DIAG_ADDR + D_TEXTPIX], ebx
    popad
    ret

; ---- 停机 ---------------------------------------------------------------------
halt_pm:
    mov al, 0xFF
    out 0x21, al
    out 0xA1, al
    sti
.loop:
    hlt
    jmp .loop

; ============================================================================
;  数据
; ============================================================================
align 8
gdt_begin:
    dq 0x0000000000000000                       ; 空描述符
gdt_code:                                       ; 选择子 0x08
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00
gdt_data:                                       ; 选择子 0x10
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
gdt_end:
gdt_descriptor:
    dw gdt_end - gdt_begin - 1
    dd gdt_begin

idt_descriptor:
    dw 256 * 8 - 1
    dd IDT_ADDR

best_mode  dw 0xFFFF
best_score dw 0
mode_seg   dw 0
mode_off   dw 0
cursor_x   dd 0
cursor_y   dd 0
msg_x0     dd 0
msg_y0     dd 0

hex_digits  db "0123456789ABCDEF", 0
msg_vbe_fail db "VBE not available", 13, 10, 0
msg_set_fail db "VBE set mode failed", 13, 10, 0
msg_bad_head db "bad video params:", 13, 10, 0
msg_lbl_fb   db "  FB    = 0x", 0
msg_lbl_pitch db 13, 10, "  PITCH = 0x", 0
msg_lbl_wh   db 13, 10, "  W     = 0x", 0
msg_sep      db 13, 10, "  H     = 0x", 0
msg_lbl_pix  db 13, 10, "  PIXB  = 0x", 0
msg_crlf     db 13, 10, 0

%include "font.inc"