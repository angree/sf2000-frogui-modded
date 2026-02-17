#include "gfx_theme.h"
#include "render.h"
#include "avi_bg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>

// v37: Debug logging
extern void xlog(const char *fmt, ...);

// External from render.c for PNG loading (using lodepng)
extern int load_png_rgb565(const char* filename, uint16_t** data, int* width, int* height);

// v19: Load PNG with alpha channel (for transparent overlays)
// Returns: 1 on success, 0 on failure
// pixels: RGB565 pixel data (caller must free)
// alpha: 8-bit alpha data (caller must free)
extern int load_png_rgba565(const char* filename, uint16_t** pixels, uint8_t** alpha, int* width, int* height);

// Animated background state
static bool main_bg_is_animated = false;
static char main_bg_avi_path[MAX_THEME_PATH_LEN] = "";

// v19: PNG overlay on animation
static uint16_t* main_bg_overlay_pixels = NULL;
static uint8_t* main_bg_overlay_alpha = NULL;
static bool main_bg_has_overlay = false;

// v28: Pre-computed overlay blend factors (avoid per-pixel alpha math at runtime)
// For each pixel: if alpha > 250, use overlay directly; if alpha < 5, use bg directly
// Otherwise store pre-multiplied values: overlay_premult = overlay * alpha / 255
static uint16_t* overlay_premult_rgb = NULL;  // Pre-multiplied overlay RGB
static uint8_t* overlay_blend_mode = NULL;    // 0=transparent, 1=blend, 2=opaque

// v62: Global sections overlay (resources/sections/background_anim.png)
// Used for all platforms when no platform-specific background exists
static uint16_t* sections_overlay_pixels = NULL;
static uint8_t* sections_overlay_alpha = NULL;
static uint8_t* sections_overlay_blend_mode = NULL;
static bool sections_has_overlay = false;

// v62: Dither matrix for 16-bit blending (4x4 Bayer)
static const int8_t dither_matrix[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 }
};

// v19: Composite buffer for applying overlay to animation frames
static uint16_t* composite_buffer = NULL;

// v28: Fast overlay application using pre-computed blend data
static void apply_overlay_to_frame_fast(uint16_t* dst, const uint16_t* src) {
    if (!main_bg_has_overlay || !overlay_blend_mode) {
        // No overlay - just copy
        memcpy(dst, src, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));
        return;
    }

    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        uint8_t mode = overlay_blend_mode[i];
        if (mode == 0) {
            // Transparent - use source directly
            dst[i] = src[i];
        } else if (mode == 2) {
            // Opaque - use overlay directly
            dst[i] = main_bg_overlay_pixels[i];
        } else {
            // Blend mode - use pre-computed alpha
            uint8_t alpha = main_bg_overlay_alpha[i];
            uint16_t fg = main_bg_overlay_pixels[i];
            uint16_t bg = src[i];

            int fg_r = (fg >> 11) & 0x1F;
            int fg_g = (fg >> 5) & 0x3F;
            int fg_b = fg & 0x1F;

            int bg_r = (bg >> 11) & 0x1F;
            int bg_g = (bg >> 5) & 0x3F;
            int bg_b = bg & 0x1F;

            int a = alpha + 1;
            int inv_a = 257 - a;

            int r = (fg_r * a + bg_r * inv_a) >> 8;
            int g = (fg_g * a + bg_g * inv_a) >> 8;
            int b = (fg_b * a + bg_b * inv_a) >> 8;

            dst[i] = (r << 11) | (g << 5) | b;
        }
    }
}

// v28: Pre-compute overlay blend modes (called once at load time)
// v37: Removed 95% transparency check that was incorrectly disabling overlays
static void precompute_overlay_blend(void) {
    if (!main_bg_overlay_alpha || !main_bg_overlay_pixels) return;

    // Allocate blend mode buffer
    if (overlay_blend_mode) { free(overlay_blend_mode); }
    overlay_blend_mode = (uint8_t*)malloc(SCREEN_WIDTH * SCREEN_HEIGHT);
    if (!overlay_blend_mode) return;

    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        uint8_t alpha = main_bg_overlay_alpha[i];
        if (alpha < 5) {
            overlay_blend_mode[i] = 0;  // Transparent
        } else if (alpha > 250) {
            overlay_blend_mode[i] = 2;  // Opaque
        } else {
            overlay_blend_mode[i] = 1;  // Needs blending
        }
    }
    // v37: Always keep overlay enabled if it was loaded successfully
    // (removed the 95% transparency check that was causing issues)
}

// v62: Pre-compute sections overlay blend modes
static void precompute_sections_overlay_blend(void) {
    if (!sections_overlay_alpha || !sections_overlay_pixels) return;

    if (sections_overlay_blend_mode) { free(sections_overlay_blend_mode); }
    sections_overlay_blend_mode = (uint8_t*)malloc(SCREEN_WIDTH * SCREEN_HEIGHT);
    if (!sections_overlay_blend_mode) return;

    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        uint8_t alpha = sections_overlay_alpha[i];
        if (alpha < 5) {
            sections_overlay_blend_mode[i] = 0;
        } else if (alpha > 250) {
            sections_overlay_blend_mode[i] = 2;
        } else {
            sections_overlay_blend_mode[i] = 1;
        }
    }
}

// Available GFX themes (index 0 is always "None")
static GfxTheme gfx_themes[MAX_GFX_THEMES];
static int num_gfx_themes = 0;
static int current_gfx_theme = 0;  // 0 = None/Disabled
static char current_platform[MAX_PLATFORM_NAME_LEN] = "";  // Current platform (e.g., "nes", "gba")

// Default layout
static const GfxThemeLayout default_layout = {
    // Platform list (main menu)
    .platform_list_x = DEFAULT_PLATFORM_LIST_X,
    .platform_list_y_start = DEFAULT_PLATFORM_LIST_Y_START,
    .platform_list_y_end = DEFAULT_PLATFORM_LIST_Y_END,
    .platform_item_height = DEFAULT_PLATFORM_ITEM_HEIGHT,
    .platform_visible_items = DEFAULT_PLATFORM_VISIBLE_ITEMS,
    // Game list (inside folders)
    .game_list_x = DEFAULT_GAME_LIST_X,
    .game_list_y_start = DEFAULT_GAME_LIST_Y_START,
    .game_list_y_end = DEFAULT_GAME_LIST_Y_END,
    .game_item_height = DEFAULT_GAME_ITEM_HEIGHT,
    .game_visible_items = DEFAULT_GAME_VISIBLE_ITEMS,
    // Thumbnail
    .thumb_x = DEFAULT_THUMB_X,
    .thumb_y = DEFAULT_THUMB_Y,
    .thumb_width = DEFAULT_THUMB_WIDTH,
    .thumb_height = DEFAULT_THUMB_HEIGHT,
    // Header
    .header_x = DEFAULT_HEADER_X,
    .header_y = DEFAULT_HEADER_Y,
    // Legend
    .legend_x = DEFAULT_LEGEND_X,
    .legend_y = DEFAULT_LEGEND_Y,
    // Counter
    .counter_x = DEFAULT_COUNTER_X,
    .counter_y = DEFAULT_COUNTER_Y
};

// Helper: trim whitespace from string
static char* trim(char* str) {
    if (!str) return str;

    // Trim leading whitespace
    while (isspace((unsigned char)*str)) str++;

    if (*str == 0) return str;

    // Trim trailing whitespace
    char* end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';

    return str;
}

// Helper: parse hex color like "FFFFFF" or "#FFFFFF"
static uint16_t parse_hex_color(const char* str) {
    if (!str || !*str) return 0xFFFF;  // Invalid marker

    // Skip # if present
    if (*str == '#') str++;

    unsigned int r, g, b;
    if (sscanf(str, "%2x%2x%2x", &r, &g, &b) == 3) {
        return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }

    return 0xFFFF;  // Invalid
}

// Helper: parse integer with default
static int parse_int(const char* str, int def) {
    if (!str || !*str) return def;
    return atoi(str);
}

// Parse theme.ini file
static int parse_theme_ini(const char* ini_path, GfxTheme* theme) {
    FILE* f = fopen(ini_path, "r");
    if (!f) return 0;

    char line[512];
    char section[64] = "";

    while (fgets(line, sizeof(line), f)) {
        char* trimmed = trim(line);

        // Skip empty lines and comments
        if (!*trimmed || *trimmed == ';' || *trimmed == '#') continue;

        // Section header
        if (*trimmed == '[') {
            char* end = strchr(trimmed, ']');
            if (end) {
                *end = '\0';
                strncpy(section, trimmed + 1, sizeof(section) - 1);
                section[sizeof(section) - 1] = '\0';
            }
            continue;
        }

        // Key=value pair
        char* eq = strchr(trimmed, '=');
        if (!eq) continue;

        *eq = '\0';
        char* key = trim(trimmed);
        char* value = trim(eq + 1);

        // Parse based on section
        if (strcasecmp(section, "theme") == 0 || strcasecmp(section, "general") == 0) {
            // v19: name= is IGNORED - theme name comes from folder name only
            // This prevents issues where theme.ini has wrong name after copying folders
            // background= also ignored - loaded automatically from resources/general/

            // v20: Text background options
            if (strcasecmp(key, "platform_text_background") == 0) {
                theme->platform_text_background = (atoi(value) != 0);
            } else if (strcasecmp(key, "game_text_background") == 0) {
                theme->game_text_background = (atoi(value) != 0);
            }
            // v32: Game screenshot area
            else if (strcasecmp(key, "game_screenshot_x_start") == 0) {
                theme->game_screenshot_x_start = atoi(value);
            } else if (strcasecmp(key, "game_screenshot_x_end") == 0) {
                theme->game_screenshot_x_end = atoi(value);
            } else if (strcasecmp(key, "game_screenshot_y_start") == 0) {
                theme->game_screenshot_y_start = atoi(value);
            } else if (strcasecmp(key, "game_screenshot_y_end") == 0) {
                theme->game_screenshot_y_end = atoi(value);
            }
        }
        // FrogUI layout section - our custom format
        else if (strcasecmp(section, "layout") == 0) {
            theme->has_custom_layout = true;

            // Platform list (main menu)
            if (strcasecmp(key, "platform_list_x") == 0) {
                theme->layout.platform_list_x = parse_int(value, DEFAULT_PLATFORM_LIST_X);
            } else if (strcasecmp(key, "platform_list_y_start") == 0) {
                theme->layout.platform_list_y_start = parse_int(value, DEFAULT_PLATFORM_LIST_Y_START);
            } else if (strcasecmp(key, "platform_list_y_end") == 0) {
                theme->layout.platform_list_y_end = parse_int(value, DEFAULT_PLATFORM_LIST_Y_END);
            } else if (strcasecmp(key, "platform_item_height") == 0) {
                theme->layout.platform_item_height = parse_int(value, DEFAULT_PLATFORM_ITEM_HEIGHT);
            } else if (strcasecmp(key, "platform_visible_items") == 0) {
                theme->layout.platform_visible_items = parse_int(value, DEFAULT_PLATFORM_VISIBLE_ITEMS);
            }
            // Game list (inside folders)
            else if (strcasecmp(key, "game_list_x") == 0) {
                theme->layout.game_list_x = parse_int(value, DEFAULT_GAME_LIST_X);
            } else if (strcasecmp(key, "game_list_y_start") == 0) {
                theme->layout.game_list_y_start = parse_int(value, DEFAULT_GAME_LIST_Y_START);
            } else if (strcasecmp(key, "game_list_y_end") == 0) {
                theme->layout.game_list_y_end = parse_int(value, DEFAULT_GAME_LIST_Y_END);
            } else if (strcasecmp(key, "game_item_height") == 0) {
                theme->layout.game_item_height = parse_int(value, DEFAULT_GAME_ITEM_HEIGHT);
            } else if (strcasecmp(key, "game_visible_items") == 0) {
                theme->layout.game_visible_items = parse_int(value, DEFAULT_GAME_VISIBLE_ITEMS);
            }
            // Thumbnail
            else if (strcasecmp(key, "thumb_x") == 0) {
                theme->layout.thumb_x = parse_int(value, DEFAULT_THUMB_X);
            } else if (strcasecmp(key, "thumb_y") == 0) {
                theme->layout.thumb_y = parse_int(value, DEFAULT_THUMB_Y);
            } else if (strcasecmp(key, "thumb_width") == 0) {
                theme->layout.thumb_width = parse_int(value, DEFAULT_THUMB_WIDTH);
            } else if (strcasecmp(key, "thumb_height") == 0) {
                theme->layout.thumb_height = parse_int(value, DEFAULT_THUMB_HEIGHT);
            }
            // Header
            else if (strcasecmp(key, "header_x") == 0) {
                theme->layout.header_x = parse_int(value, DEFAULT_HEADER_X);
            } else if (strcasecmp(key, "header_y") == 0) {
                theme->layout.header_y = parse_int(value, DEFAULT_HEADER_Y);
            }
            // Legend
            else if (strcasecmp(key, "legend_x") == 0) {
                theme->layout.legend_x = parse_int(value, DEFAULT_LEGEND_X);
            } else if (strcasecmp(key, "legend_y") == 0) {
                theme->layout.legend_y = parse_int(value, DEFAULT_LEGEND_Y);
            }
            // Counter
            else if (strcasecmp(key, "counter_x") == 0) {
                theme->layout.counter_x = parse_int(value, DEFAULT_COUNTER_X);
            } else if (strcasecmp(key, "counter_y") == 0) {
                theme->layout.counter_y = parse_int(value, DEFAULT_COUNTER_Y);
            }
            // v22: Text background options (moved from [theme] to [layout])
            else if (strcasecmp(key, "platform_text_background") == 0) {
                theme->platform_text_background = (atoi(value) != 0);
            } else if (strcasecmp(key, "game_text_background") == 0) {
                theme->game_text_background = (atoi(value) != 0);
            }
            // v32: Game screenshot area (also can be in [layout])
            else if (strcasecmp(key, "game_screenshot_x_start") == 0) {
                theme->game_screenshot_x_start = atoi(value);
            } else if (strcasecmp(key, "game_screenshot_x_end") == 0) {
                theme->game_screenshot_x_end = atoi(value);
            } else if (strcasecmp(key, "game_screenshot_y_start") == 0) {
                theme->game_screenshot_y_start = atoi(value);
            } else if (strcasecmp(key, "game_screenshot_y_end") == 0) {
                theme->game_screenshot_y_end = atoi(value);
            }
        }
        else if (strcasecmp(section, "colors") == 0) {
            uint16_t color = parse_hex_color(value);
            if (color != 0xFFFF) {
                theme->has_custom_colors = true;

                if (strcasecmp(key, "bg") == 0) {
                    theme->bg_color = color;
                } else if (strcasecmp(key, "text") == 0) {
                    theme->text_color = color;
                } else if (strcasecmp(key, "select_bg") == 0) {
                    theme->select_bg_color = color;
                } else if (strcasecmp(key, "select_text") == 0) {
                    theme->select_text_color = color;
                }
            }
        }
    }

    fclose(f);
    return 1;
}

// v19: Load background image with new priority:
// 1. background_anim.avi (animation)
// 2. background_anim.png (transparent overlay for animation)
// 3. If no AVI, use background.png (static)
// Also supports legacy background.avi for backward compatibility
static int load_background_image(GfxTheme* theme) {
    if (theme->background_loaded) return 1;
    if (!theme->path[0]) return 0;

    char bg_path[MAX_THEME_PATH_LEN];
    int width, height;

    // Reset overlay state
    main_bg_has_overlay = false;
    if (main_bg_overlay_pixels) { free(main_bg_overlay_pixels); main_bg_overlay_pixels = NULL; }
    if (main_bg_overlay_alpha) { free(main_bg_overlay_alpha); main_bg_overlay_alpha = NULL; }

    // Step 1: Try animated backgrounds (AVI)
    // Priority: background_anim.avi > background.avi (legacy)
    bool anim_loaded = false;

    // 1a. resources/general/background_anim.avi (new format)
    snprintf(bg_path, sizeof(bg_path), "%s/resources/general/background_anim.avi", theme->path);
    if (avi_bg_load(bg_path)) {
        main_bg_is_animated = true;
        strncpy(main_bg_avi_path, bg_path, MAX_THEME_PATH_LEN - 1);
        anim_loaded = true;
    }

    // 1b. background_anim.avi in theme root
    if (!anim_loaded) {
        snprintf(bg_path, sizeof(bg_path), "%s/background_anim.avi", theme->path);
        if (avi_bg_load(bg_path)) {
            main_bg_is_animated = true;
            strncpy(main_bg_avi_path, bg_path, MAX_THEME_PATH_LEN - 1);
            anim_loaded = true;
        }
    }

    // 1c. Legacy: resources/general/background.avi
    if (!anim_loaded) {
        snprintf(bg_path, sizeof(bg_path), "%s/resources/general/background.avi", theme->path);
        if (avi_bg_load(bg_path)) {
            main_bg_is_animated = true;
            strncpy(main_bg_avi_path, bg_path, MAX_THEME_PATH_LEN - 1);
            anim_loaded = true;
        }
    }

    // 1d. Legacy: background.avi in theme root
    if (!anim_loaded) {
        snprintf(bg_path, sizeof(bg_path), "%s/background.avi", theme->path);
        if (avi_bg_load(bg_path)) {
            main_bg_is_animated = true;
            strncpy(main_bg_avi_path, bg_path, MAX_THEME_PATH_LEN - 1);
            anim_loaded = true;
        }
    }

    // Step 2: If animation loaded, try to load transparent overlay PNG
    if (anim_loaded) {
        xlog("gfx_theme: AVI loaded, trying overlay...\n");

        // Try background_anim.png as transparent overlay
        snprintf(bg_path, sizeof(bg_path), "%s/resources/general/background_anim.png", theme->path);
        xlog("gfx_theme: Trying overlay: %s\n", bg_path);
        if (load_png_rgba565(bg_path, &main_bg_overlay_pixels, &main_bg_overlay_alpha, &width, &height)) {
            xlog("gfx_theme: Overlay loaded %dx%d\n", width, height);
            if (width == SCREEN_WIDTH && height == SCREEN_HEIGHT) {
                main_bg_has_overlay = true;
                xlog("gfx_theme: Overlay SIZE OK, has_overlay=true\n");
            } else {
                xlog("gfx_theme: Overlay SIZE MISMATCH, freeing\n");
                free(main_bg_overlay_pixels); main_bg_overlay_pixels = NULL;
                free(main_bg_overlay_alpha); main_bg_overlay_alpha = NULL;
            }
        } else {
            xlog("gfx_theme: Overlay load FAILED\n");
        }

        // Also try in theme root
        if (!main_bg_has_overlay) {
            snprintf(bg_path, sizeof(bg_path), "%s/background_anim.png", theme->path);
            xlog("gfx_theme: Trying overlay root: %s\n", bg_path);
            if (load_png_rgba565(bg_path, &main_bg_overlay_pixels, &main_bg_overlay_alpha, &width, &height)) {
                xlog("gfx_theme: Root overlay loaded %dx%d\n", width, height);
                if (width == SCREEN_WIDTH && height == SCREEN_HEIGHT) {
                    main_bg_has_overlay = true;
                    xlog("gfx_theme: Root overlay SIZE OK\n");
                } else {
                    xlog("gfx_theme: Root overlay SIZE MISMATCH\n");
                    free(main_bg_overlay_pixels); main_bg_overlay_pixels = NULL;
                    free(main_bg_overlay_alpha); main_bg_overlay_alpha = NULL;
                }
            }
        }

        // v28: Pre-compute overlay blend modes for faster runtime
        if (main_bg_has_overlay) {
            xlog("gfx_theme: Calling precompute_overlay_blend\n");
            precompute_overlay_blend();
            xlog("gfx_theme: After precompute, has_overlay=%d\n", main_bg_has_overlay);
        }

        // v62: Try to load sections overlay (resources/sections/background_anim.png)
        // This overlay is used for all platforms when no platform-specific background exists
        sections_has_overlay = false;
        if (sections_overlay_pixels) { free(sections_overlay_pixels); sections_overlay_pixels = NULL; }
        if (sections_overlay_alpha) { free(sections_overlay_alpha); sections_overlay_alpha = NULL; }
        if (sections_overlay_blend_mode) { free(sections_overlay_blend_mode); sections_overlay_blend_mode = NULL; }

        snprintf(bg_path, sizeof(bg_path), "%s/resources/sections/background_anim.png", theme->path);
        xlog("gfx_theme: Trying sections overlay: %s\n", bg_path);
        if (load_png_rgba565(bg_path, &sections_overlay_pixels, &sections_overlay_alpha, &width, &height)) {
            xlog("gfx_theme: Sections overlay loaded %dx%d\n", width, height);
            if (width == SCREEN_WIDTH && height == SCREEN_HEIGHT) {
                sections_has_overlay = true;
                precompute_sections_overlay_blend();
                xlog("gfx_theme: Sections overlay active\n");
            } else {
                free(sections_overlay_pixels); sections_overlay_pixels = NULL;
                free(sections_overlay_alpha); sections_overlay_alpha = NULL;
            }
        }

        theme->background_loaded = true;
        xlog("gfx_theme: Animation background loaded, overlay=%d, sections_overlay=%d\n", main_bg_has_overlay, sections_has_overlay);
        return 1;
    }

    // Step 3: No animation - fall back to static PNG backgrounds
    main_bg_is_animated = false;
    main_bg_avi_path[0] = '\0';

    // 3a. resources/general/background.png (SimpleMenu style)
    snprintf(bg_path, sizeof(bg_path), "%s/resources/general/background.png", theme->path);
    if (load_png_rgb565(bg_path, &theme->background_data, &width, &height)) {
        if (width == SCREEN_WIDTH && height == SCREEN_HEIGHT) {
            theme->background_loaded = true;
            return 1;
        }
        free(theme->background_data);
        theme->background_data = NULL;
    }

    // 3b. background.png in theme root
    snprintf(bg_path, sizeof(bg_path), "%s/background.png", theme->path);
    if (load_png_rgb565(bg_path, &theme->background_data, &width, &height)) {
        if (width == SCREEN_WIDTH && height == SCREEN_HEIGHT) {
            theme->background_loaded = true;
            return 1;
        }
        free(theme->background_data);
        theme->background_data = NULL;
    }

    return 0;
}

void gfx_theme_init(void) {
    memset(gfx_themes, 0, sizeof(gfx_themes));
    num_gfx_themes = 0;
    current_gfx_theme = 0;
    main_bg_is_animated = false;
    main_bg_avi_path[0] = '\0';

    // v19: Reset overlay state
    main_bg_overlay_pixels = NULL;
    main_bg_overlay_alpha = NULL;
    main_bg_has_overlay = false;

    // v19: Allocate composite buffer for overlay blending
    if (!composite_buffer) {
        composite_buffer = (uint16_t*)malloc(SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));
    }

    // Initialize animated background system
    avi_bg_init();

    // First entry is always "None"
    strcpy(gfx_themes[0].name, "None");
    num_gfx_themes = 1;
}

int gfx_theme_scan(void) {
    // Reset to just "None"
    gfx_theme_cleanup();
    gfx_theme_init();

    DIR* dir = opendir(GFX_THEMES_DIR);
    if (!dir) {
        // THEMES directory doesn't exist - that's OK
        return 0;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL && num_gfx_themes < MAX_GFX_THEMES) {
        // Skip . and ..
        if (entry->d_name[0] == '.') continue;

        // Build path to theme folder
        char theme_path[MAX_THEME_PATH_LEN];
        snprintf(theme_path, sizeof(theme_path), "%s/%s", GFX_THEMES_DIR, entry->d_name);

        // Check if it's a directory
        DIR* subdir = opendir(theme_path);
        if (!subdir) continue;
        closedir(subdir);

        // Create theme entry - NO file checking here, all on-demand
        GfxTheme* theme = &gfx_themes[num_gfx_themes];
        memset(theme, 0, sizeof(GfxTheme));
        strncpy(theme->name, entry->d_name, MAX_THEME_NAME_LEN - 1);
        strncpy(theme->path, theme_path, MAX_THEME_PATH_LEN - 1);
        theme->layout = default_layout;

        // Check for theme.ini only for layout settings (optional)
        char ini_path[MAX_THEME_PATH_LEN];
        snprintf(ini_path, sizeof(ini_path), "%s/theme.ini", theme_path);
        FILE* f = fopen(ini_path, "r");
        if (f) {
            fclose(f);
            parse_theme_ini(ini_path, theme);
        }

        // Background path will be loaded on-demand, not pre-checked
        theme->num_platforms = 0;
        num_gfx_themes++;
    }

    closedir(dir);
    return num_gfx_themes - 1;  // Don't count "None"
}

int gfx_theme_count(void) {
    return num_gfx_themes;
}

const char* gfx_theme_get_name(int index) {
    if (index < 0 || index >= num_gfx_themes) return "Unknown";
    return gfx_themes[index].name;
}

int gfx_theme_apply(int index) {
    if (index < 0 || index >= num_gfx_themes) return 0;

    // v21: Skip if same theme is already applied (don't restart animation)
    if (index == current_gfx_theme) {
        return 1;
    }

    // Free previous background
    gfx_theme_free_background();

    current_gfx_theme = index;

    // Load new background if not "None"
    if (index > 0 && gfx_themes[index].path[0]) {
        load_background_image(&gfx_themes[index]);
    }

    return 1;
}

int gfx_theme_get_current_index(void) {
    return current_gfx_theme;
}

bool gfx_theme_is_active(void) {
    return current_gfx_theme > 0;
}

const GfxTheme* gfx_theme_get_current(void) {
    if (current_gfx_theme <= 0) return NULL;
    return &gfx_themes[current_gfx_theme];
}

const GfxThemeLayout* gfx_theme_get_layout(void) {
    if (current_gfx_theme > 0 && gfx_themes[current_gfx_theme].has_custom_layout) {
        return &gfx_themes[current_gfx_theme].layout;
    }
    return &default_layout;
}

uint16_t* gfx_theme_get_background(void) {
    if (current_gfx_theme <= 0) return NULL;

    GfxTheme* theme = &gfx_themes[current_gfx_theme];

    if (!theme->background_loaded) {
        load_background_image(theme);
    }

    // If animated background is active
    if (main_bg_is_animated && avi_bg_is_active()) {
        uint16_t* frame = avi_bg_get_frame();
        if (!frame) return theme->background_data;

        // v61: DON'T apply overlay here - return just AVI frame
        // Overlay will be applied later by gfx_theme_apply_overlay()
        return frame;
    }

    return theme->background_data;
}

// v61: Apply PNG overlay to framebuffer (call after drawing thumbnails, before text)
// v62: Use sections overlay when in platform, add dithering
void gfx_theme_apply_overlay(uint16_t* framebuffer) {
    if (!framebuffer) return;
    if (!main_bg_is_animated || !avi_bg_is_active()) return;

    // v62: Select which overlay to use
    // If we're in a platform and sections overlay exists, use it
    // Otherwise use main overlay
    uint16_t* overlay_pixels = main_bg_overlay_pixels;
    uint8_t* overlay_alpha = main_bg_overlay_alpha;
    uint8_t* blend_mode = overlay_blend_mode;
    bool has_overlay = main_bg_has_overlay;

    if (current_platform[0] != '\0' && sections_has_overlay && sections_overlay_blend_mode) {
        // In a platform (section) - use sections overlay
        overlay_pixels = sections_overlay_pixels;
        overlay_alpha = sections_overlay_alpha;
        blend_mode = sections_overlay_blend_mode;
        has_overlay = true;
    }

    if (!has_overlay || !blend_mode) return;

    // Apply overlay with alpha blending and dithering
    for (int i = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        uint8_t mode = blend_mode[i];
        if (mode == 0) {
            // Transparent - keep framebuffer pixel
        } else if (mode == 2) {
            // Opaque - use overlay directly
            framebuffer[i] = overlay_pixels[i];
        } else {
            // Blend mode with dithering
            uint8_t alpha = overlay_alpha[i];
            uint16_t fg = overlay_pixels[i];
            uint16_t bg = framebuffer[i];

            int fg_r = (fg >> 11) & 0x1F;
            int fg_g = (fg >> 5) & 0x3F;
            int fg_b = fg & 0x1F;

            int bg_r = (bg >> 11) & 0x1F;
            int bg_g = (bg >> 5) & 0x3F;
            int bg_b = bg & 0x1F;

            int a = alpha + 1;
            int inv_a = 257 - a;

            // v62: Add dithering to reduce banding on 16-bit display
            int x = i % SCREEN_WIDTH;
            int y = i / SCREEN_WIDTH;
            int dither = dither_matrix[y & 3][x & 3] - 8;  // Range: -8 to +7

            int r = (fg_r * a + bg_r * inv_a + dither) >> 8;
            int g = (fg_g * a + bg_g * inv_a + (dither * 2)) >> 8;  // Green has 6 bits
            int b = (fg_b * a + bg_b * inv_a + dither) >> 8;

            // Clamp values
            if (r < 0) r = 0; else if (r > 31) r = 31;
            if (g < 0) g = 0; else if (g > 63) g = 63;
            if (b < 0) b = 0; else if (b > 31) b = 31;

            framebuffer[i] = (r << 11) | (g << 5) | b;
        }
    }
}

// Check if main background is animated
bool gfx_theme_is_animated(void) {
    return main_bg_is_animated && avi_bg_is_active();
}

// Advance animation frame (call at 15fps rate)
void gfx_theme_advance_animation(void) {
    if (main_bg_is_animated && avi_bg_is_active() && !avi_bg_is_paused()) {
        avi_bg_advance_frame();
    }
}

// Pause animation when entering platform folder with static background
void gfx_theme_pause_animation(void) {
    if (main_bg_is_animated && avi_bg_is_active()) {
        avi_bg_pause();
    }
}

// Resume animation when returning to main menu
void gfx_theme_resume_animation(void) {
    if (main_bg_is_animated && avi_bg_is_active()) {
        avi_bg_resume();
    }
}

// v20: Text background style getters
bool gfx_theme_platform_text_background(void) {
    if (current_gfx_theme <= 0) return false;
    return gfx_themes[current_gfx_theme].platform_text_background;
}

bool gfx_theme_game_text_background(void) {
    if (current_gfx_theme <= 0) return false;
    return gfx_themes[current_gfx_theme].game_text_background;
}

// v32: Game screenshot area getters
int gfx_theme_get_screenshot_x_start(void) {
    if (current_gfx_theme <= 0) return 0;
    return gfx_themes[current_gfx_theme].game_screenshot_x_start;
}

int gfx_theme_get_screenshot_x_end(void) {
    if (current_gfx_theme <= 0) return 0;
    return gfx_themes[current_gfx_theme].game_screenshot_x_end;
}

int gfx_theme_get_screenshot_y_start(void) {
    if (current_gfx_theme <= 0) return 0;
    return gfx_themes[current_gfx_theme].game_screenshot_y_start;
}

int gfx_theme_get_screenshot_y_end(void) {
    if (current_gfx_theme <= 0) return 0;
    return gfx_themes[current_gfx_theme].game_screenshot_y_end;
}

void gfx_theme_set_platform(const char* platform) {
    if (platform) {
        strncpy(current_platform, platform, MAX_PLATFORM_NAME_LEN - 1);
        current_platform[MAX_PLATFORM_NAME_LEN - 1] = '\0';
    } else {
        current_platform[0] = '\0';
    }
}

// Try to load platform background dynamically based on folder name
static uint16_t* try_load_dynamic_platform_bg(GfxTheme* theme, const char* platform) {
    if (!theme || !platform || !platform[0] || !theme->path[0]) return NULL;

    char bg_path[MAX_THEME_PATH_LEN];
    int width, height;
    uint16_t* data = NULL;

    // Convert platform name to lowercase for path matching
    char platform_lower[MAX_PLATFORM_NAME_LEN];
    strncpy(platform_lower, platform, MAX_PLATFORM_NAME_LEN - 1);
    platform_lower[MAX_PLATFORM_NAME_LEN - 1] = '\0';
    for (int i = 0; platform_lower[i]; i++) {
        if (platform_lower[i] >= 'A' && platform_lower[i] <= 'Z') {
            platform_lower[i] += 32;
        }
    }

    // Try multiple paths in order of priority:
    // 1. resources/{platform}/logo.png (SimpleMenu style - these ARE 320x240 backgrounds)
    snprintf(bg_path, sizeof(bg_path), "%s/resources/%s/logo.png", theme->path, platform_lower);
    if (load_png_rgb565(bg_path, &data, &width, &height)) {
        if (width == SCREEN_WIDTH && height == SCREEN_HEIGHT) return data;
        free(data); data = NULL;
    }

    // 2. resources/sections/{platform}/logo.png (RetroPixelBR style)
    snprintf(bg_path, sizeof(bg_path), "%s/resources/sections/%s/logo.png", theme->path, platform_lower);
    if (load_png_rgb565(bg_path, &data, &width, &height)) {
        if (width == SCREEN_WIDTH && height == SCREEN_HEIGHT) return data;
        free(data); data = NULL;
    }

    // 3. resources/{platform}/background.png
    snprintf(bg_path, sizeof(bg_path), "%s/resources/%s/background.png", theme->path, platform_lower);
    if (load_png_rgb565(bg_path, &data, &width, &height)) {
        if (width == SCREEN_WIDTH && height == SCREEN_HEIGHT) return data;
        free(data); data = NULL;
    }

    // 4. background_{platform}.png in theme root
    snprintf(bg_path, sizeof(bg_path), "%s/background_%s.png", theme->path, platform_lower);
    if (load_png_rgb565(bg_path, &data, &width, &height)) {
        if (width == SCREEN_WIDTH && height == SCREEN_HEIGHT) return data;
        free(data); data = NULL;
    }

    return NULL;
}

uint16_t* gfx_theme_get_platform_background(void) {
    if (current_gfx_theme <= 0) return NULL;

    GfxTheme* theme = &gfx_themes[current_gfx_theme];

    // If we have a current platform (folder), try to load its background dynamically
    if (current_platform[0]) {
        // Check if we already have this platform cached
        for (int i = 0; i < theme->num_platforms; i++) {
            if (strcasecmp(theme->platform_names[i], current_platform) == 0) {
                if (theme->platform_bg_data[i]) {
                    return theme->platform_bg_data[i];
                }
                // Already tried and failed for this platform
                break;
            }
        }

        // Not cached - try to load dynamically
        if (theme->num_platforms < MAX_PLATFORMS) {
            uint16_t* bg = try_load_dynamic_platform_bg(theme, current_platform);

            // Cache the result (even if NULL, to avoid retrying)
            int idx = theme->num_platforms;
            strncpy(theme->platform_names[idx], current_platform, MAX_PLATFORM_NAME_LEN - 1);
            theme->platform_names[idx][MAX_PLATFORM_NAME_LEN - 1] = '\0';
            theme->platform_bg_data[idx] = bg;
            theme->platform_bg_loaded[idx] = true;
            theme->num_platforms++;

            if (bg) return bg;
        }
    }

    // Fall back to main background
    return gfx_theme_get_background();
}

void gfx_theme_free_background(void) {
    // Close animated background if active
    if (main_bg_is_animated) {
        avi_bg_close();
        main_bg_is_animated = false;
        main_bg_avi_path[0] = '\0';
    }

    // v19: Free overlay
    if (main_bg_overlay_pixels) { free(main_bg_overlay_pixels); main_bg_overlay_pixels = NULL; }
    if (main_bg_overlay_alpha) { free(main_bg_overlay_alpha); main_bg_overlay_alpha = NULL; }
    // v28: Free pre-computed blend data
    if (overlay_blend_mode) { free(overlay_blend_mode); overlay_blend_mode = NULL; }
    main_bg_has_overlay = false;

    // v62: Free sections overlay
    if (sections_overlay_pixels) { free(sections_overlay_pixels); sections_overlay_pixels = NULL; }
    if (sections_overlay_alpha) { free(sections_overlay_alpha); sections_overlay_alpha = NULL; }
    if (sections_overlay_blend_mode) { free(sections_overlay_blend_mode); sections_overlay_blend_mode = NULL; }
    sections_has_overlay = false;

    for (int i = 0; i < num_gfx_themes; i++) {
        // v22: Always reset background_loaded flag (fixes animation not reloading after theme switch)
        gfx_themes[i].background_loaded = false;

        // Free main background
        if (gfx_themes[i].background_data) {
            free(gfx_themes[i].background_data);
            gfx_themes[i].background_data = NULL;
        }
        // Free platform backgrounds
        for (int p = 0; p < gfx_themes[i].num_platforms; p++) {
            if (gfx_themes[i].platform_bg_data[p]) {
                free(gfx_themes[i].platform_bg_data[p]);
                gfx_themes[i].platform_bg_data[p] = NULL;
            }
            gfx_themes[i].platform_bg_loaded[p] = false;
        }
        // v22: Reset platform counter
        gfx_themes[i].num_platforms = 0;

        // v36: Free theme logo
        if (gfx_themes[i].theme_logo_pixels) {
            free(gfx_themes[i].theme_logo_pixels);
            gfx_themes[i].theme_logo_pixels = NULL;
        }
        if (gfx_themes[i].theme_logo_alpha) {
            free(gfx_themes[i].theme_logo_alpha);
            gfx_themes[i].theme_logo_alpha = NULL;
        }
        gfx_themes[i].theme_logo_loaded = 0;
    }
}

void gfx_theme_cleanup(void) {
    gfx_theme_free_background();
    avi_bg_shutdown();
    current_gfx_theme = 0;

    // v19: Free composite buffer
    if (composite_buffer) {
        free(composite_buffer);
        composite_buffer = NULL;
    }
}

// v36: Get theme logo (resources/general/frogui_logo.png if it exists)
int gfx_theme_get_logo(uint16_t** pixels, uint8_t** alpha, int* width, int* height) {
    if (!pixels || !alpha || !width || !height) return 0;
    if (current_gfx_theme <= 0) return 0;  // No theme active

    GfxTheme* theme = &gfx_themes[current_gfx_theme];

    // Try to load if not loaded yet
    if (theme->theme_logo_loaded == 0 && theme->path[0]) {
        char logo_path[MAX_THEME_PATH_LEN];
        snprintf(logo_path, sizeof(logo_path), "%s/resources/general/frogui_logo.png", theme->path);

        int w, h;
        if (load_png_rgba565(logo_path, &theme->theme_logo_pixels, &theme->theme_logo_alpha, &w, &h)) {
            theme->theme_logo_width = w;
            theme->theme_logo_height = h;
            theme->theme_logo_loaded = 1;
        } else {
            theme->theme_logo_loaded = -1;  // Failed
        }
    }

    // Return logo if loaded
    if (theme->theme_logo_loaded == 1 && theme->theme_logo_pixels) {
        *pixels = theme->theme_logo_pixels;
        *alpha = theme->theme_logo_alpha;
        *width = theme->theme_logo_width;
        *height = theme->theme_logo_height;
        return 1;
    }

    return 0;  // No theme logo
}
