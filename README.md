CrimsonX is an educational, custom-built 64-bit operating system kernel written from scratch in C and Assembly. It boots directly into long mode via GRUB and launches cxsh—a feature-rich terminal environment complete with raw PS/2 interrupt handling, multi-line string wrapping, text selection highlighting, clipboard operations, and an absolute-coordinate VGA text engine.

Kernel & Hardware Drivers

x86_64 Long Mode: Fully operational 64-bit kernel execution environment.

PS/2 Keyboard Interrupts (IRQ 1): Real-time hardware interrupt handling with Shift, Ctrl, Caps Lock, and complete US QWERTY symbol/punctuation mapping.

8259 PIC Management: Configured interrupt remapping with active End-of-Interrupt (EOI) handling to ensure recurring, stable hardware events.

Absolute VGA Text Driver (0xB8000): Custom text-mode rendering supporting multi-line horizontal wrapping, dynamic scrolling, and absolute coordinate plotting.
