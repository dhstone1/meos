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
;        6. 采一份硬件参数写进诊断块（0x6200），清屏、在顶部画中文横幅；
;        7. 重映射 8259，开 IRQ0（定时器，给光标闪烁提供节拍）和 IRQ1（键盘）；
;        8. 把屏幕当成 16x24 点阵的文本控制台驱动（画字、换行、滚屏、光标）；
;        9. 进命令行主循环：中断只往环形缓冲里丢字符，
;           行编辑与命令分发在主循环里做。
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

KBD_RING_SIZE   equ 64             ; 键盘环形缓冲，主循环一轮就能喝干
KBD_RING_MASK   equ KBD_RING_SIZE - 1
LINE_MAX        equ 128            ; 一行最多 127 个字符
BANNER_Y        equ 16             ; 中文横幅贴在顶部，下面整块留给命令行
CONSOLE_TOP_ROW equ 3              ; 命令行从第 3 个文字行开始（横幅占 16..48 像素）

; 诊断块内部偏移
D_STAGE     equ 0x00               ; dword 执行到第几步（0..6，对照 tools/vmdiag.py 的 STAGES 表）
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
D_KB_IRQ    equ 0x40               ; dword 收到的键盘中断次数
D_KB_LAST   equ 0x44               ; dword 最近一次原始扫描码
D_KB_CHARS  equ 0x48               ; dword 翻译成字符的按键次数
D_TICKS     equ 0x4C               ; dword 定时器节拍数
D_CON_X     equ 0x50               ; dword 光标所在列
D_CON_Y     equ 0x54               ; dword 光标所在行
D_LINE_LEN  equ 0x58               ; dword 当前输入行长度
D_CMD_NUM   equ 0x5C               ; dword 执行过的命令行数
D_KB_SHIFT  equ 0x60               ; dword Shift 是否按住
D_KB_HIST   equ 0x64               ; 8 个 dword：最近 8 个原始扫描码，最新的在最前
D_CONPIX   equ 0x84               ; dword 整屏白像素数（自检用）
D_ROWPIX    equ 0x88               ; 25 个 dword：每个文字行各有多少个白像素

; 网络（详见 net.inc）
D_NET_STAGE   equ 0x100            ; dword 网卡初始化进度（0..9）
D_NET_VENDOR  equ 0x104            ; dword PCI vendor id
D_NET_DEVICE  equ 0x108            ; dword PCI device id
D_NET_BUSDEV  equ 0x10C            ; dword 设备号<<11 | 功能号<<8
D_NET_MMIO    equ 0x110            ; dword BAR0（MMIO 基址）
D_NET_IRQ     equ 0x114            ; dword PCI 中断线
D_NET_MAC     equ 0x118            ; 6 字节 MAC
D_NET_LINK    equ 0x120            ; dword 链路状态寄存器
D_NET_TX      equ 0x124            ; dword 发出的以太网帧数
D_NET_RX      equ 0x128            ; dword 收到的以太网帧数
D_NET_GWMAC   equ 0x12C            ; 6 字节 网关 MAC
D_NET_ARP_RX  equ 0x134            ; dword 收到的 ARP 应答数
D_NET_ICMP_TX equ 0x138            ; dword 发出的 ICMP 请求数
D_NET_ICMP_RX equ 0x13C            ; dword 收到的 ICMP 应答数
D_NET_DNS_IP  equ 0x140            ; dword DNS 解析出的 IP
D_NET_DNS_OK  equ 0x144            ; dword 1 = 解析成功
D_NET_LASTIP  equ 0x148            ; dword 最近一次 ping 的目标
D_NET_RTT     equ 0x14C            ; dword 最近一次 ping 的往返毫秒
D_NET_LOCALIP equ 0x150            ; dword 本机 IP
D_NET_ERR     equ 0x154            ; dword 最近一次错误码
D_NET_SCAN    equ 0x158            ; 8 个 dword：总线 0 上扫到的前 8 个设备（vendor | device<<16）
D_NET_PKTLEN  equ 0x180            ; dword 最近收到那一帧的长度
D_NET_PKT     equ 0x184            ; 16 个 dword：最近收到那一帧的前 64 字节

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
    call setup_idt                      ; 兜底门 + 键盘 / 定时器两个真门
    mov dword [DIAG_ADDR + D_STAGE], 1
    call probe_hardware
    call net_probe                      ; 找网卡，先把 PCI 信息记下来
    mov dword [DIAG_ADDR + D_STAGE], 2
    call con_init
    call clear_screen
    mov dword [DIAG_ADDR + D_STAGE], 3
    call draw_message                   ; 顶部的中文横幅
    mov dword [DIAG_ADDR + D_STAGE], 4
    call verify_framebuffer             ; 把显存读回来，确认到底画上没有
    mov dword [DIAG_ADDR + D_STAGE], 5
    mov dword [con_x], 0
    mov dword [con_y], CONSOLE_TOP_ROW  ; 横幅下面开始接管终端
    call pic_remap
    call shell_start
    call diag_snapshot                  ; 开机时的状态先记一次：没输入时它就不会变了
    call verify_console                 ; 开机就把自己画的东西数一遍，写进诊断块
    mov dword [DIAG_ADDR + D_STAGE], 6
    call pit_init_1000                  ; 定时器改成 1000Hz，tick 即毫秒
    sti                                 ; 到这里一切都就绪了，可以开中断
    call net_start                      ; 网络启用（要等中断开了才能计时）
    jmp shell_loop

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
    mov eax, 0x20                       ; IRQ0 定时器走真处理程序
    mov edx, timer_isr
    call set_idt_gate
    mov eax, 0x21                       ; IRQ1 键盘
    mov edx, kbd_isr
    call set_idt_gate
    lidt [idt_descriptor]
    ret

; ---- 往 IDT 里装一个中断门：EAX = 向量号，EDX = 处理程序地址 ----------------
set_idt_gate:
    push ebx
    push ecx
    push edx
    mov ecx, eax
    shl ecx, 3
    add ecx, IDT_ADDR
    mov [ecx + 0], dx
    mov word [ecx + 2], 0x0008
    mov byte [ecx + 4], 0x00
    mov byte [ecx + 5], 0x8E            ; P=1, DPL=0, 32 位中断门
    shr edx, 16
    mov [ecx + 6], dx
    pop edx
    pop ecx
    pop ebx
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

; ---- 把整句话水平居中、贴顶画出来 ---------------------------------------------
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

    mov eax, BANNER_Y                           ; 横幅贴顶，下面整块留给命令行
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

; ============================================================================
;  三、中断：8259 重映射 + 键盘 / 定时器
; ============================================================================
;  保护模式下的外部中断要走 8259。它上电默认把 IRQ0..7 映射到 INT 08h..0Fh，
;  正好压着 CPU 自己的异常向量，所以必须先重映射到 0x20 以上，再按需要放开某几条线。
;  这一阶段只用两条：IRQ0 定时器（给光标闪烁提供节拍）、IRQ1 键盘。

pic_remap:
    pushad
    mov al, 0x11                        ; ICW1：边沿触发、后面还要跟 ICW4
    out 0x20, al
    out 0xA0, al
    mov al, 0x20                        ; ICW2：主片的 IRQ0 -> INT 0x20
    out 0x21, al
    mov al, 0x28                        ; ICW2：从片的 IRQ0 -> INT 0x28
    out 0xA1, al
    mov al, 0x04                        ; ICW3：从片挂在主片的 IRQ2 上
    out 0x21, al
    mov al, 0x02
    out 0xA1, al
    mov al, 0x01                        ; ICW4：8086/88 模式
    out 0x21, al
    out 0xA1, al
    mov al, 0xFC                        ; 只放开 IRQ0、IRQ1，其余全屏蔽
    out 0x21, al
    mov al, 0xFF
    out 0xA1, al
    popad
    ret

; ---- 把 PIT 调到 1000Hz：这样 D_TICKS 就是毫秒 --------------------------------
;  默认 18.2Hz 太粗，ping 的往返时间根本量不出来。通道 0、模式 3（方波）、
;  除数 = 1193182 / 1000 = 1193。
pit_init_1000:
    mov al, 0x36                        ; 通道0 | 先低后高 | 模式3 | 二进制
    out 0x43, al
    mov ax, 1193
    out 0x40, al                        ; 低字节
    mov al, ah
    out 0x40, al                        ; 高字节
    ret

; ---- 定时器中断（IRQ0 -> INT 0x20）：数节拍，顺带让光标闪 --------------------
timer_isr:
    pushad
    inc dword [DIAG_ADDR + D_TICKS]
    mov eax, [DIAG_ADDR + D_TICKS]
    test eax, 511                       ; PIT 现在是 1000Hz，512 拍约 0.51 秒
    jnz .eoi
    cmp byte [cur_visible], 0
    je .paint
    call con_erase_cursor
    jmp .eoi
.paint:
    call con_draw_cursor
.eoi:
    mov al, 0x20                        ; 告诉 8259：这条中断处理完了
    out 0x20, al
    popad
    iretd

; ---- 键盘中断（IRQ1 -> INT 0x21）--------------------------------------------
;  从端口 0x60 读扫描码。8042 默认开着翻译，所以拿到的是「扫描码集 1」：
;    低 7 位是键号，最高位为 1 表示松键；0xE0 前缀表示扩展键（方向键那类）。
;  这里只认能产生字符的键，把结果塞进环形缓冲，真正的处理放在主循环里做——
;  中断里做的事越少越好。
kbd_isr:
    pushad
    mov dx, 0x60
    in  al, dx
    movzx ebx, al
    inc dword [DIAG_ADDR + D_KB_IRQ]
    mov [DIAG_ADDR + D_KB_LAST], ebx

    ; 扫描码历史：整体后移一格，新的放最前。
    ; 只看最后一个扫描码是不够的——只有看到整串才知道
    ; 到底是没收到按键，还是收到了但译码不对。
    mov ecx, 7
.hist:
    mov eax, [DIAG_ADDR + D_KB_HIST + ecx * 4 - 4]
    mov [DIAG_ADDR + D_KB_HIST + ecx * 4], eax
    dec ecx
    jnz .hist
    mov [DIAG_ADDR + D_KB_HIST], ebx

    mov al, bl
    call kbd_handle_scancode
    movzx eax, byte [kb_shift]
    mov [DIAG_ADDR + D_KB_SHIFT], eax
    mov al, 0x20                        ; 告诉 8259：这条中断处理完了
    out 0x20, al
    popad
    iretd

; ---- 扫描码译码：AL = 扫描码 ------------------------------------------------
;  译码单独拆成一个函数，是为了让中断和开机自检走**同一份**代码：
;  否则自检只能验证另写的一份副本，验了等于没验。
;  扫描码集 1：低 7 位是键号，最高位为 1 表示松键，0xE0 前缀表示扩展键。
kbd_handle_scancode:
    push eax
    push ebx
    movzx ebx, al

    cmp bl, 0xE0                        ; 扩展键前缀：记住，下个字节一起丢掉
    jne .not_ext
    mov byte [kb_ext], 1
    jmp .done
.not_ext:
    mov al, bl
    and al, 0x7F                        ; 去掉松键标志，留下键号
    test bl, 0x80
    jz .make

    ; ---- 松键：只有 Shift 需要关心 ----
    cmp al, 0x2A
    je .shift_up
    cmp al, 0x36
    je .shift_up
    jmp .done
.shift_up:
    mov byte [kb_shift], 0
    jmp .done

    ; ---- 按下 ----
.make:
    cmp al, 0x2A
    je .shift_down
    cmp al, 0x36
    je .shift_down
    cmp al, 0x3A
    je .caps
    cmp byte [kb_ext], 0
    jne .done                           ; 扩展键本阶段不处理
    movzx eax, al
    cmp byte [kb_shift], 0
    je .use_lo
    mov al, [kbd_map_hi + eax]
    jmp .have_char
.use_lo:
    mov al, [kbd_map_lo + eax]
.have_char:
    test al, al
    jz .done                            ; 这个键不产生字符（Shift、F1……）
    call kbd_apply_caps
    inc dword [DIAG_ADDR + D_KB_CHARS]
    call kbd_ring_push
    jmp .done
.shift_down:
    mov byte [kb_shift], 1
    jmp .done
.caps:
    xor byte [kb_caps], 1               ; 大写锁定：按一下翻一次
.done:
    mov byte [kb_ext], 0
    pop ebx
    pop eax
    ret


; ---- 大写锁定：只翻字母的大小写，符号不受影响 --------------------------------
kbd_apply_caps:
    cmp byte [kb_caps], 0
    je .done
    cmp al, 'a'
    jb .upper
    cmp al, 'z'
    ja .done
    sub al, 0x20
    ret
.upper:
    cmp al, 'A'
    jb .done
    cmp al, 'Z'
    ja .done
    add al, 0x20
.done:
    ret

; ---- 字符入环形缓冲：AL = 字符 ------------------------------------------------
;  满了一律丢掉，宁可丢键也不能覆盖还没读走的内容。
kbd_ring_push:
    push eax
    push ebx
    push edx
    mov edx, [kbd_head]
    lea ebx, [edx + 1]
    and ebx, KBD_RING_MASK
    cmp ebx, [kbd_tail]
    je .full
    mov [kbd_ring + edx], al
    mov [kbd_head], ebx
.full:
    pop edx
    pop ebx
    pop eax
    ret

; ---- 从环形缓冲取字符：EAX = 字符，-1 表示没有新字符 --------------------------
kbd_pop:
    mov edx, [kbd_tail]
    cmp edx, [kbd_head]
    je .empty
    movzx eax, byte [kbd_ring + edx]
    inc edx
    and edx, KBD_RING_MASK
    mov [kbd_tail], edx
    ret
.empty:
    mov eax, -1
    ret

; ============================================================================
;  四、文本控制台
; ============================================================================
;  屏幕按 16x24 像素切成格子，800x600 正好 50 列 25 行。
;  所有画字都是「定位到格子 -> 刷黑底 -> 铺点阵」，没有硬件滚动寄存器可用，
;  滚屏就是把显存整体往上搬一个文字行。

; ---- 按帧缓冲参数算出控制台尺寸 ----------------------------------------------
con_init:
    movzx eax, word [PARAM_ADDR + P_WIDTH]
    xor edx, edx
    mov ecx, ASCII_CELL_W
    div ecx
    mov [con_cols], eax
    movzx eax, word [PARAM_ADDR + P_HEIGHT]
    xor edx, edx
    mov ecx, ASCII_CELL_H
    div ecx
    mov [con_rows], eax
    ret

; ---- 输入：EBX = 列，ECX = 行；输出：EAX = 该格子左上角在显存里的地址 --------
;  只动 EAX，EDX / EBX / ECX 一律保持原样，方便调用方连着用。
con_cell_addr:
    push edx
    mov eax, ecx
    imul eax, ASCII_CELL_H
    movzx edx, word [PARAM_ADDR + P_PITCH]
    imul eax, edx
    push eax
    mov eax, ebx
    imul eax, ASCII_CELL_W
    mov edx, [PARAM_ADDR + P_PIXBYTES]
    imul eax, edx
    pop edx
    add eax, edx
    add eax, [PARAM_ADDR + P_FB]
    pop edx
    ret

; ---- 输入：EDI = 起点，EBP = 行数，EDX = 颜色；刷满一个格子宽 -----------------
fill_rows_edi:
    push eax
    push ecx
    push esi
.row:
    mov esi, edi
    mov ecx, ASCII_CELL_W
.col:
    cmp dword [PARAM_ADDR + P_PIXBYTES], 4
    jne .px24
    mov [esi], edx
    jmp .next
.px24:
    mov [esi + 0], dl
    mov [esi + 1], dh
    mov eax, edx
    shr eax, 16
    mov [esi + 2], al
.next:
    add esi, [PARAM_ADDR + P_PIXBYTES]
    dec ecx
    jnz .col
    movzx eax, word [PARAM_ADDR + P_PITCH]
    add edi, eax
    dec ebp
    jnz .row
    pop esi
    pop ecx
    pop eax
    ret

; ---- 输入：EDI = 单元格地址，EDX = 颜色 --------------------------------------
fill_cell_edi:
    mov ebp, ASCII_CELL_H
    jmp fill_rows_edi

; ---- 输入：EBX = 列，ECX = 行，EDX = 颜色 ------------------------------------
con_fill_cell:
    pushad
    call con_cell_addr
    mov edi, eax
    call fill_cell_edi
    popad
    ret

; ---- 输入：EDI = 单元格地址，AL = 字符 ---------------------------------------
;  只铺点阵不擦背景，越界的字符直接当空白（调用方已经刷过黑底）。
draw_char_edi:
    pushad
    movzx eax, al
    sub eax, ASCII_FIRST
    jb .done
    cmp eax, ASCII_COUNT
    jae .done
    mov ecx, ASCII_GLYPH_LEN
    imul eax, ecx
    lea esi, [ascii_font + eax]

    mov ebp, ASCII_CELL_H
.row:
    mov ebx, ASCII_ROW_BYTES
    mov edx, edi
.byte:
    movzx eax, byte [esi]
    inc esi
    mov ecx, 8
.bit:
    shl al, 1                           ; 同样必须是 8 位移位，理由见 draw_glyph
    jnc .next
    cmp dword [PARAM_ADDR + P_PIXBYTES], 4
    jne .px24
    mov dword [edx], 0x00FFFFFF
    jmp .next
.px24:
    mov byte [edx + 0], 0xFF
    mov byte [edx + 1], 0xFF
    mov byte [edx + 2], 0xFF
.next:
    add edx, [PARAM_ADDR + P_PIXBYTES]
    dec ecx
    jnz .bit
    dec ebx
    jnz .byte
    movzx eax, word [PARAM_ADDR + P_PITCH]
    add edi, eax
    dec ebp
    jnz .row
.done:
    popad
    ret

; ---- 输入：AL = 字符，EBX = 列，ECX = 行 -------------------------------------
con_putc_at:
    pushad
    mov [con_ch], al
    call con_cell_addr
    push eax                            ; 先存一份，等下铺点阵还要用
    mov edi, eax
    xor edx, edx
    call fill_cell_edi                  ; 刷黑底
    pop edi
    mov al, [con_ch]
    call draw_char_edi                  ; 再铺点阵
    popad
    ret

; ---- 光标：格子底部两行刷一条白杠 --------------------------------------------
con_draw_cursor:
    mov byte [cur_visible], 1
    mov edx, 0x00FFFFFF
    call cursor_bar
    ret

con_erase_cursor:
    mov byte [cur_visible], 0
    xor edx, edx
    call cursor_bar
    ret

cursor_bar:
    push eax
    push ebx
    push ecx
    push edi
    mov ebx, [con_x]
    mov ecx, [con_y]
    call con_cell_addr
    mov edi, eax
    movzx eax, word [PARAM_ADDR + P_PITCH]
    imul eax, ASCII_CELL_H - 2
    add edi, eax                        ; 挪到格子底部两行
    mov ebp, 2
    call fill_rows_edi
    pop edi
    pop ecx
    pop ebx
    pop eax
    ret

; ---- 滚屏：显存整体上移一个文字行，再把最后一行刷黑 --------------------------
con_scroll:
    pushad
    movzx eax, word [PARAM_ADDR + P_PITCH]
    imul eax, ASCII_CELL_H              ; EAX = 一个文字行占多少字节
    mov edx, eax
    mov ebx, [con_rows]
    dec ebx
    imul ebx, eax                       ; EBX = 要搬的总字节数

    mov esi, [PARAM_ADDR + P_FB]
    add esi, edx                        ; 源：第 1 个文字行
    mov edi, [PARAM_ADDR + P_FB]        ; 目标：第 0 个文字行
    mov ecx, ebx
    shr ecx, 2                          ; 行跨度是 4 的倍数，按双字搬就够
    cld
    rep movsd

    mov edi, [PARAM_ADDR + P_FB]
    mov eax, edx
    mov ebx, [con_rows]
    dec ebx
    imul eax, ebx
    add edi, eax                        ; 最后一行
    mov ecx, edx
    shr ecx, 2
    xor eax, eax
    rep stosd
    popad
    ret

; ---- 换行：列归零、行加一，到底了就先滚屏 ------------------------------------
con_newline:
    pushad
    mov dword [con_x], 0
    inc dword [con_y]
    mov eax, [con_rows]
    cmp [con_y], eax
    jb .done
    call con_scroll
    dec dword [con_y]
.done:
    popad
    ret

; ---- 清空整个控制台区域 ------------------------------------------------------
con_clear_all:
    pushad
    call con_erase_cursor
    mov edi, [PARAM_ADDR + P_FB]
    movzx eax, word [PARAM_ADDR + P_PITCH]
    mov ecx, [con_rows]
    imul ecx, ASCII_CELL_H
    imul ecx, eax
    shr ecx, 2
    cmp ecx, 16 * 1024 * 1024 / 4       ; 兜底：参数再离谱也不刷超过 16MB
    jbe .ok
    mov ecx, 16 * 1024 * 1024 / 4
.ok:
    xor eax, eax
    cld
    rep stosd
    mov dword [con_x], 0
    mov dword [con_y], 0
    mov byte [cur_visible], 0
    popad
    ret

; ---- 输出一个字符：AL = 字符 -------------------------------------------------
;  认得回车、换行、退格和可打印字符，其余忽略。
con_putc:
    pushad
    mov [con_ch], al
    cmp al, 13
    je .newline
    cmp al, 10
    je .newline
    cmp al, 8
    je .backspace
    cmp al, ASCII_FIRST
    jb .done
    cmp al, ASCII_LAST
    ja .done

    call con_erase_cursor
    mov ebx, [con_x]
    mov ecx, [con_y]
    mov al, [con_ch]
    call con_putc_at
    inc dword [con_x]
    mov eax, [con_cols]
    cmp [con_x], eax
    jb .draw_cursor
    call con_newline                    ; 写满一行自动折行
    jmp .draw_cursor

.backspace:
    cmp dword [con_x], 0
    je .done                            ; 这一行的开头，退无可退
    call con_erase_cursor
    dec dword [con_x]
    mov ebx, [con_x]
    mov ecx, [con_y]
    xor edx, edx
    call con_fill_cell                  ; 把那个字符擦掉
    jmp .draw_cursor

.newline:
    call con_erase_cursor
    call con_newline
.draw_cursor:
    call con_draw_cursor
.done:
    popad
    ret

; ---- 换行（会管光标） ------------------------------------------------------
;  con_newline 只管把位置挪到下一行，它不碰光标——光标是 con_putc 的事。
;  上层想换行必须用这个，否则光标横杠会留在原地没人擦。
con_crlf:
    push eax                            ; 必须保护 EAX：调用方常常刚算出一个值，
    mov al, 13                          ; 紧接着就要换行，EAX 被回车符冲掉的话
    call con_putc                       ; 会变成很难查的错（踩过：IP 首字节被改成 13）
    pop eax
    ret

; ---- 输出一个以 0 结尾的字符串：ESI = 字符串 ---------------------------------
con_puts:
    pushad
.next:
    mov al, [esi]
    test al, al
    jz .done
    inc esi
    call con_putc
    jmp .next
.done:
    popad
    ret

; ============================================================================
;  五、命令行
; ============================================================================
;  中断只负责把字符丢进环形缓冲，行编辑和命令分发都在主循环里做。
;  这样即使某条命令跑得慢，键盘也不会丢键。

; ---- 打提示符，准备接收下一行 -------------------------------------------------
shell_start:
    mov dword [line_len], 0
    mov byte [line_buf], 0
    mov esi, msg_prompt
    call con_puts
    ret

; ---- 往输入行追加一个字符：AL = 字符 -----------------------------------------
line_append:
    mov ecx, [line_len]
    cmp ecx, LINE_MAX - 1
    jae .full
    mov [line_buf + ecx], al
    inc dword [line_len]
    mov byte [line_buf + ecx + 1], 0    ; 一直保持 0 结尾，取出来就能直接当字符串用
.full:
    ret

; ---- 退格：删掉行尾一个字符，屏幕上也擦掉 -------------------------------------
line_backspace:
    cmp dword [line_len], 0
    je .done
    dec dword [line_len]
    mov ecx, [line_len]
    mov byte [line_buf + ecx], 0
    mov al, 8
    call con_putc
.done:
    ret

; ---- 主循环 ------------------------------------------------------------------
; ---- 开机自检喂码：返回 EAX = 1 表示刚喂进一个扫描码 --------------------------
;  正常构建里它就是个空函数。带 -dSELFTEST=1 编译时，它会把一串预置扫描码
;  逐个喂进 kbd_handle_scancode——和真键盘中断走的是同一条路，区别只在谁调用它。
;  这么做是因为：宕机侧没办法给 VMware 注入按键（合成输入会被丢掉），
;  而译码、行编辑、终端渲染这一整条链路仍然值得被确定性地验证。
selftest_tick:
    xor eax, eax
%ifdef SELFTEST
    mov ecx, [selftest_ptr]
    cmp ecx, selftest_len
    jae .done
    movzx edx, byte [selftest_data + ecx]
    inc dword [selftest_ptr]
    test dl, dl
    jz .done                            ; 0 当空档，这一拍什么都不喂
    mov al, dl
    call kbd_handle_scancode
    mov eax, 1
.done:
%endif
    ret

; ---- 整屏自检：数一数整个屏幕上有多少个白像素 ------------------------------
;  和横幅那个自检是同一个套路，只是范围从横幅扩到整屏。
;  宿主机那边只要把「该出现的字符」模拟一遍算出期望值，
;  两边一对，就能证明——不是“屏幕上好像有字”，而是“该画的一个像素不差”。
verify_console:
    pushad
    cmp dword [PARAM_ADDR + P_PIXBYTES], 4
    jne .done                           ; 只会数 32 位色，别的位深不装
    pushfd                              ; 光标会闪，数的时候先把中断关了；
    cli                                 ; 用 pushfd/popfd 而不是 cli/sti，
                                        ; 免得把本来还没开的中断给提前打开了
    call con_erase_cursor
    xor ebx, ebx                        ; 整屏总数
    xor ebp, ebp                        ; 第几个文字行
.row:
    mov edi, [PARAM_ADDR + P_FB]
    movzx eax, word [PARAM_ADDR + P_PITCH]
    mov ecx, ebp
    imul ecx, ASCII_CELL_H
    imul ecx, eax
    add edi, ecx                        ; 这一行的起点
    movzx edx, word [PARAM_ADDR + P_PITCH]
    shr edx, 2
    imul edx, ASCII_CELL_H              ; 这一行有多少个像素
    xor esi, esi
.col:
    cmp dword [edi], 0x00FFFFFF
    jne .next
    inc esi
.next:
    add edi, 4
    dec edx
    jnz .col
    mov [DIAG_ADDR + D_ROWPIX + ebp * 4], esi
    add ebx, esi
    inc ebp
    cmp ebp, [con_rows]
    jb .row
    mov [DIAG_ADDR + D_CONPIX], ebx
    call con_draw_cursor
    popfd
.done:
    popad
    ret

; ---- 把控制台状态抄进诊断块，方便宿主机上的 vmdiag.py 直接读 ------
;  放在空转之前做：这时候主循环没有半截状态，抄出来的数最准。
diag_snapshot:
%ifdef SELFTEST
    ; 自检流喂完、而且这一轮已经没有新字符要处理了，
    ; 就做一次整屏自检。放在这里是因为它只会在空转时跑。
    cmp byte [selftest_verified], 0
    jne .skip
    mov eax, [selftest_ptr]
    cmp eax, selftest_len
    jb .skip
    mov byte [selftest_verified], 1
    call verify_console
.skip:
%endif
    mov eax, [con_x]
    mov [DIAG_ADDR + D_CON_X], eax
    mov eax, [con_y]
    mov [DIAG_ADDR + D_CON_Y], eax
    mov eax, [line_len]
    mov [DIAG_ADDR + D_LINE_LEN], eax
    ret

shell_loop:
    call selftest_tick                  ; 自检模式下在这里逐个喂扫描码
    test eax, eax
    jnz .have_input
    hlt                                 ; 中断开着，按一个键就会醒
.have_input:
    call kbd_pop
    cmp eax, -1
    je shell_loop
    cmp al, 13
    je .enter
    cmp al, 8
    je .back
    cmp al, 9
    je .tab
    cmp al, ASCII_FIRST
    jb .idle
    cmp al, ASCII_LAST
    ja .idle
    call line_append
    call con_putc
    jmp .idle
.enter:
    call con_crlf
    call shell_exec
    call shell_start
    jmp .idle
.back:
    call line_backspace
    jmp .idle
.tab:
    mov al, ' '
    call line_append
    call con_putc
.idle:
    call diag_snapshot
    jmp shell_loop

; ---- 执行当前输入行 ----------------------------------------------------------
shell_exec:
    pushad
    inc dword [DIAG_ADDR + D_CMD_NUM]
    cmp dword [line_len], 0
    je .done

    mov esi, line_buf
    mov edi, cmd_help
    call str_eq
    test eax, eax
    jnz .help

    mov esi, line_buf
    mov edi, cmd_ver
    call str_eq
    test eax, eax
    jnz .ver

    mov esi, line_buf
    mov edi, cmd_cls
    call str_eq
    test eax, eax
    jnz .cls

    mov esi, line_buf
    mov edi, cmd_ping
    call str_eq
    test eax, eax
    jz .try_ping
    mov esi, msg_ping_usage
    call con_puts
    call con_crlf
    jmp .done
.try_ping:
    mov esi, line_buf
    mov edi, cmd_ping
    mov ecx, 4
    call str_n_eq
    test eax, eax
    jz .try_net
    cmp byte [line_buf + 4], ' '
    jne .try_net
    mov esi, line_buf + 5
    call cmd_do_ping
    jmp .done
.try_net:
    mov esi, line_buf
    mov edi, cmd_net
    call str_eq
    test eax, eax
    jz .try_echo
    call cmd_do_net
    jmp .done
.try_echo:
    mov esi, line_buf
    mov edi, cmd_echo
    mov ecx, 4
    call str_n_eq
    test eax, eax
    jz .unknown
    movzx eax, byte [line_buf + 4]      ; "echo" 后面要么就此结束，要么跟个空格
    test al, al
    jz .echo_blank
    cmp al, ' '
    jne .unknown
    mov esi, line_buf + 5
    call con_puts
    call con_crlf
    jmp .done
.echo_blank:
    call con_crlf
    jmp .done

.help:
    mov esi, msg_help
    call con_puts
    call con_crlf
    jmp .done
.ver:
    mov esi, msg_ver
    call con_puts
    call con_crlf
    jmp .done
.cls:
    call con_clear_all
    jmp .done
.unknown:
    mov esi, msg_unknown
    call con_puts
    mov esi, line_buf
    call con_puts
    call con_crlf
.done:
    popad
    ret

; ---- 字符串比较：ESI、EDI 都以 0 结尾；EAX = 1 表示相同 ----------------------
str_eq:
    push ebx
    push esi
    push edi
.loop:
    mov al, [esi]
    mov bl, [edi]
    cmp al, bl
    jne .no
    test al, al
    jz .yes
    inc esi
    inc edi
    jmp .loop
.no:
    xor eax, eax
    pop edi
    pop esi
    pop ebx
    ret
.yes:
    mov eax, 1
    pop edi
    pop esi
    pop ebx
    ret

; ---- 只比前 ECX 个字符：ESI、EDI；EAX = 1 表示相同 ---------------------------
str_n_eq:
    push ebx
    push esi
    push edi
.loop:
    test ecx, ecx
    jz .yes
    mov al, [esi]
    mov bl, [edi]
    cmp al, bl
    jne .no
    inc esi
    inc edi
    dec ecx
    jmp .loop
.no:
    xor eax, eax
    pop edi
    pop esi
    pop ebx
    ret
.yes:
    mov eax, 1
    pop edi
    pop esi
    pop ebx
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

; ---- 键盘扫描码表（集 1）：下标就是去掉最高位的扫描码 -------------------------
;  两张表的区别只有 Shift 按住时的符号和大小写；0 表示这个键不产生字符。
kbd_map_lo:
    db 0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, 9
    db 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13, 0, 'a', 's'
    db 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', 0x27, '`', 0, 0x5C, 'z', 'x', 'c', 'v'
    db 'b', 'n', 'm', ',', '.', '/', 0, 0, 0, ' ', 0, 0, 0, 0, 0, 0
    times 64 db 0

kbd_map_hi:
    db 0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8, 9
    db 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 13, 0, 'A', 'S'
    db 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', 0x22, '~', 0, 0x7C, 'Z', 'X', 'C', 'V'
    db 'B', 'N', 'M', '<', '>', '?', 0, 0, 0, ' ', 0, 0, 0, 0, 0, 0
    times 64 db 0

kbd_ring    times KBD_RING_SIZE db 0
kbd_head    dd 0
kbd_tail    dd 0
kb_shift    db 0
kb_caps     db 0
kb_ext      db 0
kb_raw      db 0

con_x       dd 0
con_y       dd 0
con_cols    dd 0
con_rows    dd 0
con_ch      db 0
cur_visible db 0

; 网卡（net.inc 用）
nic_found   dd 0
nic_vendor  dd 0
nic_device  dd 0
nic_busdev  dd 0
nic_mmio    dd 0
nic_irq     dd 0
nic_mac     times 8 db 0            ; 本机 MAC（后 2 字节是填充）
nic_rx_cur  dd 0                    ; 接收环里下一个要看的描述符
nic_tx_cur  dd 0                    ; 发送环里下一个要用的描述符
nic_rx_len  dd 0                    ; 上一次收包的长度
arp_target  dd 0                    ; 正在解析的目标 IP
arp_ok      dd 0                    ; 解析成功没有
out_mac     times 8 db 0            ; 解析出来的 MAC（后 2 字节填充）
gw_mac      times 8 db 0            ; 网关 MAC 缓存
gw_mac_valid dd 0                   ; 网关 MAC 有效没有
ping_ip     dd 0                    ; 正在 ping 的目标 IP
ping_seq    dd 0                    ; ICMP 序号
ping_t0     dd 0                    ; 发出时刻（毫秒）
dns_ip2     dd 0                    ; DNS 解析出来的 IP
dns_ok2     dd 0                    ; 解析成功没有
dns_t0      dd 0                    ; DNS 发出时刻
dns_name_len dd 0                   ; 编码后的域名长度
dns_name_buf times 128 db 0         ; DNS 名字（长度前缀格式）
dns_query_len dd 0                  ; DNS 查询总长
ping_host   dd 0                    ; 命令行传进来的目标字符串
ping_ok     dd 0                    ; 这一轮收到几个应答
ping_n      dd 0                    ; 这一轮发了几个

line_buf    times LINE_MAX db 0
line_len    dd 0


%ifdef SELFTEST
; 预置的扫描码流，对应的正是这六行：
;   help
;   echo typing works
;   MeOS 1234
;   ver
;   ping baidu.com
;   net
selftest_ptr  dd 0
selftest_verified db 0
selftest_len  equ 62
selftest_data:
    db 0x23, 0x12, 0x26, 0x19, 0x1C, 0x12, 0x2E, 0x23, 0x18, 0x39, 0x14, 0x15
    db 0x19, 0x17, 0x31, 0x22, 0x39, 0x11, 0x18, 0x13, 0x25, 0x1F, 0x1C, 0x2A
    db 0x32, 0xAA, 0x12, 0x2A, 0x18, 0xAA, 0x2A, 0x1F, 0xAA, 0x39, 0x02, 0x03
    db 0x04, 0x05, 0x1C, 0x2F, 0x12, 0x13, 0x1C, 0x19, 0x17, 0x31, 0x22, 0x39
    db 0x30, 0x1E, 0x17, 0x20, 0x16, 0x34, 0x2E, 0x18, 0x32, 0x1C, 0x31, 0x12
    db 0x14, 0x1C
%endif

msg_prompt  db "meos> ", 0
msg_help    db "commands: help  ver  echo <text>  cls  net  ping <host>", 0
msg_ver     db "MeOS 0.2 (M2) - 32-bit protected mode, PS/2 keyboard", 0
msg_unknown db "unknown command: ", 0
cmd_help    db "help", 0
cmd_ver     db "ver", 0
cmd_cls     db "cls", 0
cmd_echo    db "echo", 0
cmd_ping    db "ping", 0
cmd_net     db "net", 0
msg_ping_usage db "usage: ping <host|ip>", 0

msg_resolving db "resolving ", 0
msg_dots      db " ... ", 0
msg_dns_fail  db "cannot resolve host", 0
msg_pinging   db "Pinging ", 0
msg_reply     db "Reply from ", 0
msg_time      db ": time=", 0
msg_ms        db "ms", 0
msg_timeout   db "Request timed out", 0
msg_stat_a    db "packets: sent=", 0
msg_stat_b    db ", received=", 0
msg_net_ip    db "ip      ", 0
msg_net_gw    db "gateway ", 0
msg_net_dns   db "dns     ", 0
msg_net_mask  db "netmask ", 0
msg_net_mac   db "mac     ", 0
msg_net_link  db "link    ", 0
msg_up        db "up", 0
msg_down      db "down", 0
msg_net_cnt   db "  tx/rx ", 0

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

%include "net.inc"
%include "font.inc"
%include "ascii.inc"