//
// Copyright 2026 AUTOMATOUS.IO
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

// test_ui.c
//
// Host tests for the device UI: the button gestures (button_logic), what
// each input does in each state (ui_policy) and the LED patterns.
//
// Every row below is a row of docs/BUTTON-AND-LED.md, with the same ID and
// the same words. scripts/check-ui-spec.py compares the two and fails if a
// row is missing, extra or worded differently on either side, so the doc
// cannot drift from the behavior. Change them together (see CLAUDE.md).

#include <stdio.h>
#include <string.h>

#include "button_logic.h"
#include "led_patterns.h"
#include "ui_policy.h"

static int s_failures = 0;
static int s_checks = 0;

#define CHECK_ROW(id, cond, fmt, ...)                                            \
    do {                                                                         \
        s_checks++;                                                              \
        if (!(cond)) {                                                           \
            s_failures++;                                                        \
            printf("FAIL %s: " fmt "\n", id, __VA_ARGS__);                       \
        }                                                                        \
    } while (0)

// ---------------------------------------------------------------------------
// Gestures (doc: "Gestures", G-xx)
// ---------------------------------------------------------------------------

typedef struct {
    uint32_t hold_ms;   // How long the button is held
    uint32_t gap_ms;    // Released time before the next press
} press_t;

typedef struct {
    const char *id;
    const char *gesture;
    const char *result;
    int count;
    press_t presses[6];
} gesture_row_t;

#define FAST 150    // A gap well inside BUTTON_MULTI_GAP_MS
#define TAP 120     // A quick tap

static const gesture_row_t GESTURES[] = {
    {"G-01", "1 press, released before 1 s", "Tap", 1, {{TAP, 0}}},
    {"G-02", "2 presses less than 300 ms apart", "2 taps", 2, {{TAP, FAST}, {TAP, 0}}},
    {"G-03", "3 presses less than 300 ms apart", "3+ taps", 3, {{TAP, FAST}, {TAP, FAST}, {TAP, 0}}},
    {"G-04", "5 presses less than 300 ms apart", "3+ taps", 5, {{TAP, FAST}, {TAP, FAST}, {TAP, FAST}, {TAP, FAST}, {TAP, 0}}},
    {"G-05", "2 presses 500 ms apart", "Tap, Tap", 2, {{TAP, 500}, {TAP, 0}}},
    {"G-06", "Press held 950 ms", "Tap", 1, {{950, 0}}},
    {"G-07", "Hold 1 s", "Nothing", 1, {{1000, 0}}},
    {"G-08", "Hold 3 s", "Nothing", 1, {{3000, 0}}},
    {"G-09", "Hold 5 s", "Hold 5-10 s", 1, {{5000, 0}}},
    {"G-10", "Hold 8 s", "Hold 5-10 s", 1, {{8000, 0}}},
    {"G-11", "Hold 10 s", "Hold 10-20 s", 1, {{10000, 0}}},
    {"G-12", "Hold 15 s", "Hold 10-20 s", 1, {{15000, 0}}},
    {"G-13", "Hold 20 s or longer", "Nothing", 1, {{20000, 0}}},
    {"G-14", "2 presses, then a 6 s hold less than 300 ms later", "Hold 5-10 s", 3, {{TAP, FAST}, {TAP, FAST}, {6000, 0}}},
    {"G-15", "2 presses less than 300 ms apart, a third 500 ms later", "2 taps, Tap", 3, {{TAP, FAST}, {TAP, 500}, {TAP, 0}}},
};

static const char *gesture_name(button_action_t a)
{
    switch (a) {
    case BUTTON_ACTION_TOGGLE:
        return "Tap";
    case BUTTON_ACTION_DOUBLE_TAP:
        return "2 taps";
    case BUTTON_ACTION_PAIRING:
        return "3+ taps";
    case BUTTON_ACTION_WIFI_MODE:
        return "Hold 5-10 s";
    case BUTTON_ACTION_FACTORY_RESET:
        return "Hold 10-20 s";
    default:
        return "?";
    }
}

static void append(char *buf, size_t len, const char *s)
{
    if (buf[0] != '\0') {
        strncat(buf, ", ", len - strlen(buf) - 1);
    }
    strncat(buf, s, len - strlen(buf) - 1);
}

static void test_gestures(void)
{
    for (size_t r = 0; r < sizeof(GESTURES) / sizeof(GESTURES[0]); r++) {
        const gesture_row_t *row = &GESTURES[r];
        button_logic_t b;
        button_logic_init(&b);
        // Start near the 32-bit wrap to prove the arithmetic survives it.
        uint32_t t = 0xFFFFF000u;
        char got[128] = "";
        bool fired_while_held = false;

        for (int i = 0; i < row->count; i++) {
            button_action_t a = button_logic_update(&b, true, t);
            if (a != BUTTON_ACTION_NONE) {
                append(got, sizeof(got), gesture_name(a));
            }
            uint32_t end = t + row->presses[i].hold_ms;
            // Tick while held: nothing may fire before the release.
            for (t += 50; (int32_t)(end - t) > 0; t += 50) {
                if (button_logic_update(&b, true, t) != BUTTON_ACTION_NONE) {
                    fired_while_held = true;
                }
            }
            t = end;
            a = button_logic_update(&b, false, t);
            if (a != BUTTON_ACTION_NONE) {
                append(got, sizeof(got), gesture_name(a));
            }
            uint32_t next = t + row->presses[i].gap_ms;
            for (t += 50; (int32_t)(next - t) > 0; t += 50) {
                a = button_logic_update(&b, false, t);
                if (a != BUTTON_ACTION_NONE) {
                    append(got, sizeof(got), gesture_name(a));
                }
            }
            t = next;
        }
        // Let any pending taps resolve.
        for (int i = 0; i < 40; i++, t += 50) {
            button_action_t a = button_logic_update(&b, false, t);
            if (a != BUTTON_ACTION_NONE) {
                append(got, sizeof(got), gesture_name(a));
            }
        }
        if (got[0] == '\0') {
            strcpy(got, "Nothing");
        }
        CHECK_ROW(row->id, strcmp(got, row->result) == 0, "%s: got \"%s\", want \"%s\"", row->gesture, got,
                  row->result);
        CHECK_ROW(row->id, !fired_while_held, "%s: an action fired while the button was held", row->gesture);
        CHECK_ROW(row->id, !button_logic_busy(&b), "%s: still busy after the gesture", row->gesture);
    }
}

// ---------------------------------------------------------------------------
// LED while held (doc: "While the button is held", H-xx)
// ---------------------------------------------------------------------------

typedef struct {
    const char *id;
    const char *held;
    const char *shows;
    uint32_t held_ms;
} hint_row_t;

static const hint_row_t HINTS[] = {
    {"H-01", "Less than 1 s", "Normal pattern", 500},
    {"H-02", "1 s to 5 s", "Off", 1000},
    {"H-03", "5 s to 10 s", "Hold for Wi-Fi mode", 5000},
    {"H-04", "10 s to 20 s", "Hold for factory reset", 10000},
    {"H-05", "20 s or longer", "Normal pattern", 20000},
};

static const char *hint_name(button_hint_t h)
{
    switch (h) {
    case BUTTON_HINT_NONE:
        return "Normal pattern";
    case BUTTON_HINT_WAIT:
        return "Off";
    case BUTTON_HINT_WIFI:
        return "Hold for Wi-Fi mode";
    case BUTTON_HINT_RESET:
        return "Hold for factory reset";
    default:
        return "?";
    }
}

static void test_hints(void)
{
    for (size_t r = 0; r < sizeof(HINTS) / sizeof(HINTS[0]); r++) {
        const hint_row_t *row = &HINTS[r];
        button_logic_t b;
        button_logic_init(&b);
        button_logic_update(&b, true, 1000);
        const char *got = hint_name(button_logic_hint(&b, 1000 + row->held_ms));
        CHECK_ROW(row->id, strcmp(got, row->shows) == 0, "held %s: got \"%s\", want \"%s\"", row->held, got,
                  row->shows);
    }
    button_logic_t b;
    button_logic_init(&b);
    CHECK_ROW("H-01", button_logic_hint(&b, 5000) == BUTTON_HINT_NONE, "%s", "released button shows a hint");
}

// ---------------------------------------------------------------------------
// Decisions (doc: "What the button does", B-xx, and "Wall switch", SW-xx)
// ---------------------------------------------------------------------------

typedef struct {
    const char *name;
    ui_state_t st;
} state_fixture_t;

static const state_fixture_t STATES[] = {
    {"Booting", {.started = false, .maintenance_available = true}},
    {"Normal", {.started = true, .maintenance_available = true}},
    {"First 60 s after an update", {.started = true, .pending_verify = true, .maintenance_available = true}},
    {"Matter update downloading", {.started = true, .ota_busy = true, .maintenance_available = true}},
    {"Normal, built without Wi-Fi mode", {.started = true, .maintenance_available = false}},
    {"Wi-Fi maintenance mode", {.maintenance = true, .maintenance_available = true}},
    {"Wi-Fi mode, change waiting", {.maintenance = true, .maintenance_available = true, .change_pending = true}},
};

static const struct {
    const char *name;
    ui_input_t input;
} INPUTS[] = {
    {"Tap", UI_INPUT_TAP},
    {"2 taps", UI_INPUT_TAP2},
    {"3+ taps", UI_INPUT_PAIRING},
    {"Hold 5-10 s", UI_INPUT_WIFI_MODE},
    {"Hold 10-20 s", UI_INPUT_FACTORY_RESET},
};

// ch: the input on a 2PM (0 = SW1). Each input drives its own relay.
static const struct {
    const char *name;
    ui_switch_role_t role;
    uint8_t ch;
} ROLES[] = {
    {"Light, Outlet, Mini Opener, 2PM SW1", UI_SWITCH_TOGGLE, 0},
    {"2PM SW2", UI_SWITCH_TOGGLE, 1},
    {"1 Gen4 Opener", UI_SWITCH_CONTACT, 0},
    {"Light Switch", UI_SWITCH_BINDING, 0},
};

static const char *outcome_name(ui_outcome_t o)
{
    switch (o) {
    case UI_IGNORE:
        return "Ignored";
    case UI_REFUSE:
        return "Refused";
    case UI_TOGGLE:
        return "Toggle";
    case UI_LOCAL_TOGGLE:
        return "Toggle, without Matter";
    case UI_VARIANT:
        return "Its own use";
    case UI_OPEN_PAIRING:
        return "Open pairing";
    case UI_ENTER_MAINTENANCE:
        return "Enter Wi-Fi mode";
    case UI_EXIT_MAINTENANCE:
        return "Exit Wi-Fi mode";
    case UI_FACTORY_RESET:
        return "Factory reset";
    case UI_DEFER_RESET:
        return "Factory reset after a restart";
    case UI_CONFIRM_CHANGE:
        return "Save the change";
    case UI_TURN_ON:
        return "Turn on";
    case UI_TURN_OFF:
        return "Turn off";
    case UI_LOCAL_TURN_ON:
        return "Turn on, without Matter";
    case UI_LOCAL_TURN_OFF:
        return "Turn off, without Matter";
    default:
        return "?";
    }
}

typedef struct {
    const char *id;
    const char *state;
    const char *input;
    const char *outcome;
} decision_row_t;

static const decision_row_t DECISIONS[] = {
    {"B-01", "Booting", "Tap", "Refused"},
    {"B-02", "Booting", "3+ taps", "Refused"},
    {"B-03", "Booting", "Hold 5-10 s", "Refused"},
    {"B-04", "Booting", "Hold 10-20 s", "Factory reset after a restart"},
    {"B-05", "Normal", "Tap", "Toggle"},
    {"B-06", "Normal", "3+ taps", "Open pairing"},
    {"B-07", "Normal", "Hold 5-10 s", "Enter Wi-Fi mode"},
    {"B-08", "Normal", "Hold 10-20 s", "Factory reset"},
    {"B-09", "First 60 s after an update", "Tap", "Toggle"},
    {"B-10", "First 60 s after an update", "3+ taps", "Open pairing"},
    {"B-11", "First 60 s after an update", "Hold 5-10 s", "Refused"},
    {"B-12", "First 60 s after an update", "Hold 10-20 s", "Refused"},
    {"B-13", "Matter update downloading", "Tap", "Toggle"},
    {"B-14", "Matter update downloading", "3+ taps", "Open pairing"},
    {"B-15", "Matter update downloading", "Hold 5-10 s", "Refused"},
    {"B-16", "Matter update downloading", "Hold 10-20 s", "Factory reset"},
    {"B-17", "Normal, built without Wi-Fi mode", "Tap", "Toggle"},
    {"B-18", "Normal, built without Wi-Fi mode", "3+ taps", "Open pairing"},
    {"B-19", "Normal, built without Wi-Fi mode", "Hold 5-10 s", "Refused"},
    {"B-20", "Normal, built without Wi-Fi mode", "Hold 10-20 s", "Factory reset"},
    {"B-21", "Wi-Fi maintenance mode", "Tap", "Toggle, without Matter"},
    {"B-22", "Wi-Fi maintenance mode", "3+ taps", "Refused"},
    {"B-23", "Wi-Fi maintenance mode", "Hold 5-10 s", "Exit Wi-Fi mode"},
    {"B-24", "Wi-Fi maintenance mode", "Hold 10-20 s", "Factory reset after a restart"},
    {"B-25", "Wi-Fi mode, change waiting", "Tap", "Toggle, without Matter"},
    {"B-26", "Wi-Fi mode, change waiting", "3+ taps", "Save the change"},
    {"B-27", "Wi-Fi mode, change waiting", "Hold 5-10 s", "Exit Wi-Fi mode"},
    {"B-28", "Wi-Fi mode, change waiting", "Hold 10-20 s", "Factory reset after a restart"},
    {"B-29", "Booting", "2 taps", "Refused"},
    {"B-30", "Normal", "2 taps", "Toggle"},
    {"B-31", "First 60 s after an update", "2 taps", "Toggle"},
    {"B-32", "Matter update downloading", "2 taps", "Toggle"},
    {"B-33", "Normal, built without Wi-Fi mode", "2 taps", "Toggle"},
    {"B-34", "Wi-Fi maintenance mode", "2 taps", "Toggle, without Matter"},
    {"B-35", "Wi-Fi mode, change waiting", "2 taps", "Toggle, without Matter"},
};

typedef struct {
    const char *id;
    const char *variant;
    const char *state;
    const char *outcome;
} switch_row_t;

static const switch_row_t SWITCHES[] = {
    {"SW-01", "Light, Outlet, Mini Opener, 2PM SW1", "Booting", "Ignored"},
    {"SW-02", "Light, Outlet, Mini Opener, 2PM SW1", "Normal", "Toggle"},
    {"SW-03", "Light, Outlet, Mini Opener, 2PM SW1", "Wi-Fi maintenance mode", "Toggle, without Matter"},
    {"SW-04", "1 Gen4 Opener", "Booting", "Ignored"},
    {"SW-05", "1 Gen4 Opener", "Normal", "Its own use"},
    {"SW-06", "1 Gen4 Opener", "Wi-Fi maintenance mode", "Ignored"},
    {"SW-07", "Light Switch", "Booting", "Ignored"},
    {"SW-08", "Light Switch", "Normal", "Its own use"},
    {"SW-09", "Light Switch", "Wi-Fi maintenance mode", "Ignored"},
    {"SW-10", "2PM SW2", "Booting", "Ignored"},
    {"SW-11", "2PM SW2", "Normal", "Toggle"},
    {"SW-12", "2PM SW2", "Wi-Fi maintenance mode", "Toggle, without Matter"},
};

// Wall switch modes (doc: "Wall switch modes", SM-xx): the toggle role only.
typedef struct {
    const char *id;
    const char *mode;
    const char *change;
    const char *normal;
    const char *wifi;
} switch_mode_row_t;

static const switch_mode_row_t SWITCH_MODES[] = {
    {"SM-01", "Toggle", "Either", "Toggle", "Toggle, without Matter"},
    {"SM-02", "Push button", "Press", "Toggle", "Toggle, without Matter"},
    {"SM-03", "Push button", "Release", "Ignored", "Ignored"},
    {"SM-04", "Follow", "To on", "Turn on", "Turn on, without Matter"},
    {"SM-05", "Follow", "To off", "Turn off", "Turn off, without Matter"},
    {"SM-06", "Disabled", "Either", "Ignored", "Ignored"},
};

static const struct {
    const char *name;
    ui_switch_mode_t mode;
} MODES[] = {
    {"Toggle", UI_SWITCH_MODE_TOGGLE},
    {"Push button", UI_SWITCH_MODE_PUSH},
    {"Follow", UI_SWITCH_MODE_FOLLOW},
    {"Disabled", UI_SWITCH_MODE_DISABLED},
};

// Which switch levels a Change covers: bit 0 = to off/release, bit 1 = to on/press.
static int change_mask(const char *change)
{
    if (strcmp(change, "Either") == 0) {
        return 3;
    }
    if (strcmp(change, "Press") == 0 || strcmp(change, "To on") == 0) {
        return 2;
    }
    if (strcmp(change, "Release") == 0 || strcmp(change, "To off") == 0) {
        return 1;
    }
    return 0;
}

static const ui_state_t *find_state(const char *name)
{
    for (size_t i = 0; i < sizeof(STATES) / sizeof(STATES[0]); i++) {
        if (strcmp(STATES[i].name, name) == 0) {
            return &STATES[i].st;
        }
    }
    return NULL;
}

static void test_decisions(void)
{
    // Every state x input pair has exactly one row: no mode is left out.
    size_t pairs = sizeof(STATES) / sizeof(STATES[0]) * sizeof(INPUTS) / sizeof(INPUTS[0]);
    CHECK_ROW("B", sizeof(DECISIONS) / sizeof(DECISIONS[0]) == pairs, "%zu rows for %zu state/input pairs",
              sizeof(DECISIONS) / sizeof(DECISIONS[0]), pairs);
    for (size_t s = 0; s < sizeof(STATES) / sizeof(STATES[0]); s++) {
        for (size_t i = 0; i < sizeof(INPUTS) / sizeof(INPUTS[0]); i++) {
            int found = 0;
            for (size_t r = 0; r < sizeof(DECISIONS) / sizeof(DECISIONS[0]); r++) {
                found += strcmp(DECISIONS[r].state, STATES[s].name) == 0 &&
                         strcmp(DECISIONS[r].input, INPUTS[i].name) == 0;
            }
            CHECK_ROW("B", found == 1, "%s / %s has %d rows", STATES[s].name, INPUTS[i].name, found);
        }
    }

    for (size_t r = 0; r < sizeof(DECISIONS) / sizeof(DECISIONS[0]); r++) {
        const decision_row_t *row = &DECISIONS[r];
        const ui_state_t *st = find_state(row->state);
        ui_input_t input = UI_INPUT_TAP;
        bool known = false;
        for (size_t i = 0; i < sizeof(INPUTS) / sizeof(INPUTS[0]); i++) {
            if (strcmp(INPUTS[i].name, row->input) == 0) {
                input = INPUTS[i].input;
                known = true;
            }
        }
        CHECK_ROW(row->id, st != NULL && known, "unknown state \"%s\" or input \"%s\"", row->state, row->input);
        if (st == NULL || !known) {
            continue;
        }
        const char *got = outcome_name(ui_decide(input, st));
        CHECK_ROW(row->id, strcmp(got, row->outcome) == 0, "%s / %s: got \"%s\", want \"%s\"", row->state,
                  row->input, got, row->outcome);
        // The same decision on a 2PM, where 2 taps act on relay 2 and
        // everything else on relay 1; one relay everywhere else.
        ui_state_t two = *st;
        two.channels = 2;
        got = outcome_name(ui_decide(input, &two));
        CHECK_ROW(row->id, strcmp(got, row->outcome) == 0, "%s / %s on a 2PM: got \"%s\"", row->state, row->input, got);
        uint8_t want_ch = input == UI_INPUT_TAP2 ? 1 : 0;
        CHECK_ROW(row->id, ui_decide_channel(input, &two) == want_ch, "%s / %s on a 2PM: relay %u", row->state,
                  row->input, ui_decide_channel(input, &two) + 1u);
        CHECK_ROW(row->id, ui_decide_channel(input, st) == 0, "%s / %s on one relay: relay %u", row->state,
                  row->input, ui_decide_channel(input, st) + 1u);
    }

    for (size_t r = 0; r < sizeof(SWITCHES) / sizeof(SWITCHES[0]); r++) {
        const switch_row_t *row = &SWITCHES[r];
        const ui_state_t *base = find_state(row->state);
        bool known = false;
        ui_state_t st = {0};
        if (base != NULL) {
            st = *base;
        }
        uint8_t ch = 0;
        for (size_t i = 0; i < sizeof(ROLES) / sizeof(ROLES[0]); i++) {
            if (strcmp(ROLES[i].name, row->variant) == 0) {
                st.switch_role = ROLES[i].role;
                ch = ROLES[i].ch;
                known = true;
            }
        }
        if (ch > 0) {
            st.channels = 2;
            st.switch_channel = ch;
        }
        CHECK_ROW(row->id, base != NULL && known, "unknown variant \"%s\" or state \"%s\"", row->variant, row->state);
        if (base == NULL || !known) {
            continue;
        }
        const char *got = outcome_name(ui_decide(UI_INPUT_SWITCH, &st));
        CHECK_ROW(row->id, strcmp(got, row->outcome) == 0, "%s / %s: got \"%s\", want \"%s\"", row->variant,
                  row->state, got, row->outcome);
        CHECK_ROW(row->id, ui_decide_channel(UI_INPUT_SWITCH, &st) == ch, "%s: acts on relay %u", row->variant,
                  ui_decide_channel(UI_INPUT_SWITCH, &st) + 1u);
    }
    // A switch channel the board doesn't have falls back to relay 1.
    ui_state_t one = {.started = true, .switch_channel = 1};
    CHECK_ROW("SW", ui_decide_channel(UI_INPUT_SWITCH, &one) == 0, "%s", "SW2 on a one-relay board");
    CHECK_ROW("B", ui_decide_channel(UI_INPUT_TAP2, NULL) == 0, "%s", "NULL state channel");
    CHECK_ROW("B", ui_decide(UI_INPUT_TAP, NULL) == UI_IGNORE, "%s", "NULL state");

    // Wall switch modes: every mode has rows covering both changes exactly once.
    CHECK_ROW("SM", sizeof(MODES) / sizeof(MODES[0]) == UI_SWITCH_MODE_COUNT, "%s", "every mode has a name");
    for (size_t m = 0; m < sizeof(MODES) / sizeof(MODES[0]); m++) {
        int covered = 0;
        for (size_t r = 0; r < sizeof(SWITCH_MODES) / sizeof(SWITCH_MODES[0]); r++) {
            if (strcmp(SWITCH_MODES[r].mode, MODES[m].name) == 0) {
                int mask = change_mask(SWITCH_MODES[r].change);
                CHECK_ROW(SWITCH_MODES[r].id, (covered & mask) == 0 && mask != 0, "%s: change \"%s\" overlaps or is unknown",
                          MODES[m].name, SWITCH_MODES[r].change);
                covered |= mask;
            }
        }
        CHECK_ROW("SM", covered == 3, "%s: rows cover both changes", MODES[m].name);
    }
    const ui_state_t *normal = find_state("Normal");
    const ui_state_t *wifi = find_state("Wi-Fi maintenance mode");
    const ui_state_t *booting = find_state("Booting");
    for (size_t r = 0; r < sizeof(SWITCH_MODES) / sizeof(SWITCH_MODES[0]); r++) {
        const switch_mode_row_t *row = &SWITCH_MODES[r];
        const ui_switch_mode_t *mode = NULL;
        for (size_t m = 0; m < sizeof(MODES) / sizeof(MODES[0]); m++) {
            if (strcmp(MODES[m].name, row->mode) == 0) {
                mode = &MODES[m].mode;
            }
        }
        int mask = change_mask(row->change);
        CHECK_ROW(row->id, mode != NULL && mask != 0, "unknown mode \"%s\" or change \"%s\"", row->mode, row->change);
        if (mode == NULL || mask == 0) {
            continue;
        }
        for (int active = 0; active <= 3; active++) {
            // 0-1: SW1 (any board) off/on; 2-3: a 2PM's SW2 off/on.
            uint8_t ch = (uint8_t)(active >> 1);
            if (!(mask & (1 << (active & 1)))) {
                continue;
            }
            bool on = (active & 1) != 0;
            const ui_state_t *bases[] = {normal, wifi, booting};
            const char *wants[] = {row->normal, row->wifi, "Ignored"};
            const char *names[] = {"Normal", "Wi-Fi mode", "Booting"};
            ui_state_t st;
            for (int b = 0; b < 3; b++) {
                // Booting ignores the switch in every mode (SW-01).
                st = *bases[b];
                st.channels = ch ? 2 : 0;
                st.switch_channel = ch;
                st.switch_role = UI_SWITCH_TOGGLE;
                st.switch_mode = *mode;
                st.switch_active = on;
                const char *got = outcome_name(ui_decide(UI_INPUT_SWITCH, &st));
                CHECK_ROW(row->id, strcmp(got, wants[b]) == 0, "%s / %s / %s / SW%u: got \"%s\", want \"%s\"",
                          row->mode, row->change, names[b], ch + 1u, got, wants[b]);
                CHECK_ROW(row->id, ui_decide_channel(UI_INPUT_SWITCH, &st) == ch, "%s: SW%u acts on relay %u",
                          row->mode, ch + 1u, ui_decide_channel(UI_INPUT_SWITCH, &st) + 1u);
            }
        }
    }
    // The page's names round-trip, and nothing else parses.
    for (int m = 0; m < UI_SWITCH_MODE_COUNT; m++) {
        ui_switch_mode_t back = UI_SWITCH_MODE_COUNT;
        CHECK_ROW("SM", ui_switch_mode_parse(ui_switch_mode_name((ui_switch_mode_t)m), &back) && back == (ui_switch_mode_t)m,
                  "mode %d name round-trip", m);
    }
    ui_switch_mode_t junk = UI_SWITCH_MODE_TOGGLE;
    CHECK_ROW("SM", !ui_switch_mode_parse("Toggle", &junk) && !ui_switch_mode_parse("", &junk) &&
                        !ui_switch_mode_parse(NULL, &junk) && !ui_switch_mode_parse("follow ", &junk),
              "%s", "unknown names refused");
    CHECK_ROW("SM", strcmp(ui_switch_mode_name(UI_SWITCH_MODE_COUNT), "toggle") == 0, "%s", "unknown mode names as toggle");

    // A mode value the page never sets is ignored, not acted on.
    ui_state_t odd = *normal;
    odd.switch_role = UI_SWITCH_TOGGLE;
    odd.switch_mode = UI_SWITCH_MODE_COUNT;
    CHECK_ROW("SM", ui_decide(UI_INPUT_SWITCH, &odd) == UI_IGNORE, "%s", "unknown mode");
}

// ---------------------------------------------------------------------------
// LED patterns (doc: "LED", LED-xx; the timings are checked by
// check-ui-spec.py against led_patterns.h)
// ---------------------------------------------------------------------------

static void test_led_patterns(void)
{
    bool done;
    // Repeating blink.
    const led_pattern_t *p = &LED_PATTERNS[LED_PAT_PAIRING];
    CHECK_ROW("LED-02", led_pattern_level(p, 0, &done) && !done, "%s", "starts on");
    CHECK_ROW("LED-02", !led_pattern_level(p, 200, &done), "%s", "off after 200 ms");
    CHECK_ROW("LED-02", led_pattern_level(p, 400, &done), "%s", "repeats");
    // Four-step double blink.
    p = &LED_PATTERNS[LED_PAT_MAINTENANCE];
    CHECK_ROW("LED-08", led_pattern_level(p, 50, NULL) && !led_pattern_level(p, 150, NULL) &&
                            led_pattern_level(p, 250, NULL) && !led_pattern_level(p, 500, NULL) &&
                            led_pattern_level(p, 1000, NULL),
              "%s", "double blink");
    // Solid and off.
    CHECK_ROW("LED-05", led_pattern_level(&LED_PATTERNS[LED_PAT_CONNECTED], 12345, NULL), "%s", "solid");
    CHECK_ROW("LED-01", !led_pattern_level(&LED_PATTERNS[LED_PAT_OFF], 12345, NULL), "%s", "off");
    // One-shots end.
    p = &LED_PATTERNS[LED_PAT_REFUSED];
    CHECK_ROW("LED-14", led_pattern_level(p, 0, &done) && !done, "%s", "refusal starts on");
    led_pattern_level(p, 600, &done);
    CHECK_ROW("LED-14", done, "%s", "refusal ends after 600 ms");
    p = &LED_PATTERNS[LED_PAT_PRESS];
    CHECK_ROW("LED-10", !led_pattern_level(p, 0, &done) && !done, "%s", "press blip is off");
    led_pattern_level(p, 100, &done);
    CHECK_ROW("LED-10", done, "%s", "press blip ends");
    // Every pattern that repeats has a period, and no pattern overflows.
    p = &LED_PATTERNS[LED_PAT_UPDATING];
    CHECK_ROW("LED-16", led_pattern_level(p, 0, NULL) && led_pattern_level(p, 999, NULL), "%s", "update: on for 1 s");
    CHECK_ROW("LED-16", !led_pattern_level(p, 1000, NULL) && !led_pattern_level(p, 1999, NULL), "%s",
              "update: off for 1 s");
    // No other pattern looks like the update pattern (not even the hold hints).
    for (int i = 0; i < LED_PAT_COUNT; i++) {
        const led_pattern_t *q = &LED_PATTERNS[i];
        bool same = i != LED_PAT_UPDATING && q->first_on == p->first_on && q->repeat == p->repeat &&
                    q->steps == p->steps && memcmp(q->ms, p->ms, sizeof(q->ms)) == 0;
        CHECK_ROW("LED-16", !same, "pattern %d looks like the update pattern", i);
    }

    for (int i = 0; i < LED_PAT_COUNT; i++) {
        CHECK_ROW("LED", LED_PATTERNS[i].steps <= LED_PATTERN_MAX_STEPS, "pattern %d has too many steps", i);
        CHECK_ROW("LED", LED_PATTERNS[i].repeat || LED_PATTERNS[i].steps > 0, "one-shot pattern %d never ends", i);
    }
}

int main(void)
{
    test_gestures();
    test_hints();
    test_decisions();
    test_led_patterns();
    printf("%d UI checks, %d failures\n", s_checks, s_failures);
    return s_failures == 0 ? 0 : 1;
}
