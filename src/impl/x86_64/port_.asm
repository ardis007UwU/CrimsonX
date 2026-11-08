global port_inb
global port_inw
global port_inl
global port_outb
global port_outw
global port_outl

port_inb:
	mov dx, di ; load `port` argument
	in al, dx ; read from port given by `port`
	movzx rax, al ; zero-extend
	ret

port_inw:
	mov dx, di
	in ax, dx
	movzx rax, ax
	ret

port_inl:
	mov dx, di
	in eax, dx
	ret

port_outb:
	mov dx, di ; load `port` argument
	mov al, sil ; load `data` argument
	out dx, al ; write to port given by `port` with data given by `data`
	ret

port_outw:
	mov dx, di
	mov ax, si
	out dx, ax
	ret

port_outl:
	mov dx, di
	mov eax, esi
	out dx, eax
	ret
