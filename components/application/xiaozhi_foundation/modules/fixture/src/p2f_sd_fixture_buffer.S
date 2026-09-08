/* Validation-only writable buffer backing the legacy P2-F linker symbols. */

    .section .bss.xiaozhi_p2f_fixture,"aw",@nobits
    .balign 4
    .global _binary_xiaozhi_p2f_fixture_start
_binary_xiaozhi_p2f_fixture_start:
    .space 6030
    .global _binary_xiaozhi_p2f_fixture_end
_binary_xiaozhi_p2f_fixture_end:
