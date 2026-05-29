#if defined(__linux__)
    .section .rodata.smartiecoin_sapling_params,"a",@progbits
    .balign 16

    .globl smartiecoin_sapling_spend_params_start
    .type smartiecoin_sapling_spend_params_start, @object
smartiecoin_sapling_spend_params_start:
    .incbin "../params/sapling-spend.params"
    .globl smartiecoin_sapling_spend_params_end
    .type smartiecoin_sapling_spend_params_end, @object
smartiecoin_sapling_spend_params_end:
    .size smartiecoin_sapling_spend_params_start, smartiecoin_sapling_spend_params_end - smartiecoin_sapling_spend_params_start

    .balign 16
    .globl smartiecoin_sapling_output_params_start
    .type smartiecoin_sapling_output_params_start, @object
smartiecoin_sapling_output_params_start:
    .incbin "../params/sapling-output.params"
    .globl smartiecoin_sapling_output_params_end
    .type smartiecoin_sapling_output_params_end, @object
smartiecoin_sapling_output_params_end:
    .size smartiecoin_sapling_output_params_start, smartiecoin_sapling_output_params_end - smartiecoin_sapling_output_params_start

    .section .note.GNU-stack,"",@progbits
#endif
