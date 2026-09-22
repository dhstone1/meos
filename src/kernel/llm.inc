; ============================================================================
;  LLM：TCP + HTTP 客户端，经宿主转发调用 DeepSeek
; ============================================================================
;  这一块补上网卡驱动的最后一块短板——**TCP**。有了它，系统才能自己
;  发起一次 HTTP 请求，把问题送到 DeepSeek，再把回答显示出来。
;
;  为什么走"宿主转发"而不是直连 api.deepseek.com：
;  DeepSeek 只提供 HTTPS，而 TLS 需要在核里做 RSA/ECDHE + AES + SHA256，
;  那是另一个量级的工程量。所以这里让内核说**明文 HTTP**，由宿主上的
;  tools/llm_proxy.py 负责 TLS 那一半。内核负责把 TCP 说对：
;
;     ARP → SYN → SYN/ACK → ACK → POST → 收数据 → FIN/ACK
;
;  缓冲区：请求走 llm_http_buf，应答走 llm_buf（都在 kernel.asm 里）。
;  进度记在诊断块 D_NET_TCP：1=握手中 2=已连接 0xFF=失败，出问题一眼能看出卡在哪。

BITS 32

; ---- TCP 头布局（相对 TCP_OFF）---------------------------------------------
TCP_OFF     equ IP_OFF + IP_HDR_LEN     ; 34 = 14 + 20
T_SPORT     equ 0
T_DPORT     equ 2
T_SEQ       equ 4
T_ACK       equ 8
T_OFFB      equ 12                      ; 高 4 位 = 数据偏移（以 dword 计）
T_FLAGS     equ 13
T_WIN       equ 14
T_CSUM      equ 16
T_URG       equ 18
TCP_HDR_LEN equ 20
TCP_OFF_VAL equ 0x50                    ; 数据偏移 5（=20 字节），高 4 位

TF_FIN      equ 0x01
TF_SYN      equ 0x02
TF_RST      equ 0x04
TF_PSH      equ 0x08
TF_ACK      equ 0x10

IP_PROTO_TCP equ 6

; ---- 代理地址（宿主上的 llm_proxy.py）--------------------------------------
;  VMware NAT 模式下 .1 就是宿主机，默认监听 8080。
LLM_PROXY_PORT equ 8080

; ============================================================================
;  小工具
; ============================================================================

; ---- 以 0 结尾的字符串拷贝：ESI = 源，EDI = 目标（结束时 EDI 指向末尾）------
str_copy:
    mov al, [esi]
    test al, al
    jz .done
    mov [edi], al
    inc esi
    inc edi
    jmp str_copy
.done:
    ret

; ---- 把 EAX 按十进制写进 [EDI]，EDI 前进。用于 Content-Length ----------------
putdec_buf:
    push eax
    push ebx
    push ecx
    push edx
    mov ebx, 10
    xor ecx, ecx
.div:
    xor edx, edx
    div ebx
    push edx
    inc ecx
    test eax, eax
    jnz .div
.emit:
    pop eax
    add al, '0'
    mov [edi], al
    inc edi
    dec ecx
    jnz .emit
    pop edx
    pop ecx
    pop ebx
    pop eax
    ret

; ---- 校验和累加器：ESI = 数据，ECX = 字节数，加到 EDI 上 ---------------------
;  奇数字节按"低字节补零"处理，和 RFC 1071 一致。
l4_acc:
    cmp ecx, 2
    jb .tail
.loop:
    movzx eax, word [esi]
    add edi, eax
    add esi, 2
    sub ecx, 2
    cmp ecx, 2
    jae .loop
.tail:
    test ecx, ecx
    jz .done
    movzx eax, byte [esi]
    add edi, eax
.done:
    ret

; ---- 把 EDI 里的 32 位累加值折叠成 16 位反码，结果在 EAX ---------------------
l4_fold:
    mov eax, edi
    mov edx, eax
    shr edx, 16
    and eax, 0xFFFF
    add eax, edx
    mov edx, eax
    shr edx, 16
    add eax, edx
    not eax
    and eax, 0xFFFF
    ret

; ============================================================================
;  TCP：发一个段
; ============================================================================

; ---- 组包并发出一个 TCP 段 ---------------------------------------------------
;  ECX = 载荷长度（0 = 纯控制段），ESI = 载荷指针，AL = 标志字节
;  返回 EAX = 1 已发出 / 0 失败
;  序号、端口、目标 IP 都从全局变量取；MAC 每次现查（ARP 有缓存，不慢）。
llm_tcp_send_seg:
    pushad
    mov [seg_flags], al
    mov [seg_len], ecx
    mov [seg_ptr], esi

    ; --- 下一跳 MAC ---
    mov eax, [tcp_dst_ip]
    call net_next_hop
    call net_arp_for
    test eax, eax
    jz .fail

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
    mov eax, [seg_len]
    add eax, IP_HDR_LEN + TCP_HDR_LEN
    mov byte [edi + IP_OFF + I_TLEN + 0], ah
    mov byte [edi + IP_OFF + I_TLEN + 1], al
    mov byte [edi + IP_OFF + I_ID + 0], 0x4C
    mov byte [edi + IP_OFF + I_ID + 1], 0x4D
    mov byte [edi + IP_OFF + I_FRAG + 0], 0x40   ; DF，网络序：高字节写在前面
    mov byte [edi + IP_OFF + I_FRAG + 1], 0
    mov byte [edi + IP_OFF + I_TTL], 64
    mov byte [edi + IP_OFF + I_PROTO], IP_PROTO_TCP
    mov word [edi + IP_OFF + I_CSUM], 0
    mov eax, NET_IP
    mov [edi + IP_OFF + I_SRC], eax
    mov eax, [tcp_dst_ip]
    mov [edi + IP_OFF + I_DST], eax
    mov esi, edi
    add esi, IP_OFF
    mov ecx, IP_HDR_LEN
    call ip_checksum
    mov [edi + IP_OFF + I_CSUM], ax

    ; --- TCP 头 ---
    mov ax, [tcp_src_port]
    xchg al, ah
    mov [edi + TCP_OFF + T_SPORT], ax
    mov ax, [tcp_dst_port]
    xchg al, ah
    mov [edi + TCP_OFF + T_DPORT], ax
    mov eax, [tcp_seq]
    bswap eax
    mov [edi + TCP_OFF + T_SEQ], eax
    mov eax, [tcp_ack]
    bswap eax
    mov [edi + TCP_OFF + T_ACK], eax
    mov byte [edi + TCP_OFF + T_OFFB], TCP_OFF_VAL
    mov al, [seg_flags]
    mov [edi + TCP_OFF + T_FLAGS], al
    mov word [edi + TCP_OFF + T_WIN], 0xFFFF
    mov word [edi + TCP_OFF + T_CSUM], 0
    mov word [edi + TCP_OFF + T_URG], 0

    ; --- 载荷 ---
    mov ecx, [seg_len]
    test ecx, ecx
    jz .no_payload
    mov esi, [seg_ptr]
    mov edi, PKT_BUF + TCP_OFF + TCP_HDR_LEN
    cld
    rep movsb

.no_payload:
    ; --- TCP 校验和：伪头（源IP + 目的IP + 0 + 协议 + 段长）+ TCP 段 ---
    mov edi, 0
    mov esi, PKT_BUF + IP_OFF + I_SRC
    mov ecx, 8
    call l4_acc
    movzx eax, byte [PKT_BUF + IP_OFF + I_PROTO]
    shl eax, 8                     ; 伪头里协议字节占高半边，小端读出来是 0xhh00
    add edi, eax
    mov eax, [seg_len]
    add eax, TCP_HDR_LEN
    xchg al, ah
    movzx eax, ax
    add edi, eax
    mov esi, PKT_BUF + TCP_OFF
    mov ecx, [seg_len]
    add ecx, TCP_HDR_LEN
    call l4_acc
    call l4_fold
    mov [PKT_BUF + TCP_OFF + T_CSUM], ax

    ; --- 发 ---
    mov esi, PKT_BUF
    mov ecx, [seg_len]
    add ecx, ETH_HDR_LEN + IP_HDR_LEN + TCP_HDR_LEN
    call net_send
    popad
    mov eax, 1
    ret
.fail:
    popad
    xor eax, eax
    ret

; ============================================================================
;  TCP：等一个属于自己的段、建连接
; ============================================================================

; ---- 等一个发给我们的 TCP 段 -------------------------------------------------
;  ECX = 最多等多少个定时器节拍（1 拍 = 1ms，PIT 跑在 1000Hz）
;  返回 EAX = 1 表示 PKT_BUF 里正躺着一个匹配的段
;  只认"来源 IP + 目的端口"都对得上的，别的包（ARP、ICMP、DNS 应答）直接丢。
tcp_wait_seg:
    pushad
    mov ebx, [DIAG_ADDR + D_TICKS]
    add ebx, ecx
.wait:
    call net_poll
    test eax, eax
    jz .tick
    cmp word [PKT_BUF + E_TYPE], ETH_IP
    jne .tick
    cmp byte [PKT_BUF + IP_OFF + I_PROTO], IP_PROTO_TCP
    jne .tick
    mov eax, [PKT_BUF + IP_OFF + I_SRC]
    cmp eax, [tcp_dst_ip]
    jne .tick
    mov ax, [PKT_BUF + TCP_OFF + T_SPORT]   ; 来源端口也得对上（线上大端 -> 主机序）
    xchg al, ah
    cmp ax, [tcp_dst_port]
    jne .tick
    mov ax, [PKT_BUF + TCP_OFF + T_DPORT]
    xchg al, ah                    ; 线上是大端，变量里是主机序
    cmp ax, [tcp_src_port]
    jne .tick
    popad
    mov eax, 1
    ret
.tick:
    mov eax, [DIAG_ADDR + D_TICKS]
    cmp eax, ebx
    jb .wait
    popad
    xor eax, eax
    ret

; ---- 建连接（三次握手）：EAX = 目标 IP，EBX = 目标端口 ------------------------
;  返回 EAX = 1 成功（tcp_state = 2）
tcp_connect:
    pushad
    mov [tcp_dst_ip], eax
    movzx ebx, bx
    mov [tcp_dst_port], ebx

    ; 源端口：拿节拍数凑一个，避开 1..1023
    mov eax, [DIAG_ADDR + D_TICKS]
    and eax, 0x3FFF
    add eax, 40000
    mov [tcp_src_port], eax

    ; 初始序号：不需要密码学随机，够乱就行
    mov eax, [DIAG_ADDR + D_TICKS]
    shl eax, 8
    xor eax, 0x5A5A5A5A
    mov [tcp_seq], eax

    mov dword [tcp_ack], 0
    mov dword [tcp_fin], 0
    mov dword [llm_rx_len], 0
    mov dword [DIAG_ADDR + D_NET_TCP], 1
    mov dword [tcp_retry], 3

.syn:
    xor ecx, ecx
    xor esi, esi
    mov al, TF_SYN
    call llm_tcp_send_seg
    test eax, eax
    jz .retry                      ; SYN 没发出去（多半是 ARP 没来得及回）
    mov ecx, 3000                  ; 3 秒没回应就重来
    call tcp_wait_seg
    test eax, eax
    jz .retry

    mov dl, [PKT_BUF + TCP_OFF + T_FLAGS]
    test dl, TF_RST                ; 对方直接拒绝
    jnz .fail
    mov al, dl
    and al, TF_SYN | TF_ACK
    cmp al, TF_SYN | TF_ACK        ; 必须同时有 SYN 和 ACK
    jne .retry

    ; 对方的序号 + 1 = 我们期望收到的第一个字节
    mov eax, [PKT_BUF + TCP_OFF + T_SEQ]
    bswap eax
    inc eax
    mov [tcp_ack], eax

    ; 对方确认了我们的 SYN，我们的下一序号从这儿起
    mov eax, [PKT_BUF + TCP_OFF + T_ACK]
    bswap eax
    mov [tcp_seq], eax

    ; 补最后一个 ACK，握手完成
    xor ecx, ecx
    xor esi, esi
    mov al, TF_ACK
    call llm_tcp_send_seg
    mov dword [tcp_state], 2
    mov dword [DIAG_ADDR + D_NET_TCP], 2
    popad
    mov eax, 1
    ret

.retry:
    dec dword [tcp_retry]
    jnz .syn
.fail:
    mov dword [tcp_state], 0
    mov dword [DIAG_ADDR + D_NET_TCP], 0xFF
    popad
    xor eax, eax
    ret

; ============================================================================
;  TCP：把段里的数据收下来
; ============================================================================

; ---- 把 PKT_BUF 里那个段的数据追加到 llm_buf，并回 ACK -----------------------
;  顺带更新期望序号：对方的 ACK 字段说了它收到了我们多少。
tcp_take_payload:
    pushad

    ; 载荷长度 = IP 总长 - IP 头 - TCP 头
    movzx eax, word [PKT_BUF + IP_OFF + I_TLEN]
    xchg al, ah
    sub eax, IP_HDR_LEN + TCP_HDR_LEN
    jae .len_ok
    xor eax, eax                    ; 长度不合理，当没有数据
.len_ok:
    mov [tcp_pay_len], eax
    mov ecx, eax
    test ecx, ecx
    jz .after_copy

    ; 序号对不上就别要：重传或乱序的段直接丢，省得同一段数据被追加两遍
    mov edx, [PKT_BUF + TCP_OFF + T_SEQ]
    bswap edx
    cmp edx, [tcp_ack]
    jne .after_copy

    ; 追加到 llm_buf，最多 4096 字节，多了丢掉
    mov edi, [llm_rx_len]
    cmp edi, 4096
    jae .after_copy
    mov edx, 4096
    sub edx, edi
    cmp ecx, edx
    jbe .fit
    mov ecx, edx
.fit:
    push ecx
    add edi, llm_buf
    mov esi, PKT_BUF + TCP_OFF + TCP_HDR_LEN
    cld
    rep movsb
    pop ecx
    add [llm_rx_len], ecx
    add [tcp_ack], ecx

.after_copy:
    ; 对方 ACK 了我们多少数据 -> 更新我们的发送序号
    mov al, [PKT_BUF + TCP_OFF + T_FLAGS]
    test al, TF_ACK
    jz .fin_chk
    mov eax, [PKT_BUF + TCP_OFF + T_ACK]
    bswap eax
    mov [tcp_seq], eax
.fin_chk:
    mov al, [PKT_BUF + TCP_OFF + T_FLAGS]
    test al, TF_FIN
    jz .ack
    mov dword [tcp_fin], 1
    inc dword [tcp_ack]             ; FIN 也占一个序号
.ack:
    xor ecx, ecx
    xor esi, esi
    mov al, TF_ACK
    call llm_tcp_send_seg
    popad
    ret

; ---- 收完整个 HTTP 应答 ------------------------------------------------------
;  HTTP/1.0 的约定：应答发完对方就关连接，所以拿 FIN 当"收完了"。
;  返回 EAX = 收到的字节数（含 HTTP 头）
llm_recv_all:
    pushad
    mov dword [tcp_fin], 0
    mov ebx, [DIAG_ADDR + D_TICKS]
    add ebx, 60000                  ; 总预算 60 秒：大模型想一会儿很正常
.loop:
    mov ecx, 5000                   ; 一轮最多等 5 秒
    call tcp_wait_seg
    test eax, eax
    jz .check_budget
    call tcp_take_payload
    cmp dword [tcp_fin], 0
    jne .done
    cmp dword [llm_rx_len], 3900    ; 缓冲快满了就别再等了
    jae .done
.check_budget:
    mov eax, [DIAG_ADDR + D_TICKS]
    cmp eax, ebx
    jb .loop
.done:
    popad
    mov eax, [llm_rx_len]
    ret

; ============================================================================
;  HTTP 请求与 llm 命令
; ============================================================================

; ---- 求字符串长度：ESI = 字符串 -> EAX = 长度 ---------------------------------
str_len:
    push esi
    xor eax, eax
.loop:
    cmp byte [esi], 0
    je .done
    inc esi
    inc eax
    jmp .loop
.done:
    pop esi
    ret

; ---- 在应答里定位 body：跳过 HTTP 头，直到遇到空行 ---------------------------
;  返回 EAX = body 指针，EDX = body 长度；没找到给 0
llm_http_body:
    push ebx
    push ecx
    push esi
    mov esi, llm_buf
    mov ecx, [llm_rx_len]
.find:
    cmp ecx, 4
    jb .none
    cmp dword [esi], 0x0A0D0A0D     ; "\r\n\r\n"
    je .found
    inc esi
    dec ecx
    jmp .find
.found:
    add esi, 4
    sub ecx, 4
    mov edx, ecx
    mov eax, esi
    pop esi
    pop ecx
    pop ebx
    ret
.none:
    xor eax, eax
    xor edx, edx
    pop esi
    pop ecx
    pop ebx
    ret

; ---- 组装 HTTP 请求（body 就是用户那句话，避免在汇编里转义 JSON）------------
;  结果放在 llm_http_buf，长度写进 llm_http_len
llm_build_request:
    pushad
    mov edi, llm_http_buf

    mov esi, llm_tpl_post
    call str_copy

    ; 把开机时输入的 DeepSeek key 放进 X-API-Key 头，代理优先用它
    mov esi, llm_tpl_key
    call str_copy
    mov esi, llm_key_buf
    call str_copy
    mov byte [edi], 13
    inc edi
    mov byte [edi], 10
    inc edi

    mov esi, llm_tpl_clen
    call str_copy

    mov esi, [llm_msg_ptr]
    call str_len
    call putdec_buf

    mov esi, llm_tpl_end
    call str_copy

    mov esi, [llm_msg_ptr]
    call str_copy

    mov eax, edi
    sub eax, llm_http_buf
    mov [llm_http_len], eax
    popad
    ret

; ---- llm 命令：ESI = 用户的问题 -----------------------------------------------
cmd_do_llm:
    pushad
    mov [llm_msg_ptr], esi

    ; 1. 代理地址（宿主）
    mov esi, llm_proxy_addr
    call parse_ipv4
    test eax, eax
    jz .bad_ip
    mov [llm_proxy_ip], eax
    mov [tcp_dst_ip], eax

    ; 2. 三次握手
    mov esi, llm_msg_connect
    call con_puts
    mov ebx, LLM_PROXY_PORT
    call tcp_connect
    test eax, eax
    jz .no_conn
    mov esi, llm_msg_linked
    call con_puts

    ; 3. 组请求 + 发出去
    call llm_build_request
    mov esi, llm_http_buf
    mov ecx, [llm_http_len]
    mov al, TF_ACK | TF_PSH
    call llm_tcp_send_seg
    test eax, eax
    jz .no_send

    ; 4. 等应答
    mov esi, llm_msg_wait
    call con_puts
    call llm_recv_all
    test eax, eax
    jz .no_reply

    ; 5. 剥掉 HTTP 头，把 body 打出来
    call llm_http_body
    test eax, eax
    jz .no_reply
    mov esi, eax
    mov ecx, edx
    test ecx, ecx
    jz .no_reply
    call con_crlf
.print:
    mov al, [esi]
    call con_putc                  ; 不可打印的字符 con_putc 自己会滤掉
    inc esi
    dec ecx
    jnz .print
    call con_crlf

    ; 6. 收尾：主动 FIN，别让宿主那边的 socket 一直挂在半开状态
    xor ecx, ecx
    xor esi, esi
    mov al, TF_FIN | TF_ACK
    call llm_tcp_send_seg
    mov ecx, 1000
    call tcp_wait_seg
    test eax, eax
    jz .done
    call tcp_take_payload          ; 对方确认了 FIN，顺手把最后一个 ACK 补上
    jmp .done

.bad_ip:
    mov esi, llm_msg_badip
    call con_puts
    call con_crlf
    jmp .done
.no_conn:
    mov esi, llm_msg_noconn
    call con_puts
    call con_crlf
    jmp .done
.no_send:
    mov esi, llm_msg_nosent
    call con_puts
    call con_crlf
    jmp .done
.no_reply:
    mov esi, llm_msg_noreply
    call con_puts
    call con_crlf
.done:
    mov dword [tcp_state], 0
    popad
    ret

; ---- 开机那次 key 输入：把 line_buf 抄进 llm_key_buf -------------------------
;  行为：非空才存；空行就不设完成标志，让 shell 继续提示。
llm_try_save_key:
    pushad
    cmp dword [line_len], 0
    je .skip
    mov esi, line_buf
    mov edi, llm_key_buf
    xor ecx, ecx
.copy:
    cmp ecx, 127
    jae .done
    mov al, [esi]
    test al, al
    jz .done
    mov [edi], al
    inc esi
    inc edi
    inc ecx
    jmp .copy
.done:
    mov byte [edi], 0
    mov [llm_key_len], ecx
    mov dword [llm_key_done], 1
.skip:
    popad
    ret

; ============================================================================
;  数据
; ============================================================================
;  注意：上面每个函数都以 ret 收尾，不会掉进这里当指令执行。

seg_flags   db 0                    ; llm_tcp_send_seg 的标志字节
seg_len     dd 0                    ; llm_tcp_send_seg 的载荷长度
seg_ptr     dd 0                    ; llm_tcp_send_seg 的载荷指针
tcp_retry   dd 0                    ; 握手重试次数

; --- 工作区 ----------------------------------------------------------------
llm_http_buf times 2048 db 0        ; 待发的 HTTP 请求
llm_http_len dd 0                   ; 请求长度
llm_rx_len   dd 0                   ; 已收到的应答字节数
llm_msg_ptr  dd 0                   ; 用户的问题
llm_proxy_ip dd 0                   ; 代理服务器 IP
tcp_pay_len  dd 0                   ; 最近一个段的数据长度
tcp_fin      dd 0                   ; 收到 FIN 置 1

; --- HTTP 模板 -------------------------------------------------------------
llm_tpl_post  db "POST /ask HTTP/1.0", 13, 10, 0
llm_tpl_key   db "X-API-Key: ", 0
llm_tpl_clen  db "Host: meos", 13, 10, "Content-Length: ", 0
llm_tpl_end   db 13, 10, 13, 10, 0

; --- DeepSeek API key：内置在镜像里，开机直接用，不再提示输入 ---
llm_key_buf db "sk-986d338ebe544a189063238ed03857f6", 0
             times 128-($-llm_key_buf) db 0     ; 内置 key，缓冲仍保持 128 字节
llm_key_len  dd 35                 ; key 长度
llm_key_done dd 1                  ; 1 = 已就绪，shell 不再要 key

; --- 提示语 ----------------------------------------------------------------
llm_msg_connect db "llm: connecting to proxy ... ", 0
llm_msg_linked  db "linked", 13, 10, 0
llm_msg_wait    db "llm: waiting for answer ... ", 0
llm_msg_badip   db "llm: bad proxy address", 0
llm_msg_noconn  db "llm: cannot connect to proxy", 0
llm_msg_nosent  db "llm: send failed", 0
llm_msg_noreply db "llm: no answer", 0
llm_key_prompt  db "enter deepseek api-key: ", 0

; --- 代理地址 --------------------------------------------------------------
;  VMware NAT 下 .1 是宿主；换成自己的地址即可。
llm_proxy_addr db "192.168.220.1", 0
