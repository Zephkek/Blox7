.code
read_fs PROC
    mov eax, ecx
    mov rax, qword ptr fs:[rax]
    ret
read_fs ENDP

read_fs_selector PROC
    xor eax, eax
    mov ax, fs
    ret
read_fs_selector ENDP
END
