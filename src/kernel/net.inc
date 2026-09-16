; ============================================================================
;  网络：PCI 枚举 + e1000 驱动 + ARP / IP / ICMP + UDP / DNS
; ============================================================================
;  这一整块是为了让系统能 ping 通外网。分几层，每层都往诊断块里记状态，
;  出问题的时候能一眼看出卡在哪一层：
;
;     PCI 枚举 → e1000 初始化 → 收发包 → ARP → IPv4/ICMP → UDP/DNS
;
;  调试块里 D_NET_STAGE 就是这条链路的进度条。

BITS 32

; e1000 家族的 PCI device id（8254x 系列寄存器接口一致，按家族认）
e1000_dev_ids:
    dw 0x100E, 0x100F, 0x1008, 0x1009, 0x100C, 0x100D, 0x1010, 0x1012
    dw 0x1013, 0x1015, 0x1016, 0x1017, 0x1018, 0x1019, 0x101A, 0x1026
    dw 0x1027, 0x1028, 0x105E, 0x105F, 0x1060, 0x10D3, 0x10F6, 0

; ---- 读 PCI 配置空间：EAX = 地址字（不含 bit31），输出 EAX = 读到的 dword ----
pci_read32:
    or  eax, 0x80000000
    mov dx, 0xCF8
    out dx, eax
    mov dx, 0xCFC
    in  eax, dx
    ret

; ---- 写 PCI 配置空间：EAX = 地址字，EDX = 要写的值 --------------------------
pci_write32:
    or  eax, 0x80000000
    push edx
    mov dx, 0xCF8
    out dx, eax
    pop eax
    mov dx, 0xCFC
    out dx, eax
    ret

; ---- 在总线 0 上找 Intel e1000 网卡 -----------------------------------------
;  VMware 把 virtualDev = "e1000" 报成 82545EM（100F），不是 82540EM（100E），
;  所以不能只认一个 ID。8254x 全家的寄存器接口是一样的，按家族认。
;  找到就把 vendor / device / BAR0 / 中断线填进变量，找不到就把 nic_found 清零。
net_find_nic:
    pushad
    mov dword [nic_found], 0

    xor ebp, ebp                        ; EBP = 设备号 0..31
.dev_loop:
    xor edi, edi                        ; EDI = 功能号 0..7
.fn_loop:
    ; 配置地址 = 总线0 | 设备<<11 | 功能<<8 | 寄存器0
    mov eax, ebp
    shl eax, 11
    mov ecx, edi
    shl ecx, 8
    or  eax, ecx
    call pci_read32
    movzx ebx, ax                       ; EBX = vendor id
    shr eax, 16                         ; EAX = device id
    cmp ebx, 0xFFFF                     ; 0xFFFF 说明这个号上没有设备
    je .next_fn
    cmp ebx, 0x8086                     ; Intel
    jne .next_fn
    mov [nic_device], eax               ; 先把 device id 存下来
    mov esi, e1000_dev_ids
.check_id:
    movzx ecx, word [esi]               ; 表以 0 结尾
    test ecx, ecx
    jz .next_fn                         ; 走完整张表都不是 e1000
    cmp ecx, eax
    je .is_e1000
    add esi, 2
    jmp .check_id
.is_e1000:
    mov eax, [nic_device]
    mov [nic_vendor], ebx
    mov eax, ebp
    shl eax, 11
    mov ecx, edi
    shl ecx, 8
    or  eax, ecx
    mov [nic_busdev], eax               ; 设备<<11 | 功能<<8，用作配置地址的底子

    ; BAR0（0x10）：低 4 位是标志位，抹掉才是真正的 MMIO 基址
    mov eax, [nic_busdev]
    or  eax, 0x10
    call pci_read32
    and eax, 0xFFFFFFF0
    mov [nic_mmio], eax

    ; 中断线（0x3C）低字节
    mov eax, [nic_busdev]
    or  eax, 0x3C
    call pci_read32
    and eax, 0xFF
    mov [nic_irq], eax

    mov dword [nic_found], 1
    jmp .done

.next_fn:
    inc edi
    cmp edi, 8
    jb .fn_loop
    inc ebp
    cmp ebp, 32
    jb .dev_loop
.done:
    popad
    ret

; ---- 把总线 0 上扫到的前 8 个设备记进诊断块 --------------------------------
;  纯粹为了排障：扫描没找到网卡时，先确认 PCI 配置空间到底读不读得到。
;  每一项的低 16 位是 vendor id，高 16 位是 device id。
net_scan_dump:
    pushad
    mov edi, DIAG_ADDR + D_NET_SCAN
    mov ecx, 8
    xor eax, eax
.clr:
    mov [edi], eax
    add edi, 4
    dec ecx
    jnz .clr

    mov edi, DIAG_ADDR + D_NET_SCAN
    xor esi, esi                        ; 已记录数量
    xor ebp, ebp                        ; 设备号
.d_loop:
    xor ecx, ecx                        ; 功能号
.f_loop:
    mov eax, ebp
    shl eax, 11
    mov edx, ecx
    shl edx, 8
    or  eax, edx
    call pci_read32
    cmp ax, 0xFFFF                      ; 这个号上没设备
    je .next_f
    mov [edi + esi * 4], eax
    inc esi
    cmp esi, 8
    jae .done
.next_f:
    inc ecx
    cmp ecx, 8
    jb .f_loop
    inc ebp
    cmp ebp, 32
    jb .d_loop
.done:
    popad
    ret

; ---- 采一份网卡信息写进诊断块，供宿主机 vmdiag.py 读取 ----------------------
net_probe:
    pushad
    mov dword [DIAG_ADDR + D_NET_STAGE], 0
    call net_scan_dump
    call net_find_nic
    mov dword [DIAG_ADDR + D_NET_STAGE], 1
    mov eax, [nic_found]
    test eax, eax
    jz .done
    mov eax, [nic_vendor]
    mov [DIAG_ADDR + D_NET_VENDOR], eax
    mov eax, [nic_device]
    mov [DIAG_ADDR + D_NET_DEVICE], eax
    mov eax, [nic_busdev]
    mov [DIAG_ADDR + D_NET_BUSDEV], eax
    mov eax, [nic_mmio]
    mov [DIAG_ADDR + D_NET_MMIO], eax
    mov eax, [nic_irq]
    mov [DIAG_ADDR + D_NET_IRQ], eax
    mov dword [DIAG_ADDR + D_NET_STAGE], 2
    call net_init                       ; 找到了就顺手初始化
.done:
    popad
    ret

; ---- 网络启用：解析网关 MAC --------------------------------------------------
;  这一段要等中断开了才能跑——net_recv_wait 靠 D_TICKS 计时，
;  定时器没响的话它会一直转下去。
net_start:
    pushad
    cmp dword [nic_found], 0
    jz .done
    mov eax, NET_GW
    call arp_resolve
    test eax, eax
    jz .fail
    mov eax, [out_mac]
    mov [DIAG_ADDR + D_NET_GWMAC], eax
    movzx eax, word [out_mac + 4]
    mov [DIAG_ADDR + D_NET_GWMAC + 4], eax
    mov eax, [out_mac]                  ; 再缓存一份，以后不用重复问
    mov [gw_mac], eax
    movzx eax, word [out_mac + 4]
    mov [gw_mac + 4], ax
    mov dword [gw_mac_valid], 1
    mov dword [DIAG_ADDR + D_NET_STAGE], 6

    ; 顺手 ping 一下网关：这是"IP + ICMP 通了没有"的第一个硬证据
    mov eax, NET_IP
    mov [DIAG_ADDR + D_NET_LOCALIP], eax

    mov eax, NET_GW
    call ping_once
    test eax, eax
    jz .noicmp
    mov dword [DIAG_ADDR + D_NET_STAGE], 7

    ; 再解析一次 baidu.com，验证 UDP + DNS 这条路
    mov esi, dns_default_name
    call dns_resolve
    test eax, eax
    jz .done
    mov dword [DIAG_ADDR + D_NET_STAGE], 8

    ; 最后一步：直接 ping 解析出来的地址。走到 9 就说明
    ; PCI → 网卡 → ARP → IP → ICMP → UDP → DNS 整条链路全通了。
    mov eax, [dns_ip2]
    mov [DIAG_ADDR + D_NET_LASTIP], eax
    call ping_once
    test eax, eax
    jz .baidu_fail
    mov dword [DIAG_ADDR + D_NET_STAGE], 9
    jmp .done
.baidu_fail:
    mov dword [DIAG_ADDR + D_NET_ERR], 6    ; 解析出来了但 ping 不通
    jmp .done
.noicmp:
    mov dword [DIAG_ADDR + D_NET_ERR], 4    ; ARP 通了但 ICMP 没回
    jmp .done
.fail:
    mov dword [DIAG_ADDR + D_NET_ERR], 3    ; 网关都不理我们
.done:
    popad
    ret
; ---- e1000 寄存器偏移（相对 MMIO 基址）-------------------------------------
E_CTRL      equ 0x0000
E_STATUS    equ 0x0008
E_ICR       equ 0x00C0
E_IMS       equ 0x00D0
E_IMC       equ 0x00D8
E_RCTL      equ 0x0100
E_TCTL      equ 0x0400
E_TIPG      equ 0x0410
E_RDBAL     equ 0x2800
E_RDBAH     equ 0x2804
E_RDLEN     equ 0x2808
E_RDH       equ 0x2810
E_RDT       equ 0x2818
E_TDBAL     equ 0x3800
E_TDBAH     equ 0x3804
E_TDLEN     equ 0x3808
E_TDH       equ 0x3810
E_TDT       equ 0x3818
E_MTA       equ 0x5200
E_RAL       equ 0x5400
E_RAH       equ 0x5404

E_CTRL_RST  equ 0x04000000          ; bit26 软复位
E_CTRL_SLU  equ 0x00000040          ; bit6  链路升起来

RX_COUNT    equ 32                  ; 接收描述符个数
TX_COUNT    equ 8                   ; 发送描述符个数
NIC_BUF_SZ  equ 2048                ; 每个收包缓冲大小（BSIZE=2048）

; 收发环和缓冲放在内核和栈之间的空地上（内核约 12KB，栈顶 0x7C000）
NET_BASE    equ 0x10000
NIC_RX_RING equ NET_BASE + 0x0000   ; 32 × 16 = 512 字节
NIC_TX_RING equ NET_BASE + 0x0200   ;  8 × 16 = 128 字节
NIC_RX_BUF  equ NET_BASE + 0x1000   ; 32 × 2048 = 64KB
NIC_TX_BUF  equ NET_BASE + 0x11000  ; 2048
PKT_BUF     equ NET_BASE + 0x12000  ; 2048，拼包用

; 发送描述符的 cmd/status 位
TX_CMD_EOP  equ 0x01
TX_CMD_IFCS equ 0x02
TX_CMD_RS   equ 0x08
TX_STAT_DD  equ 0x01
; 接收描述符的 status 位
RX_STAT_DD  equ 0x01
RX_STAT_EOP equ 0x02

; ---- 读写 e1000 寄存器 -------------------------------------------------------
;  读：EAX = 偏移 -> EAX = 值
nic_read:
    add eax, [nic_mmio]
    mov eax, [eax]
    ret

;  写：EAX = 偏移，EDX = 值（EAX、EDX 都保持原样）
nic_write:
    push eax
    add eax, [nic_mmio]
    mov [eax], edx
    pop eax
    ret

; ---- 初始化 e1000 ------------------------------------------------------------
;  复位 → 清组播表 → 建收发环 → 配 RCTL/TCTL → 升链路 → 读 MAC
net_init:
    pushad
    cmp dword [nic_found], 0
    jz .done
    mov dword [DIAG_ADDR + D_NET_STAGE], 3

    ; 关掉全部中断：这一版用轮询，简单且不会和键盘中断抢
    mov eax, E_IMC
    mov edx, 0xFFFFFFFF
    call nic_write

    ; ---- 软复位 ----
    mov eax, E_CTRL
    call nic_read
    or  eax, E_CTRL_RST
    mov edx, eax
    mov eax, E_CTRL
    call nic_write
    mov ecx, 1000000                    ; 等 RST 位自己清零，带上限防死循环
.wait_rst:
    mov eax, E_CTRL
    call nic_read
    test eax, E_CTRL_RST
    jz .rst_ok
    dec ecx
    jnz .wait_rst
    mov dword [DIAG_ADDR + D_NET_ERR], 1   ; 复位超时
    jmp .done
.rst_ok:
    ; 复位会把 CTRL 清干净，重新把链路升起来
    mov eax, E_CTRL
    mov edx, E_CTRL_SLU
    call nic_write

    ; ---- 清组播表（128 个 dword）----
    mov eax, E_MTA
    xor edx, edx
    mov ecx, 128
.mta:
    call nic_write
    add eax, 4
    dec ecx
    jnz .mta

    ; ---- 填接收描述符：每项指向一块 2048 字节的缓冲 ----
    mov edi, NIC_RX_RING
    mov esi, NIC_RX_BUF
    mov ecx, RX_COUNT
.rxd:
    mov [edi + 0], esi                  ; 缓冲地址低 32 位
    mov dword [edi + 4], 0              ; 高 32 位（我们用不到 4GB 以上）
    mov dword [edi + 8], 0              ; 长度 + 校验和
    mov dword [edi + 12], 0             ; 状态 + 错误 + 特殊
    add esi, NIC_BUF_SZ
    add edi, 16
    dec ecx
    jnz .rxd

    ; ---- 发送描述符先全部清零 ----
    mov edi, NIC_TX_RING
    mov ecx, TX_COUNT * 4
    xor eax, eax
.txd:
    mov [edi], eax
    add edi, 4
    dec ecx
    jnz .txd

    ; ---- 接收环寄存器 ----
    mov eax, E_RDBAL
    mov edx, NIC_RX_RING
    call nic_write
    mov eax, E_RDBAH
    xor edx, edx
    call nic_write
    mov eax, E_RDLEN
    mov edx, RX_COUNT * 16
    call nic_write
    mov eax, E_RDH
    xor edx, edx
    call nic_write
    mov eax, E_RDT
    mov edx, RX_COUNT - 1               ; 把整圈都交给硬件
    call nic_write

    ; ---- 发送环寄存器 ----
    mov eax, E_TDBAL
    mov edx, NIC_TX_RING
    call nic_write
    mov eax, E_TDBAH
    xor edx, edx
    call nic_write
    mov eax, E_TDLEN
    mov edx, TX_COUNT * 16
    call nic_write
    mov eax, E_TDH
    xor edx, edx
    call nic_write
    mov eax, E_TDT
    xor edx, edx
    call nic_write

    ; ---- 收：EN | SBP | UPE | MPE | BAM，缓冲 2048 ----
    mov eax, E_RCTL
    mov edx, 0x0000801E
    call nic_write

    ; ---- 发：EN | PSP | CT=0x10 | COLD=0x40 ----
    mov eax, E_TCTL
    mov edx, 0x0004010A
    call nic_write
    mov eax, E_TIPG
    mov edx, 0x0060200A                 ; 标准包间隔
    call nic_write

    mov dword [nic_rx_cur], 0
    mov dword [nic_tx_cur], 0
    mov dword [DIAG_ADDR + D_NET_STAGE], 4

    ; ---- 再升一次链路，然后读状态 ----
    mov eax, E_CTRL
    mov edx, E_CTRL_SLU
    call nic_write
    mov eax, E_STATUS
    call nic_read
    mov [DIAG_ADDR + D_NET_LINK], eax

    ; ---- MAC：低 4 字节在 RAL，高 2 字节在 RAH 的低 16 位 ----
    mov eax, E_RAL
    call nic_read
    mov [nic_mac + 0], eax
    mov eax, E_RAH
    call nic_read
    mov [nic_mac + 4], ax
    movzx eax, word [nic_mac + 4]
    mov [DIAG_ADDR + D_NET_MAC + 4], eax
    mov eax, [nic_mac]
    mov [DIAG_ADDR + D_NET_MAC], eax

    mov dword [DIAG_ADDR + D_NET_STAGE], 5
.done:
    popad
    ret
; ============================================================================
;  网络配置
; ============================================================================
;  这几个值对应 VMware 的 NAT 网段（VMnet8）。改机器/改网段时要动这里。
;  来源：C:\ProgramData\VMware\vmnetdhcp.conf 和 vmnetnat.conf
;     子网      192.168.220.0/24
;     网关/NAT  192.168.220.2
;     DNS       192.168.220.2（NAT 自己转发）
;     DHCP 池   192.168.220.128 .. .254
;  我们静态用 .50，落在 DHCP 池外面，不会和别的虚拟机撞。
;
;  **字节序提醒**：IP 在内存里按"点分四段的自然顺序"存（192,168,220,50），
;  所以写成 dword 常量时是反的。协议头里的多字节字段（ethertype、oper 之类）
;  是网络序（大端），而我们的小端机器要存成字节反过来的样子——下面每个常量
;  后面都标了它在内存里的实际字节。
NET_IP      equ 0x32DCA8C0          ; 192.168.220.50   (C0 A8 DC 32)
NET_GW      equ 0x02DCA8C0          ; 192.168.220.2    (C0 A8 DC 02)
NET_DNS     equ 0x02DCA8C0          ; 192.168.220.2
NET_MASK    equ 0x00FFFFFF          ; 255.255.255.0    (FF FF FF 00)

ETH_ARP     equ 0x0608              ; ethertype 0x0806 网络序 -> 内存 06 08
ETH_IP      equ 0x0008              ; ethertype 0x0800 -> 内存 08 00
ARP_HTYPE   equ 0x0100              ; 1  -> 内存 00 01
ARP_PTYPE   equ 0x0008              ; 0x0800 -> 内存 08 00
ARP_REQ     equ 0x0100              ; 1  -> 内存 00 01
ARP_REPLY   equ 0x0200              ; 2  -> 内存 00 02

ETH_HDR_LEN equ 14

dns_default_name:
    db "baidu.com", 0

icmp_fill:
    db "MeOS ping payload 0123456789"   ; 正好 28 字节，后面补到 32
    db 0, 0, 0, 0
; 以太网头之后各字段的偏移
E_DST       equ 0
E_SRC       equ 6
E_TYPE      equ 12
A_HTYPE     equ 14
A_PTYPE     equ 16
A_HLEN      equ 18
A_PLEN      equ 19
A_OPER      equ 20
A_SHA       equ 22                  ; 发送方 MAC
A_SPA       equ 28                  ; 发送方 IP
A_THA       equ 32                  ; 目标 MAC
A_TPA       equ 38                  ; 目标 IP
ARP_PKT_LEN equ 42

; ---- 发一帧：ESI = 数据，ECX = 长度 ------------------------------------------
;  拷进发送缓冲、填描述符、踢一下 TDT，然后等硬件把 DD 置起来。
net_send:
    pushad
    cmp dword [nic_found], 0
    jz .done
    mov ebp, ecx                        ; rep movsb 会把 ECX 清零，先把长度挪走
    mov edi, NIC_TX_BUF
    cld
    rep movsb

    mov ebx, [nic_tx_cur]
    mov edi, NIC_TX_RING
    mov eax, ebx
    shl eax, 4
    add edi, eax                        ; EDI = 发送描述符
    mov dword [edi + 0], NIC_TX_BUF
    mov dword [edi + 4], 0
    mov [edi + 8], bp                   ; 长度
    mov byte [edi + 11], TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS
    mov byte [edi + 12], 0              ; 状态清零，等下等它变成 DD

    mov edx, ebx
    inc edx
    and edx, TX_COUNT - 1
    mov [nic_tx_cur], edx
    mov eax, E_TDT                      ; 告诉硬件：这个描述符可以发了
    call nic_write

    mov ecx, 2000000                    ; 等 DD，带上限
.wait:
    test byte [edi + 12], TX_STAT_DD
    jnz .sent
    dec ecx
    jnz .wait
    mov dword [DIAG_ADDR + D_NET_ERR], 2    ; 发送超时
    jmp .done
.sent:
    inc dword [DIAG_ADDR + D_NET_TX]
.done:
    popad
    ret

; ---- 收一帧：EAX = 长度（0 表示没有），内容在 PKT_BUF --------------------------
net_poll:
    pushad
    cmp dword [nic_found], 0
    jz .none
    mov eax, E_RDH
    call nic_read
    and eax, RX_COUNT - 1
    cmp eax, [nic_rx_cur]               ; 硬件写的格子和我们读的格子一样 = 没新包
    je .none

    mov ebp, [nic_rx_cur]
    mov edi, NIC_RX_RING
    mov ecx, ebp
    shl ecx, 4
    add edi, ecx
    test byte [edi + 12], RX_STAT_DD    ; 描述符得标着"收完了"
    jz .none
    movzx ecx, word [edi + 8]           ; 长度
    test ecx, ecx
    jz .none

    mov byte [edi + 12], 0              ; 状态清掉
    mov eax, E_RDT                      ; 这一格还给硬件
    mov edx, ebp
    call nic_write

    push ecx
    mov esi, [edi + 0]                  ; 数据在哪个缓冲
    mov edi, PKT_BUF
    cld
    rep movsb
    pop ecx

    mov [nic_rx_len], ecx
    inc ebp
    and ebp, RX_COUNT - 1
    mov [nic_rx_cur], ebp
    inc dword [DIAG_ADDR + D_NET_RX]

    ; 把这一帧的前 64 字节抄进诊断块：收是收到了，但解析不出来的时候，
    ; 宿主机能直接看到包长什么样，比猜快得多。
    mov [DIAG_ADDR + D_NET_PKTLEN], ecx
    push esi
    push edi
    push ecx
    mov esi, PKT_BUF
    mov edi, DIAG_ADDR + D_NET_PKT
    mov ecx, 16
.dump:
    mov eax, [esi]
    mov [edi], eax
    add esi, 4
    add edi, 4
    dec ecx
    jnz .dump
    pop ecx
    pop edi
    pop esi

    popad
    mov eax, [nic_rx_len]
    ret
.none:
    popad
    xor eax, eax
    ret

; ---- 收一帧，等一会儿：EAX = 长度（0 = 超时）--------------------------------
;  ECX = 最多等多少个定时器节拍
net_recv_wait:
    pushad
    mov ebp, ecx
    add ebp, [DIAG_ADDR + D_TICKS]
.wait:
    call net_poll
    test eax, eax
    jnz .got
    mov eax, [DIAG_ADDR + D_TICKS]
    cmp eax, ebp
    jb .wait
    popad
    xor eax, eax
    ret
.got:
    popad
    mov eax, [nic_rx_len]
    ret

; ---- 发一个 ARP 请求，问"谁是 <目标 IP>"：EAX = 目标 IP ----------------------
arp_request:
    pushad
    mov [arp_target], eax

    mov edi, PKT_BUF
    ; 目的 MAC = 广播
    mov dword [edi + E_DST + 0], 0xFFFFFFFF
    mov word  [edi + E_DST + 4], 0xFFFF
    ; 源 MAC
    mov eax, [nic_mac]
    mov [edi + E_SRC], eax
    movzx eax, word [nic_mac + 4]
    mov [edi + E_SRC + 4], ax
    mov word [edi + E_TYPE], ETH_ARP

    mov word [edi + A_HTYPE], ARP_HTYPE
    mov word [edi + A_PTYPE], ARP_PTYPE
    mov byte [edi + A_HLEN], 6
    mov byte [edi + A_PLEN], 4
    mov word [edi + A_OPER], ARP_REQ
    mov eax, [nic_mac]
    mov [edi + A_SHA], eax
    movzx eax, word [nic_mac + 4]
    mov [edi + A_SHA + 4], ax
    mov eax, NET_IP
    mov [edi + A_SPA], eax
    mov dword [edi + A_THA + 0], 0      ; 还不知道对方 MAC，填 0
    mov word  [edi + A_THA + 4], 0
    mov eax, [arp_target]
    mov [edi + A_TPA], eax

    mov esi, PKT_BUF
    mov ecx, ARP_PKT_LEN
    call net_send
    popad
    ret

; ---- 解析一个 IP 的 MAC：EAX = IP，返回 EAX = 1 成功（MAC 存进 out_mac）------
;  只处理"同一个子网"的情况：目标不在本网段就先解析网关（见 net_next_hop）。
arp_resolve:
    pushad
    mov [arp_target], eax
    mov dword [arp_ok], 0
    mov ecx, 3                          ; 最多重试 3 次
.retry:
    push ecx
    mov eax, [arp_target]
    call arp_request
    mov ecx, 20                         ; 每次等 20 个节拍（约 1 秒）
    call net_recv_wait
    pop ecx
    test eax, eax
    jz .next
    call arp_check_reply
    cmp dword [arp_ok], 0
    jne .done
.next:
    dec ecx
    jnz .retry
.done:
    popad
    mov eax, [arp_ok]
    ret

; ---- 看看 PKT_BUF 里这一帧是不是我们要的 ARP 应答 ----------------------------
arp_check_reply:
    pushad
    mov edi, PKT_BUF
    mov ax, [edi + E_TYPE]
    cmp ax, ETH_ARP
    jne .no
    mov ax, [edi + A_OPER]
    cmp ax, ARP_REPLY
    jne .no
    mov eax, [edi + A_SPA]              ; 发送方 IP 得是我们要问的那个
    cmp eax, [arp_target]
    jne .no
    mov eax, [edi + A_SHA]
    mov [out_mac], eax
    movzx eax, word [edi + A_SHA + 4]
    mov [out_mac + 4], ax
    mov dword [arp_ok], 1
    inc dword [DIAG_ADDR + D_NET_ARP_RX]
.no:
    popad
    ret

; ---- 算下一跳：目标在同一网段就是它自己，否则是网关 --------------------------
;  EAX = 目标 IP，返回 EAX = 下一跳 IP
net_next_hop:
    mov edx, eax
    xor edx, NET_IP
    test edx, ~NET_MASK
    jz .same
    mov eax, NET_GW
.same:
    ret
; ============================================================================
;  IPv4 + ICMP
; ============================================================================
;  多字节字段在协议头里都是**网络序（大端）**，而我们这台机器是小端。
;  IP 地址则天然按点分顺序存（192,168,220,50），正好就是网络序，可以直接搬。
;  长度/校验和这类字段一律用"逐字节写"的写法，省得把字节序绕晕。
IP_OFF       equ ETH_HDR_LEN        ; IP 头在包里的偏移 = 14
I_VER_IHL    equ 0
I_TOS        equ 1
I_TLEN       equ 2
I_ID         equ 4
I_FRAG       equ 6
I_TTL        equ 8
I_PROTO      equ 9
I_CSUM       equ 10
I_SRC        equ 12
I_DST        equ 16
IP_HDR_LEN   equ 20

ICMP_OFF     equ IP_OFF + IP_HDR_LEN    ; 34
C_TYPE       equ 0
C_CODE       equ 1
C_CSUM       equ 2
C_ID         equ 4
C_SEQ        equ 6
C_DATA       equ 8

IP_PROTO_ICMP equ 1
IP_PROTO_UDP  equ 17
ICMP_ECHO_REQ equ 8
ICMP_ECHO_REP equ 0
ICMP_DATA_LEN equ 32

; ---- 16 位反码校验和：ESI = 数据，ECX = 字节数 -> AX ------------------------
;  小端机器上直接按 16 位字求和就行：一补数加法对字节序是对称的，
;  算出来的 AX 直接写进包里就是正确的网络序校验和。
ip_checksum:
    push ebx
    push edx
    push esi
    push ecx
    xor eax, eax
.sum:
    cmp ecx, 2
    jb .tail
    movzx edx, word [esi]
    add eax, edx
    adc eax, 0
    add esi, 2
    sub ecx, 2
    jmp .sum
.tail:
    test ecx, ecx
    jz .fold
    movzx edx, byte [esi]
    add eax, edx
    adc eax, 0
.fold:
    mov edx, eax
    shr edx, 16
    and eax, 0xFFFF
    add eax, edx
    adc eax, 0
    mov edx, eax
    shr edx, 16
    add eax, edx
    not eax
    and eax, 0xFFFF
    pop ecx
    pop esi
    pop edx
    pop ebx
    ret

; ---- 解析任意 IP 的 MAC：EAX = IP，返回 EAX = 1 成功（MAC 在 out_mac）--------
;  网关的 MAC 开机时已经解析过，缓存起来不再重复问。
net_arp_for:
    cmp eax, NET_GW
    jne .ask
    cmp dword [gw_mac_valid], 0
    je .ask
    mov edx, [gw_mac]
    mov [out_mac], edx
    mov dx, [gw_mac + 4]
    mov [out_mac + 4], dx
    mov eax, 1
    ret
.ask:
    call arp_resolve
    ret

; ---- 拼一个 ICMP echo 请求并发出去：目标 IP 在 ping_ip -----------------------
icmp_build_send:
    pushad
    mov edi, PKT_BUF

    ; --- 以太网头 ---
    mov eax, [out_mac]
    mov [edi + E_DST], eax
    movzx eax, word [out_mac + 4]
    mov [edi + E_DST + 4], ax
    mov eax, [nic_mac]
    mov [edi + E_SRC], eax
    movzx eax, word [nic_mac + 4]
    mov [edi + E_SRC + 4], ax
    mov word [edi + E_TYPE], ETH_IP

    ; --- IP 头 ---
    mov byte [edi + IP_OFF + I_VER_IHL], 0x45
    mov byte [edi + IP_OFF + I_TOS], 0
    mov byte [edi + IP_OFF + I_TLEN + 0], 0
    mov byte [edi + IP_OFF + I_TLEN + 1], IP_HDR_LEN + 8 + ICMP_DATA_LEN
    mov byte [edi + IP_OFF + I_ID + 0], 0x12
    mov byte [edi + IP_OFF + I_ID + 1], 0x34
    mov byte [edi + IP_OFF + I_FRAG + 0], 0
    mov byte [edi + IP_OFF + I_FRAG + 1], 0
    mov byte [edi + IP_OFF + I_TTL], 64
    mov byte [edi + IP_OFF + I_PROTO], IP_PROTO_ICMP
    mov word [edi + IP_OFF + I_CSUM], 0
    mov eax, NET_IP
    mov [edi + IP_OFF + I_SRC], eax
    mov eax, [ping_ip]
    mov [edi + IP_OFF + I_DST], eax
    ; IP 头校验和
    mov esi, edi
    add esi, IP_OFF
    mov ecx, IP_HDR_LEN
    call ip_checksum
    mov [edi + IP_OFF + I_CSUM], ax

    ; --- ICMP 头 ---
    mov byte [edi + ICMP_OFF + C_TYPE], ICMP_ECHO_REQ
    mov byte [edi + ICMP_OFF + C_CODE], 0
    mov word [edi + ICMP_OFF + C_CSUM], 0
    mov byte [edi + ICMP_OFF + C_ID + 0], 0x4D
    mov byte [edi + ICMP_OFF + C_ID + 1], 0x45        ; "ME"
    mov eax, [ping_seq]
    mov byte [edi + ICMP_OFF + C_SEQ + 0], ah         ; 序号按大端写
    mov byte [edi + ICMP_OFF + C_SEQ + 1], al
    ; 数据段填个花样，方便在抓包里认出来
    mov ecx, ICMP_DATA_LEN
    mov esi, icmp_fill
    lea edx, [edi + ICMP_OFF + C_DATA]
.fill:
    mov al, [esi]
    mov [edx], al
    inc esi
    inc edx
    dec ecx
    jnz .fill
    ; ICMP 校验和（覆盖 ICMP 头 + 数据）
    mov esi, edi
    add esi, ICMP_OFF
    mov ecx, 8 + ICMP_DATA_LEN
    call ip_checksum
    mov [edi + ICMP_OFF + C_CSUM], ax

    ; --- 发 ---
    mov esi, PKT_BUF
    mov ecx, ETH_HDR_LEN + IP_HDR_LEN + 8 + ICMP_DATA_LEN
    call net_send
    popad
    ret

; ---- 看 PKT_BUF 里这一帧是不是我们等的 ICMP 应答：EAX = 1 是 -----------------
;  不能用 pushad/popad —— 返回值放在 EAX 里，popad 会把它冲掉。
icmp_check_reply:
    push ebx
    push edi
    xor ebx, ebx                        ; 先假定"不是"
    mov edi, PKT_BUF
    mov ax, [edi + E_TYPE]
    cmp ax, ETH_IP
    jne .out
    cmp byte [edi + IP_OFF + I_PROTO], IP_PROTO_ICMP
    jne .out
    mov eax, [edi + IP_OFF + I_SRC]     ; 来源得是我们 ping 的那个 IP
    cmp eax, [ping_ip]
    jne .out
    cmp byte [edi + ICMP_OFF + C_TYPE], ICMP_ECHO_REP
    jne .out
    mov ebx, 1
.out:
    mov eax, ebx
    pop edi
    pop ebx
    ret

; ---- ping 一次：EAX = 目标 IP，返回 EAX = 1 收到应答 --------------------------
;  往返毫秒写进 D_NET_RTT。等应答的时候一直轮询收包，2 秒没回就算超时。
ping_once:
    pushad
    mov [ping_ip], eax
    call net_next_hop                   ; 同网段就是它自己，否则是网关
    call net_arp_for                    ; 先把下一跳的 MAC 要到
    test eax, eax
    jz .fail

    mov eax, [DIAG_ADDR + D_TICKS]
    mov [ping_t0], eax

    inc dword [ping_seq]
    call icmp_build_send
    inc dword [DIAG_ADDR + D_NET_ICMP_TX]

.wait:
    call net_poll
    test eax, eax
    jz .tick
    call icmp_check_reply
    test eax, eax
    jnz .got
.tick:
    mov eax, [DIAG_ADDR + D_TICKS]
    sub eax, [ping_t0]
    cmp eax, 2000
    jb .wait
    jmp .fail
.got:
    mov eax, [DIAG_ADDR + D_TICKS]
    sub eax, [ping_t0]
    mov [DIAG_ADDR + D_NET_RTT], eax
    inc dword [DIAG_ADDR + D_NET_ICMP_RX]
    popad
    mov eax, 1
    ret
.fail:
    popad
    xor eax, eax
    ret
; ============================================================================
;  UDP + DNS
; ============================================================================
;  只实现够用的一点点：发一个 A 记录查询，从应答里抠出第一个 IPv4。
;  UDP 校验和填 0（IPv4 允许——省掉伪首部那一套）。
UDP_OFF      equ IP_OFF + IP_HDR_LEN    ; 34
U_SPORT      equ 0
U_DPORT      equ 2
U_LEN        equ 4
U_CSUM       equ 6
DNS_OFF      equ UDP_OFF + 8            ; 42

DNS_ID          equ 0x4D45              ; "ME"，和 ICMP 的标识呼应

; ---- 拼一个 DNS A 查询并发出去 ----------------------------------------------
dns_build_send:
    pushad
    mov edi, PKT_BUF

    ; --- 以太网头 ---
    mov eax, [out_mac]
    mov [edi + E_DST], eax
    movzx eax, word [out_mac + 4]
    mov [edi + E_DST + 4], ax
    mov eax, [nic_mac]
    mov [edi + E_SRC], eax
    movzx eax, word [nic_mac + 4]
    mov [edi + E_SRC + 4], ax
    mov word [edi + E_TYPE], ETH_IP

    ; --- IP 头 ---
    mov byte [edi + IP_OFF + I_VER_IHL], 0x45
    mov byte [edi + IP_OFF + I_TOS], 0
    mov eax, [dns_query_len]            ; 查询总长 = 12 + 域名 + 4
    add eax, IP_HDR_LEN + 8
    mov byte [edi + IP_OFF + I_TLEN + 0], ah    ; 长度是网络序：高字节在前
    mov byte [edi + IP_OFF + I_TLEN + 1], al
    mov byte [edi + IP_OFF + I_ID + 0], 0x12
    mov byte [edi + IP_OFF + I_ID + 1], 0x35
    mov byte [edi + IP_OFF + I_FRAG + 0], 0
    mov byte [edi + IP_OFF + I_FRAG + 1], 0
    mov byte [edi + IP_OFF + I_TTL], 64
    mov byte [edi + IP_OFF + I_PROTO], IP_PROTO_UDP
    mov word [edi + IP_OFF + I_CSUM], 0
    mov eax, NET_IP
    mov [edi + IP_OFF + I_SRC], eax
    mov eax, NET_DNS
    mov [edi + IP_OFF + I_DST], eax
    mov esi, edi
    add esi, IP_OFF
    mov ecx, IP_HDR_LEN
    call ip_checksum
    mov [edi + IP_OFF + I_CSUM], ax

    ; --- UDP 头（校验和填 0 = 不校验）---
    mov byte [edi + UDP_OFF + U_SPORT + 0], 0x30
    mov byte [edi + UDP_OFF + U_SPORT + 1], 0x39        ; 源端口 12345
    mov byte [edi + UDP_OFF + U_DPORT + 0], 0x00
    mov byte [edi + UDP_OFF + U_DPORT + 1], 0x35        ; 目的端口 53
    mov eax, [dns_query_len]
    add eax, 8
    mov byte [edi + UDP_OFF + U_LEN + 0], ah
    mov byte [edi + UDP_OFF + U_LEN + 1], al
    mov word [edi + UDP_OFF + U_CSUM], 0

    ; --- DNS 查询 ---
    mov byte [edi + DNS_OFF + 0], 0x4D                  ; ID 高
    mov byte [edi + DNS_OFF + 1], 0x45                  ; ID 低
    mov byte [edi + DNS_OFF + 2], 0x01                  ; flags 高：标准查询 + RD
    mov byte [edi + DNS_OFF + 3], 0x00
    mov byte [edi + DNS_OFF + 4], 0x00                  ; QDCOUNT = 1
    mov byte [edi + DNS_OFF + 5], 0x01
    mov byte [edi + DNS_OFF + 6], 0x00                  ; ANCOUNT = 0
    mov byte [edi + DNS_OFF + 7], 0x00
    mov byte [edi + DNS_OFF + 8], 0x00                  ; NSCOUNT = 0
    mov byte [edi + DNS_OFF + 9], 0x00
    mov byte [edi + DNS_OFF + 10], 0x00                 ; ARCOUNT = 0
    mov byte [edi + DNS_OFF + 11], 0x00
    ; QNAME（用命令行传进来的域名）
    mov esi, dns_name_buf
    lea edx, [edi + DNS_OFF + 12]
    mov ecx, [dns_name_len]
    test ecx, ecx                       ; 长度为 0 直接跳过 —— 不加这个判断的话
    jz .nocopy                          ; dec ecx 会从 0 绕成 4G，变成失控写内存
.copy:
    mov al, [esi]
    mov [edx], al
    inc esi
    inc edx
    dec ecx
    jnz .copy
.nocopy:
    ; QTYPE = 1 (A), QCLASS = 1 (IN)，紧跟在 QNAME 后面
    mov eax, [dns_name_len]
    lea edx, [edi + DNS_OFF + 12]
    add edx, eax
    mov byte [edx + 0], 0x00
    mov byte [edx + 1], 0x01
    mov byte [edx + 2], 0x00
    mov byte [edx + 3], 0x01

    mov esi, PKT_BUF
    mov ecx, [dns_query_len]
    add ecx, ETH_HDR_LEN + IP_HDR_LEN + 8
    call net_send
    popad
    ret

; ---- 跳过 DNS 里的一个名字：EDI 指向名字，返回时 EDI 在名字之后 --------------
;  DNS 用 0xC0 开头的两字节指针做压缩，遇到要当 2 字节处理，不能按长度走。
dns_skip_name:
    push eax
.skip:
    mov al, [edi]
    test al, al
    jz .zero
    and al, 0xC0
    cmp al, 0xC0
    je .ptr
    movzx eax, byte [edi]               ; 普通标签：长度 + 内容
    inc edi
    add edi, eax
    jmp .skip
.ptr:
    add edi, 2
    pop eax
    ret
.zero:
    inc edi                             ; 跳过结尾那个 0
    pop eax
    ret

; ---- 从 PKT_BUF 的应答里取第一个 A 记录，IP 存进 dns_ip2 ---------------------
dns_parse_reply:
    pushad
    mov edi, PKT_BUF
    mov ax, [edi + E_TYPE]
    cmp ax, ETH_IP
    jne .done
    cmp byte [edi + IP_OFF + I_PROTO], IP_PROTO_UDP
    jne .done
    mov ax, [edi + UDP_OFF + U_SPORT]
    cmp ax, 0x3500                      ; 源端口 53（网络序 00 35）
    jne .done
    cmp byte [edi + DNS_OFF + 0], 0x4D  ; ID 得对得上
    jne .done
    cmp byte [edi + DNS_OFF + 1], 0x45
    jne .done

    add edi, DNS_OFF + 12               ; 跳过 DNS 头
    call dns_skip_name                  ; 跳过问题里的 QNAME
    add edi, 4                          ; 跳过 QTYPE + QCLASS

    mov ecx, 8                          ; 最多看 8 条记录
.ans:
    call dns_skip_name                  ; 跳到本条记录的 TYPE
    movzx eax, word [edi + 0]
    cmp ax, 0x0100                      ; TYPE == 1 (A)？
    jne .skip
    movzx edx, word [edi + 8]
    cmp dx, 0x0400                      ; RDLENGTH == 4？
    jne .skip
    mov eax, [edi + 10]                 ; RDATA 就是 IPv4，网络序
    mov [dns_ip2], eax
    mov dword [dns_ok2], 1
    jmp .done
.skip:
    movzx edx, word [edi + 8]           ; RDLENGTH 是网络序，换过来才是真长度
    xchg dl, dh
    movzx edx, dx
    lea edi, [edi + edx + 10]
    dec ecx
    jnz .ans
.done:
    popad
    ret

; ---- 解析域名：ESI = 点分域名，返回 EAX = IP（0 = 失败）----------------------
;  编码这一步由它自己负责——之前的版本靠调用方先调 dns_encode，
;  漏调一次就变成零长度域名，拷贝循环直接失控（踩过）。
dns_resolve:
    pushad
    mov dword [dns_ok2], 0
    call dns_encode
    test eax, eax
    jz .fail
    mov [dns_name_len], eax
    add eax, 12 + 4                     ; DNS 头 + 域名 + QTYPE/QCLASS
    mov [dns_query_len], eax
    mov eax, NET_GW
    call net_arp_for
    test eax, eax
    jz .fail
    call dns_build_send

    mov eax, [DIAG_ADDR + D_TICKS]
    mov [dns_t0], eax
.wait:
    call net_poll
    test eax, eax
    jz .tick
    call dns_parse_reply
    cmp dword [dns_ok2], 0
    jne .got
.tick:
    mov eax, [DIAG_ADDR + D_TICKS]
    sub eax, [dns_t0]
    cmp eax, 3000                       ; 3 秒
    jb .wait
    jmp .fail
.got:
    mov eax, [dns_ip2]
    mov [DIAG_ADDR + D_NET_DNS_IP], eax
    mov dword [DIAG_ADDR + D_NET_DNS_OK], 1
    popad
    mov eax, [dns_ip2]
    ret
.fail:
    mov dword [DIAG_ADDR + D_NET_ERR], 5
    popad
    xor eax, eax
    ret
; ============================================================================
;  命令行用的格式化与网络命令
; ============================================================================

; ---- 打印无符号十进制：EAX = 数值 ------------------------------------------
putdec:
    pushad
    mov ebx, 10
    xor ecx, ecx
    test eax, eax
    jnz .loop
    mov al, '0'
    call con_putc
    jmp .done
.loop:
    xor edx, edx
    div ebx                             ; EDX:EAX / 10 -> 商 EAX，余数 EDX
    push edx
    inc ecx
    test eax, eax
    jnz .loop
.pop:
    pop eax
    add al, '0'
    call con_putc
    dec ecx
    jnz .pop
.done:
    popad
    ret

; ---- 打印点分 IP：EAX = IP -------------------------------------------------
putip:
    pushad
    mov ebx, eax
    mov ecx, 4
.loop:
    movzx eax, bl
    call putdec
    shr ebx, 8
    dec ecx
    jz .done
    mov al, '.'
    call con_putc
    jmp .loop
.done:
    popad
    ret

; ---- 打印 MAC：ESI = 6 字节 -------------------------------------------------
putmac:
    pushad
    mov ebx, esi
    mov ecx, 6
.loop:
    movzx eax, byte [ebx]
    mov edx, eax                        ; 存一份，等下还要取低 4 位
    shr eax, 4
    mov al, [hex_digits + eax]
    call con_putc
    mov eax, edx
    and eax, 0x0F
    mov al, [hex_digits + eax]
    call con_putc
    inc ebx
    dec ecx
    jz .done
    mov al, ':'
    call con_putc
    jmp .loop
.done:
    popad
    ret

; ---- 解析点分十进制 IP：ESI = 字符串 -> EAX = IP（0 = 不是合法 IP）----------
;  注意拼装方向：第一个字节要落在最低位，这样内存里的字节序就和点分顺序一致。
parse_ipv4:
    push ebx
    push ecx
    push edx
    push esi
    xor ebx, ebx                        ; 累积结果
    xor ecx, ecx                        ; 第几段
.seg:
    xor edx, edx                        ; 本段的数值
.digit:
    movzx eax, byte [esi]
    test al, al
    jz .endseg
    cmp al, '0'
    jb .not_digit
    cmp al, '9'
    ja .not_digit
    sub al, '0'
    imul edx, edx, 10
    movzx eax, al
    add edx, eax
    inc esi
    jmp .digit
.not_digit:
    cmp al, '.'
    jne .bad
.endseg:
    cmp edx, 255
    ja .bad
    mov eax, ecx
    shl eax, 3                          ; 第 ecx 段 -> 左移 8*ecx 位
    push ecx
    mov ecx, eax
    mov eax, edx
    shl eax, cl
    pop ecx
    or  ebx, eax
    inc ecx
    movzx eax, byte [esi]
    test al, al
    jz .finish
    inc esi                             ; 跳过 '.'
    cmp ecx, 4
    jb .seg
.bad:
    xor eax, eax
    jmp .out
.finish:
    cmp ecx, 4
    jne .bad
    mov eax, ebx
.out:
    pop esi
    pop edx
    pop ecx
    pop ebx
    ret

; ---- 把点分域名编码成 DNS 名字：ESI = "baidu.com" --------------------------
;  输出写到 dns_name_buf，返回 EAX = 编码后的字节数。
dns_encode:
    push ebx
    push ecx
    push edx
    push esi
    push edi
    mov edi, dns_name_buf
    mov ebx, edi
    inc edi                             ; 第一个字节留给"第一段的长度"
    xor ecx, ecx
.loop:
    mov al, [esi]
    test al, al
    jz .end
    cmp al, '.'
    je .dot
    mov [edi], al
    inc edi
    inc ecx
    inc esi
    jmp .loop
.dot:
    mov [ebx], cl                       ; 补上这一段的长度
    mov ebx, edi
    inc edi
    xor ecx, ecx
    inc esi
    jmp .loop
.end:
    mov [ebx], cl
    mov byte [edi], 0                   ; 名字结束标记
    inc edi
    mov eax, edi
    sub eax, dns_name_buf
    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    ret

; ---- ping 命令：ESI = 目标（域名或点分 IP）---------------------------------
cmd_do_ping:
    pushad
    mov [ping_host], esi
    call parse_ipv4
    test eax, eax
    jnz .have_ip

    ; --- 不是 IP，当域名解析 ---
    mov esi, msg_resolving
    call con_puts
    mov esi, [ping_host]
    call con_puts
    mov esi, msg_dots
    call con_puts
    mov esi, [ping_host]
    call dns_resolve
    test eax, eax
    jz .dns_fail
    mov [ping_ip], eax                  ; 先存下来，再打印
    call putip
    call con_crlf
    mov eax, [ping_ip]                  ; 不指望 con_crlf 保护 EAX，自己装回来
    jmp .have_ip

.dns_fail:
    mov esi, msg_dns_fail
    call con_puts
    call con_crlf
    jmp .done

.have_ip:
    mov [ping_ip], eax
    mov [DIAG_ADDR + D_NET_LASTIP], eax
    mov esi, msg_pinging
    call con_puts
    mov eax, [ping_ip]
    call putip
    call con_crlf

    mov dword [ping_ok], 0
    mov dword [ping_n], 0
.round:
    mov eax, [ping_ip]
    call ping_once
    test eax, eax
    jz .timeout
    mov esi, msg_reply
    call con_puts
    mov eax, [ping_ip]
    call putip
    mov esi, msg_time
    call con_puts
    mov eax, [DIAG_ADDR + D_NET_RTT]
    call putdec
    mov esi, msg_ms
    call con_puts
    call con_crlf
    inc dword [ping_ok]
    jmp .next
.timeout:
    mov esi, msg_timeout
    call con_puts
    call con_crlf
.next:
    inc dword [ping_n]
    cmp dword [ping_n], 4
    jb .round

    mov esi, msg_stat_a
    call con_puts
    mov eax, [ping_n]
    call putdec
    mov esi, msg_stat_b
    call con_puts
    mov eax, [ping_ok]
    call putdec
    call con_crlf
.done:
    popad
    ret

; ---- net 命令：把网络配置打出来 --------------------------------------------
cmd_do_net:
    pushad
    mov esi, msg_net_ip
    call con_puts
    mov eax, NET_IP
    call putip
    call con_crlf

    mov esi, msg_net_gw
    call con_puts
    mov eax, NET_GW
    call putip
    call con_crlf

    mov esi, msg_net_dns
    call con_puts
    mov eax, NET_DNS
    call putip
    call con_crlf

    mov esi, msg_net_mask
    call con_puts
    mov eax, NET_MASK
    call putip
    call con_crlf

    mov esi, msg_net_mac
    call con_puts
    mov esi, nic_mac
    call putmac
    call con_crlf

    mov esi, msg_net_link
    call con_puts
    mov eax, [DIAG_ADDR + D_NET_LINK]
    test eax, 2                         ; STATUS bit1 = 链路 up
    jz .down
    mov esi, msg_up
    call con_puts
    jmp .tx
.down:
    mov esi, msg_down
    call con_puts
.tx:
    mov esi, msg_net_cnt
    call con_puts
    mov eax, [DIAG_ADDR + D_NET_TX]
    call putdec
    mov al, '/'
    call con_putc
    mov eax, [DIAG_ADDR + D_NET_RX]
    call putdec
    call con_crlf
    popad
    ret