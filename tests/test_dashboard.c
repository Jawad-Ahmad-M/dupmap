/* Fixed cards never shrink; smaller viewports fall back to rows. */
#define main dupmap_program_main
#include "../src/main.c"
#undef main
#include <assert.h>

int main(void) {
    for (int width = 0; width <= 200; ++width) {
        for (int height = 0; height <= 60; ++height) {
            int columns, rows;
            int cards = card_geometry(width, height, &columns, &rows);
            if (cards) {
                assert(columns >= 1 && rows >= 1);
                assert(columns * (CARD_WIDTH + 2) - 2 <= width);
                assert(rows * (CARD_HEIGHT + 1) - 1 <= height);
            } else assert(width < CARD_WIDTH || height < CARD_HEIGHT);
            size_t page = cards ? (size_t)columns * rows : (size_t)(height > 0 ? height : 1);
            size_t stride = cards ? (size_t)columns : 1;
            for (size_t count = 0; count <= 301; count += 17) {
                ViewState state = {.selected = count ? count - 1 : 0, .first = 1000};
                keep_visible(&state, count, page, stride);
                if (!count) assert(state.selected == 0 && state.first == 0);
                else {
                    assert(state.selected < count);
                    assert(state.first <= state.selected);
                    assert(state.selected - state.first < page);
                    assert(state.first % stride == 0);
                    state.selected = 0;
                    keep_visible(&state, count, page, stride);
                    assert(state.first == 0);
                }
            }
        }
    }
    ViewState state = {.selected = 299};
    keep_visible(&state, 300, 12, 3);
    assert(state.first == 288);
    keep_visible(&state, 300, 1, 1);
    assert(state.first == 299);
    puts("dashboard dimension tests: all passed");
    return 0;
}
