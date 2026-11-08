.386
.model flat, stdcall
option casemap:none

includelib kernel32.lib

GetStdHandle PROTO :DWORD
WriteConsoleA PROTO :DWORD, :DWORD, :DWORD, :DWORD, :DWORD

SetConsoleCP PROTO :DWORD
SetConsoleOutputCP PROTO :DWORD

.data
debugBuffer BYTE 128 DUP(0)
debugBytesWritten DWORD 0

.code
DebugNumber PROC value:DWORD, bits:DWORD
    LOCAL digitCount:DWORD
    LOCAL hOutput:DWORD

    push ebx
    push ecx
    push edx
    push esi
    push edi

    mov eax, value
    mov ecx, bits

    cmp ecx, 1
    jb DN_USE32
    cmp ecx, 32
    ja DN_USE32
    cmp ecx, 32
    je DN_MASK_DONE

    mov edx, 1
    shl edx, cl
    dec edx
    and eax, edx
    jmp DN_MASK_DONE

DN_USE32:
    mov ecx, 32

DN_MASK_DONE:
    mov digitCount, 0
    lea edi, debugBuffer

    test eax, eax
    jnz DN_CONVERT
    mov BYTE PTR [edi], '0'
    inc edi
    mov digitCount, 1
    jmp DN_PRINT

DN_CONVERT:
    mov ebx, 10

DN_LOOP:
    xor edx, edx
    div ebx
    push edx
    inc digitCount
    test eax, eax
    jnz DN_LOOP

DN_WRITE:
    pop eax
    add al, '0'
    mov BYTE PTR [edi], al
    inc edi
    dec digitCount
    jnz DN_WRITE

    ; Число записано в буфер. Повторно вычисляем длину:
    ; EDI - начало debugBuffer.
    lea eax, debugBuffer
    sub edi, eax
    mov digitCount, edi

DN_PRINT:
    invoke GetStdHandle, -11
    mov hOutput, eax
    invoke WriteConsoleA, hOutput, ADDR debugBuffer, digitCount, ADDR debugBytesWritten, 0

    mov BYTE PTR debugBuffer[0], 13
    mov BYTE PTR debugBuffer[1], 10
    invoke WriteConsoleA, hOutput, ADDR debugBuffer, 2, ADDR debugBytesWritten, 0

    pop edi
    pop esi
    pop edx
    pop ecx
    pop ebx
    ret
DebugNumber ENDP

END
