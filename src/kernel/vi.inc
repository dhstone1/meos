; ============================================================================
;  vi.inc —— vi 风格最小模态编辑器（v1）
; ----------------------------------------------------------------------------
;  不是 Vim 源码移植（那需要 libc / termios / 大内存），而是按 vi 的操作习惯
;  重写的行式全屏编辑器：
;    普通模式：h j k l 0 $ g G i a o O x dd : （方向键/Home/End/PgUp/PgDn 也认）
;    插入模式：可打印字符、Enter、Backspace、Tab（展开 4 空格）、Esc
;    命令行：  :w  :q  :q!  :wq  :x
;  限制：只写 ext2 `/hd`；文件 <= 12KB；最多 128 行；每行 <= 96 字节。
; ============================================================================

VI_LINES        equ 128
VI_LINE_CAP     equ 96
VI_OUT_CAP      equ 12288
VI_PATH_MAX     equ 64
VI_CMD_MAX      equ 32
VI_MSG_MAX      equ 64
VI_STATUS_MAX   equ 80

VI_MODE_NORMAL  equ 0
VI_MODE_INSERT  equ 1
VI_MODE_COMMAND equ 2

VI_KEY_LEFT     equ 0x80
VI_KEY_RIGHT    equ 0x81
VI_KEY_UP       equ 0x82
VI_KEY_DOWN     equ 0x83
VI_KEY_DEL      equ 0x84
VI_KEY_HOME     equ 0x85
VI_KEY_END      equ 0x86
VI_KEY_PGUP     equ 0x87
VI_KEY_PGDN     equ 0x88

; ---- 命令入口：ESI = 路径（由 FS_CMD 传入） ----------------------------------
cmd_do_vi:
    pushad
    mov  dword [fs_cmd_name], msg_vi_name
    call fs_ensure
    test eax, eax
    jz   .done
    test esi, esi
    jz   .usage
    call fs_skip_spaces
    mov  edi, vi_path
    mov  ecx, VI_PATH_MAX - 1
.copy:
    lodsb
    test al, al
    jz   .copied
    stosb
    dec  ecx
    jnz  .copy
.copied:
    mov  byte [edi], 0
    cmp  byte [vi_path], 0
    je   .usage
    ; 只接受 ext2 挂载点（MeFS 的 VFS 写接口是 -1，devfs 更不能编辑）
    mov  esi, vi_path
    call fs_action
    test eax, eax
    jz   .perr
    lea  esi, [path_buf]
    call vfs_mount_match
    cmp  eax, DRV_EXT2
    jne  .notext2
    call vi_reset
    call vi_load
    test eax, eax
    jz   .done
    call vi_loop
    call con_clear_all
    mov  dword [con_x], 0
    mov  dword [con_y], 0
    jmp  .done
.notext2:
    mov  esi, msg_vi_ext2
    call con_puts
    call con_crlf
    jmp  .done
.usage:
    mov  esi, msg_vi_usage
    call con_puts
    call con_crlf
    jmp  .done
.perr:
    call fs_perror
.done:
    call fs_diag
    popad
    ret

; ---- 复位内存状态 ------------------------------------------------------------
vi_reset:
    pushad
    mov  dword [vi_total], 0
    mov  dword [vi_nlines], 1
    mov  dword [vi_cy], 0
    mov  dword [vi_cx], 0
    mov  dword [vi_top], 0
    mov  dword [vi_xoff], 0
    mov  dword [vi_cmdlen], 0
    mov  byte [vi_mode], VI_MODE_NORMAL
    mov  byte [vi_dirty], 0
    mov  byte [vi_pending], 0
    mov  byte [vi_quit], 0
    mov  byte [vi_msg], 0
    mov  byte [vi_cmd], 0
    mov  edi, vi_llen
    xor  eax, eax
    mov  ecx, VI_LINES / 2
    rep  stosd
    mov  edi, vi_buf
    mov  ecx, VI_LINE_CAP / 4
    rep  stosd
    popad
    ret

; ---- 从磁盘读入：返回 EAX = 1 成功 / 0 失败（失败已打印原因） ----------------
vi_load:
    pushad
    cld
    mov  dword [vi_total], 0
    mov  esi, vi_path
    call vfs_open
    test eax, eax
    js   .open_fail
    mov  [vi_fd], eax
    call vfs_fd_ptr
    cmp  byte [eax + FD_VN + VN_KIND], VN_FILE
    jne  .isdir
.read_loop:
    mov  eax, [vi_fd]
    mov  edi, vi_rd
    mov  ecx, 512
    call vfs_read
    test eax, eax
    js   .io
    jz   .read_done
    mov  [vi_tmp], eax
    xor  ebx, ebx
.parse:
    cmp  ebx, [vi_tmp]
    jae  .next_read
    mov  al, [vi_rd + ebx]
    inc  ebx
    cmp  al, 13
    je   .parse
    cmp  al, 10
    je   .newline
    mov  ecx, [vi_cy]
    movzx edx, word [vi_llen + ecx * 2]
    cmp  edx, VI_LINE_CAP
    jae  .toolong
    mov  edi, ecx
    imul edi, VI_LINE_CAP
    add  edi, vi_buf
    add  edi, edx
    mov  [edi], al
    inc  edx
    mov  [vi_llen + ecx * 2], dx
    inc  dword [vi_total]
    cmp  dword [vi_total], VI_OUT_CAP
    ja   .toolarge
    jmp  .parse
.newline:
    inc  dword [vi_total]
    cmp  dword [vi_total], VI_OUT_CAP
    ja   .toolarge
    mov  eax, [vi_nlines]
    cmp  eax, VI_LINES
    jae  .toomany
    mov  edi, eax
    imul edi, VI_LINE_CAP
    add  edi, vi_buf
    mov  ecx, VI_LINE_CAP / 4
    xor  eax, eax
    rep  stosd
    inc  dword [vi_nlines]
    mov  eax, [vi_nlines]
    dec  eax
    mov  [vi_cy], eax
    mov  word [vi_llen + eax * 2], 0
    jmp  .parse
.next_read:
    jmp  .read_loop
.read_done:
    mov  eax, [vi_fd]
    call vfs_close
    ; 末尾换行产生一个空行：丢掉它，避免每次保存都多一行
    cmp  dword [vi_nlines], 1
    jbe  .normalize
    mov  eax, [vi_nlines]
    dec  eax
    cmp  word [vi_llen + eax * 2], 0
    jne  .normalize
    dec  dword [vi_nlines]
.normalize:
    mov  dword [vi_cy], 0
    mov  dword [vi_cx], 0
    mov  dword [vi_top], 0
    mov  dword [vi_xoff], 0
    mov  byte [vi_dirty], 0
    mov  dword [esp + 28], 1
    popad
    ret
.open_fail:
    cmp  dword [fs_err], FS_NOENT
    jne  .open_err
    mov  dword [fs_err], FS_OK
    mov  dword [DIAG_ADDR + D_EXT2_ERR], 0
    mov  dword [esp + 28], 1
    popad
    ret
.open_err:
    mov  esi, msg_vi_openfail
    jmp  .fail_msg
.isdir:
    mov  esi, msg_vi_isdir
    jmp  .fail_close
.toolong:
    mov  esi, msg_vi_toolong
    jmp  .fail_close
.toomany:
    mov  esi, msg_vi_toomany
    jmp  .fail_close
.toolarge:
    mov  esi, msg_vi_toolarge
    jmp  .fail_close
.io:
    mov  esi, msg_vi_openfail
.fail_close:
    mov  eax, [vi_fd]
    call vfs_close
.fail_msg:
    call con_puts
    call con_crlf
    mov  dword [esp + 28], 0
    popad
    ret

; ---- 主循环 ------------------------------------------------------------------
vi_loop:
    call vi_render
.again:
    call vi_getkey
    cmp  byte [vi_mode], VI_MODE_INSERT
    je   vi_key_insert
    cmp  byte [vi_mode], VI_MODE_COMMAND
    je   vi_key_command
    call vi_key_normal
.after:
    call vi_adjust_view
    call vi_render
    cmp  byte [vi_quit], 0
    jne  .done
    jmp  .again
.done:
    ret

; ---- 阻塞取一个键：EAX = 0..255 ---------------------------------------------
vi_getkey:
    call kbd_pop
    cmp  eax, -1
    jne  .got
    hlt
    jmp  vi_getkey
.got:
    ret

; ---- 普通模式 ----------------------------------------------------------------
vi_key_normal:
    cmp  al, 27
    je   .esc
    cmp  byte [vi_pending], 'd'
    jne  .not_dd
    mov  byte [vi_pending], 0
    cmp  al, 'd'
    jne  .done
    call vi_delete_line
    jmp  .done
.not_dd:
    cmp  al, 'd'
    jne  .not_cmd_d
    mov  byte [vi_pending], 'd'
    jmp  .done
.not_cmd_d:
    cmp  al, 'h'
    je   .left
    cmp  al, VI_KEY_LEFT
    je   .left
    cmp  al, 'l'
    je   .right
    cmp  al, VI_KEY_RIGHT
    je   .right
    cmp  al, 'j'
    je   .down
    cmp  al, VI_KEY_DOWN
    je   .down
    cmp  al, 'k'
    je   .up
    cmp  al, VI_KEY_UP
    je   .up
    cmp  al, '0'
    je   .home
    cmp  al, VI_KEY_HOME
    je   .home
    cmp  al, '$'
    je   .end
    cmp  al, VI_KEY_END
    je   .end
    cmp  al, 'g'
    je   .top
    cmp  al, 'G'
    je   .bottom
    cmp  al, VI_KEY_PGUP
    je   .pgup
    cmp  al, VI_KEY_PGDN
    je   .pgdn
    cmp  al, 'i'
    je   .insert
    cmp  al, 'a'
    je   .append
    cmp  al, 'o'
    je   .open_below
    cmp  al, 'O'
    je   .open_above
    cmp  al, 'x'
    je   .delchar
    cmp  al, VI_KEY_DEL
    je   .delchar
    cmp  al, ':'
    je   .command
    jmp  .done
.esc:
    mov  byte [vi_pending], 0
    jmp  .done
.left:
    call vi_move_left
    jmp  .done
.right:
    call vi_move_right
    jmp  .done
.down:
    call vi_move_down
    jmp  .done
.up:
    call vi_move_up
    jmp  .done
.home:
    mov  dword [vi_cx], 0
    jmp  .done
.end:
    mov  eax, [vi_cy]
    movzx eax, word [vi_llen + eax * 2]
    mov  [vi_cx], eax
    call vi_clamp_cx
    jmp  .done
.top:
    mov  dword [vi_cy], 0
    call vi_clamp_cx
    jmp  .done
.bottom:
    mov  eax, [vi_nlines]
    dec  eax
    mov  [vi_cy], eax
    call vi_clamp_cx
    jmp  .done
.pgup:
    mov  eax, [vi_cy]
    sub  eax, [vi_rows]
    jns  .pgup_set
    xor  eax, eax
.pgup_set:
    mov  [vi_cy], eax
    call vi_clamp_cx
    jmp  .done
.pgdn:
    mov  eax, [vi_cy]
    add  eax, [vi_rows]
    mov  edx, [vi_nlines]
    dec  edx
    cmp  eax, edx
    jbe  .pgdn_set
    mov  eax, edx
.pgdn_set:
    mov  [vi_cy], eax
    call vi_clamp_cx
    jmp  .done
.insert:
    mov  byte [vi_mode], VI_MODE_INSERT
    jmp  .done
.append:
    call vi_clamp_cx
    mov  eax, [vi_cy]
    movzx edx, word [vi_llen + eax * 2]
    cmp  [vi_cx], edx
    jae  .append_mode
    inc  dword [vi_cx]
.append_mode:
    mov  byte [vi_mode], VI_MODE_INSERT
    jmp  .done
.open_below:
    mov  eax, [vi_cy]
    inc  eax
    call vi_insert_line
    test eax, eax
    jz   .done
    inc  dword [vi_cy]
    mov  dword [vi_cx], 0
    mov  byte [vi_mode], VI_MODE_INSERT
    jmp  .done
.open_above:
    mov  eax, [vi_cy]
    call vi_insert_line
    test eax, eax
    jz   .done
    mov  dword [vi_cx], 0
    mov  byte [vi_mode], VI_MODE_INSERT
    jmp  .done
.delchar:
    call vi_delete_char
    call vi_clamp_cx
    jmp  .done
.command:
    mov  byte [vi_mode], VI_MODE_COMMAND
    mov  dword [vi_cmdlen], 0
    mov  byte [vi_cmd], 0
    mov  byte [vi_msg], 0
.done:
    ret

; ---- 插入模式 ----------------------------------------------------------------
vi_key_insert:
    cmp  al, 27
    je   .esc
    cmp  al, 13
    je   .enter
    cmp  al, 8
    je   .backspace
    cmp  al, 9
    je   .tab
    cmp  al, VI_KEY_LEFT
    je   .left
    cmp  al, VI_KEY_RIGHT
    je   .right
    cmp  al, VI_KEY_UP
    je   .up
    cmp  al, VI_KEY_DOWN
    je   .down
    cmp  al, VI_KEY_HOME
    je   .home
    cmp  al, VI_KEY_END
    je   .end
    cmp  al, VI_KEY_DEL
    je   .delchar
    cmp  al, 32
    jb   .done
    cmp  al, 126
    ja   .done
    call vi_insert_char
    jmp  .done
.esc:
    mov  byte [vi_mode], VI_MODE_NORMAL
    cmp  dword [vi_cx], 0
    je   .done
    dec  dword [vi_cx]
    jmp  .done
.enter:
    call vi_split_line
    jmp  .done
.backspace:
    cmp  dword [vi_cx], 0
    je   .join
    dec  dword [vi_cx]
    call vi_delete_char
    jmp  .done
.join:
    call vi_join_prev
    jmp  .done
.tab:
    mov  dword [vi_j], 4
.tabloop:
    mov  al, ' '
    call vi_insert_char
    dec  dword [vi_j]
    jnz  .tabloop
    jmp  .done
.left:
    call vi_move_left
    jmp  .done
.right:
    call vi_move_right
    jmp  .done
.up:
    call vi_move_up
    jmp  .done
.down:
    call vi_move_down
    jmp  .done
.home:
    mov  dword [vi_cx], 0
    jmp  .done
.end:
    mov  eax, [vi_cy]
    movzx eax, word [vi_llen + eax * 2]
    mov  [vi_cx], eax
    jmp  .done
.delchar:
    call vi_delete_char
.done:
    ret

; ---- 命令行模式 --------------------------------------------------------------
vi_key_command:
    cmp  al, 27
    je   .cancel
    cmp  al, 13
    je   .run
    cmp  al, 8
    je   .back
    cmp  al, 32
    jb   .done
    cmp  al, 126
    ja   .done
    mov  ecx, [vi_cmdlen]
    cmp  ecx, VI_CMD_MAX - 1
    jae  .done
    mov  [vi_cmd + ecx], al
    inc  ecx
    mov  [vi_cmdlen], ecx
    mov  byte [vi_cmd + ecx], 0
    jmp  .done
.back:
    mov  ecx, [vi_cmdlen]
    test ecx, ecx
    jz   .done
    dec  ecx
    mov  [vi_cmdlen], ecx
    mov  byte [vi_cmd + ecx], 0
    jmp  .done
.cancel:
    mov  byte [vi_mode], VI_MODE_NORMAL
    jmp  .done
.run:
    call vi_run_command
    cmp  byte [vi_quit], 0
    jne  .done
    mov  byte [vi_mode], VI_MODE_NORMAL
.done:
    ret

vi_run_command:
    pushad
    mov  esi, vi_cmd
    mov  edi, msg_cmd_w
    call str_eq
    test eax, eax
    jnz  .do_w
    mov  esi, vi_cmd
    mov  edi, msg_cmd_q
    call str_eq
    test eax, eax
    jnz  .do_q
    mov  esi, vi_cmd
    mov  edi, msg_cmd_qbang
    call str_eq
    test eax, eax
    jnz  .do_quit
    mov  esi, vi_cmd
    mov  edi, msg_cmd_wq
    call str_eq
    test eax, eax
    jnz  .do_wq
    mov  esi, vi_cmd
    mov  edi, msg_cmd_x
    call str_eq
    test eax, eax
    jnz  .do_wq
    cmp  dword [vi_cmdlen], 0
    je   .done
    mov  esi, msg_vi_unknown
    call vi_set_msg
    jmp  .done
.do_w:
    call vi_save
    jmp  .done
.do_q:
    cmp  byte [vi_dirty], 0
    je   .do_quit
    mov  esi, msg_vi_nodirty
    call vi_set_msg
    jmp  .done
.do_wq:
    call vi_save
    test eax, eax
    jz   .done
.do_quit:
    mov  byte [vi_quit], 1
.done:
    popad
    ret

; ---- 保存：构建连续缓冲 → create/truncate → open → write → close -------------
vi_save:
    pushad
    cld
    mov  dword [vi_total], 0
    xor  ebx, ebx
.build:
    cmp  ebx, [vi_nlines]
    jae  .built
    movzx ecx, word [vi_llen + ebx * 2]
    mov  eax, [vi_total]
    lea  edx, [eax + ecx + 1]
    cmp  edx, VI_OUT_CAP
    ja   .toolarge
    mov  edi, vi_out
    add  edi, eax
    mov  esi, ebx
    imul esi, VI_LINE_CAP
    add  esi, vi_buf
    rep  movsb
    mov  al, 10
    stosb
    mov  [vi_total], edx
    inc  ebx
    jmp  .build
.built:
    cmp  dword [vi_nlines], 1
    jne  .create
    cmp  word [vi_llen], 0
    jne  .create
    mov  dword [vi_total], 0
.create:
    mov  esi, vi_path
    xor  eax, eax
    call vfs_create
    test eax, eax
    jz   .writefail
    mov  esi, vi_path
    call vfs_open
    test eax, eax
    js   .writefail
    mov  [vi_fd], eax
    cmp  dword [vi_total], 0
    je   .close_ok
    mov  eax, [vi_fd]
    mov  edi, vi_out
    mov  ecx, [vi_total]
    call vfs_write
    cmp  eax, [vi_total]
    jne  .writefail_close
.close_ok:
    mov  eax, [vi_fd]
    call vfs_close
    mov  byte [vi_dirty], 0
    mov  esi, msg_vi_written
    call vi_set_msg
    mov  dword [esp + 28], 1
    popad
    ret
.writefail_close:
    mov  eax, [vi_fd]
    call vfs_close
.writefail:
    mov  esi, msg_vi_writefail
    call vi_set_msg
    mov  dword [esp + 28], 0
    popad
    ret
.toolarge:
    mov  esi, msg_vi_toolarge
    call vi_set_msg
    mov  dword [esp + 28], 0
    popad
    ret

; ---- 光标移动 ----------------------------------------------------------------
vi_move_left:
    cmp  dword [vi_cx], 0
    je   .done
    dec  dword [vi_cx]
.done:
    ret
vi_move_right:
    call vi_clamp_cx
    mov  eax, [vi_cy]
    movzx edx, word [vi_llen + eax * 2]
    cmp  [vi_cx], edx
    jae  .done
    inc  dword [vi_cx]
    call vi_clamp_cx
.done:
    ret
vi_move_down:
    mov  eax, [vi_nlines]
    dec  eax
    cmp  [vi_cy], eax
    jae  .clamp
    inc  dword [vi_cy]
.clamp:
    call vi_clamp_cx
    ret
vi_move_up:
    cmp  dword [vi_cy], 0
    je   .clamp
    dec  dword [vi_cy]
.clamp:
    call vi_clamp_cx
    ret
vi_clamp_cx:
    mov  eax, [vi_cy]
    movzx eax, word [vi_llen + eax * 2]
    cmp  byte [vi_mode], VI_MODE_INSERT
    je   .limit
    test eax, eax
    jz   .limit
    dec  eax
.limit:
    cmp  [vi_cx], eax
    jbe  .done
    mov  [vi_cx], eax
.done:
    ret

; ---- 行指针：EAX = 行号 → EAX = 行首地址 -------------------------------------
vi_line_ptr:
    imul eax, VI_LINE_CAP
    add  eax, vi_buf
    ret

; ---- 插入/删除字符 -----------------------------------------------------------
vi_insert_char:
    pushad
    mov  [vi_ch], al
    mov  eax, [vi_cy]
    movzx ecx, word [vi_llen + eax * 2]
    cmp  ecx, VI_LINE_CAP
    jae  .done
    call vi_line_ptr
    mov  edi, eax
    mov  edx, [vi_cx]
    mov  ecx, [vi_cy]
    movzx ecx, word [vi_llen + ecx * 2]
    sub  ecx, edx
    jz   .store
    lea  esi, [edi + ecx]
    add  esi, edx
    dec  esi
    lea  edi, [edi + ecx]
    add  edi, edx
    std
    rep  movsb
    cld
.store:
    mov  eax, [vi_cy]
    call vi_line_ptr
    add  eax, [vi_cx]
    mov  dl, [vi_ch]
    mov  [eax], dl
    mov  eax, [vi_cy]
    inc  word [vi_llen + eax * 2]
    inc  dword [vi_cx]
    mov  byte [vi_dirty], 1
.done:
    popad
    ret

vi_delete_char:
    pushad
    cld
    mov  eax, [vi_cy]
    movzx ecx, word [vi_llen + eax * 2]
    mov  edx, [vi_cx]
    cmp  edx, ecx
    jae  .done
    sub  ecx, edx
    dec  ecx
    jz   .shrink
    call vi_line_ptr
    lea  esi, [eax + edx + 1]
    lea  edi, [eax + edx]
    rep  movsb
.shrink:
    mov  eax, [vi_cy]
    dec  word [vi_llen + eax * 2]
    mov  byte [vi_dirty], 1
.done:
    popad
    ret

; ---- 行操作 ------------------------------------------------------------------
vi_split_line:
    pushad
    cld
    mov  eax, [vi_nlines]
    cmp  eax, VI_LINES
    jae  .full
    mov  ebx, [vi_cy]
    movzx eax, word [vi_llen + ebx * 2]
    mov  [vi_len_tmp], eax              ; 旧行长
    mov  eax, [vi_cx]
    mov  [vi_tmp], eax                  ; 切点
    lea  eax, [ebx + 1]
    call vi_shift_down
    lea  eax, [ebx + 1]
    call vi_line_ptr
    mov  edi, eax
    mov  eax, ebx
    call vi_line_ptr
    add  eax, [vi_tmp]
    mov  esi, eax
    mov  ecx, [vi_len_tmp]
    sub  ecx, [vi_tmp]
    rep  movsb
    mov  eax, [vi_len_tmp]
    sub  eax, [vi_tmp]
    lea  edx, [ebx + 1]
    mov  [vi_llen + edx * 2], ax
    mov  eax, [vi_tmp]
    mov  [vi_llen + ebx * 2], ax
    inc  dword [vi_nlines]
    inc  dword [vi_cy]
    mov  dword [vi_cx], 0
    mov  byte [vi_dirty], 1
    jmp  .done
.full:
    mov  esi, msg_vi_toomany
    call vi_set_msg
.done:
    popad
    ret

vi_insert_line:
    ; EAX = 新行要占的槽号 → EAX = 1/0
    pushad
    mov  [vi_i], eax
    cmp  dword [vi_nlines], VI_LINES
    jae  .full
    call vi_shift_down
    mov  eax, [vi_i]
    call vi_line_ptr
    mov  edi, eax
    mov  ecx, VI_LINE_CAP / 4
    xor  eax, eax
    rep  stosd
    mov  eax, [vi_i]
    mov  word [vi_llen + eax * 2], 0
    inc  dword [vi_nlines]
    mov  dword [esp + 28], 1
    popad
    ret
.full:
    mov  esi, msg_vi_toomany
    call vi_set_msg
    mov  dword [esp + 28], 0
    popad
    ret

vi_delete_line:
    pushad
    cmp  dword [vi_nlines], 1
    jne  .multi
    xor  eax, eax
    call vi_line_ptr
    mov  edi, eax
    mov  ecx, VI_LINE_CAP / 4
    xor  eax, eax
    rep  stosd
    mov  word [vi_llen], 0
    mov  dword [vi_cx], 0
    mov  byte [vi_dirty], 1
    jmp  .done
.multi:
    mov  eax, [vi_cy]
    call vi_shift_up
    mov  eax, [vi_nlines]
    cmp  [vi_cy], eax
    jb   .clamp
    dec  eax
    mov  [vi_cy], eax
.clamp:
    call vi_clamp_cx
    mov  byte [vi_dirty], 1
.done:
    popad
    ret

vi_join_prev:
    pushad
    cld
    cmp  dword [vi_cy], 0
    je   .done
    mov  ebx, [vi_cy]
    dec  ebx
    movzx ecx, word [vi_llen + ebx * 2]
    mov  [vi_tmp], ecx
    movzx edx, word [vi_llen + ebx * 2 + 2]
    mov  eax, ecx
    add  eax, edx
    cmp  eax, VI_LINE_CAP
    ja   .full
    mov  eax, ebx
    call vi_line_ptr
    add  eax, ecx
    mov  edi, eax
    lea  eax, [ebx + 1]
    call vi_line_ptr
    mov  esi, eax
    mov  ecx, edx
    rep  movsb
    mov  eax, ebx
    mov  ecx, [vi_tmp]
    add  ecx, edx
    mov  [vi_llen + eax * 2], cx
    lea  eax, [ebx + 1]
    call vi_shift_up
    mov  [vi_cy], ebx
    mov  eax, [vi_tmp]
    mov  [vi_cx], eax
    call vi_clamp_cx
    mov  byte [vi_dirty], 1
    jmp  .done
.full:
    mov  esi, msg_vi_toolong
    call vi_set_msg
.done:
    popad
    ret

; ---- 行块搬移：vi_i = 槽号；vi_j = 循环下标 --------------------------------
vi_shift_down:
    pushad
    cld
    mov  [vi_i], eax
    mov  eax, [vi_nlines]
    dec  eax
    mov  [vi_j], eax
.loop:
    mov  eax, [vi_j]
    cmp  eax, [vi_i]
    jl   .done
    call vi_line_ptr
    mov  esi, eax
    mov  eax, [vi_j]
    inc  eax
    call vi_line_ptr
    mov  edi, eax
    mov  ecx, VI_LINE_CAP
    rep  movsb
    mov  eax, [vi_j]
    movzx edx, word [vi_llen + eax * 2]
    inc  eax
    mov  [vi_llen + eax * 2], dx
    dec  dword [vi_j]
    jmp  .loop
.done:
    popad
    ret

vi_shift_up:
    pushad
    cld
    mov  [vi_i], eax
    mov  eax, [vi_nlines]
    dec  eax
    mov  [vi_j], eax
.loop:
    mov  eax, [vi_i]
    cmp  eax, [vi_j]
    jae  .done
    inc  eax
    call vi_line_ptr
    mov  esi, eax
    mov  eax, [vi_i]
    call vi_line_ptr
    mov  edi, eax
    mov  ecx, VI_LINE_CAP
    rep  movsb
    mov  eax, [vi_i]
    inc  eax
    movzx edx, word [vi_llen + eax * 2]
    dec  eax
    mov  [vi_llen + eax * 2], dx
    inc  dword [vi_i]
    jmp  .loop
.done:
    dec  dword [vi_nlines]
    popad
    ret

; ---- 视口与渲染 --------------------------------------------------------------
vi_adjust_view:
    pushad
    mov  eax, [con_rows]
    dec  eax
    mov  [vi_rows], eax
    mov  eax, [con_cols]
    mov  [vi_cols], eax
    ; 垂直
    mov  eax, [vi_cy]
    cmp  eax, [vi_top]
    jae  .below_top
    mov  [vi_top], eax
.below_top:
    mov  eax, [vi_top]
    add  eax, [vi_rows]
    cmp  [vi_cy], eax
    jb   .vert_ok
    mov  eax, [vi_cy]
    sub  eax, [vi_rows]
    inc  eax
    mov  [vi_top], eax
.vert_ok:
    ; 水平
    mov  eax, [vi_cx]
    cmp  eax, [vi_xoff]
    jae  .right_of_off
    mov  [vi_xoff], eax
.right_of_off:
    mov  eax, [vi_xoff]
    add  eax, [vi_cols]
    cmp  [vi_cx], eax
    jb   .horiz_ok
    mov  eax, [vi_cx]
    sub  eax, [vi_cols]
    inc  eax
    mov  [vi_xoff], eax
.horiz_ok:
    ; top 不能超过最后一行
    mov  eax, [vi_nlines]
    cmp  eax, [vi_rows]
    jbe  .top_zero
    sub  eax, [vi_rows]
    cmp  [vi_top], eax
    jbe  .done
    mov  [vi_top], eax
    jmp  .done
.top_zero:
    mov  dword [vi_top], 0
.done:
    popad
    ret

vi_render:
    pushad
    call con_erase_cursor
    mov  eax, [con_cols]
    mov  [vi_cols], eax
    mov  eax, [con_rows]
    dec  eax
    mov  [vi_rows], eax
    mov  dword [vi_r], 0
.rowloop:
    mov  eax, [vi_r]
    cmp  eax, [vi_rows]
    jae  .status
    mov  dword [vi_c], 0
.colloop:
    mov  eax, [vi_c]
    cmp  eax, [vi_cols]
    jae  .nextrow
    mov  edx, [vi_r]
    add  edx, [vi_top]
    cmp  edx, [vi_nlines]
    jae  .space
    movzx esi, word [vi_llen + edx * 2]
    mov  eax, [vi_xoff]
    add  eax, [vi_c]
    cmp  eax, esi
    jae  .space
    mov  edi, edx
    imul edi, VI_LINE_CAP
    add  edi, vi_buf
    add  edi, eax
    mov  al, [edi]
    jmp  .draw
.space:
    mov  al, ' '
.draw:
    mov  ebx, [vi_c]
    mov  ecx, [vi_r]
    call con_putc_at
    inc  dword [vi_c]
    jmp  .colloop
.nextrow:
    inc  dword [vi_r]
    jmp  .rowloop
.status:
    call vi_build_status
    mov  dword [vi_c], 0
.sloop:
    mov  eax, [vi_c]
    cmp  eax, [vi_cols]
    jae  .cursor
    cmp  eax, VI_STATUS_MAX - 1
    jae  .s_space
    mov  al, [vi_status + eax]
    test al, al
    jz   .s_space
    jmp  .s_draw
.s_space:
    mov  al, ' '
.s_draw:
    mov  ebx, [vi_c]
    mov  ecx, [vi_rows]
    call con_putc_at
    inc  dword [vi_c]
    jmp  .sloop
.cursor:
    cmp  byte [vi_mode], VI_MODE_COMMAND
    jne  .text_cursor
    mov  eax, [vi_rows]
    mov  [con_y], eax
    mov  eax, [vi_cmdlen]
    inc  eax
    jmp  .set_cx
.text_cursor:
    mov  eax, [vi_cy]
    sub  eax, [vi_top]
    jns  .set_cy
    xor  eax, eax
.set_cy:
    mov  [con_y], eax
    mov  eax, [vi_cx]
    sub  eax, [vi_xoff]
    jns  .set_cx
    xor  eax, eax
.set_cx:
    cmp  eax, [vi_cols]
    jb   .cx_ok
    mov  eax, [vi_cols]
    dec  eax
.cx_ok:
    mov  [con_x], eax
    call con_draw_cursor
    popad
    ret

; ---- 状态行字符串 ------------------------------------------------------------
vi_build_status:
    pushad
    mov  edi, vi_status
    mov  ecx, VI_STATUS_MAX / 4
    xor  eax, eax
    rep  stosd
    mov  dword [vi_sptr], vi_status
    cmp  byte [vi_mode], VI_MODE_COMMAND
    je   .cmd
    cmp  byte [vi_mode], VI_MODE_INSERT
    je   .ins
    mov  esi, msg_mode_normal
    jmp  .mode
.ins:
    mov  esi, msg_mode_insert
.mode:
    call vi_sputs
    mov  al, ' '
    call vi_sputc
    mov  esi, vi_path
    call vi_sputs
    mov  al, ' '
    call vi_sputc
    mov  eax, [vi_cy]
    inc  eax
    call vi_sputdec
    mov  al, ':'
    call vi_sputc
    mov  eax, [vi_cx]
    inc  eax
    call vi_sputdec
    cmp  byte [vi_dirty], 0
    je   .msg
    mov  al, ' '
    call vi_sputc
    mov  al, '+'
    call vi_sputc
.msg:
    cmp  byte [vi_msg], 0
    je   .done
    mov  esi, msg_sep2
    call vi_sputs
    mov  esi, vi_msg
    call vi_sputs
    jmp  .done
.cmd:
    mov  al, ':'
    call vi_sputc
    mov  esi, vi_cmd
    call vi_sputs
    mov  esi, msg_cmd_hint
    call vi_sputs
.done:
    mov  edi, [vi_sptr]
    mov  byte [edi], 0
    popad
    ret

vi_sputc:
    push edi
    mov  edi, [vi_sptr]
    mov  edx, vi_status + VI_STATUS_MAX - 1
    cmp  edi, edx
    jae  .done
    stosb
    mov  [vi_sptr], edi
.done:
    pop  edi
    ret

vi_sputs:
    push esi
.loop:
    lodsb
    test al, al
    jz   .done
    call vi_sputc
    jmp  .loop
.done:
    pop  esi
    ret

vi_sputdec:
    push eax
    push ebx
    push ecx
    push edx
    mov  ebx, 10
    xor  ecx, ecx
.div:
    xor  edx, edx
    div  ebx
    push edx
    inc  ecx
    test eax, eax
    jnz  .div
.pop:
    pop  eax
    add  al, '0'
    call vi_sputc
    dec  ecx
    jnz  .pop
    pop  edx
    pop  ecx
    pop  ebx
    pop  eax
    ret

vi_set_msg:
    pushad
    mov  edi, vi_msg
    mov  ecx, VI_MSG_MAX - 1
.loop:
    lodsb
    test al, al
    jz   .done
    stosb
    dec  ecx
    jnz  .loop
.done:
    mov  byte [edi], 0
    popad
    ret

%ifdef VI_TEST
; ---- 非交互自测（仅 -dVI_TEST）：写两行 → 读回 → 改短 → 读回 → 删除 ---------
vi_selftest:
    pushad
    mov  esi, msg_vi_testpath
    mov  edi, vi_path
.path_copy:
    lodsb
    stosb
    test al, al
    jnz  .path_copy
    mov  dword [DIAG_ADDR + D_VI_TEST], 0
    call vi_reset
    mov  dword [vi_nlines], 2
    mov  word [vi_llen], 3
    mov  word [vi_llen + 2], 5
    mov  esi, msg_vi_testline0
    mov  edi, vi_buf
    mov  ecx, 3
    rep  movsb
    mov  esi, msg_vi_testline1
    mov  edi, vi_buf + VI_LINE_CAP
    mov  ecx, 5
    rep  movsb
    call vi_save
    test eax, eax
    jz   .fail1
    call vi_reset
    call vi_load
    test eax, eax
    jz   .fail2
    cmp  dword [vi_nlines], 2
    jne  .fail3
    cmp  word [vi_llen], 3
    jne  .fail3
    cmp  word [vi_llen + 2], 5
    jne  .fail3
    cmp  byte [vi_buf + 0], 'a'
    jne  .fail3
    cmp  byte [vi_buf + 1], 'b'
    jne  .fail3
    cmp  byte [vi_buf + 2], 'c'
    jne  .fail3
    cmp  byte [vi_buf + VI_LINE_CAP + 0], 'd'
    jne  .fail3
    cmp  byte [vi_buf + VI_LINE_CAP + 4], 'h'
    jne  .fail3
    ; Enter split: line0 abc at cx=2 -> ab / c
    mov  dword [vi_cy], 0
    mov  dword [vi_cx], 2
    call vi_split_line
    cmp  dword [vi_nlines], 3
    jne  .fail7
    cmp  word [vi_llen], 2
    jne  .fail7
    cmp  word [vi_llen + 2], 1
    jne  .fail7
    cmp  word [vi_llen + 4], 5
    jne  .fail7
    cmp  byte [vi_buf + 2], 'c'
    jne  .fail7
    cmp  byte [vi_buf + VI_LINE_CAP + 0], 'c'
    jne  .fail7
    cmp  byte [vi_buf + VI_LINE_CAP * 2 + 0], 'd'
    jne  .fail7
    ; Insert char: append Z to ab
    mov  dword [vi_cy], 0
    mov  dword [vi_cx], 2
    mov  byte [vi_mode], VI_MODE_INSERT
    mov  al, 'Z'
    call vi_insert_char
    cmp  word [vi_llen], 3
    jne  .fail7
    cmp  byte [vi_buf + 2], 'Z'
    jne  .fail7
    ; Delete the Z
    mov  dword [vi_cx], 2
    call vi_delete_char
    cmp  word [vi_llen], 2
    jne  .fail7
    cmp  byte [vi_buf + 1], 'b'
    jne  .fail7
    mov  byte [vi_mode], VI_MODE_NORMAL
    ; 改成更短的内容，验证 create 的截断路径释放旧块
    call vi_reset
    mov  dword [vi_nlines], 1
    mov  word [vi_llen], 2
    mov  byte [vi_buf + 0], 'x'
    mov  byte [vi_buf + 1], 'y'
    call vi_save
    test eax, eax
    jz   .fail4
    call vi_reset
    call vi_load
    test eax, eax
    jz   .fail5
    cmp  dword [vi_nlines], 1
    jne  .fail5
    cmp  word [vi_llen], 2
    jne  .fail5
    cmp  byte [vi_buf + 0], 'x'
    jne  .fail5
    cmp  byte [vi_buf + 1], 'y'
    jne  .fail5
    mov  esi, vi_path
    call vfs_unlink
    test eax, eax
    jz   .fail6
    mov  dword [DIAG_ADDR + D_VI_TEST], 0x600D
    popad
    ret
.fail1:
    mov  eax, 1
    jmp  .fail
.fail2:
    mov  eax, 2
    jmp  .fail
.fail3:
    mov  eax, 3
    jmp  .fail
.fail4:
    mov  eax, 4
    jmp  .fail
.fail5:
    mov  eax, 5
    jmp  .fail
.fail6:
    mov  eax, 6
    jmp  .fail
.fail7:
    mov  eax, 7
.fail:
    mov  [DIAG_ADDR + D_VI_TEST], eax
    popad
    ret
%endif

; ---- 数据 --------------------------------------------------------------------
vi_buf       times VI_LINES * VI_LINE_CAP db 0
vi_llen      times VI_LINES dw 0
vi_out       times VI_OUT_CAP db 0
vi_rd        times 512 db 0
vi_nlines    dd 1
vi_cy        dd 0
vi_cx        dd 0
vi_top       dd 0
vi_xoff      dd 0
vi_mode      db VI_MODE_NORMAL
vi_dirty     db 0
vi_pending   db 0
vi_quit      db 0
vi_ch        db 0
vi_path      times VI_PATH_MAX db 0
vi_cmd       times VI_CMD_MAX db 0
vi_cmdlen    dd 0
vi_msg       times VI_MSG_MAX db 0
vi_status    times VI_STATUS_MAX db 0
vi_fd        dd 0
vi_total     dd 0
vi_i         dd 0
vi_j         dd 0
vi_tmp       dd 0
vi_len_tmp   dd 0
vi_sptr      dd 0
vi_cols      dd 0
vi_rows      dd 0
vi_r         dd 0
vi_c         dd 0

msg_vi_name      db "vi", 0
msg_vi_usage     db "usage: vi <file>   (ext2 /hd only)", 0
msg_vi_ext2      db "vi: only ext2 /hd is writable", 0
msg_vi_openfail  db "vi: cannot open file", 0
msg_vi_isdir     db "vi: is a directory", 0
msg_vi_toolong   db "vi: line too long (max 96)", 0
msg_vi_toomany   db "vi: too many lines (max 128)", 0
msg_vi_toolarge  db "vi: file too large (max 12288)", 0
msg_vi_writefail db "vi: write failed", 0
msg_vi_written   db "written", 0
msg_vi_nodirty   db "no write since last change (:q!)", 0
msg_vi_unknown   db "unknown command", 0
msg_mode_normal  db "NORMAL", 0
msg_mode_insert  db "INSERT", 0
msg_sep2         db "  ", 0
msg_cmd_hint     db "  Enter=run Esc=cancel", 0
msg_cmd_w        db "w", 0
msg_cmd_q        db "q", 0
msg_cmd_qbang    db "q!", 0
msg_cmd_wq       db "wq", 0
msg_cmd_x        db "x", 0
msg_vi_testpath  db "/hd/vitest.txt", 0
msg_vi_testline0 db "abc", 0
msg_vi_testline1 db "defgh", 0
