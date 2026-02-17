#include "render.h"
#include "theme.h"
#include "gfx_theme.h"
#include "font.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <dirent.h>

#include "lodepng.h"

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

// v56: Universal buffer for all image operations - 6MB for image viewer
// Used for: WebP/PNG/JPEG/GIF/BMP decode, thumbnail conversion, raw thumbnail loading, image viewer
// NOT thread-safe - operations are sequential, buffer is reused
// 6MB can hold 1732x1732 RGB565 image (3MB pixels) with room for decode temp buffers
#define UNIVERSAL_BUFFER_BYTES (6 * 1024 * 1024)  // 6MB
static uint8_t universal_buffer[UNIVERSAL_BUFFER_BYTES];

// Helper macros for buffer access
#define universal_buffer_u16 ((uint16_t*)universal_buffer)
#define UNIVERSAL_MAX_PIXELS_RGB565 (UNIVERSAL_BUFFER_BYTES / sizeof(uint16_t))  // 3,145,728 pixels

// Track if we're in platform menu or game list
static bool in_platform_menu = true;

void render_set_in_platform_menu(bool is_platform_menu) {
    in_platform_menu = is_platform_menu;
}

bool render_is_in_platform_menu(void) {
    return in_platform_menu;
}

// Draw text with drop shadow (for GFX themes) - OPTIMIZED: only 2 draws instead of 9
void font_draw_text_outlined(uint16_t *framebuffer, int fb_width, int fb_height,
                             int x, int y, const char *text, uint16_t color) {
    // Simple drop shadow - much faster than full outline (2 draws vs 9)
    uint16_t shadow_color = 0x0000; // Black shadow
    font_draw_text(framebuffer, fb_width, fb_height, x+2, y+2, text, shadow_color);
    // Draw main text on top
    font_draw_text(framebuffer, fb_width, fb_height, x, y, text, color);
}

void render_init(uint16_t *framebuffer) {
    if (framebuffer) {
        render_clear_screen(framebuffer);
    }
}

void render_clear_screen(uint16_t *framebuffer) {
    if (!framebuffer) return;
    
    // Fill with background color
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        framebuffer[i] = COLOR_BG;
    }
}

void render_fill_rect(uint16_t *framebuffer, int x, int y, int width, int height, uint16_t color) {
    if (!framebuffer) return;
    
    for (int py = y; py < y + height && py < SCREEN_HEIGHT; py++) {
        for (int px = x; px < x + width && px < SCREEN_WIDTH; px++) {
            if (px >= 0 && py >= 0) {
                framebuffer[py * SCREEN_WIDTH + px] = color;
            }
        }
    }
}

// v24: Alias for render_fill_rect
void render_filled_rect(uint16_t *framebuffer, int x, int y, int width, int height, uint16_t color) {
    render_fill_rect(framebuffer, x, y, width, height, color);
}

// v24: Draw rectangle outline (border only)
void render_rect(uint16_t *framebuffer, int x, int y, int width, int height, uint16_t color) {
    if (!framebuffer) return;

    // Top edge
    for (int px = x; px < x + width && px < SCREEN_WIDTH; px++) {
        if (px >= 0 && y >= 0 && y < SCREEN_HEIGHT) {
            framebuffer[y * SCREEN_WIDTH + px] = color;
        }
    }
    // Bottom edge
    int bottom_y = y + height - 1;
    for (int px = x; px < x + width && px < SCREEN_WIDTH; px++) {
        if (px >= 0 && bottom_y >= 0 && bottom_y < SCREEN_HEIGHT) {
            framebuffer[bottom_y * SCREEN_WIDTH + px] = color;
        }
    }
    // Left edge
    for (int py = y; py < y + height && py < SCREEN_HEIGHT; py++) {
        if (x >= 0 && x < SCREEN_WIDTH && py >= 0) {
            framebuffer[py * SCREEN_WIDTH + x] = color;
        }
    }
    // Right edge
    int right_x = x + width - 1;
    for (int py = y; py < y + height && py < SCREEN_HEIGHT; py++) {
        if (right_x >= 0 && right_x < SCREEN_WIDTH && py >= 0) {
            framebuffer[py * SCREEN_WIDTH + right_x] = color;
        }
    }
}

void render_rounded_rect(uint16_t *framebuffer, int x, int y, int width, int height, int radius, uint16_t color) {
    if (!framebuffer) return;
    
    // Draw main body (excluding corners)
    render_fill_rect(framebuffer, x + radius, y, width - 2 * radius, height, color);
    render_fill_rect(framebuffer, x, y + radius, width, height - 2 * radius, color);
    
    // Draw rounded corners using circle approximation
    for (int corner_y = 0; corner_y < radius; corner_y++) {
        for (int corner_x = 0; corner_x < radius; corner_x++) {
            int dx = radius - corner_x;
            int dy = radius - corner_y;
            int dist_sq = dx * dx + dy * dy;
            int radius_sq = radius * radius;
            
            if (dist_sq <= radius_sq) {
                // Top-left corner
                int px = x + corner_x;
                int py = y + corner_y;
                if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) {
                    framebuffer[py * SCREEN_WIDTH + px] = color;
                }
                
                // Top-right corner
                px = x + width - 1 - corner_x;
                py = y + corner_y;
                if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) {
                    framebuffer[py * SCREEN_WIDTH + px] = color;
                }
                
                // Bottom-left corner
                px = x + corner_x;
                py = y + height - 1 - corner_y;
                if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) {
                    framebuffer[py * SCREEN_WIDTH + px] = color;
                }
                
                // Bottom-right corner
                px = x + width - 1 - corner_x;
                py = y + height - 1 - corner_y;
                if (px >= 0 && px < SCREEN_WIDTH && py >= 0 && py < SCREEN_HEIGHT) {
                    framebuffer[py * SCREEN_WIDTH + px] = color;
                }
            }
        }
    }
}

void render_text_pillbox(uint16_t *framebuffer, int x, int y, const char *text,
                        uint16_t bg_color, uint16_t text_color, int padding) {
    if (!framebuffer || !text) return;

    // Calculate text dimensions using proper measurement
    int text_width = font_measure_text(text);
    int text_height = FONT_CHAR_HEIGHT;

    // Calculate pillbox dimensions - left padding stays at 6, right padding uses parameter
    int left_padding = 6;
    int pillbox_width = text_width + left_padding + padding; // padding only on right
    int pillbox_height = text_height + padding;
    int pillbox_x = x - left_padding;
    int pillbox_y = y - (padding / 2);
    
    // Draw pillbox background
    render_rounded_rect(framebuffer, pillbox_x, pillbox_y, pillbox_width, pillbox_height, 8, bg_color);
    
    // Draw text
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, x, y, text, text_color);
}

void render_header(uint16_t *framebuffer, const char *title) {
    if (!framebuffer || !title) return;

    // v22: Check if we should use text background for header
    bool use_text_bg = false;
    if (gfx_theme_is_active()) {
        // Use platform_text_background setting for header (same as main menu items)
        if (in_platform_menu) {
            use_text_bg = gfx_theme_platform_text_background();
        } else {
            use_text_bg = gfx_theme_game_text_background();
        }
    }

    // Draw folder/section name in header area
    if (use_text_bg) {
        render_text_pillbox(framebuffer, PADDING, 10, title, 0x0000, COLOR_HEADER, 7);
    } else if (gfx_theme_is_active()) {
        font_draw_text_outlined(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, 10, title, COLOR_HEADER);
    } else {
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, PADDING, 10, title, COLOR_HEADER);
    }
}

void render_legend(uint16_t *framebuffer, int x_button_mode) {
    if (!framebuffer) return;

    int legend_y = SCREEN_HEIGHT - 24;
    int spacing = 8; // Space between legend items

    // Draw "SEL - SETTINGS" legend in bottom right with highlight
    const char *settings_legend = " SEL - SETTINGS ";
    int settings_width = font_measure_text(settings_legend);
    int settings_x = SCREEN_WIDTH - settings_width - 12;
    render_rounded_rect(framebuffer, settings_x - 4, legend_y - 2, settings_width + 8, 20, 10, COLOR_LEGEND_BG);
    font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, settings_x, legend_y, settings_legend, COLOR_LEGEND);

    // Draw X button legend to the left of settings
    if (x_button_mode != LEGEND_X_NONE) {
        const char *x_legend = (x_button_mode == LEGEND_X_REMOVE) ? " X - REMOVE " : " X - FAVOURITE ";
        int x_width = font_measure_text(x_legend);
        int x_x = settings_x - x_width - spacing - 12;
        render_rounded_rect(framebuffer, x_x - 4, legend_y - 2, x_width + 8, 20, 10, COLOR_LEGEND_BG);
        font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, x_x, legend_y, x_legend, COLOR_LEGEND);
    }
}

void render_menu_item(uint16_t *framebuffer, int index, const char *name, int is_dir,
                     int is_selected, int scroll_offset, int is_favorited) {
    if (!framebuffer || !name) return;

    // Get layout from GFX theme if active, otherwise use defaults
    int list_x = PADDING;
    int list_y = START_Y;
    int item_height = ITEM_HEIGHT;
    int visible_items = VISIBLE_ENTRIES;
    bool use_outline = false;

    if (gfx_theme_is_active()) {
        const GfxThemeLayout* layout = gfx_theme_get_layout();
        if (layout) {
            // Use different layout based on whether we're in platform menu or game list
            if (in_platform_menu) {
                list_x = layout->platform_list_x;
                list_y = layout->platform_list_y_start;
                item_height = layout->platform_item_height;
                visible_items = layout->platform_visible_items;
            } else {
                list_x = layout->game_list_x;
                list_y = layout->game_list_y_start;
                item_height = layout->game_item_height;
                visible_items = layout->game_visible_items;
            }
        }
        use_outline = true;  // Use text outline when GFX theme is active
    }

    int visible_index = index - scroll_offset;
    if (visible_index < 0 || visible_index >= visible_items) return;

    int y = list_y + (visible_index * item_height);

    // Draw favorite star if favorited
    int text_x = list_x;
    if (is_favorited) {
        const char *star = "*"; // Asterisk as favorite marker
        if (use_outline) {
            font_draw_text_outlined(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, list_x, y, star, COLOR_HEADER);
        } else {
            font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, list_x, y, star, COLOR_HEADER);
        }
        text_x = list_x + 15; // Offset text to the right of the star
    }

    if (is_selected) {
        // Use unified pillbox rendering (pillbox already handles text)
        render_text_pillbox(framebuffer, text_x, y, name, COLOR_SELECT_BG, COLOR_SELECT_TEXT, 7);
    } else {
        // Draw normal text
        uint16_t text_color = is_dir ? COLOR_FOLDER : COLOR_TEXT;

        // v20: Check if we should use rounded black background instead of outline
        bool use_text_bg = false;
        if (gfx_theme_is_active()) {
            if (in_platform_menu) {
                use_text_bg = gfx_theme_platform_text_background();
            } else {
                use_text_bg = gfx_theme_game_text_background();
            }
        }

        if (use_text_bg) {
            // v20: Draw rounded black background behind text (inverted colors from selected)
            // Black background with normal text color on top
            render_text_pillbox(framebuffer, text_x, y, name, 0x0000, text_color, 7);
        } else if (use_outline) {
            font_draw_text_outlined(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, text_x, y, name, text_color);
        } else {
            font_draw_text(framebuffer, SCREEN_WIDTH, SCREEN_HEIGHT, text_x, y, name, text_color);
        }
    }
}

// Thumbnail implementation

void get_thumbnail_path(const char *game_path, char *thumb_path, size_t thumb_path_size) {
    if (!game_path || !thumb_path || game_path[0] == '\0') {
        thumb_path[0] = '\0';
        return;
    }
    
    // Find the last slash to get directory
    const char *last_slash = strrchr(game_path, '/');
    if (!last_slash) {
        thumb_path[0] = '\0';
        return;
    }
    
    // Copy directory path
    size_t dir_len = last_slash - game_path;
    if (dir_len + 1 >= thumb_path_size) {
        thumb_path[0] = '\0';
        return;
    }
    
    strncpy(thumb_path, game_path, dir_len);
    thumb_path[dir_len] = '\0';
    
    // Add /.res/ subdirectory
    strncat(thumb_path, "/.res/", thumb_path_size - strlen(thumb_path) - 1);
    
    // Get filename without extension
    const char *filename = last_slash + 1;
    const char *last_dot = strrchr(filename, '.');
    
    if (last_dot) {
        size_t name_len = last_dot - filename;
        strncat(thumb_path, filename, min(name_len, thumb_path_size - strlen(thumb_path) - 1));
    } else {
        strncat(thumb_path, filename, thumb_path_size - strlen(thumb_path) - 1);
    }
    
    // Use raw RGB565 format - no parsing, fixed size, minimal memory
    strncat(thumb_path, ".rgb565", thumb_path_size - strlen(thumb_path) - 1);
}

static uint16_t rgb24_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

// Debug logging
extern void xlog(const char *fmt, ...);

int load_thumbnail(const char *rgb565_path, Thumbnail *thumb) {
    if (!rgb565_path || !thumb) return 0;

    // Initialize thumbnail
    thumb->data = NULL;
    thumb->width = 0;
    thumb->height = 0;

    // v42: Try multiple formats
    // 1. rgb565 from .res folder (original format, fastest)
    // 2. webp/png/jpg from ROM folder (same folder as game file)

    xlog("THUMB: input=%s\n", rgb565_path);

    // 1. Try raw RGB565 from .res folder
    if (load_raw_rgb565(rgb565_path, thumb)) {
        xlog("THUMB: rgb565 OK\n");
        return 1;
    }

    // v72: Use universal buffer for converted thumbnails
    uint16_t *loaded_data = NULL;
    int w = 0, h = 0;
    char try_path[520];

    // Build .res folder base path (remove .rgb565 extension)
    char res_base[512];
    strncpy(res_base, rgb565_path, sizeof(res_base) - 1);
    res_base[sizeof(res_base) - 1] = '\0';
    size_t res_len = strlen(res_base);
    if (res_len > 7 && strcmp(res_base + res_len - 7, ".rgb565") == 0) {
        res_base[res_len - 7] = '\0';
    }

    // v72: 2. Try other formats in .res folder (PNG, JPG, WebP, BMP, GIF)
    snprintf(try_path, sizeof(try_path), "%s.png", res_base);
    if (load_png_rgb565(try_path, &loaded_data, &w, &h)) {
        xlog("THUMB: .res png OK %dx%d\n", w, h);
        goto convert_success;
    }

    snprintf(try_path, sizeof(try_path), "%s.jpg", res_base);
    if (load_jpeg_rgb565(try_path, &loaded_data, &w, &h)) {
        xlog("THUMB: .res jpg OK %dx%d\n", w, h);
        goto convert_success;
    }

    snprintf(try_path, sizeof(try_path), "%s.webp", res_base);
    if (load_webp_rgb565(try_path, &loaded_data, &w, &h)) {
        xlog("THUMB: .res webp OK %dx%d\n", w, h);
        goto convert_success;
    }

    snprintf(try_path, sizeof(try_path), "%s.bmp", res_base);
    if (load_bmp_rgb565(try_path, &loaded_data, &w, &h)) {
        xlog("THUMB: .res bmp OK %dx%d\n", w, h);
        goto convert_success;
    }

    snprintf(try_path, sizeof(try_path), "%s.gif", res_base);
    if (load_gif_rgb565(try_path, &loaded_data, &w, &h)) {
        xlog("THUMB: .res gif OK %dx%d\n", w, h);
        goto convert_success;
    }

    // 3. Build ROM folder path by removing "/.res/" from path
    // rgb565_path: /roms/nes/.res/mario.rgb565
    // we want:     /roms/nes/mario
    char rom_path[512];
    strncpy(rom_path, rgb565_path, sizeof(rom_path) - 1);
    rom_path[sizeof(rom_path) - 1] = '\0';

    // Find and remove /.res/ from path
    char *res_ptr = strstr(rom_path, "/.res/");
    if (!res_ptr) {
        res_ptr = strstr(rom_path, "\\.res\\");  // Windows style
    }
    if (res_ptr) {
        // Move everything after /.res/ to replace it
        memmove(res_ptr + 1, res_ptr + 6, strlen(res_ptr + 6) + 1);
    }

    // Remove .rgb565 extension
    size_t len = strlen(rom_path);
    if (len > 7 && strcmp(rom_path + len - 7, ".rgb565") == 0) {
        rom_path[len - 7] = '\0';
    }

    // 4. Try formats in ROM folder (WebP, PNG, JPG, BMP, GIF)
    snprintf(try_path, sizeof(try_path), "%s.webp", rom_path);
    if (load_webp_rgb565(try_path, &loaded_data, &w, &h)) {
        xlog("THUMB: rom webp OK %dx%d\n", w, h);
        goto convert_success;
    }

    snprintf(try_path, sizeof(try_path), "%s.png", rom_path);
    if (load_png_rgb565(try_path, &loaded_data, &w, &h)) {
        xlog("THUMB: rom png OK %dx%d\n", w, h);
        goto convert_success;
    }

    snprintf(try_path, sizeof(try_path), "%s.jpg", rom_path);
    if (load_jpeg_rgb565(try_path, &loaded_data, &w, &h)) {
        xlog("THUMB: rom jpg OK %dx%d\n", w, h);
        goto convert_success;
    }

    snprintf(try_path, sizeof(try_path), "%s.bmp", rom_path);
    if (load_bmp_rgb565(try_path, &loaded_data, &w, &h)) {
        xlog("THUMB: rom bmp OK %dx%d\n", w, h);
        goto convert_success;
    }

    snprintf(try_path, sizeof(try_path), "%s.gif", rom_path);
    if (load_gif_rgb565(try_path, &loaded_data, &w, &h)) {
        xlog("THUMB: rom gif OK %dx%d\n", w, h);
        goto convert_success;
    }

    // Nothing found
    xlog("THUMB: nothing found\n");
    return 0;

convert_success:
    // v42: Copy to universal buffer if it fits
    if ((size_t)(w * h) <= UNIVERSAL_MAX_PIXELS_RGB565) {
        memcpy(universal_buffer_u16, loaded_data, w * h * sizeof(uint16_t));
        free(loaded_data);
        thumb->data = universal_buffer_u16;
        thumb->width = w;
        thumb->height = h;
        return 1;
    }

    // Too large for universal buffer
    xlog("THUMB: too large %dx%d > %d\n", w, h, (int)UNIVERSAL_MAX_PIXELS_RGB565);
    free(loaded_data);
    return 0;
}

// v42: load_raw_rgb565 uses universal_buffer

int load_raw_rgb565(const char *path, Thumbnail *thumb) {
    // Check if file exists
    if (access(path, F_OK) != 0) {
        return 0;
    }
    
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    
    // Try common dimensions - including larger sizes (v42: added 320x240, 320x256, 400x300)
    int dimensions[][2] = {{64,64}, {128,128}, {160,160}, {200,200}, {250,200}, {200,250}, {320,240}, {320,256}, {400,300}};
    int num_dims = sizeof(dimensions) / sizeof(dimensions[0]);
    
    for (int i = 0; i < num_dims; i++) {
        int w = dimensions[i][0];
        int h = dimensions[i][1];
        if (w * h * 2 == file_size) {

            // v42: Check if it fits in universal buffer
            if ((size_t)(w * h) > UNIVERSAL_MAX_PIXELS_RGB565) {
                fclose(fp);
                return 0;
            }

            thumb->width = w;
            thumb->height = h;
            thumb->data = universal_buffer_u16; // v42: Use universal buffer

            size_t read_bytes = fread(thumb->data, 1, file_size, fp);
            fclose(fp);
            
            if (read_bytes == file_size) {
                return 1;
            } else {
                return 0;
            }
        }
    }
    
    fclose(fp);
    return 0;
}

void free_thumbnail(Thumbnail *thumb) {
    if (thumb) {
        // No need to free static buffer, just reset pointer
        thumb->data = NULL;
        thumb->width = 0;
        thumb->height = 0;
    }
}

void render_thumbnail(uint16_t *framebuffer, const Thumbnail *thumb) {
    if (!framebuffer || !thumb || !thumb->data) {
        return;
    }
    
    // Calculate scaled dimensions to fit in thumbnail area
    int display_width = thumb->width;
    int display_height = thumb->height;
    
    // Scale down if too large
    if (display_width > THUMBNAIL_MAX_WIDTH) {
        display_height = (display_height * THUMBNAIL_MAX_WIDTH) / display_width;
        display_width = THUMBNAIL_MAX_WIDTH;
    }
    
    if (display_height > THUMBNAIL_MAX_HEIGHT) {
        display_width = (display_width * THUMBNAIL_MAX_HEIGHT) / display_height;
        display_height = THUMBNAIL_MAX_HEIGHT;
    }
    
    // Center in thumbnail area (vertically) and align to right edge
    int start_x = SCREEN_WIDTH - display_width;  // Align to right edge of screen
    
    // Center thumbnail vertically on screen
    int start_y = (SCREEN_HEIGHT - display_height) / 2;
    
    // Draw background frame with dark gray border and light gray fill
    #define FRAME_COLOR 0x39E7      // Dark gray border (RGB565: 7,15,7)
    #define BG_COLOR    0x2104      // Very dark gray background (RGB565: 4,8,4)
    
    int frame_x = start_x - 2;
    int frame_y = start_y - 2; 
    int frame_w = display_width + 4;
    int frame_h = display_height + 4;
    
    // Draw border frame
    render_fill_rect(framebuffer, frame_x, frame_y, frame_w, frame_h, FRAME_COLOR);
    // Draw inner background
    render_fill_rect(framebuffer, start_x, start_y, display_width, display_height, BG_COLOR);
    
    // v61: Draw scaled thumbnail with bilinear filtering
    for (int y = 0; y < display_height; y++) {
        for (int x = 0; x < display_width; x++) {
            int screen_x = start_x + x;
            int screen_y = start_y + y;

            if (screen_x >= 0 && screen_x < SCREEN_WIDTH &&
                screen_y >= 0 && screen_y < SCREEN_HEIGHT) {

                // Fixed-point source coordinates (8 fractional bits)
                int src_x_fp = (x * thumb->width * 256) / display_width;
                int src_y_fp = (y * thumb->height * 256) / display_height;

                int src_x0 = src_x_fp >> 8;
                int src_y0 = src_y_fp >> 8;
                int frac_x = src_x_fp & 0xFF;
                int frac_y = src_y_fp & 0xFF;

                int src_x1 = (src_x0 + 1 < thumb->width) ? src_x0 + 1 : src_x0;
                int src_y1 = (src_y0 + 1 < thumb->height) ? src_y0 + 1 : src_y0;

                // Get 4 surrounding pixels
                uint16_t p00 = thumb->data[src_y0 * thumb->width + src_x0];
                uint16_t p10 = thumb->data[src_y0 * thumb->width + src_x1];
                uint16_t p01 = thumb->data[src_y1 * thumb->width + src_x0];
                uint16_t p11 = thumb->data[src_y1 * thumb->width + src_x1];

                // Extract RGB components
                int r00 = (p00 >> 11) & 0x1F, g00 = (p00 >> 5) & 0x3F, b00 = p00 & 0x1F;
                int r10 = (p10 >> 11) & 0x1F, g10 = (p10 >> 5) & 0x3F, b10 = p10 & 0x1F;
                int r01 = (p01 >> 11) & 0x1F, g01 = (p01 >> 5) & 0x3F, b01 = p01 & 0x1F;
                int r11 = (p11 >> 11) & 0x1F, g11 = (p11 >> 5) & 0x3F, b11 = p11 & 0x1F;

                // Bilinear interpolation
                int inv_frac_x = 256 - frac_x;
                int inv_frac_y = 256 - frac_y;

                int r = (r00 * inv_frac_x * inv_frac_y + r10 * frac_x * inv_frac_y +
                         r01 * inv_frac_x * frac_y + r11 * frac_x * frac_y) >> 16;
                int g = (g00 * inv_frac_x * inv_frac_y + g10 * frac_x * inv_frac_y +
                         g01 * inv_frac_x * frac_y + g11 * frac_x * frac_y) >> 16;
                int b = (b00 * inv_frac_x * inv_frac_y + b10 * frac_x * inv_frac_y +
                         b01 * inv_frac_x * frac_y + b11 * frac_x * frac_y) >> 16;

                uint16_t pixel = (r << 11) | (g << 5) | b;

                // Only draw non-black pixels, let dark gray background show through
                if (pixel != 0x0000) {
                    framebuffer[screen_y * SCREEN_WIDTH + screen_x] = pixel;
                }
            }
        }
    }
}

// ===== GFX THEME SUPPORT =====

// Load PNG file to RGB565 format (using lodepng like santa_game)
int load_png_rgb565(const char* filename, uint16_t** data, int* width, int* height) {
    unsigned char* rgba_data = NULL;
    unsigned int w, h;

    // Load PNG with lodepng (RGBA 8-bit)
    unsigned error = lodepng_decode32_file(&rgba_data, &w, &h, filename);

    if (error) {
        // PNG load failed
        return 0;
    }

    // Allocate RGB565 buffer
    *data = (uint16_t*)malloc(w * h * sizeof(uint16_t));
    if (!*data) {
        free(rgba_data);
        return 0;
    }

    // Convert RGBA -> RGB565 (like santa_game)
    for (unsigned int y = 0; y < h; y++) {
        for (unsigned int x = 0; x < w; x++) {
            unsigned int rgba_idx = (y * w + x) * 4;
            unsigned int idx = y * w + x;

            unsigned char r = rgba_data[rgba_idx + 0];
            unsigned char g = rgba_data[rgba_idx + 1];
            unsigned char b = rgba_data[rgba_idx + 2];
            // Alpha ignored for background

            (*data)[idx] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        }
    }

    free(rgba_data);

    *width = (int)w;
    *height = (int)h;
    return 1;
}

// v19: Load PNG file to RGB565 format WITH alpha channel (for transparent overlays)
// COPIED FROM santa_game120 load_city_skyline pattern
int load_png_rgba565(const char* filename, uint16_t** pixels, uint8_t** alpha, int* width, int* height) {
    unsigned char* rgba_data = NULL;
    unsigned int w, h;

    // Load PNG with lodepng (RGBA 8-bit)
    unsigned error = lodepng_decode32_file(&rgba_data, &w, &h, filename);

    if (error) {
        // PNG load failed
        return 0;
    }

    // Allocate RGB565 buffer and alpha buffer
    *pixels = (uint16_t*)malloc(w * h * sizeof(uint16_t));
    *alpha = (uint8_t*)malloc(w * h);

    if (!*pixels || !*alpha) {
        if (*pixels) { free(*pixels); *pixels = NULL; }
        if (*alpha) { free(*alpha); *alpha = NULL; }
        free(rgba_data);
        return 0;
    }

    // Convert RGBA -> RGB565 + separate alpha (like santa_game120)
    for (unsigned int y = 0; y < h; y++) {
        for (unsigned int x = 0; x < w; x++) {
            unsigned int rgba_idx = (y * w + x) * 4;
            unsigned int idx = y * w + x;

            unsigned char r = rgba_data[rgba_idx + 0];
            unsigned char g = rgba_data[rgba_idx + 1];
            unsigned char b = rgba_data[rgba_idx + 2];
            unsigned char a = rgba_data[rgba_idx + 3];

            (*pixels)[idx] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            (*alpha)[idx] = a;
        }
    }

    free(rgba_data);

    *width = (int)w;
    *height = (int)h;
    return 1;
}

// v40: JPEG loading using stb_image (supports progressive JPEG)
#include "stb_image.h"
#include "gifdec.h"
// v42: WebP loading using simplewebp (lossy + lossless, integer-only)
#include "simplewebp.h"

// v40: Load JPEG file to RGB565 format using stb_image
int load_jpeg_rgb565(const char* filename, uint16_t** data, int* width, int* height) {
    // Load file into memory first (stb_image needs memory buffer when STBI_NO_STDIO)
    FILE* fp = fopen(filename, "rb");
    if (!fp) return 0;

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t* file_data = (uint8_t*)malloc(file_size);
    if (!file_data) {
        fclose(fp);
        return 0;
    }

    if (fread(file_data, 1, file_size, fp) != (size_t)file_size) {
        free(file_data);
        fclose(fp);
        return 0;
    }
    fclose(fp);

    // Decode JPEG using stb_image
    int w, h, channels;
    uint8_t* rgb_data = stbi_load_from_memory(file_data, file_size, &w, &h, &channels, 3);
    free(file_data);

    if (!rgb_data) {
        return 0;
    }

    // Allocate RGB565 output buffer
    *width = w;
    *height = h;
    *data = (uint16_t*)malloc(w * h * sizeof(uint16_t));
    if (!*data) {
        stbi_image_free(rgb_data);
        return 0;
    }

    // Convert RGB888 to RGB565
    for (int i = 0; i < w * h; i++) {
        uint8_t r = rgb_data[i * 3 + 0];
        uint8_t g = rgb_data[i * 3 + 1];
        uint8_t b = rgb_data[i * 3 + 2];
        (*data)[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }

    stbi_image_free(rgb_data);
    return 1;
}

// v42: Load WebP file to RGB565 format using simplewebp (supports lossy + lossless)
// Uses universal_buffer for output, malloc's temp RGBA buffer for decode

int load_webp_rgb565(const char* filename, uint16_t** data, int* width, int* height) {
    // Load file into memory
    FILE* fp = fopen(filename, "rb");
    if (!fp) {
        xlog("WEBP: fopen failed: %s\n", filename);
        return 0;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    xlog("WEBP: file size=%ld\n", file_size);

    // Limit file size to avoid huge allocations
    if (file_size > 500000) {  // 500KB max file size
        xlog("WEBP: file too large\n");
        fclose(fp);
        return 0;
    }

    uint8_t* file_data = (uint8_t*)malloc(file_size);
    if (!file_data) {
        xlog("WEBP: malloc failed\n");
        fclose(fp);
        return 0;
    }

    if (fread(file_data, 1, file_size, fp) != (size_t)file_size) {
        xlog("WEBP: fread failed\n");
        free(file_data);
        fclose(fp);
        return 0;
    }
    fclose(fp);

    // Load WebP using simplewebp
    simplewebp* webp = NULL;
    simplewebp_error err = simplewebp_load_from_memory(file_data, file_size, NULL, &webp);
    /* v59: DO NOT free file_data here - simplewebp holds a pointer to it! */

    if (err != SIMPLEWEBP_NO_ERROR || !webp) {
        xlog("WEBP: load_from_memory failed err=%d\n", (int)err);
        free(file_data);  /* v59: Free only on early exit */
        return 0;
    }

    // Get dimensions BEFORE attempting decode
    size_t w, h;
    simplewebp_get_dimensions(webp, &w, &h);
    xlog("WEBP: dimensions %dx%d\n", (int)w, (int)h);

    // v42: Check if fits in universal buffer (256,000 pixels for RGB565)
    if (w == 0 || h == 0 || w * h > UNIVERSAL_MAX_PIXELS_RGB565) {
        xlog("WEBP: too large for buffer (%d > %d)\n", (int)(w * h), (int)UNIVERSAL_MAX_PIXELS_RGB565);
        simplewebp_unload(webp);
        free(file_data);  /* v59: Free only on early exit */
        return 0;
    }

    // Malloc temp RGBA buffer for decode (freed after conversion)
    size_t pixel_count = w * h;
    uint8_t* rgba_temp = (uint8_t*)malloc(pixel_count * 4);
    if (!rgba_temp) {
        xlog("WEBP: rgba malloc failed\n");
        simplewebp_unload(webp);
        free(file_data);  /* v59: Free only on early exit */
        return 0;
    }

    // Decode to RGBA
    xlog("WEBP: decoding...\n");
    err = simplewebp_decode(webp, rgba_temp, NULL);
    simplewebp_unload(webp);
    free(file_data);  /* v59: NOW safe to free - after decode and unload */

    if (err != SIMPLEWEBP_NO_ERROR) {
        xlog("WEBP: decode failed err=%d\n", (int)err);
        free(rgba_temp);
        return 0;
    }
    xlog("WEBP: decode OK\n");

    // Convert RGBA8888 to RGB565 using universal buffer
    xlog("WEBP: converting to RGB565...\n");
    for (size_t i = 0; i < pixel_count; i++) {
        uint8_t r = rgba_temp[i * 4 + 0];
        uint8_t g = rgba_temp[i * 4 + 1];
        uint8_t b = rgba_temp[i * 4 + 2];
        universal_buffer_u16[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }
    free(rgba_temp);

    // Malloc output for caller
    *width = (int)w;
    *height = (int)h;
    xlog("WEBP: allocating output %d bytes\n", (int)(pixel_count * sizeof(uint16_t)));
    *data = (uint16_t*)malloc(pixel_count * sizeof(uint16_t));
    if (!*data) {
        xlog("WEBP: output malloc failed\n");
        return 0;
    }

    // Copy from universal buffer to malloc'd buffer
    memcpy(*data, universal_buffer_u16, pixel_count * sizeof(uint16_t));

    xlog("WEBP: done\n");
    return 1;
}

// v40: Helper to count trailing zeros (for mask shift calculation)
static int count_shift(uint32_t mask) {
    if (mask == 0) return 0;
    int shift = 0;
    while ((mask & 1) == 0) { mask >>= 1; shift++; }
    return shift;
}

// v40: Helper to count bits in mask
static int count_bits(uint32_t mask) {
    int bits = 0;
    while (mask) { bits += (mask & 1); mask >>= 1; }
    return bits;
}

// v40: Load BMP file to RGB565 format (supports 1/4/8/16/24/32 bit)
int load_bmp_rgb565(const char* filename, uint16_t** data, int* width, int* height) {
    FILE* fp = fopen(filename, "rb");
    if (!fp) return 0;

    // Read BMP file header + DIB header (need up to 70 bytes for BITMAPV2INFOHEADER with masks)
    uint8_t header[70];
    if (fread(header, 1, 70, fp) < 54) {
        fclose(fp);
        return 0;
    }

    // Check BMP signature
    if (header[0] != 'B' || header[1] != 'M') {
        fclose(fp);
        return 0;
    }

    // Parse header fields
    uint32_t data_offset = header[10] | (header[11] << 8) | (header[12] << 16) | (header[13] << 24);
    uint32_t dib_size = header[14] | (header[15] << 8) | (header[16] << 16) | (header[17] << 24);
    int32_t img_width = header[18] | (header[19] << 8) | (header[20] << 16) | (header[21] << 24);
    int32_t img_height = header[22] | (header[23] << 8) | (header[24] << 16) | (header[25] << 24);
    uint16_t bit_depth = header[28] | (header[29] << 8);
    uint32_t compression = header[30] | (header[31] << 8) | (header[32] << 16) | (header[33] << 24);

    // Handle negative height (top-down DIB)
    int top_down = 0;
    if (img_height < 0) {
        img_height = -img_height;
        top_down = 1;
    }

    // Validate dimensions
    if (img_width <= 0 || img_height <= 0 || img_width > 2048 || img_height > 2048) {
        fclose(fp);
        return 0;
    }

    // Only support uncompressed or BI_BITFIELDS
    if (compression != 0 && compression != 3) {
        fclose(fp);
        return 0;
    }

    // v40: Read color masks for BI_BITFIELDS (16/32-bit)
    uint32_t r_mask = 0, g_mask = 0, b_mask = 0;
    int r_shift = 0, g_shift = 0, b_shift = 0;
    int r_bits = 0, g_bits = 0, b_bits = 0;

    if (compression == 3 && (bit_depth == 16 || bit_depth == 32)) {
        // Masks are at offset 54 in BITMAPINFOHEADER (after 40-byte DIB header)
        r_mask = header[54] | (header[55] << 8) | (header[56] << 16) | (header[57] << 24);
        g_mask = header[58] | (header[59] << 8) | (header[60] << 16) | (header[61] << 24);
        b_mask = header[62] | (header[63] << 8) | (header[64] << 16) | (header[65] << 24);
        r_shift = count_shift(r_mask);
        g_shift = count_shift(g_mask);
        b_shift = count_shift(b_mask);
        r_bits = count_bits(r_mask);
        g_bits = count_bits(g_mask);
        b_bits = count_bits(b_mask);
    }

    // Read color palette for indexed formats
    uint8_t palette[1024] = {0};  // 256 colors * 4 bytes (BGRA)
    int palette_colors = 0;
    if (bit_depth <= 8) {
        palette_colors = 1 << bit_depth;
        fseek(fp, 14 + dib_size, SEEK_SET);  // Palette starts after DIB header
        fread(palette, 4, palette_colors, fp);
    }

    // Allocate output buffer
    *width = img_width;
    *height = img_height;
    *data = (uint16_t*)malloc(img_width * img_height * sizeof(uint16_t));
    if (!*data) {
        fclose(fp);
        return 0;
    }

    // Calculate row size (rows are 4-byte aligned)
    int bits_per_row = img_width * bit_depth;
    int row_size = ((bits_per_row + 31) / 32) * 4;
    uint8_t* row_buffer = (uint8_t*)malloc(row_size);
    if (!row_buffer) {
        free(*data);
        *data = NULL;
        fclose(fp);
        return 0;
    }

    // Seek to pixel data
    fseek(fp, data_offset, SEEK_SET);

    // Read and convert pixel data
    for (int y = 0; y < img_height; y++) {
        int dst_y = top_down ? y : (img_height - 1 - y);
        if (fread(row_buffer, 1, row_size, fp) != (size_t)row_size) break;

        for (int x = 0; x < img_width; x++) {
            uint8_t r, g, b;

            switch (bit_depth) {
                case 1: {
                    int byte_idx = x / 8;
                    int bit_idx = 7 - (x % 8);
                    int pal_idx = (row_buffer[byte_idx] >> bit_idx) & 1;
                    b = palette[pal_idx * 4 + 0];
                    g = palette[pal_idx * 4 + 1];
                    r = palette[pal_idx * 4 + 2];
                    break;
                }
                case 4: {
                    int byte_idx = x / 2;
                    int pal_idx = (x % 2 == 0) ? (row_buffer[byte_idx] >> 4) : (row_buffer[byte_idx] & 0x0F);
                    b = palette[pal_idx * 4 + 0];
                    g = palette[pal_idx * 4 + 1];
                    r = palette[pal_idx * 4 + 2];
                    break;
                }
                case 8: {
                    int pal_idx = row_buffer[x];
                    b = palette[pal_idx * 4 + 0];
                    g = palette[pal_idx * 4 + 1];
                    r = palette[pal_idx * 4 + 2];
                    break;
                }
                case 16: {
                    uint16_t pixel = row_buffer[x * 2] | (row_buffer[x * 2 + 1] << 8);
                    if (compression == 3 && r_mask) {
                        // BI_BITFIELDS - use masks (RGB565, RGB555, etc.)
                        int rv = (pixel & r_mask) >> r_shift;
                        int gv = (pixel & g_mask) >> g_shift;
                        int bv = (pixel & b_mask) >> b_shift;
                        // Scale to 8-bit
                        r = r_bits == 5 ? (rv << 3) | (rv >> 2) : (rv << (8 - r_bits));
                        g = g_bits == 6 ? (gv << 2) | (gv >> 4) : g_bits == 5 ? (gv << 3) | (gv >> 2) : (gv << (8 - g_bits));
                        b = b_bits == 5 ? (bv << 3) | (bv >> 2) : (bv << (8 - b_bits));
                    } else {
                        // Default: RGB555 (X1R5G5B5)
                        r = ((pixel >> 10) & 0x1F) << 3;
                        g = ((pixel >> 5) & 0x1F) << 3;
                        b = (pixel & 0x1F) << 3;
                    }
                    break;
                }
                case 24: {
                    b = row_buffer[x * 3 + 0];
                    g = row_buffer[x * 3 + 1];
                    r = row_buffer[x * 3 + 2];
                    break;
                }
                case 32: {
                    // 32-bit: BGRA, ignore alpha
                    b = row_buffer[x * 4 + 0];
                    g = row_buffer[x * 4 + 1];
                    r = row_buffer[x * 4 + 2];
                    break;
                }
                default:
                    r = g = b = 0;
                    break;
            }

            // Convert to RGB565
            (*data)[dst_y * img_width + x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        }
    }

    free(row_buffer);
    fclose(fp);
    return 1;
}

// v40: Load GIF file to RGB565 format (first frame only for screenshots)
int load_gif_rgb565(const char* filename, uint16_t** data, int* width, int* height) {
    gd_GIF* gif = gd_open_gif(filename);
    if (!gif) return 0;

    // Get first frame
    int ret = gd_get_frame(gif);
    if (ret != 1) {
        gd_close_gif(gif);
        return 0;
    }

    // Allocate RGB888 buffer for rendering
    uint8_t* rgb_buffer = (uint8_t*)malloc(gif->width * gif->height * 3);
    if (!rgb_buffer) {
        gd_close_gif(gif);
        return 0;
    }

    // Render frame to RGB888
    gd_render_frame(gif, rgb_buffer);

    // Allocate RGB565 output buffer
    *width = gif->width;
    *height = gif->height;
    *data = (uint16_t*)malloc(gif->width * gif->height * sizeof(uint16_t));
    if (!*data) {
        free(rgb_buffer);
        gd_close_gif(gif);
        return 0;
    }

    // Convert RGB888 to RGB565
    for (int i = 0; i < gif->width * gif->height; i++) {
        uint8_t r = rgb_buffer[i * 3 + 0];
        uint8_t g = rgb_buffer[i * 3 + 1];
        uint8_t b = rgb_buffer[i * 3 + 2];
        (*data)[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }

    free(rgb_buffer);
    gd_close_gif(gif);
    return 1;
}

// ============================================================================
// v70: Memory-based image loaders for chunked loading
// ============================================================================

int load_png_rgb565_mem(const uint8_t* buffer, uint32_t size, uint16_t** data, int* width, int* height) {
    unsigned char* rgba_data = NULL;
    unsigned int w, h;

    // Decode PNG from memory
    unsigned error = lodepng_decode32(&rgba_data, &w, &h, buffer, size);
    if (error) return 0;

    // Allocate RGB565 buffer
    *data = (uint16_t*)malloc(w * h * sizeof(uint16_t));
    if (!*data) {
        free(rgba_data);
        return 0;
    }

    // Convert RGBA -> RGB565
    for (unsigned int i = 0; i < w * h; i++) {
        unsigned char r = rgba_data[i * 4 + 0];
        unsigned char g = rgba_data[i * 4 + 1];
        unsigned char b = rgba_data[i * 4 + 2];
        (*data)[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }

    free(rgba_data);
    *width = (int)w;
    *height = (int)h;
    return 1;
}

int load_jpeg_rgb565_mem(const uint8_t* buffer, uint32_t size, uint16_t** data, int* width, int* height) {
    int w, h, channels;
    uint8_t* rgb_data = stbi_load_from_memory(buffer, size, &w, &h, &channels, 3);
    if (!rgb_data) return 0;

    *width = w;
    *height = h;
    *data = (uint16_t*)malloc(w * h * sizeof(uint16_t));
    if (!*data) {
        stbi_image_free(rgb_data);
        return 0;
    }

    // Convert RGB888 to RGB565
    for (int i = 0; i < w * h; i++) {
        uint8_t r = rgb_data[i * 3 + 0];
        uint8_t g = rgb_data[i * 3 + 1];
        uint8_t b = rgb_data[i * 3 + 2];
        (*data)[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }

    stbi_image_free(rgb_data);
    return 1;
}

int load_bmp_rgb565_mem(const uint8_t* buffer, uint32_t size, uint16_t** data, int* width, int* height) {
    // BMP from memory - parse header and pixel data directly
    if (size < 54) return 0;  // Minimum BMP header size

    // Check BMP signature
    if (buffer[0] != 'B' || buffer[1] != 'M') return 0;

    // Get header info
    uint32_t data_offset = *(uint32_t*)(buffer + 10);
    int32_t w = *(int32_t*)(buffer + 18);
    int32_t h = *(int32_t*)(buffer + 22);
    uint16_t bpp = *(uint16_t*)(buffer + 28);

    if (w <= 0 || h == 0) return 0;
    int flip = (h > 0);  // BMP is bottom-up if h > 0
    if (h < 0) h = -h;

    // Only support 24-bit BMP for simplicity in memory version
    if (bpp != 24) return 0;

    if (data_offset + w * h * 3 > size) return 0;

    *width = w;
    *height = h;
    *data = (uint16_t*)malloc(w * h * sizeof(uint16_t));
    if (!*data) return 0;

    int row_size = ((w * 3 + 3) / 4) * 4;  // Row padding
    for (int y = 0; y < h; y++) {
        int src_y = flip ? (h - 1 - y) : y;
        const uint8_t* row = buffer + data_offset + src_y * row_size;
        for (int x = 0; x < w; x++) {
            uint8_t b = row[x * 3 + 0];
            uint8_t g = row[x * 3 + 1];
            uint8_t r = row[x * 3 + 2];
            (*data)[y * w + x] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        }
    }
    return 1;
}

int load_gif_rgb565_mem(const uint8_t* buffer, uint32_t size, uint16_t** data, int* width, int* height) {
    // GIF memory loading not supported - gifdec only has file API
    // Fallback: write temp file and use file loader
    (void)buffer; (void)size; (void)data; (void)width; (void)height;
    return 0;  // Not implemented - will fall back to file-based loading
}

int load_webp_rgb565_mem(const uint8_t* buffer, uint32_t size, uint16_t** data, int* width, int* height) {
    // NOTE: simplewebp holds a pointer to buffer, so caller must NOT free it
    // until after this function returns. The chunked loader handles this.
    simplewebp* webp = NULL;
    simplewebp_error err = simplewebp_load_from_memory((void*)buffer, size, NULL, &webp);
    if (err != SIMPLEWEBP_NO_ERROR || !webp) return 0;

    size_t w, h;
    simplewebp_get_dimensions(webp, &w, &h);
    if (w == 0 || h == 0) {
        simplewebp_unload(webp);
        return 0;
    }

    // Temp RGBA buffer
    uint8_t* rgba_temp = (uint8_t*)malloc(w * h * 4);
    if (!rgba_temp) {
        simplewebp_unload(webp);
        return 0;
    }

    err = simplewebp_decode(webp, rgba_temp, NULL);
    simplewebp_unload(webp);

    if (err != SIMPLEWEBP_NO_ERROR) {
        free(rgba_temp);
        return 0;
    }

    // Allocate RGB565 output
    *width = (int)w;
    *height = (int)h;
    *data = (uint16_t*)malloc(w * h * sizeof(uint16_t));
    if (!*data) {
        free(rgba_temp);
        return 0;
    }

    // Convert RGBA -> RGB565
    for (size_t i = 0; i < w * h; i++) {
        uint8_t r = rgba_temp[i * 4 + 0];
        uint8_t g = rgba_temp[i * 4 + 1];
        uint8_t b = rgba_temp[i * 4 + 2];
        (*data)[i] = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    }

    free(rgba_temp);
    return 1;
}

// Clear screen with GFX theme background if active
void render_clear_screen_gfx(uint16_t *framebuffer) {
    if (!framebuffer) return;

    // Check if GFX theme is active and has background (platform-aware)
    uint16_t* bg = gfx_theme_get_platform_background();
    if (bg) {
        // Copy background image
        memcpy(framebuffer, bg, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));
    } else {
        // Fall back to solid color
        render_clear_screen(framebuffer);
    }
}

// Get current visible items count (from gfx_theme if active, otherwise default)
int render_get_visible_items(void) {
    if (gfx_theme_is_active()) {
        const GfxThemeLayout* layout = gfx_theme_get_layout();
        if (layout) {
            if (in_platform_menu) {
                return layout->platform_visible_items > 0 ? layout->platform_visible_items : VISIBLE_ENTRIES;
            } else {
                return layout->game_visible_items > 0 ? layout->game_visible_items : VISIBLE_ENTRIES;
            }
        }
    }
    return VISIBLE_ENTRIES;
}

// v56: Access universal buffer for image viewer
uint8_t* render_get_universal_buffer(void) {
    return universal_buffer;
}

size_t render_get_universal_buffer_size(void) {
    return UNIVERSAL_BUFFER_BYTES;
}