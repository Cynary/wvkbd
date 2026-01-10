#include "proto/virtual-keyboard-unstable-v1-client-protocol.h"
#include <linux/input-event-codes.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ctype.h>
#include <wayland-client-protocol.h>
#include "keyboard.h"
#include "drw.h"
#include "os-compatibility.h"
#include "utf8.h"

#define MAX_LAYERS 25

/* lazy die macro */
#define die(...)                                                               \
    fprintf(stderr, __VA_ARGS__);                                              \
    exit(1)

#ifndef KEYMAP
#error "make sure to define KEYMAP"
#endif
#include KEYMAP

void
kbd_switch_layout(struct kbd *kb, struct layout *l, size_t layer_index)
{
    kb->prevlayout = kb->layout;
    if ((kb->layer_index != kb->last_abc_index) && (kb->layout->abc)) {
        kb->last_abc_layout = kb->layout;
        kb->last_abc_index = kb->layer_index;
    }
    kb->layer_index = layer_index;
    kb->layout = l;
    if (kb->debug)
        fprintf(stderr, "Switching to layout %s, layer_index %ld\n",
                kb->layout->name, layer_index);
    if (!l->keymap_name)
        fprintf(stderr, "Layout has no keymap!"); // sanity check
    if ((!kb->prevlayout) ||
        (strcmp(kb->prevlayout->keymap_name, kb->layout->keymap_name) != 0)) {
        fprintf(stderr, "Switching to keymap %s\n", kb->layout->keymap_name);
        create_and_upload_keymap(kb, kb->layout->keymap_name, 0, 0);
    }
    kbd_draw_layout(kb);
}

void
kbd_next_layer(struct kbd *kb, struct key *k, bool invert)
{
    size_t layer_index = kb->layer_index;
    if ((kb->mods & Ctrl) || (kb->mods & Alt) || (kb->mods & AltGr) ||
        ((bool)kb->compose)) {
        // with modifiers ctrl/alt/altgr: switch to the first layer
        layer_index = 0;
        kb->mods = 0;
    } else if ((kb->mods & Shift) || (kb->mods & CapsLock) || (invert)) {
        // with modifiers shift/capslock or invert set: switch to the previous
        // layout in the layer sequence
        if (layer_index > 0) {
            layer_index--;
        } else {
            size_t layercount = 0;
            for (size_t i = 0; layercount == 0; i++) {
                if (kb->landscape) {
                    if (kb->landscape_layers[i] == NumLayouts)
                        layercount = i;
                } else {
                    if (kb->layers[i] == NumLayouts)
                        layercount = i;
                }
            }
            layer_index = layercount - 1;
        }
        if (!invert)
            kb->mods ^= Shift;
    } else {
        // normal behaviour: switch to the next layout in the layer sequence
        layer_index++;
    }
    size_t layercount = 0;
    for (size_t i = 0; layercount == 0; i++) {
        if (kb->landscape) {
            if (kb->landscape_layers[i] == NumLayouts)
                layercount = i;
        } else {
            if (kb->layers[i] == NumLayouts)
                layercount = i;
        }
    }
    if (layer_index >= layercount) {
        if (kb->debug)
            fprintf(stderr, "wrapping layer_index back to start\n");
        layer_index = 0;
    }
    enum layout_id layer;
    if (kb->landscape) {
        layer = kb->landscape_layers[layer_index];
    } else {
        layer = kb->layers[layer_index];
    }
    if (((bool)kb->compose) && (k)) {
        kb->compose = 0;
        kbd_draw_key(kb, k, Unpress);
    }
    kbd_switch_layout(kb, &kb->layouts[layer], layer_index);
}

uint8_t
kbd_get_rows(struct layout *l)
{
    uint8_t rows = 0;
    struct key *k = l->keys;
    while (k->type != Last) {
        if (k->type == EndRow) {
            rows++;
        }
        k++;
    }
    return rows + 1;
}

enum layout_id *
kbd_init_layers(char *layer_names_list)
{
    enum layout_id *layers;
    uint8_t numlayers = 0;
    bool found;
    char *s;
    int i;

    layers = malloc(MAX_LAYERS * sizeof(enum layout_id));
    s = strtok(layer_names_list, ",");
    while (s != NULL) {
        if (numlayers + 1 == MAX_LAYERS) {
            fprintf(stderr, "too many layers specified");
            exit(3);
        }
        found = false;
        for (i = 0; i < NumLayouts - 1; i++) {
            if (layouts[i].name && strcmp(layouts[i].name, s) == 0) {
                fprintf(stderr, "layer #%d = %s\n", numlayers + 1, s);
                layers[numlayers++] = i;
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "No such layer: %s\n", s);
            exit(3);
        }
        s = strtok(NULL, ",");
    }
    layers[numlayers] = NumLayouts; // mark the end of the sequence
    if (numlayers == 0) {
        fprintf(stderr, "No layers defined\n");
        exit(3);
    }

    return layers;
}

void
kbd_init(struct kbd *kb, struct layout *layouts, char *layer_names_list,
         char *landscape_layer_names_list)
{
    int i;

    fprintf(stderr, "Initializing keyboard\n");

    kb->layouts = layouts;

    for (i = 0; i < NumLayouts - 1; i++)
        ;
    fprintf(stderr, "Found %d layouts\n", i);

    kb->layer_index = 0;
    kb->last_abc_index = 0;

    if (layer_names_list)
        kb->layers = kbd_init_layers(layer_names_list);
    if (landscape_layer_names_list)
        kb->landscape_layers = kbd_init_layers(landscape_layer_names_list);

    i = 0;
    enum layout_id lid = kb->layers[0];
    while (lid != NumLayouts) {
        lid = kb->layers[++i];
    }
    fprintf(stderr, "Found %d layers\n", i);

    enum layout_id layer;
    if (kb->landscape) {
        layer = kb->landscape_layers[kb->layer_index];
    } else {
        layer = kb->layers[kb->layer_index];
    }

    kb->layout = &kb->layouts[layer];
    kb->last_abc_layout = &kb->layouts[layer];

    if (kb->suggest_visible_count <= 0) {
        kb->suggest_visible_count = 3;
    }
    kb->suggestions_len = 0;
    kb->suggest_mode = WVKBD_SMODE_NONE;
    kb->suggest_scroll_x = 0;
    kb->suggest_content_width = 0;
    kb->suggest_cancel_visible = false;
    kb->suggest_cancel_x = kb->suggest_cancel_y = 0;
    kb->suggest_cancel_w = kb->suggest_cancel_h = 0;
    for (int si = 0; si < WVKBD_MAX_SUGGESTIONS; si++) {
        kb->suggest_pill_x[si] = 0;
        kb->suggest_pill_w[si] = 0;
    }

    kb->current_token[0] = '\0';
    kb->current_token_len = 0;
    kb->context_words_len = 0;
    kb->context_words_pos = 0;
    if (kb->context_words_max <= 0) {
        kb->context_words_max = 5;
    }

    kb->input_down = false;
    kb->input_mode = KBD_INPUT_NONE;
    kb->preview_key = NULL;

    kb->swipe_threshold_px = 18;
    kb->swipe_points_len = 0;
    kb->swipe_last_suggest_time = 0;
    kb->pending_swipe = false;
    kb->pending_swipe_word[0] = '\0';
    kb->dismissed_words_len = 0;

    kb->trail_enabled = true;
    kb->trail_fade_ms = 800;
    kb->trail_fade_distance_px = 0.0;
    kb->trail_width_px = 10;
    kb->trail_color = kb->schemes[0].swipe;
    kb->trail_now_ms = 0;
    kb->trail_last_input_ms = 0;
    kb->trail_last_mono_ms = 0;

    /* upload keymap */
    create_and_upload_keymap(kb, kb->layout->keymap_name, 0, 0);
}

void
kbd_init_layout(struct layout *l, uint32_t width, uint32_t height,
                uint32_t y_offset)
{
    uint32_t x = 0, y = y_offset;
    uint8_t rows = kbd_get_rows(l);

    l->keyheight = height / rows;

    struct key *k = l->keys;
    double rowlength = kbd_get_row_length(k);
    double rowwidth = 0.0;
    while (k->type != Last) {
        if (k->type == EndRow) {
            y += l->keyheight;
            x = 0;
            rowwidth = 0.0;
            rowlength = kbd_get_row_length(k + 1);
        } else if (k->width > 0) {
            k->x = x;
            k->y = y;
            k->w = ((double)width / rowlength) * k->width;
            x += k->w;
            rowwidth += k->width;
            if (x < (rowwidth / rowlength) * (double)width) {
                k->w++;
                x++;
            }
        }
        k->h = l->keyheight;
        k++;
    }
}

double
kbd_get_row_length(struct key *k)
{
    double l = 0.0;
    while ((k->type != Last) && (k->type != EndRow)) {
        l += k->width;
        k++;
    }
    return l;
}

struct key *
kbd_get_key(struct kbd *kb, uint32_t x, uint32_t y)
{
    struct layout *l = kb->layout;
    struct key *k = l->keys;
    if (kb->debug)
        fprintf(stderr, "get key: +%d+%d\n", x, y);
    while (k->type != Last) {
        if ((k->type != EndRow) && (k->type != Pad) && (k->type != Pad) &&
            (x >= k->x) && (y >= k->y) && (x < k->x + k->w) &&
            (y < k->y + k->h)) {
            return k;
        }
        k++;
    }
    return NULL;
}

size_t
kbd_get_layer_index(struct kbd *kb, struct layout *l)
{
    for (size_t i = 0; i < NumLayouts - 1; i++) {
        if (l == &kb->layouts[i]) {
            return i;
        }
    }
    return 0;
}

void
kbd_unpress_key(struct kbd *kb, uint32_t time)
{
    bool unlatch_shift, unlatch_ctrl, unlatch_alt, unlatch_super, unlatch_altgr;
    unlatch_shift = unlatch_ctrl = unlatch_alt = unlatch_super = unlatch_altgr = false;

    if (kb->last_press) {
        unlatch_shift = (kb->mods & Shift) == Shift;
        unlatch_ctrl = (kb->mods & Ctrl) == Ctrl;
        unlatch_alt = (kb->mods & Alt) == Alt;
        unlatch_super = (kb->mods & Super) == Super;
        unlatch_altgr = (kb->mods & AltGr) == AltGr;

        if (unlatch_shift) kb->mods ^= Shift;
        if (unlatch_ctrl) kb->mods ^= Ctrl;
        if (unlatch_alt) kb->mods ^= Alt;
        if (unlatch_super) kb->mods ^= Super;
        if (unlatch_altgr) kb->mods ^= AltGr;

        if (unlatch_shift||unlatch_ctrl||unlatch_alt||unlatch_super||unlatch_altgr) {
            zwp_virtual_keyboard_v1_modifiers(kb->vkbd, kb->mods, 0, 0, 0);
        }

        if (kb->last_press->type == Copy) {
            zwp_virtual_keyboard_v1_key(kb->vkbd, time, 127, // COMP key
                                        WL_KEYBOARD_KEY_STATE_RELEASED);
        } else {
            if ((kb->shift_space_is_tab) && (kb->last_press->code == KEY_SPACE) && (unlatch_shift)) {
                // shift + space is tab
                zwp_virtual_keyboard_v1_key(kb->vkbd, time, KEY_TAB,
                                            WL_KEYBOARD_KEY_STATE_RELEASED);
            } else {
                zwp_virtual_keyboard_v1_key(kb->vkbd, time,
                                            kb->last_press->code,
                                            WL_KEYBOARD_KEY_STATE_RELEASED);
            }
        }

        if (kb->compose >= 2) {
            kb->compose = 0;
            kbd_switch_layout(kb, kb->last_abc_layout, kb->last_abc_index);
        } else if (unlatch_shift||unlatch_ctrl||unlatch_alt||unlatch_super||unlatch_altgr) {
            kbd_draw_layout(kb);
        } else {
            kbd_draw_key(kb, kb->last_press, Unpress);
        }

        kb->last_press = NULL;
    }
}

void
kbd_release_key(struct kbd *kb, uint32_t time)
{
    kbd_unpress_key(kb, time);
    if (kb->print_intersect && kb->last_swipe) {
        printf("\n");
        // Important so autocompleted words get typed in time
        fflush(stdout);
        kbd_draw_layout(kb);
        kb->last_swipe = NULL;
    }

    kbd_clear_last_popup(kb);
}

void
kbd_motion_key(struct kbd *kb, uint32_t time, uint32_t x, uint32_t y)
{
    // Output intersecting keys
    // (for external 'swiping'-based accelerators).
    if (kb->print_intersect) {
        if (kb->last_press) {
            kbd_unpress_key(kb, time);
            // Redraw last press as a swipe.
            kbd_draw_key(kb, kb->last_swipe, Swipe);
        }
        struct key *intersect_key;
        intersect_key = kbd_get_key(kb, x, y);
        if (intersect_key && (!kb->last_swipe ||
                              intersect_key->label != kb->last_swipe->label)) {
            kbd_print_key_stdout(kb, intersect_key);
            kb->last_swipe = intersect_key;
            kbd_draw_key(kb, kb->last_swipe, Swipe);
        }
    } else {
        kbd_unpress_key(kb, time);
    }

    kbd_clear_last_popup(kb);
}

void
kbd_press_key(struct kbd *kb, struct key *k, uint32_t time)
{
    if ((kb->compose == 1) && (k->type != Compose) && (k->type != Mod)) {
        if ((k->type == NextLayer) || (k->type == BackLayer) ||
            ((k->type == Code) && (k->code == KEY_SPACE))) {
            kb->compose = 0;
            if (kb->debug)
                fprintf(stderr, "showing layout index\n");
            kbd_switch_layout(kb, &kb->layouts[Index], 0);
            return;
        } else if (k->layout) {
            kb->compose++;
            if (kb->debug)
                fprintf(stderr, "showing compose %d\n", kb->compose);
            kbd_switch_layout(kb, k->layout,
                              kbd_get_layer_index(kb, k->layout));
            return;
        } else {
            return;
        }
    }

    switch (k->type) {
    case Code:
        if (k->code_mod) {
            if (k->reset_mod) {
                zwp_virtual_keyboard_v1_modifiers(kb->vkbd, k->code_mod, 0, 0,
                                                  0);
            } else {
                zwp_virtual_keyboard_v1_modifiers(
                    kb->vkbd, kb->mods ^ k->code_mod, 0, 0, 0);
            }
        } else {
            zwp_virtual_keyboard_v1_modifiers(kb->vkbd, kb->mods, 0, 0, 0);
        }
        kb->last_swipe = kb->last_press = k;
        kbd_draw_key(kb, k, Press);
        if ((kb->shift_space_is_tab) && (k->code == KEY_SPACE) && (kb->mods & Shift)) {
            // shift space is tab
            zwp_virtual_keyboard_v1_modifiers(kb->vkbd, 0, 0, 0, 0);
            zwp_virtual_keyboard_v1_key(kb->vkbd, time, KEY_TAB,
                                        WL_KEYBOARD_KEY_STATE_PRESSED);
        } else {
            zwp_virtual_keyboard_v1_key(kb->vkbd, time, kb->last_press->code,
                                        WL_KEYBOARD_KEY_STATE_PRESSED);
        }
        if (kb->print || kb->print_intersect)
            kbd_print_key_stdout(kb, k);
        if (kb->compose) {
            if (kb->debug)
                fprintf(stderr, "pressing composed key\n");
            kb->compose++;
        }
        break;
    case Mod:
        kb->mods ^= k->code;
        if ((k->code == Shift) || (k->code == CapsLock)) {
            kbd_draw_layout(kb);
        } else {
            if (kb->mods & k->code) {
                kbd_draw_key(kb, k, Press);
            } else {
                kbd_draw_key(kb, k, Unpress);
            }
        }
        zwp_virtual_keyboard_v1_modifiers(kb->vkbd, kb->mods, 0, 0, 0);
        break;
    case Layout:
        // switch to the layout determined by the key
        kbd_switch_layout(kb, k->layout, kbd_get_layer_index(kb, k->layout));
        // reset previous layout to default/first so we don't get any weird
        // cycles
        kb->last_abc_index = 0;
        if (kb->landscape) {
            kb->last_abc_layout = &kb->layouts[kb->landscape_layers[0]];
        } else {
            kb->last_abc_layout = &kb->layouts[kb->layers[0]];
        }
        break;
    case Compose:
        // switch to the associated layout determined by the *next* keypress
        if (kb->compose == 0) {
            kb->compose = 1;
        } else {
            kb->compose = 0;
        }
        if ((bool)kb->compose) {
            kbd_draw_key(kb, k, Press);
        } else {
            kbd_draw_key(kb, k, Unpress);
        }
        break;
    case NextLayer: //(also handles previous layer when shift modifier is on, or
                    //"first layer" with other modifiers)
        kbd_next_layer(kb, k, false);
        break;
    case BackLayer: // triggered when "Abc" keys are pressed
        // switch to the last active alphabetical layout
        if (kb->last_abc_layout) {
            kb->compose = 0;
            kbd_switch_layout(kb, kb->last_abc_layout, kb->last_abc_index);
            // reset previous layout to default/first so we don't get any weird
            // cycles
            kb->last_abc_index = 0;
            if (kb->landscape) {
                kb->last_abc_layout = &kb->layouts[kb->landscape_layers[0]];
            } else {
                kb->last_abc_layout = &kb->layouts[kb->layers[0]];
            }
        }
        break;
    case Copy:
        // copy code as unicode chr by setting a temporary keymap
        kb->last_swipe = kb->last_press = k;
        kbd_draw_key(kb, k, Press);
        if (kb->debug)
            fprintf(stderr, "pressing copy key\n");
        create_and_upload_keymap(kb, kb->layout->keymap_name, k->code,
                                 k->code_mod);
        zwp_virtual_keyboard_v1_modifiers(kb->vkbd, kb->mods, 0, 0, 0);
        zwp_virtual_keyboard_v1_key(kb->vkbd, time, 127, // COMP key
                                    WL_KEYBOARD_KEY_STATE_PRESSED);
        if (kb->print || kb->print_intersect)
            kbd_print_key_stdout(kb, k);
        break;
    default:
        break;
    }
}

void
kbd_print_key_stdout(struct kbd *kb, struct key *k)
{
    /* printed keys may slightly differ from the actual output
     * we generally print what is on the key LABEL and only support the normal
     * and shift layers. Other modifiers produce no output (Ctrl,Alt)
     * */

    bool handled = true;
    if (k->type == Code) {
        switch (k->code) {
        case KEY_SPACE:
            printf(" ");
            break;
        case KEY_ENTER:
            printf("\n");
            break;
        case KEY_BACKSPACE:
            printf("\b");
            break;
        case KEY_TAB:
            printf("\t");
            break;
        default:
            handled = false;
            break;
        }
    } else if (k->type != Copy) {
        return;
    }

    if (!handled) {
        if ((kb->mods & Shift) || 
            ((kb->mods & CapsLock) & (strlen(k->label) == 1 && isalpha(k->label[0]))))
            printf("%s", k->shift_label);
        else if (!(kb->mods & Ctrl) && !(kb->mods & Alt) && !(kb->mods & Super))
            printf("%s", k->label);
    }
    fflush(stdout);
}

void
kbd_clear_last_popup(struct kbd *kb)
{
    if (kb->last_popup_w && kb->last_popup_h) {
        drw_do_clear(kb->popup_surf, kb->last_popup_x, kb->last_popup_y,
                     kb->last_popup_w, kb->last_popup_h);
        kb->last_popup_w = kb->last_popup_h = 0;
    }
}

void
kbd_draw_key(struct kbd *kb, struct key *k, enum key_draw_type type)
{
    const char *label = ((kb->mods & Shift)||((kb->mods & CapsLock) && 
        strlen(k->label) == 1 && isalpha(k->label[0]))) ? k->shift_label : k->label;
    if (kb->debug)
        fprintf(stderr, "Draw key +%d+%d %dx%d -> %s\n", k->x, k->y, k->w, k->h,
                label);
    struct clr_scheme *scheme = &kb->schemes[k->scheme];

    switch (type) {
    case None:
    case Unpress:
        draw_inset(kb->surf, k->x, k->y, k->w, k->h, KBD_KEY_BORDER,
                   scheme->fg, scheme->rounding);
        break;
    case Press:
        draw_inset(kb->surf, k->x, k->y, k->w, k->h, KBD_KEY_BORDER,
                   scheme->high, scheme->rounding);
        break;
    case Swipe:
        draw_over_inset(kb->surf, k->x, k->y, k->w, k->h, KBD_KEY_BORDER,
                        scheme->swipe, scheme->rounding);
        break;
    }

    drw_draw_text(kb->surf, scheme->text, k->x, k->y, k->w, k->h,
                  KBD_KEY_BORDER, label, scheme->font_description);
}

static const char *
kbd_suggestion_word(const struct wvkbd_suggestion *s)
{
    if (!s) {
        return "";
    }
    if (s->kind == WVKBD_SUGGEST_ADD_WORD) {
        return s->inline_word;
    }
    return s->word ? s->word : "";
}

static void
kbd_adjust_suggestion_case(struct kbd *kb, const char *word, uint8_t mods,
                           char out[WVKBD_MAX_TOKEN_BYTES]);

static void
kbd_draw_suggestions(struct kbd *kb)
{
    if (!kb->suggest_height) {
        return;
    }

    uint32_t bar_h = kb->suggest_height;
    struct clr_scheme *scheme = &kb->schemes[1];
    drw_fill_rectangle(kb->surf, scheme->bg, 0, 0, kb->w, bar_h, 0);

    const uint32_t pad_x = 8;
    const uint32_t pad_y = 6;
    const uint32_t gap_x = 8;
    const uint32_t trash_w = 26;

    uint32_t pill_h = bar_h - (pad_y * 2);

    bool has_word = false;
    for (int i = 0; i < kb->suggestions_len; i++) {
        const struct wvkbd_suggestion *s = &kb->suggestions[i];
        if (s->kind == WVKBD_SUGGEST_WORD && s->word && s->word[0]) {
            has_word = true;
            break;
        }
    }

    kb->suggest_cancel_visible =
        (kb->suggest_mode == WVKBD_SMODE_SWIPE) && has_word;
    uint32_t reserved_left = 0;
    if (kb->suggest_cancel_visible && pill_h > 0) {
        uint32_t cancel_w = pill_h;
        if (cancel_w < 32)
            cancel_w = 32;
        if (cancel_w > 52)
            cancel_w = 52;
        kb->suggest_cancel_x = pad_x;
        kb->suggest_cancel_y = pad_y;
        kb->suggest_cancel_w = cancel_w;
        kb->suggest_cancel_h = pill_h;
        draw_inset(kb->surf, kb->suggest_cancel_x, kb->suggest_cancel_y,
                   kb->suggest_cancel_w, kb->suggest_cancel_h, 1, scheme->fg,
                   scheme->rounding);
        drw_draw_text(kb->surf, scheme->text, kb->suggest_cancel_x,
                      kb->suggest_cancel_y, kb->suggest_cancel_w,
                      kb->suggest_cancel_h, 0, "⊗", scheme->font_description);
        reserved_left = pad_x + cancel_w + gap_x;
    } else {
        kb->suggest_cancel_x = kb->suggest_cancel_y = 0;
        kb->suggest_cancel_w = kb->suggest_cancel_h = 0;
        kb->suggest_cancel_visible = false;
    }

    for (int i = 0; i < kb->suggestions_len; i++) {
        kb->suggest_pill_x[i] = 0;
        kb->suggest_pill_w[i] = 0;
    }

    uint32_t widths[WVKBD_MAX_SUGGESTIONS];
    for (int i = 0; i < WVKBD_MAX_SUGGESTIONS; i++) {
        widths[i] = 0;
    }

    int pills = 0;
    double pills_w = 0.0;
    char disp[WVKBD_MAX_TOKEN_BYTES];
    for (int i = 0; i < kb->suggestions_len; i++) {
        const struct wvkbd_suggestion *s = &kb->suggestions[i];
        const char *word = kbd_suggestion_word(s);
        if (!word || !word[0]) {
            continue;
        }
        if (s->kind == WVKBD_SUGGEST_WORD) {
            kbd_adjust_suggestion_case(kb, word, kb->mods, disp);
            if (disp[0]) {
                word = disp;
            }
        }

        int text_w = 0, text_h = 0;
        drw_measure_text(kb->surf, word, scheme->font_description, &text_w,
                         &text_h);

        uint32_t afford_w = (s->kind == WVKBD_SUGGEST_WORD) ? trash_w : 0;
        uint32_t pill_w = (uint32_t)(text_w + 2 * pad_x + afford_w);
        if (pill_w < 90)
            pill_w = 90;
        if (pill_w > 260)
            pill_w = 260;
        widths[i] = pill_w;
        pills++;
        pills_w += (double)pill_w;
    }

    if (pills > 1) {
        pills_w += (double)gap_x * (double)(pills - 1);
    }

    double avail_w = (reserved_left < kb->w) ? (double)(kb->w - reserved_left)
                                             : 0.0;
    kb->suggest_content_width = pills_w + (double)(2 * pad_x);
    double max_scroll = kb->suggest_content_width - avail_w;
    if (max_scroll < 0.0) {
        max_scroll = 0.0;
    }
    if (kb->suggest_scroll_x < 0.0) {
        kb->suggest_scroll_x = 0.0;
    }
    if (kb->suggest_scroll_x > max_scroll) {
        kb->suggest_scroll_x = max_scroll;
    }

    double x = 0.0;
    if (max_scroll <= 0.0) {
        kb->suggest_scroll_x = 0.0;
        x = (double)reserved_left + (avail_w - pills_w) / 2.0;
    } else {
        x = (double)reserved_left + (double)pad_x - kb->suggest_scroll_x;
    }

    for (int i = 0; i < kb->suggestions_len; i++) {
        const struct wvkbd_suggestion *s = &kb->suggestions[i];
        const char *word = kbd_suggestion_word(s);
        if (!word || !word[0]) {
            continue;
        }
        if (s->kind == WVKBD_SUGGEST_WORD) {
            kbd_adjust_suggestion_case(kb, word, kb->mods, disp);
            if (disp[0]) {
                word = disp;
            }
        }

        uint32_t pill_w = widths[i];
        if (pill_w == 0) {
            continue;
        }

        uint32_t pill_x = (uint32_t)lround(x);
        uint32_t pill_y = pad_y;
        kb->suggest_pill_x[i] = pill_x;
        kb->suggest_pill_w[i] = pill_w;

        if ((double)pill_x + (double)pill_w < -64.0 ||
            pill_x > kb->w + 64) {
            x += pill_w + gap_x;
            continue;
        }

        draw_inset(kb->surf, pill_x, pill_y, pill_w, pill_h, 1, scheme->fg,
                   scheme->rounding);

        uint32_t afford_w = (s->kind == WVKBD_SUGGEST_WORD) ? trash_w : 0;
        uint32_t text_area_w = pill_w - afford_w;
        drw_draw_text(kb->surf, scheme->text, pill_x, pill_y, text_area_w,
                      pill_h, 4, word, scheme->font_description);

        if (s->kind == WVKBD_SUGGEST_WORD) {
            uint32_t trash_x = pill_x + pill_w - trash_w;
            drw_draw_text(kb->surf, scheme->text, trash_x, pill_y, trash_w,
                          pill_h, 2, "×", scheme->font_description);
        }

        x += pill_w + gap_x;
    }
}

static void
kbd_draw_trail(struct kbd *kb)
{
    if (!kb || !kb->trail_enabled || kb->swipe_points_len < 2 ||
        (kb->trail_fade_ms == 0 && kb->trail_fade_distance_px <= 0.0)) {
        return;
    }

    uint32_t now = kb->trail_now_ms;
    uint32_t last_t = kb->swipe_points[kb->swipe_points_len - 1].time_ms;
    if (now == 0) {
        now = last_t;
    }

    if (kb->trail_fade_ms > 0 && now > last_t &&
        (now - last_t) > kb->trail_fade_ms) {
        // If we're using time-based fading only, we can stop drawing once the
        // last point is fully expired.
        if (kb->trail_fade_distance_px <= 0.0) {
            kb->swipe_points_len = 0;
            return;
        }
    }

    double xs[WVKBD_MAX_SWIPE_POINTS];
    double ys[WVKBD_MAX_SWIPE_POINTS];
    uint8_t alphas[WVKBD_MAX_SWIPE_POINTS];
    double dist_to_end[WVKBD_MAX_SWIPE_POINTS];

    if (kb->trail_fade_distance_px > 0.0) {
        dist_to_end[kb->swipe_points_len - 1] = 0.0;
        for (int i = kb->swipe_points_len - 2; i >= 0; i--) {
            double dx = kb->swipe_points[i + 1].x - kb->swipe_points[i].x;
            double dy = kb->swipe_points[i + 1].y - kb->swipe_points[i].y;
            dist_to_end[i] = dist_to_end[i + 1] + hypot(dx, dy);
        }
    } else {
        for (int i = 0; i < kb->swipe_points_len; i++) {
            dist_to_end[i] = 0.0;
        }
    }

    for (int i = 0; i < kb->swipe_points_len; i++) {
        xs[i] = kb->swipe_points[i].x;
        ys[i] = kb->swipe_points[i].y;

        double t_time = 1.0;
        if (kb->trail_fade_ms > 0) {
            uint32_t dt = (now >= kb->swipe_points[i].time_ms)
                              ? (now - kb->swipe_points[i].time_ms)
                              : 0;
            t_time = 1.0 - ((double)dt / (double)kb->trail_fade_ms);
        }

        double t_dist = 1.0;
        if (kb->trail_fade_distance_px > 0.0) {
            t_dist = 1.0 - (dist_to_end[i] / kb->trail_fade_distance_px);
        }

        double t = t_time < t_dist ? t_time : t_dist;
        if (t < 0.0)
            t = 0.0;
        if (t > 1.0)
            t = 1.0;
        alphas[i] = (uint8_t)lrint(t * 255.0);
    }

    drw_over_polyline(kb->surf, kb->trail_color, kb->trail_width_px, xs, ys,
                      alphas, (size_t)kb->swipe_points_len);
}

void
kbd_draw_layout(struct kbd *kb)
{
    struct drwsurf *d = kb->surf;
    struct key *next_key = kb->layout->keys;
    if (kb->debug)
        fprintf(stderr, "Draw layout\n");

    drw_fill_rectangle(d, kb->schemes[0].bg, 0, 0, kb->w, kb->h, 0);
    kbd_draw_suggestions(kb);

    while (next_key->type != Last) {
        if ((next_key->type == Pad) || (next_key->type == EndRow)) {
            next_key++;
            continue;
        }
        if ((next_key->type == Mod && kb->mods & next_key->code) ||
            (next_key->type == Compose && kb->compose)) {
            kbd_draw_key(kb, next_key, Press);
        } else {
            kbd_draw_key(kb, next_key, None);
        }
        next_key++;
    }

    kbd_draw_trail(kb);
}

void
kbd_resize(struct kbd *kb, struct layout *layouts, uint8_t layoutcount)
{
    fprintf(stderr, "Resize %dx%d %f, %d layouts\n", kb->w, kb->h, kb->scale,
            layoutcount);

    drwsurf_resize(kb->surf, kb->w, kb->h, kb->scale);
    drwsurf_resize(kb->popup_surf, kb->w, kb->h * 2, kb->scale);
    for (int i = 0; i < layoutcount; i++) {
        if (kb->debug) {
            if (layouts[i].name)
                fprintf(stderr, "Initialising layout %s, keymap %s\n",
                        layouts[i].name, layouts[i].keymap_name);
            else
                fprintf(stderr, "Initialising unnamed layout %d, keymap %s\n",
                        i, layouts[i].keymap_name);
        }
        uint32_t y_offset = kb->suggest_height;
        uint32_t key_h = kb->h;
        if (key_h > y_offset) {
            key_h -= y_offset;
        } else {
            key_h = 0;
        }
        kbd_init_layout(&layouts[i], kb->w, key_h, y_offset);
    }
    kbd_draw_layout(kb);
}

void
kbd_set_suggest_height(struct kbd *kb, uint32_t suggest_height)
{
    if (!kb) {
        return;
    }
    kb->suggest_height = suggest_height;
}

void
kbd_set_predictor(struct kbd *kb, struct wvkbd_predictor *predictor)
{
    if (!kb) {
        return;
    }
    kb->predictor = predictor;
}

static const char *
kbd_last_context_word(struct kbd *kb)
{
    if (!kb || kb->context_words_len <= 0) {
        return NULL;
    }
    int idx = kb->context_words_pos - 1;
    if (idx < 0) {
        idx = kb->context_words_max - 1;
    }
    return kb->context_words[idx];
}

static void
kbd_context_push_word(struct kbd *kb, const char *word)
{
    if (!kb || !word || !word[0] || kb->context_words_max <= 0) {
        return;
    }
    char tmp[WVKBD_MAX_TOKEN_BYTES] = {0};
    strncpy(tmp, word, sizeof(tmp) - 1);
    ascii_lower_inplace(tmp);

    char *dup = strdup(tmp);
    if (!dup) {
        return;
    }

    int idx = kb->context_words_pos % WVKBD_MAX_CONTEXT_WORDS;
    if (kb->context_words[idx]) {
        free(kb->context_words[idx]);
        kb->context_words[idx] = NULL;
    }
    kb->context_words[idx] = dup;
    kb->context_words_pos = (kb->context_words_pos + 1) % kb->context_words_max;
    if (kb->context_words_len < kb->context_words_max) {
        kb->context_words_len++;
    }
}

static bool
kbd_is_separator_label(const char *label)
{
    if (!label || !label[0]) {
        return false;
    }
    if (strcmp(label, " ") == 0) {
        return true;
    }
    if (strcmp(label, "\n") == 0) {
        return true;
    }
    if (label[1] == '\0') {
        unsigned char c = (unsigned char)label[0];
        if (isalnum(c)) {
            return false;
        }
        // Commonly considered part of words in practice.
        if (c == '\'' || c == '_') {
            return false;
        }
        return true;
    }
    return false;
}

static bool
kbd_is_token_char_label(const char *label)
{
    if (!label || !label[0]) {
        return false;
    }
    if (label[1] != '\0') {
        return false;
    }
    unsigned char c = (unsigned char)label[0];
    if (isalnum(c)) {
        return true;
    }
    if (c == '\'' || c == '_') {
        return true;
    }
    return false;
}

static void
kbd_commit_token_if_needed(struct kbd *kb)
{
    if (!kb || kb->current_token_len <= 0) {
        return;
    }
    kbd_context_push_word(kb, kb->current_token);
    kb->current_token[0] = '\0';
    kb->current_token_len = 0;
}

static void
kbd_suggestions_from_candidates(struct kbd *kb, struct wvkbd_candidate *cands,
                                int cands_len)
{
    kb->suggestions_len = 0;
    kb->suggest_scroll_x = 0;

    char lower[WVKBD_MAX_TOKEN_BYTES];
    for (int i = 0; i < cands_len && kb->suggestions_len < WVKBD_MAX_SUGGESTIONS;
         i++) {
        if (cands[i].word && cands[i].word[0] && kb->dismissed_words_len > 0) {
            strncpy(lower, cands[i].word, sizeof(lower) - 1);
            lower[sizeof(lower) - 1] = '\0';
            ascii_lower_inplace(lower);
            bool dismissed = false;
            for (int di = 0; di < kb->dismissed_words_len; di++) {
                if (strcmp(kb->dismissed_words[di], lower) == 0) {
                    dismissed = true;
                    break;
                }
            }
            if (dismissed) {
                continue;
            }
        }
        struct wvkbd_suggestion *s = &kb->suggestions[kb->suggestions_len++];
        s->kind = WVKBD_SUGGEST_WORD;
        s->word = cands[i].word;
        s->inline_word[0] = '\0';
        s->score = cands[i].score;
    }

    bool can_add = (kb->current_token_len > 0) &&
                   (kb->suggestions_len < WVKBD_MAX_SUGGESTIONS);
    if (can_add && kb->predictor &&
        wvkbd_predictor_user_has_word(kb->predictor, kb->current_token)) {
        can_add = false;
    }
    if (can_add) {
        char tok[WVKBD_MAX_TOKEN_BYTES] = {0};
        strncpy(tok, kb->current_token, sizeof(tok) - 1);
        ascii_lower_inplace(tok);
        for (int i = 0; i < kb->suggestions_len; i++) {
            const struct wvkbd_suggestion *s = &kb->suggestions[i];
            if (s->kind != WVKBD_SUGGEST_WORD || !s->word) {
                continue;
            }
            char w[WVKBD_MAX_TOKEN_BYTES] = {0};
            strncpy(w, s->word, sizeof(w) - 1);
            ascii_lower_inplace(w);
            if (strcmp(w, tok) == 0) {
                can_add = false;
                break;
            }
        }
    }

    if (can_add) {
        struct wvkbd_suggestion *s = &kb->suggestions[kb->suggestions_len++];
        s->kind = WVKBD_SUGGEST_ADD_WORD;
        s->word = NULL;
        strcpy(s->inline_word, "+ ");
        strncat(s->inline_word, kb->current_token,
                sizeof(s->inline_word) - strlen(s->inline_word) - 1);
        s->inline_word[sizeof(s->inline_word) - 1] = '\0';
        s->score = 0;
    }
}

static void
kbd_update_suggestions_prefix(struct kbd *kb)
{
    if (!kb || !kb->predictor) {
        kb->suggestions_len = 0;
        kb->suggest_mode = WVKBD_SMODE_NONE;
        return;
    }
    struct wvkbd_candidate cands[WVKBD_PREDICT_MAX_OUT] = {0};
    int n = wvkbd_predict_prefix(kb->predictor, kb->current_token, cands,
                                kb->suggest_visible_count);
    kbd_suggestions_from_candidates(kb, cands, n);
    kb->suggest_mode = WVKBD_SMODE_PREFIX;
    kbd_draw_layout(kb);
}

static void
kbd_update_suggestions_next_word(struct kbd *kb)
{
    if (!kb || !kb->predictor) {
        kb->suggestions_len = 0;
        kb->suggest_mode = WVKBD_SMODE_NONE;
        return;
    }
    const char *lw = kbd_last_context_word(kb);
    struct wvkbd_candidate cands[WVKBD_PREDICT_MAX_OUT] = {0};
    int n = wvkbd_predict_next_word(kb->predictor, lw, cands,
                                   kb->suggest_visible_count);
    kbd_suggestions_from_candidates(kb, cands, n);
    kb->suggest_mode = WVKBD_SMODE_NEXT_WORD;
    kbd_draw_layout(kb);
}

static void
kbd_build_key_pos_map(struct kbd *kb, struct wvkbd_key_pos_map *pos)
{
    memset(pos, 0, sizeof(*pos));
    if (!kb || !kb->last_abc_layout) {
        return;
    }
    struct key *k = kb->last_abc_layout->keys;
    while (k->type != Last) {
        if (k->type == Code && k->label && k->label[0] && k->label[1] == '\0') {
            unsigned char c = (unsigned char)tolower((unsigned char)k->label[0]);
            pos->has[c] = true;
            pos->x[c] = k->x + (k->w / 2.0);
            pos->y[c] = k->y + (k->h / 2.0);
        }
        k++;
    }
}

static void
kbd_set_pending_swipe_from_suggestions(struct kbd *kb);

static void
kbd_update_suggestions_swipe(struct kbd *kb)
{
    if (!kb || !kb->predictor || kb->swipe_points_len < 2) {
        return;
    }
    struct wvkbd_key_pos_map pos;
    kbd_build_key_pos_map(kb, &pos);

    struct wvkbd_candidate cands[WVKBD_PREDICT_MAX_OUT] = {0};
    const char *lw = kbd_last_context_word(kb);
    int n = wvkbd_predict_swipe(kb->predictor, &pos, kb->swipe_points,
                               kb->swipe_points_len, kb->current_token, lw,
                               cands, kb->suggest_visible_count);
    kbd_suggestions_from_candidates(kb, cands, n);
    kb->suggest_mode = WVKBD_SMODE_SWIPE;
    kbd_set_pending_swipe_from_suggestions(kb);
    kbd_draw_layout(kb);
}

static void
kbd_dismiss_word(struct kbd *kb, const char *word)
{
    if (!kb || !word || !word[0]) {
        return;
    }
    char w[WVKBD_MAX_TOKEN_BYTES];
    strncpy(w, word, sizeof(w) - 1);
    w[sizeof(w) - 1] = '\0';
    ascii_lower_inplace(w);

    for (int i = 0; i < kb->dismissed_words_len; i++) {
        if (strcmp(kb->dismissed_words[i], w) == 0) {
            return;
        }
    }
    if (kb->dismissed_words_len >= WVKBD_MAX_DISMISSED_WORDS) {
        memmove(kb->dismissed_words[0], kb->dismissed_words[1],
                sizeof(kb->dismissed_words[0]) *
                    (size_t)(WVKBD_MAX_DISMISSED_WORDS - 1));
        kb->dismissed_words_len = WVKBD_MAX_DISMISSED_WORDS - 1;
    }
    strncpy(kb->dismissed_words[kb->dismissed_words_len], w,
            sizeof(kb->dismissed_words[0]) - 1);
    kb->dismissed_words[kb->dismissed_words_len]
                      [sizeof(kb->dismissed_words[0]) - 1] = '\0';
    kb->dismissed_words_len++;
}

static void
kbd_refresh_suggestions(struct kbd *kb)
{
    if (!kb) {
        return;
    }
    switch (kb->suggest_mode) {
    case WVKBD_SMODE_SWIPE:
        kbd_update_suggestions_swipe(kb);
        break;
    case WVKBD_SMODE_NEXT_WORD:
        kbd_update_suggestions_next_word(kb);
        break;
    case WVKBD_SMODE_PREFIX:
    default:
        kbd_update_suggestions_prefix(kb);
        break;
    }
}

static void
kbd_cancel_swipe(struct kbd *kb)
{
    if (!kb) {
        return;
    }
    kb->pending_swipe = false;
    kb->pending_swipe_word[0] = '\0';
    kb->swipe_points_len = 0;

    if (kb->current_token_len > 0) {
        kbd_update_suggestions_prefix(kb);
    } else {
        kbd_update_suggestions_next_word(kb);
    }
}

static const char *
kbd_top_word_suggestion(struct kbd *kb)
{
    if (!kb) {
        return NULL;
    }
    for (int i = 0; i < kb->suggestions_len; i++) {
        const struct wvkbd_suggestion *s = &kb->suggestions[i];
        if (s->kind == WVKBD_SUGGEST_WORD && s->word && s->word[0]) {
            return s->word;
        }
    }
    return NULL;
}

static void
kbd_preview_set_key(struct kbd *kb, struct key *k)
{
    if (kb->preview_key == k) {
        return;
    }
    kb->preview_key = k;
    kbd_draw_layout(kb);
    if (kb->preview_key) {
        kbd_draw_key(kb, kb->preview_key, Press);
    }
}

static bool
kbd_hit_test_suggestion(struct kbd *kb, uint32_t x, uint32_t y, int *out_index,
                        bool *out_trash, bool *out_cancel)
{
    if (out_cancel) {
        *out_cancel = false;
    }

    if (!kb || !kb->suggest_height || y >= kb->suggest_height) {
        return false;
    }

    if (kb->suggest_cancel_visible && kb->suggest_cancel_w > 0 &&
        kb->suggest_cancel_h > 0) {
        if (x >= kb->suggest_cancel_x &&
            x <= kb->suggest_cancel_x + kb->suggest_cancel_w &&
            y >= kb->suggest_cancel_y &&
            y <= kb->suggest_cancel_y + kb->suggest_cancel_h) {
            if (out_cancel) {
                *out_cancel = true;
            }
            return true;
        }
    }

    if (kb->suggestions_len <= 0) {
        return false;
    }
    const uint32_t trash_w = 26;

    for (int i = 0; i < kb->suggestions_len; i++) {
        const struct wvkbd_suggestion *s = &kb->suggestions[i];
        const char *word = kbd_suggestion_word(s);
        if (!word || !word[0]) {
            continue;
        }
        uint32_t pill_x = kb->suggest_pill_x[i];
        uint32_t pill_w = kb->suggest_pill_w[i];
        if (pill_w == 0) {
            continue;
        }
        if (x >= pill_x && x <= pill_x + pill_w) {
            if (out_index) {
                *out_index = i;
            }
            if (out_trash) {
                *out_trash = (s->kind == WVKBD_SUGGEST_WORD) &&
                             (x >= pill_x + pill_w - trash_w);
            }
            return true;
        }
    }
    return false;
}

static void
kbd_type_codepoint(struct kbd *kb, uint32_t time_ms, uint32_t cp)
{
    create_and_upload_keymap(kb, kb->layout->keymap_name, cp, cp);
    zwp_virtual_keyboard_v1_modifiers(kb->vkbd, 0, 0, 0, 0);
    zwp_virtual_keyboard_v1_key(kb->vkbd, time_ms, 127,
                                WL_KEYBOARD_KEY_STATE_PRESSED);
    zwp_virtual_keyboard_v1_key(kb->vkbd, time_ms, 127,
                                WL_KEYBOARD_KEY_STATE_RELEASED);
}

struct wvkbd_char_key {
    bool has;
    uint32_t code;      // evdev keycode (linux/input-event-codes.h)
    uint8_t mods;       // Shift/CapsLock/etc (we only use Shift here)
};

static void
kbd_build_char_keymap(struct kbd *kb, struct wvkbd_char_key out[128])
{
    for (int i = 0; i < 128; i++) {
        out[i].has = false;
        out[i].code = 0;
        out[i].mods = 0;
    }
    if (!kb || !kb->last_abc_layout) {
        return;
    }
    struct key *k = kb->last_abc_layout->keys;
    while (k->type != Last) {
        if (k->type == Code && k->label && k->label[0] && k->label[1] == '\0') {
            unsigned char c = (unsigned char)k->label[0];
            if (c < 128 && !out[c].has) {
                out[c].has = true;
                out[c].code = k->code;
                out[c].mods = 0;
            }
        }
        if (k->type == Code && k->shift_label && k->shift_label[0] &&
            k->shift_label[1] == '\0') {
            unsigned char c = (unsigned char)k->shift_label[0];
            if (c < 128 && !out[c].has) {
                out[c].has = true;
                out[c].code = k->code;
                out[c].mods = Shift;
            }
        }
        k++;
    }
}

static bool
kbd_type_text_mapped(struct kbd *kb, uint32_t time_ms, const char *text)
{
    if (!kb || !kb->vkbd || !text) {
        return false;
    }

    struct wvkbd_char_key map[128];
    kbd_build_char_keymap(kb, map);

    uint32_t t = time_ms;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        unsigned char c = *p;
        if (c >= 128 || !map[c].has) {
            return false;
        }
        zwp_virtual_keyboard_v1_modifiers(kb->vkbd, map[c].mods, 0, 0, 0);
        zwp_virtual_keyboard_v1_key(kb->vkbd, t, map[c].code,
                                    WL_KEYBOARD_KEY_STATE_PRESSED);
        zwp_virtual_keyboard_v1_key(kb->vkbd, t, map[c].code,
                                    WL_KEYBOARD_KEY_STATE_RELEASED);
        t++;
    }

    // Restore OSK modifier state.
    zwp_virtual_keyboard_v1_modifiers(kb->vkbd, kb->mods, 0, 0, 0);
    return true;
}

static void
kbd_type_text_utf8(struct kbd *kb, uint32_t time_ms, const char *text)
{
    if (!kb || !kb->vkbd || !text) {
        return;
    }
    uint32_t t = time_ms;
    const unsigned char *s = (const unsigned char *)text;
    while (*s) {
        uint32_t cp = 0;
        if (*s < 0x80) {
            cp = *s;
            s++;
        } else if ((*s & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
            cp = ((*s & 0x1F) << 6) | (s[1] & 0x3F);
            s += 2;
        } else if ((*s & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 &&
                   (s[2] & 0xC0) == 0x80) {
            cp = ((*s & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
            s += 3;
        } else if ((*s & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 &&
                   (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
            cp = ((*s & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
                 ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
            s += 4;
        } else {
            cp = *s;
            s++;
        }
        kbd_type_codepoint(kb, t, cp);
        t++;
    }
    create_and_upload_keymap(kb, kb->layout->keymap_name, 0, 0);
}

static void
kbd_adjust_suggestion_case(struct kbd *kb, const char *word, uint8_t mods,
                           char out[WVKBD_MAX_TOKEN_BYTES])
{
    if (!out) {
        return;
    }
    out[0] = '\0';
    if (!kb || !word || !word[0]) {
        return;
    }

    strncpy(out, word, WVKBD_MAX_TOKEN_BYTES - 1);
    out[WVKBD_MAX_TOKEN_BYTES - 1] = '\0';
    ascii_lower_inplace(out);

    // Preserve simple capitalization intent from the already-typed prefix.
    // If there is no prefix yet, use Shift/CapsLock as the intent.
    bool first_upper = false;
    bool saw_alpha = false;
    bool all_upper = true;
    for (int i = 0; kb->current_token[i]; i++) {
        unsigned char c = (unsigned char)kb->current_token[i];
        if (isalpha(c)) {
            saw_alpha = true;
            if (i == 0 && isupper(c)) {
                first_upper = true;
            }
            if (!isupper(c)) {
                all_upper = false;
            }
        }
    }
    if (!saw_alpha) {
        all_upper = false;
    }

    if (kb->current_token_len <= 0) {
        if (mods & CapsLock) {
            all_upper = true;
            first_upper = false;
        } else if (mods & Shift) {
            first_upper = true;
        }
    }

    if (all_upper) {
        for (size_t i = 0; out[i]; i++) {
            unsigned char c = (unsigned char)out[i];
            if (c < 0x80) {
                out[i] = (char)toupper(c);
            }
        }
    } else if (first_upper && out[0]) {
        out[0] = (char)toupper((unsigned char)out[0]);
    }
}

static void
kbd_commit_suggestion(struct kbd *kb, uint32_t time_ms, const char *word)
{
    if (!kb || !word || !word[0]) {
        return;
    }

    // `word` may alias `kb->pending_swipe_word` (auto-commit on separator),
    // so copy it before mutating swipe state.
    char word_in[WVKBD_MAX_TOKEN_BYTES] = {0};
    strncpy(word_in, word, sizeof(word_in) - 1);
    word_in[sizeof(word_in) - 1] = '\0';

    kb->pending_swipe = false;
    kb->pending_swipe_word[0] = '\0';

    char token_l[WVKBD_MAX_TOKEN_BYTES] = {0};
    strncpy(token_l, kb->current_token, sizeof(token_l) - 1);
    ascii_lower_inplace(token_l);

    char word_l[WVKBD_MAX_TOKEN_BYTES] = {0};
    strncpy(word_l, word_in, sizeof(word_l) - 1);
    ascii_lower_inplace(word_l);

    if (kb->current_token_len > 0 && !utf8_startswith(word_l, token_l)) {
        return;
    }

    char adjusted[WVKBD_MAX_TOKEN_BYTES] = {0};
    kbd_adjust_suggestion_case(kb, word_in, kb->mods, adjusted);

    if (kb->current_token_len > 0) {
        const char *suffix = adjusted + strlen(kb->current_token);
        if (suffix[0]) {
            if (!kbd_type_text_mapped(kb, time_ms, suffix)) {
                kbd_type_text_utf8(kb, time_ms, suffix);
            }
        }
    } else {
        if (!kbd_type_text_mapped(kb, time_ms, adjusted)) {
            kbd_type_text_utf8(kb, time_ms, adjusted);
        }
    }

    strncpy(kb->current_token, adjusted, sizeof(kb->current_token) - 1);
    kb->current_token[sizeof(kb->current_token) - 1] = '\0';
    kb->current_token_len = (int)strlen(kb->current_token);
    kbd_update_suggestions_prefix(kb);
}

static void
kbd_handle_committed_key(struct kbd *kb, struct key *k, uint8_t mods_before)
{
    if (!kb || !k) {
        return;
    }

    if (k->type == Code) {
        if (k->code == KEY_BACKSPACE) {
            kb->pending_swipe = false;
            kb->pending_swipe_word[0] = '\0';
            utf8_pop_last(kb->current_token);
            kb->current_token_len = (int)strlen(kb->current_token);
            kbd_update_suggestions_prefix(kb);
            return;
        }
        if (k->code == KEY_SPACE || k->code == KEY_ENTER) {
            kb->pending_swipe = false;
            kb->pending_swipe_word[0] = '\0';
            kbd_commit_token_if_needed(kb);
            kbd_update_suggestions_next_word(kb);
            return;
        }

        const char *label = k->label;
        bool shift = (mods_before & Shift) ||
                     ((mods_before & CapsLock) && label && label[1] == '\0' &&
                      isalpha((unsigned char)label[0]));
        if (shift) {
            label = k->shift_label;
        }
        if (label && kbd_is_separator_label(label)) {
            kb->pending_swipe = false;
            kb->pending_swipe_word[0] = '\0';
            kbd_commit_token_if_needed(kb);
            kbd_update_suggestions_next_word(kb);
            return;
        }
        if (label && kbd_is_token_char_label(label)) {
            kb->pending_swipe = false;
            kb->pending_swipe_word[0] = '\0';
            size_t len = strlen(label);
            if ((size_t)kb->current_token_len + len + 1 <
                sizeof(kb->current_token)) {
                strcat(kb->current_token, label);
                kb->current_token_len = (int)strlen(kb->current_token);
                kbd_update_suggestions_prefix(kb);
            }
        }
    } else if (k->type == Copy) {
        const char *label = k->label;
        if (label && kbd_is_separator_label(label)) {
            kb->pending_swipe = false;
            kb->pending_swipe_word[0] = '\0';
            kbd_commit_token_if_needed(kb);
            kbd_update_suggestions_next_word(kb);
            return;
        }
        if (label && kbd_is_token_char_label(label)) {
            kb->pending_swipe = false;
            kb->pending_swipe_word[0] = '\0';
            size_t len = strlen(label);
            if ((size_t)kb->current_token_len + len + 1 <
                sizeof(kb->current_token)) {
                strcat(kb->current_token, label);
                kb->current_token_len = (int)strlen(kb->current_token);
                kbd_update_suggestions_prefix(kb);
            }
        }
    }
}

static bool
kbd_key_is_separator(struct kbd *kb, struct key *k, uint8_t mods_before)
{
    if (!kb || !k) {
        return false;
    }
    if (k->type == Code) {
        if (k->code == KEY_SPACE || k->code == KEY_ENTER) {
            return true;
        }
        const char *label = k->label;
        bool shift = (mods_before & Shift) ||
                     ((mods_before & CapsLock) && label && label[1] == '\0' &&
                      isalpha((unsigned char)label[0]));
        if (shift) {
            label = k->shift_label;
        }
        return label && kbd_is_separator_label(label);
    }
    if (k->type == Copy) {
        const char *label = k->label;
        return label && kbd_is_separator_label(label);
    }
    return false;
}

static void
kbd_set_pending_swipe_from_suggestions(struct kbd *kb)
{
    if (!kb) {
        return;
    }
    kb->pending_swipe = false;
    kb->pending_swipe_word[0] = '\0';
    for (int i = 0; i < kb->suggestions_len; i++) {
        const struct wvkbd_suggestion *s = &kb->suggestions[i];
        if (s->kind != WVKBD_SUGGEST_WORD) {
            continue;
        }
        const char *w = s->word;
        if (!w || !w[0]) {
            continue;
        }
        strncpy(kb->pending_swipe_word, w, sizeof(kb->pending_swipe_word) - 1);
        kb->pending_swipe_word[sizeof(kb->pending_swipe_word) - 1] = '\0';
        kb->pending_swipe = true;
        break;
    }
}

void
kbd_input_down(struct kbd *kb, uint32_t time_ms, uint32_t x, uint32_t y)
{
    if (!kb) {
        return;
    }
    kb->input_down = true;
    kb->input_mode = (y < kb->suggest_height) ? KBD_INPUT_SUGGEST_SCROLL
                                              : KBD_INPUT_TAP;
    kb->input_down_time = time_ms;
    kb->input_down_x = kb->input_last_x = (int)x;
    kb->input_down_y = kb->input_last_y = (int)y;
    kb->input_moved = false;

    kb->suggest_drag_start_x = (double)x;
    kb->suggest_drag_start_scroll_x = kb->suggest_scroll_x;

    kb->swipe_points_len = 0;
    kb->swipe_last_suggest_time = 0;

    if (kb->input_mode == KBD_INPUT_TAP) {
        struct key *k = kbd_get_key(kb, x, y);
        kbd_preview_set_key(kb, k);
    }
}

void
kbd_input_motion(struct kbd *kb, uint32_t time_ms, uint32_t x, uint32_t y)
{
    if (!kb || !kb->input_down) {
        return;
    }

    int dx = (int)x - kb->input_down_x;
    int dy = (int)y - kb->input_down_y;
    if ((dx * dx + dy * dy) > (int)(kb->swipe_threshold_px * kb->swipe_threshold_px)) {
        kb->input_moved = true;
    }

    kb->input_last_x = (int)x;
    kb->input_last_y = (int)y;

    if (kb->input_mode == KBD_INPUT_SUGGEST_SCROLL) {
        double delta = kb->suggest_drag_start_x - (double)x;
        kb->suggest_scroll_x = kb->suggest_drag_start_scroll_x + delta;
        kbd_draw_layout(kb);
        return;
    }

    if (kb->input_mode == KBD_INPUT_TAP) {
        if (kb->predictor && kb->input_moved && y >= kb->suggest_height) {
            kb->input_mode = KBD_INPUT_SWIPE;
            kb->preview_key = NULL;
            kbd_draw_layout(kb);
            kb->swipe_points[0] = (struct wvkbd_point){.x = kb->input_down_x,
                                                      .y = kb->input_down_y,
                                                      .time_ms = time_ms};
            kb->swipe_points[1] = (struct wvkbd_point){
                .x = x, .y = y, .time_ms = time_ms};
            kb->swipe_points_len = 2;
            kbd_update_suggestions_swipe(kb);
            return;
        }

        struct key *k = kbd_get_key(kb, x, y);
        kbd_preview_set_key(kb, k);
        return;
    }

    if (kb->input_mode == KBD_INPUT_SWIPE) {
        if (kb->swipe_points_len < WVKBD_MAX_SWIPE_POINTS) {
            kb->swipe_points[kb->swipe_points_len++] =
                (struct wvkbd_point){.x = x, .y = y, .time_ms = time_ms};
        }
        if ((time_ms - kb->swipe_last_suggest_time) > 40) {
            kb->swipe_last_suggest_time = time_ms;
            kbd_update_suggestions_swipe(kb);
        }
        return;
    }
}

void
kbd_input_up(struct kbd *kb, uint32_t time_ms, uint32_t x, uint32_t y)
{
    if (!kb) {
        return;
    }
    kb->input_down = false;

    if (kb->input_mode == KBD_INPUT_SUGGEST_SCROLL) {
        if (!kb->input_moved) {
            int idx = -1;
            bool trash = false;
            bool cancel = false;
            if (kbd_hit_test_suggestion(kb, x, y, &idx, &trash, &cancel)) {
                if (cancel) {
                    kbd_cancel_swipe(kb);
                } else if (idx >= 0 && idx < kb->suggestions_len) {
                    struct wvkbd_suggestion *s = &kb->suggestions[idx];
                    const char *word = kbd_suggestion_word(s);
                    if (trash && s->kind == WVKBD_SUGGEST_WORD) {
                        if (kb->predictor) {
                            if (!wvkbd_predictor_remove_user_word(kb->predictor,
                                                                 word)) {
                                kbd_dismiss_word(kb, word);
                            }
                        } else {
                            kbd_dismiss_word(kb, word);
                        }
                        kbd_refresh_suggestions(kb);
                    } else if (s->kind == WVKBD_SUGGEST_ADD_WORD) {
                        if (kb->predictor) {
                            wvkbd_predictor_add_user_word(kb->predictor,
                                                          kb->current_token);
                        }
                        kbd_update_suggestions_prefix(kb);
                    } else {
                        kbd_commit_suggestion(kb, time_ms, word);
                    }
                }
            }
        }
        kb->input_mode = KBD_INPUT_NONE;
        return;
    }

    if (kb->input_mode == KBD_INPUT_TAP) {
        struct key *k = kbd_get_key(kb, x, y);
        if (!k) {
            k = kb->preview_key;
        }
        kb->preview_key = NULL;

        if (k) {
            uint8_t mods_before = kb->mods;
            bool is_sep = kbd_key_is_separator(kb, k, mods_before);
            bool did_autocommit = false;
            if (!is_sep &&
                (kb->pending_swipe ||
                 kb->suggest_mode == WVKBD_SMODE_SWIPE)) {
                kb->pending_swipe = false;
                kb->pending_swipe_word[0] = '\0';
                kb->swipe_points_len = 0;
                kb->suggestions_len = 0;
                kb->suggest_mode = WVKBD_SMODE_NONE;
                kb->suggest_scroll_x = 0.0;
            }

            if (is_sep &&
                (kb->pending_swipe ||
                 kb->suggest_mode == WVKBD_SMODE_SWIPE)) {
                const char *w = kb->pending_swipe
                                    ? kb->pending_swipe_word
                                    : kbd_top_word_suggestion(kb);
                if (w && w[0]) {
                    kbd_commit_suggestion(kb, time_ms, w);
                    did_autocommit = true;
                }
            }

            uint32_t key_time = time_ms;
            if (did_autocommit) {
                key_time += 32;
            }
            kbd_press_key(kb, k, key_time);
            kbd_release_key(kb, key_time);
            kbd_handle_committed_key(kb, k, mods_before);
        }
        kbd_draw_layout(kb);
        kb->input_mode = KBD_INPUT_NONE;
        return;
    }

    if (kb->input_mode == KBD_INPUT_SWIPE) {
        // Compute final suggestions and cache the current best. Commit happens
        // on suggestion tap, or implicitly on the next separator (space/punct).
        kbd_update_suggestions_swipe(kb);
        kbd_set_pending_swipe_from_suggestions(kb);
        kb->input_mode = KBD_INPUT_NONE;
        return;
    }

    kb->input_mode = KBD_INPUT_NONE;
}

void
draw_inset(struct drwsurf *ds, uint32_t x, uint32_t y, uint32_t width,
           uint32_t height, uint32_t border, Color color, int rounding)
{
    drw_fill_rectangle(ds, color, x + border, y + border, width - (border * 2),
                       height - (border * 2), rounding);
}
void
draw_over_inset(struct drwsurf *ds, uint32_t x, uint32_t y, uint32_t width,
                uint32_t height, uint32_t border, Color color, int rounding)
{
    drw_over_rectangle(ds, color, x + border, y + border, width - (border * 2),
                       height - (border * 2), rounding);
}

void
create_and_upload_keymap(struct kbd *kb, const char *name, uint32_t comp_unichr,
                         uint32_t comp_shift_unichr)
{
    int keymap_index = -1;
    for (int i = 0; i < NUMKEYMAPS; i++) {
        if (!strcmp(keymap_names[i], name)) {
            keymap_index = i;
        }
    }
    if (keymap_index == -1) {
        fprintf(stderr, "No such keymap defined: %s\n", name);
        exit(9);
    }
    const char *keymap_template = keymaps[keymap_index];
    size_t keymap_size = strlen(keymap_template) + 64;
    char *keymap_str = malloc(keymap_size);
    sprintf(keymap_str, keymap_template, comp_unichr, comp_shift_unichr);
    keymap_size = strlen(keymap_str);
    int keymap_fd = os_create_anonymous_file(keymap_size);
    if (keymap_fd < 0) {
        die("could not create keymap fd\n");
    }
    void *ptr = mmap(NULL, keymap_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     keymap_fd, 0);
    if (ptr == (void *)-1) {
        die("could not map keymap data\n");
    }
    if (kb->vkbd == NULL) {
        die("kb.vkbd = NULL\n");
    }
    strcpy(ptr, keymap_str);
    zwp_virtual_keyboard_v1_keymap(kb->vkbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                   keymap_fd, keymap_size);
    free((void *)keymap_str);
}
