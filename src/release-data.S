.section .rodata
.global launch_config_data
.hidden launch_config_data
launch_config_data:
    .incbin "config.ini"
    .byte 0

.global launch_emoji_data
.hidden launch_emoji_data
launch_emoji_data:
    .incbin "assets/emoji.tsv"
    .byte 0

.section .note.GNU-stack,""
