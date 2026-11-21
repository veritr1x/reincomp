// Apply the pinned Bunker modules to the Global APK's code before lifting it.
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "camera_edge.h"
#include "gimmick_capacity.h"
#include "remain_days.h"
#include "text_buffer.h"

int bunker_game_japanese = 0;

static int write_word(uint32_t *at, uint32_t value) {
    *at = value;
    return 0;
}

__declspec(dllexport) const char *apply_bunker_library_patches(uint8_t *image) {
    // These two game methods are adapted at their translated entry points.
    // Check the same Global instructions that the upstream modules expect.
    static const uint32_t roulette[] = {0xa9be4ff4, 0xa9017bfd, 0x910043fd, 0xd00237f4};
    static const uint32_t timetable[] = {0xa9427bfd, 0xa9414ff4, 0xfc4307e8, 0xd65f03c0};
    static const uint32_t changing[] = {0xa9ba6ffc, 0xa90167fa, 0xa9025ff8, 0xa90357f6};
    static const uint32_t entering[] = {0xf81d0ff5, 0xa9014ff4, 0xa9027bfd, 0x910083fd};
    if (memcmp(image + 0x3291fb4, roulette, sizeof roulette)) return "Special Gift Roulette";
    if (memcmp(image + 0x2b01f88, timetable, sizeof timetable)) return "Guerrilla timetable";
    if (memcmp(image + 0x3a0ecf8, changing, sizeof changing)) return "story state change";
    if (memcmp(image + 0x363082c, entering, sizeof entering)) return "story collision";
    if (text_buffer_fix(image, write_word)) return "ArtStringBuilder capacity";
    if (remain_days_cap(image, write_word)) return "remaining days";
    if (gimmick_capacity_raise(image, write_word)) return "world map gimmick capacity";
    if (camera_edge_fix(image, write_word)) return "widescreen story camera";
    return NULL;
}
