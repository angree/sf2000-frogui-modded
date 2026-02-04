// v24: Display filtering options for platforms
#include "display_opts.h"
#include "osk.h"
#include "font.h"
#include "render.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

// External function to get core name from folder (defined in frogos.c)
extern const char* get_core_name_for_console(const char* console_name);

// Debug logging
extern void xlog(const char *fmt, ...);

// Current display options
static DisplayOptions current_opts;
static char current_folder[64] = "";
static char current_core_name[64] = "";  // Core name for settings (e.g., "gpSP" for "gba")
static int menu_active = 0;
static int menu_selected = 0;
static int menu_scroll = 0;
static int editing_pattern = -1;  // Which pattern is being edited (-1 = none)
static int core_settings_loaded = 0;  // Whether we loaded core-specific settings
static int total_menu_items = 0;  // Total items including core settings
static int rescan_needed = 0;  // v31: Flag to tell caller to rescan directory after save

// Display options items (before core settings)
#define DISPLAY_OPTS_ITEMS 7
#define VISIBLE_MENU_ITEMS 8  // How many items fit on screen

void display_opts_init(void) {
    memset(&current_opts, 0, sizeof(current_opts));
    current_opts.mode = DISPLAY_FILES_AND_DIRS;
    current_opts.pattern_count = 0;
    current_opts.disk1_only = 0;
    current_opts.modified = 0;
    current_folder[0] = '\0';
    menu_active = 0;
    core_settings_loaded = 0;
}

// Build path to display options file
static void get_opts_path(const char *folder_name, char *path, size_t path_size) {
    char folder_lower[64];
    strncpy(folder_lower, folder_name, sizeof(folder_lower) - 1);
    folder_lower[sizeof(folder_lower) - 1] = '\0';
    for (int i = 0; folder_lower[i]; i++) {
        folder_lower[i] = tolower((unsigned char)folder_lower[i]);
    }

    snprintf(path, path_size, "/mnt/sda1/configs/%s/%s_display.opt", folder_lower, folder_lower);
}

void display_opts_load(const char *folder_name) {
    if (!folder_name) return;

    strncpy(current_folder, folder_name, sizeof(current_folder) - 1);
    current_folder[sizeof(current_folder) - 1] = '\0';

    // Reset to defaults
    current_opts.mode = DISPLAY_FILES_AND_DIRS;
    current_opts.pattern_count = 0;
    for (int i = 0; i < MAX_DISPLAY_PATTERNS; i++) {
        current_opts.patterns[i][0] = '\0';
    }
    current_opts.disk1_only = 0;
    current_opts.modified = 0;

    // Build path and try to load
    char path[256];
    get_opts_path(folder_name, path, sizeof(path));

    xlog("display_opts_load: folder=%s path=%s\n", folder_name, path);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        xlog("display_opts_load: file not found\n");
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        nl = strchr(line, '\r');
        if (nl) *nl = '\0';

        if (line[0] == '#' || line[0] == '\0') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;

        while (*key == ' ') key++;
        while (*value == ' ') value++;

        if (strcmp(key, "mode") == 0) {
            current_opts.mode = (strcmp(value, "files_only") == 0) ?
                               DISPLAY_FILES_ONLY : DISPLAY_FILES_AND_DIRS;
        } else if (strcmp(key, "pattern_count") == 0) {
            current_opts.pattern_count = atoi(value);
            if (current_opts.pattern_count < 0) current_opts.pattern_count = 0;
            if (current_opts.pattern_count > MAX_DISPLAY_PATTERNS)
                current_opts.pattern_count = MAX_DISPLAY_PATTERNS;
        } else if (strncmp(key, "pattern", 7) == 0) {
            int idx = key[7] - '0';
            if (idx >= 0 && idx < MAX_DISPLAY_PATTERNS) {
                strncpy(current_opts.patterns[idx], value, MAX_PATTERN_LEN - 1);
                current_opts.patterns[idx][MAX_PATTERN_LEN - 1] = '\0';
            }
        } else if (strcmp(key, "disk1_only") == 0) {
            current_opts.disk1_only = (strcmp(value, "true") == 0) ? 1 : 0;
        }
    }

    fclose(fp);
}

// Helper: create directory and parents if needed
static void ensure_directory(const char *path) {
    char tmp[256];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

void display_opts_save(void) {
    xlog("display_opts_save: modified=%d folder=%s\n", current_opts.modified, current_folder);
    if (!current_opts.modified || current_folder[0] == '\0') return;

    char path[256];
    get_opts_path(current_folder, path, sizeof(path));

    xlog("display_opts_save: path=%s mode=%d\n", path, current_opts.mode);

    // Ensure directory exists
    char dir_path[256];
    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = '\0';
    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
        ensure_directory(dir_path);
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        xlog("display_opts_save: FAILED to open file!\n");
        return;
    }

    fprintf(fp, "# FrogUI Display Options for %s\n", current_folder);
    fprintf(fp, "mode=%s\n", current_opts.mode == DISPLAY_FILES_ONLY ?
                             "files_only" : "files_and_dirs");
    fprintf(fp, "pattern_count=%d\n", current_opts.pattern_count);
    for (int i = 0; i < MAX_DISPLAY_PATTERNS; i++) {
        fprintf(fp, "pattern%d=%s\n", i, current_opts.patterns[i]);
    }
    fprintf(fp, "disk1_only=%s\n", current_opts.disk1_only ? "true" : "false");

    // Force write to disk
    fflush(fp);
    fclose(fp);
    current_opts.modified = 0;
    xlog("display_opts_save: SUCCESS\n");
}

const DisplayOptions* display_opts_get(void) {
    return &current_opts;
}

const char* display_opts_get_core_name(void) {
    return current_folder;
}

// OSK callback for pattern editing
static void pattern_edit_callback(int result, const char *input) {
    if (result && editing_pattern >= 0 && editing_pattern < MAX_DISPLAY_PATTERNS) {
        strncpy(current_opts.patterns[editing_pattern], input, MAX_PATTERN_LEN - 1);
        current_opts.patterns[editing_pattern][MAX_PATTERN_LEN - 1] = '\0';
        current_opts.modified = 1;
    }
    editing_pattern = -1;
}

// Check if menu item is disabled (inactive pattern or separator)
static int is_item_disabled(int item_idx) {
    // Skip inactive patterns
    if (item_idx >= 2 && item_idx <= 5) {
        int pattern_idx = item_idx - 2;
        return (pattern_idx >= current_opts.pattern_count);
    }
    // Skip separator line
    if (item_idx == DISPLAY_OPTS_ITEMS && core_settings_loaded) {
        return 1;
    }
    return 0;
}

// Find next enabled item in direction
static int find_next_enabled(int current, int direction) {
    int next = current + direction;
    int iterations = 0;

    while (iterations < total_menu_items) {
        if (next < 0) next = total_menu_items - 1;
        if (next >= total_menu_items) next = 0;

        if (!is_item_disabled(next)) {
            return next;
        }
        next += direction;
        iterations++;
    }
    return current;  // No enabled item found, stay put
}

void display_opts_show_menu(const char *folder_name) {
    display_opts_load(folder_name);

    // Get core name from folder name for settings
    const char *core_name = get_core_name_for_console(folder_name);
    if (core_name) {
        strncpy(current_core_name, core_name, sizeof(current_core_name) - 1);
        current_core_name[sizeof(current_core_name) - 1] = '\0';
        // Try to load core-specific settings using CORE name
        core_settings_loaded = settings_load_core(core_name);
    } else {
        current_core_name[0] = '\0';
        core_settings_loaded = 0;
    }

    // Calculate total menu items
    total_menu_items = DISPLAY_OPTS_ITEMS;
    if (core_settings_loaded) {
        total_menu_items += 1 + settings_get_count();  // +1 for separator
    }

    menu_active = 1;
    menu_selected = 0;
    menu_scroll = 0;
    editing_pattern = -1;
}

int display_opts_handle_input(int up, int down, int left, int right, int a, int b) {
    static int prev_up = 0, prev_down = 0, prev_left = 0, prev_right = 0;
    static int prev_a = 0, prev_b = 0;

    if (!menu_active) {
        prev_up = up; prev_down = down;
        prev_left = left; prev_right = right;
        prev_a = a; prev_b = b;
        return 0;
    }

    // If OSK is active, let it handle input
    if (osk_is_active()) {
        // v34: Pass 0, 0 for L/R since display_opts uses normal OSK mode (not editor mode)
        if (osk_handle_input(up, down, left, right, a, b, 0, 0)) {
            // OSK closed
        }
        prev_up = up; prev_down = down;
        prev_left = left; prev_right = right;
        prev_a = a; prev_b = b;
        return 0;
    }

    // Navigation - skip disabled items
    if (prev_up && !up) {
        menu_selected = find_next_enabled(menu_selected, -1);
        // Adjust scroll
        if (menu_selected < menu_scroll) {
            menu_scroll = menu_selected;
        }
    }
    if (prev_down && !down) {
        menu_selected = find_next_enabled(menu_selected, 1);
        // Adjust scroll
        if (menu_selected >= menu_scroll + VISIBLE_MENU_ITEMS) {
            menu_scroll = menu_selected - VISIBLE_MENU_ITEMS + 1;
        }
    }

    // A button - save and close menu (v31: trigger rescan of game list)
    if (prev_a && !a) {
        // Force save even if not modified (user explicitly pressed save)
        current_opts.modified = 1;
        display_opts_save();
        // Also save core settings if loaded
        if (core_settings_loaded) {
            settings_save();
        }
        // v31: Set flag to rescan directory, then close menu
        rescan_needed = 1;
        menu_active = 0;
        prev_a = a;
        return 1;
    }

    // B button - close menu (force save first)
    if (prev_b && !b) {
        // Force save even on B button
        current_opts.modified = 1;
        display_opts_save();
        if (core_settings_loaded) {
            settings_save();
        }
        menu_active = 0;
        prev_b = b;
        return 1;
    }

    // Left/Right - change value
    int direction = 0;
    if (prev_left && !left) direction = -1;
    if (prev_right && !right) direction = 1;

    if (direction != 0) {
        if (menu_selected < DISPLAY_OPTS_ITEMS) {
            // Display options
            switch (menu_selected) {
                case 0:  // Display mode
                    current_opts.mode = (current_opts.mode == DISPLAY_FILES_AND_DIRS) ?
                                       DISPLAY_FILES_ONLY : DISPLAY_FILES_AND_DIRS;
                    current_opts.modified = 1;
                    break;

                case 1:  // Pattern count
                    current_opts.pattern_count += direction;
                    if (current_opts.pattern_count < 0) current_opts.pattern_count = MAX_DISPLAY_PATTERNS;
                    if (current_opts.pattern_count > MAX_DISPLAY_PATTERNS) current_opts.pattern_count = 0;
                    current_opts.modified = 1;
                    break;

                case 2: case 3: case 4: case 5:  // Patterns
                    {
                        int pattern_idx = menu_selected - 2;
                        if (pattern_idx < current_opts.pattern_count) {
                            char title[32];
                            snprintf(title, sizeof(title), "PATTERN %d:", pattern_idx + 1);
                            editing_pattern = pattern_idx;
                            osk_open(title, current_opts.patterns[pattern_idx], pattern_edit_callback);
                        }
                    }
                    break;

                case 6:  // Disk 1 only
                    current_opts.disk1_only = !current_opts.disk1_only;
                    current_opts.modified = 1;
                    break;
            }
        } else if (core_settings_loaded && menu_selected > DISPLAY_OPTS_ITEMS) {
            // Core settings (skip separator at DISPLAY_OPTS_ITEMS)
            int core_idx = menu_selected - DISPLAY_OPTS_ITEMS - 1;
            if (core_idx >= 0 && core_idx < settings_get_count()) {
                settings_cycle_option(core_idx);
            }
        }
    }

    prev_up = up; prev_down = down;
    prev_left = left; prev_right = right;
    prev_a = a; prev_b = b;

    return 0;
}

int display_opts_is_active(void) {
    return menu_active;
}

// v31: Check if caller should rescan directory (clears flag on read)
int display_opts_needs_rescan(void) {
    int result = rescan_needed;
    rescan_needed = 0;
    return result;
}

// External function to clear screen with animated background
extern void render_clear_screen_gfx(uint16_t *framebuffer);

// Render display options menu
void display_opts_render(uint16_t *framebuffer) {
    if (!menu_active || !framebuffer) return;

    // v24 FIX: Always clear screen with animated background first
    render_clear_screen_gfx(framebuffer);

    // If OSK is active, render it on top
    if (osk_is_active()) {
        osk_render(framebuffer);
        return;
    }

    // Menu dimensions - v33: widened by 16px on each side
    int menu_x = 14;   // was 30, now 14 (-16px)
    int menu_y = 30;
    int menu_w = 292;  // was 260, now 292 (+32px total)
    int menu_h = 180;  // Was 160, now 180 (+20 for more items)
    int item_h = 18;

    // Background
    render_filled_rect(framebuffer, menu_x, menu_y, menu_w, menu_h, 0x2104);
    render_rect(framebuffer, menu_x, menu_y, menu_w, menu_h, 0xFFFF);

    // Title
    char title[64];
    snprintf(title, sizeof(title), "OPTIONS: %s", current_folder);
    font_draw_text(framebuffer, 320, 240, menu_x + 4, menu_y + 2, title, 0x07E0);

    int y = menu_y + 20;

    // Display options labels
    const char* display_labels[] = {
        "DISPLAY:",
        "PATTERNS:",
        "  PATTERN 1:",
        "  PATTERN 2:",
        "  PATTERN 3:",
        "  PATTERN 4:",
        "DISK 1 ONLY:"
    };

    // Render visible items
    for (int vi = 0; vi < VISIBLE_MENU_ITEMS && (vi + menu_scroll) < total_menu_items; vi++) {
        int i = vi + menu_scroll;
        uint16_t fg = 0xFFFF;  // White
        const char *label = "";
        char value[48] = "";
        int value_x = menu_x + 150;

        if (i < DISPLAY_OPTS_ITEMS) {
            // Display options
            label = display_labels[i];
            int is_pattern = (i >= 2 && i <= 5);
            int pattern_idx = i - 2;
            int pattern_disabled = is_pattern && (pattern_idx >= current_opts.pattern_count);

            if (pattern_disabled) {
                fg = 0x8410;  // Gray for disabled
            }

            switch (i) {
                case 0:
                    strcpy(value, current_opts.mode == DISPLAY_FILES_AND_DIRS ?
                                  "< FILES+DIRS >" : "< FILES ONLY >");
                    break;
                case 1:
                    snprintf(value, sizeof(value), "< %d >", current_opts.pattern_count);
                    break;
                case 2: case 3: case 4: case 5:
                    if (!pattern_disabled) {
                        if (current_opts.patterns[pattern_idx][0]) {
                            snprintf(value, sizeof(value), "< %s >", current_opts.patterns[pattern_idx]);
                        } else {
                            strcpy(value, "< *.* >");
                        }
                    } else {
                        strcpy(value, "---");
                    }
                    break;
                case 6:
                    strcpy(value, current_opts.disk1_only ? "< YES >" : "< NO >");
                    break;
            }
        } else if (i == DISPLAY_OPTS_ITEMS) {
            // Separator
            label = "--- CORE SETTINGS ---";
            fg = 0x07E0;  // Green
        } else if (core_settings_loaded) {
            // Core settings
            int core_idx = i - DISPLAY_OPTS_ITEMS - 1;
            const SettingsOption *opt = settings_get_option(core_idx);
            if (opt) {
                label = opt->name;
                snprintf(value, sizeof(value), "< %s >", opt->current_value);
            }
        }

        // Highlight selected (but not separator)
        if (i == menu_selected && i != DISPLAY_OPTS_ITEMS) {
            render_filled_rect(framebuffer, menu_x + 2, y - 1, menu_w - 4, item_h, 0x001F);
            fg = 0xFFE0;  // Yellow
        }

        // Draw label and value
        font_draw_text(framebuffer, 320, 240, menu_x + 4, y, label, fg);
        if (value[0]) {
            font_draw_text(framebuffer, 320, 240, value_x, y, value, fg);
        }

        y += item_h;
    }

    // Scroll indicators
    if (menu_scroll > 0) {
        font_draw_text(framebuffer, 320, 240, menu_x + menu_w - 20, menu_y + 20, "^", 0xFFE0);
    }
    if (menu_scroll + VISIBLE_MENU_ITEMS < total_menu_items) {
        font_draw_text(framebuffer, 320, 240, menu_x + menu_w - 20, menu_y + menu_h - 30, "v", 0xFFE0);
    }

    // Instructions
    font_draw_text(framebuffer, 320, 240, menu_x + 4, menu_y + menu_h - 14,
                   "A:SAVE  L/R:CHANGE  B:CLOSE", 0x8410);
}

// Filtering functions
int display_opts_should_show_dirs(void) {
    return (current_opts.mode == DISPLAY_FILES_AND_DIRS);
}

// Simple wildcard matching (* and ?)
static int wildcard_match(const char *pattern, const char *str) {
    while (*pattern && *str) {
        if (*pattern == '*') {
            pattern++;
            if (*pattern == '\0') return 1;
            while (*str) {
                if (wildcard_match(pattern, str)) return 1;
                str++;
            }
            return 0;
        } else if (*pattern == '?') {
            pattern++;
            str++;
        } else {
            if (tolower((unsigned char)*pattern) != tolower((unsigned char)*str)) {
                return 0;
            }
            pattern++;
            str++;
        }
    }

    while (*pattern == '*') pattern++;

    return (*pattern == '\0' && *str == '\0');
}

int display_opts_matches_pattern(const char *filename) {
    if (!filename) return 0;

    if (current_opts.pattern_count == 0) return 1;

    for (int i = 0; i < current_opts.pattern_count; i++) {
        const char *pattern = current_opts.patterns[i];
        if (pattern[0] == '\0') {
            return 1;
        }
        if (wildcard_match(pattern, filename)) {
            return 1;
        }
    }

    return 0;
}

// v28: Pre-computed lowercase disk indicators (avoid per-file conversion)
// v36: Added patterns without underscore (e.g., "2." matches "game2.zip")
// v60: Also filter out disk 0 (save disks)
static const char *disk_indicators_lower[] = {
    // v60: Disk 0 / save disk patterns
    "(disk 0)", "(disc 0)", "(save disk)", "(save disc)", "(savedisk)", "(savedisc)",
    "_d0", " d0", "_0.", "0.",
    // Disk 2+ patterns
    "(disk 2)", "(disk 3)", "(disk 4)", "(disk 5)", "(disk 6)", "(disk 7)", "(disk 8)", "(disk 9)",
    "(disc 2)", "(disc 3)", "(disc 4)", "(disc 5)", "(disc 6)", "(disc 7)", "(disc 8)", "(disc 9)",
    "(side b)", "(side c)", "(side d)",
    "(disk2)", "(disk3)", "(disk4)", "(disk5)", "(disk6)", "(disk7)", "(disk8)", "(disk9)",
    "(disc2)", "(disc3)", "(disc4)", "(disc5)", "(disc6)", "(disc7)", "(disc8)", "(disc9)",
    "disk 2 of", "disk 3 of", "disk 4 of", "disk 5 of", "disk 6 of", "disk 7 of", "disk 8 of", "disk 9 of",
    "disc 2 of", "disc 3 of", "disc 4 of", "disc 5 of", "disc 6 of", "disc 7 of", "disc 8 of", "disc 9 of",
    "_d2", "_d3", "_d4", "_d5", "_d6", "_d7", "_d8", "_d9",
    " d2", " d3", " d4", " d5", " d6", " d7", " d8", " d9",
    "_2.", "_3.", "_4.", "_5.", "_6.", "_7.", "_8.", "_9.",
    // v36: Also match files like "game2.zip" (number directly before extension)
    "2.", "3.", "4.", "5.", "6.", "7.", "8.", "9.",
    NULL
};

int display_opts_is_disk1(const char *filename) {
    if (!filename) return 0;
    if (!current_opts.disk1_only) return 1;

    // v28: Convert filename to lowercase once
    char lower[256];
    strncpy(lower, filename, sizeof(lower) - 1);
    lower[sizeof(lower) - 1] = '\0';
    for (int i = 0; lower[i]; i++) {
        lower[i] = tolower((unsigned char)lower[i]);
    }

    // v28: Use pre-computed lowercase indicators (no conversion in loop)
    for (int i = 0; disk_indicators_lower[i]; i++) {
        if (strstr(lower, disk_indicators_lower[i])) {
            return 0;
        }
    }

    return 1;
}
