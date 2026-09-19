"""GoldenEye's animation ids, which its AI lists' PlayAnimation names.

An id indexes `animation_table_ptrs1[]` (initanitable.c), whose entries are
offsets into the `animation_data` segment - the same records geanim.py reads.
The segment's ROM address is BASE: `idle` is id 0 at offset 0x1c and the
intro's own list has it at 0x28e99c.

Generated from the GoldenEye decompilation, `src/game/initanitable.c` for the
order and `assets/animationtable_data.h` for each one's offset.

`VEHICLES` is `animation_table_ptrs2[]`, the three an aircraft plays. An id
means one of these when the AI list belongs to a vehicle prop and one of
TABLE's when it belongs to a guard, so the two share their numbering and only
the list's owner tells them apart (aicommands.def, PlayAnimation).
"""

BASE = 0x28e980

TABLE = [
    ('idle', 0x0001c),  # 0
    ('fire_standing', 0x00144),  # 1
    ('fire_standing_fast', 0x00214),  # 2
    ('fire_hip', 0x00318),  # 3
    ('fire_shoulder_left', 0x003c4),  # 4
    ('fire_turn_right1', 0x00610),  # 5
    ('fire_turn_right2', 0x00814),  # 6
    ('fire_kneel_right_leg', 0x00990),  # 7
    ('fire_kneel_left_leg', 0x00b84),  # 8
    ('fire_kneel_left', 0x00db4),  # 9
    ('fire_kneel_right', 0x01028),  # 10
    ('fire_roll_left', 0x01334),  # 11
    ('fire_roll_right1', 0x01578),  # 12
    ('fire_roll_left_fast', 0x017b4),  # 13
    ('hit_left_shoulder', 0x0186c),  # 14
    ('hit_right_shoulder', 0x01984),  # 15
    ('hit_left_arm', 0x01a6c),  # 16
    ('hit_right_arm', 0x01b54),  # 17
    ('hit_left_hand', 0x01c9c),  # 18
    ('hit_right_hand', 0x01e40),  # 19
    ('hit_left_leg', 0x01f84),  # 20
    ('hit_right_leg', 0x02134),  # 21
    ('death_genitalia', 0x0282c),  # 22
    ('hit_neck', 0x0299c),  # 23
    ('death_neck', 0x02e64),  # 24
    ('death_stagger_back_to_wall', 0x02f94),  # 25
    ('death_forward_face_down', 0x030b8),  # 26
    ('death_forward_spin_face_up', 0x031dc),  # 27
    ('death_backward_fall_face_up1', 0x032c8),  # 28
    ('death_backward_spin_face_down_right', 0x033ac),  # 29
    ('death_backward_spin_face_up_right', 0x034d4),  # 30
    ('death_backward_spin_face_down_left', 0x035c8),  # 31
    ('death_backward_spin_face_up_left', 0x036d8),  # 32
    ('death_forward_face_down_hard', 0x0384c),  # 33
    ('death_forward_face_down_soft', 0x039c0),  # 34
    ('death_fetal_position_right', 0x03af0),  # 35
    ('death_fetal_position_left', 0x03c10),  # 36
    ('death_backward_fall_face_up2', 0x03d04),  # 37
    ('side_step_left', 0x03d9c),  # 38
    ('fire_roll_right2', 0x03fa0),  # 39
    ('walking', 0x04018),  # 40
    ('sprinting', 0x04070),  # 41
    ('running', 0x040d4),  # 42
    ('bond_eye_walk', 0x04144),  # 43
    ('bond_eye_fire', 0x04298),  # 44
    ('bond_watch', 0x042c8),  # 45
    ('surrendering_armed', 0x04384),  # 46
    ('surrendering_armed_drop_weapon', 0x04504),  # 47
    ('fire_walking', 0x04574),  # 48
    ('fire_running', 0x045cc),  # 49
    ('null50', 0x00001),  # 50
    ('null51', 0x00001),  # 51
    ('fire_jump_to_side_left', 0x047bc),  # 52
    ('fire_jump_to_side_right', 0x04a40),  # 53
    ('hit_butt_long', 0x04ce0),  # 54
    ('hit_butt_short', 0x04f14),  # 55
    ('death_head', 0x051c4),  # 56
    ('death_left_leg', 0x0540c),  # 57
    ('slide_right', 0x054a0),  # 58
    ('slide_left', 0x05554),  # 59
    ('jump_backwards', 0x05684),  # 60
    ('extending_left_hand', 0x05744),  # 61
    ('fire_throw_grenade', 0x05964),  # 62
    ('spotting_bond', 0x05d10),  # 63
    ('look_around', 0x05ef0),  # 64
    ('fire_standing_one_handed_weapon', 0x060d4),  # 65
    ('fire_standing_draw_one_handed_weapon_fast', 0x06254),  # 66
    ('fire_standing_draw_one_handed_weapon_slow', 0x0637c),  # 67
    ('fire_hip_one_handed_weapon_fast', 0x06484),  # 68
    ('fire_hip_one_handed_weapon_slow', 0x06554),  # 69
    ('fire_hip_forward_one_handed_weapon', 0x06644),  # 70
    ('fire_standing_right_one_handed_weapon', 0x06738),  # 71
    ('fire_step_right_one_handed_weapon', 0x06808),  # 72
    ('fire_standing_left_one_handed_weapon_slow', 0x0694c),  # 73
    ('fire_standing_left_one_handed_weapon_fast', 0x06a18),  # 74
    ('fire_kneel_forward_one_handed_weapon_slow', 0x06c18),  # 75
    ('fire_kneel_forward_one_handed_weapon_fast', 0x06d50),  # 76
    ('fire_kneel_right_one_handed_weapon_slow', 0x06f08),  # 77
    ('fire_kneel_right_one_handed_weapon_fast', 0x0700c),  # 78
    ('fire_kneel_left_one_handed_weapon_slow', 0x071d0),  # 79
    ('fire_kneel_left_one_handed_weapon_fast', 0x07304),  # 80
    ('fire_kneel_left_one_handed_weapon', 0x07430),  # 81
    ('aim_walking_one_handed_weapon', 0x074a4),  # 82
    ('aim_walking_left_one_handed_weapon', 0x07514),  # 83
    ('aim_walking_right_one_handed_weapon', 0x07588),  # 84
    ('aim_running_one_handed_weapon', 0x075ec),  # 85
    ('aim_running_right_one_handed_weapon', 0x07650),  # 86
    ('aim_running_left_one_handed_weapon', 0x076b8),  # 87
    ('aim_sprinting_one_handed_weapon', 0x07714),  # 88
    ('running_one_handed_weapon', 0x0777c),  # 89
    ('sprinting_one_handed_weapon', 0x077d4),  # 90
    ('null91', 0x00001),  # 91
    ('null92', 0x00001),  # 92
    ('null93', 0x00001),  # 93
    ('null94', 0x00001),  # 94
    ('null95', 0x00001),  # 95
    ('null96', 0x00001),  # 96
    ('draw_one_handed_weapon_and_look_around', 0x078c8),  # 97
    ('draw_one_handed_weapon_and_stand_up', 0x07aa8),  # 98
    ('aim_one_handed_weapon_left_right', 0x07c4c),  # 99
    ('cock_one_handed_weapon_and_turn_around', 0x07d04),  # 100
    ('holster_one_handed_weapon_and_cross_arms', 0x07dd8),  # 101
    ('cock_one_handed_weapon_turn_around_and_stand_up', 0x07f0c),  # 102
    ('draw_one_handed_weapon_and_turn_around', 0x07fb4),  # 103
    ('step_forward_and_hold_one_handed_weapon', 0x08080),  # 104
    ('holster_one_handed_weapon_and_adjust_suit', 0x08164),  # 105
    ('idle_unarmed', 0x08194),  # 106
    ('walking_unarmed', 0x08204),  # 107
    ('fire_walking_dual_wield', 0x08274),  # 108
    ('fire_walking_dual_wield_hands_crossed', 0x082e0),  # 109
    ('fire_running_dual_wield', 0x08340),  # 110
    ('fire_running_dual_wield_hands_crossed', 0x083a4),  # 111
    ('fire_sprinting_dual_wield', 0x08404),  # 112
    ('fire_sprinting_dual_wield_hands_crossed', 0x0845c),  # 113
    ('walking_female', 0x084c4),  # 114
    ('running_female', 0x08520),  # 115
    ('fire_kneel_dual_wield', 0x08698),  # 116
    ('fire_kneel_dual_wield_left', 0x08800),  # 117
    ('fire_kneel_dual_wield_right', 0x08978),  # 118
    ('fire_kneel_dual_wield_hands_crossed', 0x08aac),  # 119
    ('fire_kneel_dual_wield_hands_crossed_left', 0x08bf0),  # 120
    ('fire_kneel_dual_wield_hands_crossed_right', 0x08d28),  # 121
    ('fire_standing_dual_wield', 0x08e1c),  # 122
    ('fire_standing_dual_wield_left', 0x08f2c),  # 123
    ('fire_standing_dual_wield_right', 0x09084),  # 124
    ('fire_standing_dual_wield_hands_crossed_left', 0x09194),  # 125
    ('fire_standing_dual_wield_hands_crossed_right', 0x092ec),  # 126
    ('fire_standing_aiming_down_sights', 0x09444),  # 127
    ('fire_kneel_aiming_down_sights', 0x095fc),  # 128
    ('hit_taser', 0x097bc),  # 129
    ('death_explosion_forward', 0x098c8),  # 130
    ('death_explosion_left1', 0x09a2c),  # 131
    ('death_explosion_back_left', 0x09b48),  # 132
    ('death_explosion_back1', 0x09c4c),  # 133
    ('death_explosion_right', 0x09d5c),  # 134
    ('death_explosion_forward_right1', 0x09e44),  # 135
    ('death_explosion_back2', 0x09f48),  # 136
    ('death_explosion_forward_roll', 0x0a094),  # 137
    ('death_explosion_forward_face_down', 0x0a1b8),  # 138
    ('death_explosion_left2', 0x0a2f8),  # 139
    ('death_explosion_forward_right2', 0x0a424),  # 140
    ('death_explosion_forward_right2_alt', 0x0a538),  # 141
    ('death_explosion_forward_right3', 0x0a650),  # 142
    ('null143', 0x00001),  # 143
    ('null144', 0x00001),  # 144
    ('null145', 0x00001),  # 145
    ('null146', 0x00001),  # 146
    ('running_hands_up', 0x0a6b0),  # 147
    ('sprinting_hands_up', 0x0a704),  # 148
    ('aim_and_blow_one_handed_weapon', 0x0a8bc),  # 149
    ('aim_one_handed_weapon_left', 0x0a94c),  # 150
    ('aim_one_handed_weapon_right', 0x0a9dc),  # 151
    ('conversation', 0x0acac),  # 152
    ('drop_weapon_and_show_fight_stance', 0x0b174),  # 153
    ('yawning', 0x0b2ac),  # 154
    ('swatting_flies', 0x0b528),  # 155
    ('scratching_leg', 0x0b6b0),  # 156
    ('scratching_butt', 0x0b7c8),  # 157
    ('adjusting_crotch', 0x0b854),  # 158
    ('sneeze', 0x0b9a8),  # 159
    ('conversation_cleaned', 0x0bc40),  # 160
    ('conversation_listener', 0x0bf80),  # 161
    ('startled_and_looking_around', 0x0c224),  # 162
    ('laughing_in_disbelief', 0x0c410),  # 163
    ('surrendering_unarmed', 0x0c544),  # 164
    ('coughing_standing', 0x0c838),  # 165
    ('coughing_kneel1', 0x0cb78),  # 166
    ('coughing_kneel2', 0x0ce6c),  # 167
    ('standing_up', 0x0d0a8),  # 168
    ('null169', 0x00001),  # 169
    ('dancing', 0x0d348),  # 170
    ('dancing_one_handed_weapon', 0x0d54c),  # 171
    ('keyboard_right_hand1', 0x0d5e4),  # 172
    ('keyboard_right_hand2', 0x0d668),  # 173
    ('keyboard_left_hand', 0x0d6f8),  # 174
    ('keyboard_right_hand_tapping', 0x0d728),  # 175
    ('bond_eye_fire_alt', 0x0d89c),  # 176
    ('dam_jump', 0x0dbe4),  # 177
    ('surface_vent_jump', 0x0dd20),  # 178
    ('cradle_jump', 0x0e05c),  # 179
    ('cradle_fall', 0x0e08c),  # 180
    ('credits_bond_kissing', 0x0e0bc),  # 181
    ('credits_natalya_kissing', 0x0e18c),  # 182
]

VEHICLES = [
    ('helicopter_cradle', 0x0e470),
    ('plane_runway', 0x0e5f4),
    ('helicopter_takeoff', 0x0e7c0),
]
