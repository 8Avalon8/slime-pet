/* web/index.html as the firmware sees it (ESP-IDF EMBED_TXTFILES: the bytes plus a NUL) */
    .data
    .globl _binary_index_html_start
_binary_index_html_start:
    .incbin INDEX_HTML
    .byte 0
    .globl _binary_index_html_end
_binary_index_html_end:
#if defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif
