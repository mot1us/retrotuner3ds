#ifndef TEST_THEME_STUB_DRAW_H
#define TEST_THEME_STUB_DRAW_H

#include <stdbool.h>
#include <stdint.h>

typedef struct { int unused; } Draw_image_data;
typedef enum { DRAW_X_ALIGN_CENTER } Draw_text_align_x;
typedef enum { DRAW_Y_ALIGN_CENTER } Draw_text_align_y;

Draw_image_data Draw_get_empty_image(void);
void Draw_set_refresh_needed(bool needed);
void Draw_texture(Draw_image_data *image, uint32_t color, float x, float y,
                  float width, float height);
void Draw_c(const char *text, float x, float y, float size, uint32_t color);
void Draw_align_c(const char *text, float x, float y, float size, uint32_t color,
                  Draw_text_align_x align_x, Draw_text_align_y align_y,
                  float width, float height);

#endif
