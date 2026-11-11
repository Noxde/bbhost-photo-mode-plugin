# SPDX-License-Identifier: GPL-3.0-or-later
.intel_syntax noprefix
.text
.globl _start
_start:
  push rbx
  push r12
  push r13
  mov r12, rsi
  call qword ptr [rip + orig]
  mov ebx, eax
  mov rdi, r12
  mov esi, ebx
  call padhook
  mov eax, ebx
  pop r13
  pop r12
  pop rbx
  ret
  .p2align 3, 0x90
orig:
  .quad 0
  .p2align 4, 0x90
padhook:
  push rbp
  mov rbp, rsp
  .byte 0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90
  pop rbp
  ret
