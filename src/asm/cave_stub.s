# SPDX-License-Identifier: GPL-3.0-or-later
.intel_syntax noprefix
.text
.globl _start
_start:
  push rax
  push rcx
  push rdx
  push rsi
  push rdi
  push r8
  push r9
  push r10
  push r11
  push rbp
  mov rbp, rsp
  and rsp, -16
  sub rsp, 512
  fxsave [rsp]
  mov rdi, r12
  mov rsi, r13
  mov rdx, [rbp+80]
  call dummy
  fxrstor [rsp]
  mov rsp, rbp
  pop rbp
  pop r11
  pop r10
  pop r9
  pop r8
  pop rdi
  pop rsi
  pop rdx
  pop rcx
  pop rax
  cmp byte ptr [r12+0xc4], 1
.att_syntax
  jne 0x1de346a
  jmp 0x1de3457
.intel_syntax noprefix
  .p2align 4, 0x90
dummy:
  push rbp
  mov rbp, rsp
  .byte 0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90
  pop rbp
  ret
