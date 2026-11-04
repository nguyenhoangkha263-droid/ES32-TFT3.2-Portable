/* Tu dong sinh boi tools/make_assets.py - khong sua tay */
#pragma once
#include <stdint.h>

extern const uint8_t img_page0[];
extern const uint8_t img_page1[];
extern const uint8_t img_page2[];
extern const uint8_t img_page3[];
extern const uint8_t img_fan2_off[];
extern const uint8_t img_cd[];

#define UI_KEY_RECTS { \
    {169, 67, 47, 30}, \
    {215, 67, 47, 30}, \
    {262, 67, 48, 30}, \
    {169, 95, 47, 30}, \
    {215, 95, 47, 30}, \
    {262, 95, 48, 30}, \
    {169, 124, 47, 30}, \
    {215, 124, 47, 30}, \
    {262, 124, 48, 30}, \
    {169, 153, 47, 31}, \
    {215, 153, 47, 31}, \
    {262, 153, 48, 31} }
#define UI_LOGIN_RECT  {169, 184, 141, 33}
#define UI_FAN_RECT    {172, 78, 134, 50}
#define UI_UNLOCK_RECT {172, 132, 134, 50}
#define UI_LOGOUT_RECT {190, 187, 97, 33}
#define UI_EXIT_RECT   {96, 170, 128, 48}

#define UI_DOT_CX    238.95f
#define UI_DOT_CY    50.51f
#define UI_DOT_PITCH 12.77f
#define UI_DOT_R     2.70f
#define UI_DOT_BAND  {183, 42, 112, 16}

#define UI_FAN2_RECT {169, 75, 139, 55} /* sprite FAN OFF Page 2 */
#define UI_CD_RECT   {112, 123, 96, 38} /* 11 sprite dem nguoc 0..10 */
