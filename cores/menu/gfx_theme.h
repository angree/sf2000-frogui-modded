#ifndef GFX_THEME_H
#define GFX_THEME_H

#include <stdint.h>
#include <stdbool.h>

// Maximum number of GFX themes that can be loaded
#define MAX_GFX_THEMES 32
#define MAX_THEME_NAME_LEN 64
#define MAX_THEME_PATH_LEN 256
#define MAX_PLATFORMS 64
#define MAX_PLATFORM_NAME_LEN 32

// GFX Theme Layout - defines where UI elements are positioned
typedef struct {
    // Platform list (main ROMS menu)
    int platform_list_x;
    int platform_list_y_start;    // Top of list area
    int platform_list_y_end;      // Bottom of list area
    int platform_item_height;
    int platform_visible_items;   // Auto-calculated if 0

    // Game list (inside console folders)
    int game_list_x;
    int game_list_y_start;        // Top of list area
    int game_list_y_end;          // Bottom of list area
    int game_item_height;
    int game_visible_items;       // Auto-calculated if 0

    // Thumbnail area
    int thumb_x;
    int thumb_y;
    int thumb_width;
    int thumb_height;

    // Header
    int header_x;
    int header_y;

    // Legend (bottom bar with A/B/SELECT hints)
    int legend_x;
    int legend_y;

    // Counter (e.g., "3/125" in corner)
    int counter_x;
    int counter_y;
} GfxThemeLayout;

// GFX Theme - graphical theme with background image
typedef struct {
    char name[MAX_THEME_NAME_LEN];
    char path[MAX_THEME_PATH_LEN];          // Path to theme folder

    // Layout overrides (0 = use default)
    GfxThemeLayout layout;
    bool has_custom_layout;

    // Color overrides (0xFFFF = use color theme)
    uint16_t bg_color;
    uint16_t text_color;
    uint16_t select_bg_color;
    uint16_t select_text_color;
    bool has_custom_colors;

    // Background image data (loaded on demand)
    uint16_t* background_data;
    bool background_loaded;

    // Per-platform backgrounds - cached as loaded on-demand
    char platform_names[MAX_PLATFORMS][MAX_PLATFORM_NAME_LEN];
    uint16_t* platform_bg_data[MAX_PLATFORMS];
    bool platform_bg_loaded[MAX_PLATFORMS];  // true = tried loading (even if failed)
    int num_platforms;  // Number of cached platform entries

    // v20: Text background options
    // platform_text_background: 0 = shadow/outline (default), 1 = rounded black background
    // game_text_background: 0 = shadow/outline (default), 1 = rounded black background
    bool platform_text_background;
    bool game_text_background;

    // v32: Game screenshot area
    int game_screenshot_x_start;
    int game_screenshot_x_end;
    int game_screenshot_y_start;
    int game_screenshot_y_end;

    // v36: Custom theme logo (resources/general/frogui_logo.png)
    uint16_t* theme_logo_pixels;
    uint8_t* theme_logo_alpha;
    int theme_logo_width;
    int theme_logo_height;
    int theme_logo_loaded;  // 0=not tried, 1=loaded, -1=failed
} GfxTheme;

// Theme directory on SD card
#define GFX_THEMES_DIR "/mnt/sda1/THEMES"

// Initialize GFX theme system
void gfx_theme_init(void);

// Scan THEMES directory for available themes
int gfx_theme_scan(void);

// Get number of available GFX themes
int gfx_theme_count(void);

// Get theme name by index (0 = "None/Disabled")
const char* gfx_theme_get_name(int index);

// Apply GFX theme by index (0 = disable GFX themes)
int gfx_theme_apply(int index);

// Get current GFX theme index
int gfx_theme_get_current_index(void);

// Check if GFX theme is active
bool gfx_theme_is_active(void);

// Get current GFX theme (NULL if none active)
const GfxTheme* gfx_theme_get_current(void);

// Get current layout (returns default if no custom layout)
const GfxThemeLayout* gfx_theme_get_layout(void);

// Get background image data (loads if not loaded yet)
// Returns NULL if no background or load failed
uint16_t* gfx_theme_get_background(void);

// v61: Apply PNG overlay to framebuffer (for animated backgrounds)
// Call after drawing images/thumbnails, before text
void gfx_theme_apply_overlay(uint16_t* framebuffer);

// Set current platform for per-platform backgrounds (e.g., "nes", "gba")
void gfx_theme_set_platform(const char* platform);

// Get background for current platform (falls back to main if no platform-specific)
uint16_t* gfx_theme_get_platform_background(void);

// Free background image data
void gfx_theme_free_background(void);

// Cleanup all GFX theme resources
void gfx_theme_cleanup(void);

// Animated background support (for background.avi)
// Check if main background is animated (AVI)
bool gfx_theme_is_animated(void);

// Advance animation frame - call at 15fps rate (~67ms)
void gfx_theme_advance_animation(void);

// Pause animation (e.g., when entering platform folder with static PNG)
void gfx_theme_pause_animation(void);

// Resume animation (e.g., when returning to main menu)
void gfx_theme_resume_animation(void);

// v20: Text background style options
// Returns true if menu items should have rounded black background instead of shadow
bool gfx_theme_platform_text_background(void);
// Returns true if section items (platforms) should have rounded black background
bool gfx_theme_game_text_background(void);

// v32: Get game screenshot area (returns 0 if not configured or disabled)
int gfx_theme_get_screenshot_x_start(void);
int gfx_theme_get_screenshot_x_end(void);
int gfx_theme_get_screenshot_y_start(void);
int gfx_theme_get_screenshot_y_end(void);

// v36: Get theme logo (resources/general/frogui_logo.png if it exists)
// Returns 1 if logo available, fills out parameters
// Returns 0 if no theme logo (use built-in)
int gfx_theme_get_logo(uint16_t** pixels, uint8_t** alpha, int* width, int* height);

// Default layout constants

// Platform list (main menu) - left side
#define DEFAULT_PLATFORM_LIST_X         16
#define DEFAULT_PLATFORM_LIST_Y_START   40
#define DEFAULT_PLATFORM_LIST_Y_END     208
#define DEFAULT_PLATFORM_ITEM_HEIGHT    24
#define DEFAULT_PLATFORM_VISIBLE_ITEMS  7

// Game list (inside folders) - can be different position
#define DEFAULT_GAME_LIST_X             16
#define DEFAULT_GAME_LIST_Y_START       40
#define DEFAULT_GAME_LIST_Y_END         208
#define DEFAULT_GAME_ITEM_HEIGHT        24
#define DEFAULT_GAME_VISIBLE_ITEMS      7

// Thumbnail
#define DEFAULT_THUMB_X                 160
#define DEFAULT_THUMB_Y                 40
#define DEFAULT_THUMB_WIDTH             150
#define DEFAULT_THUMB_HEIGHT            180

// Header
#define DEFAULT_HEADER_X                16
#define DEFAULT_HEADER_Y                10

// Legend (bottom bar)
#define DEFAULT_LEGEND_X                16
#define DEFAULT_LEGEND_Y                220

// Counter position (top right)
#define DEFAULT_COUNTER_X               308
#define DEFAULT_COUNTER_Y               8

#endif // GFX_THEME_H
