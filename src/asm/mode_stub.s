# SPDX-License-Identifier: GPL-3.0-or-later
.intel_syntax noprefix
.text
.globl _start
_start:
  cmp byte ptr [r12+0xc8], 0
.att_syntax
  jne 0x1de346a
.intel_syntax noprefix
  movabs rax, 0x1122334455667788
  cmp byte ptr [rax], 0
.att_syntax
  jne 0x1de346a
.intel_syntax noprefix
  mov byte ptr [r13+0x150], 1
.att_syntax
  jmp 0x1de346a
