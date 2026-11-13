/*
 * PadCheck 360 - Xbox 360 controller diagnostic homebrew
 * Copyright (c) 2026
 * SPDX-License-Identifier: MIT
 *
 * Built against the open-source xsdk project:
 * https://github.com/xenosworks/xsdk
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/text.h"

#define APP_NAME "PADCHECK 360"
#define APP_VERSION "0.2.0"

#define PLAYER_COUNT 4u
#define FRAME_BUFFERS 2u
#define SWAP_INTERVAL 1u
#define PRESENT_TIMEOUT_MS 5000u
#define EXIT_HOLD_FRAMES 120u
#define CALIBRATION_HOLD_FRAMES 45u
#define DRIFT_SAMPLE_FRAMES 180u
#define TRIGGER_DETECT_THRESHOLD 32u
#define DRIFT_THRESHOLD 3500

#define COLOR_BACKGROUND 0xFF101722u
#define COLOR_WHITE      0xFFF0F4F8u
#define COLOR_MUTED      0xFF9AA8B7u
#define COLOR_CYAN       0xFF48D7FFu
#define COLOR_GREEN      0xFF50E890u
#define COLOR_DARK       0xFF66717Du
#define COLOR_YELLOW     0xFFFFD166u
#define COLOR_RED        0xFFFF6B6Bu
#define COLOR_ORANGE     0xFFFFA94Du

#define BUTTON_TEST_MASK ( \
    INPUT_GAMEPAD_DPAD_UP | INPUT_GAMEPAD_DPAD_DOWN | \
    INPUT_GAMEPAD_DPAD_LEFT | INPUT_GAMEPAD_DPAD_RIGHT | \
    INPUT_GAMEPAD_START | INPUT_GAMEPAD_BACK | \
    INPUT_GAMEPAD_LEFT_THUMB | INPUT_GAMEPAD_RIGHT_THUMB | \
    INPUT_GAMEPAD_LEFT_SHOULDER | INPUT_GAMEPAD_RIGHT_SHOULDER | \
    INPUT_GAMEPAD_A | INPUT_GAMEPAD_B | INPUT_GAMEPAD_X | INPUT_GAMEPAD_Y)

#define TOTAL_TEST_COUNT 16u

#define TRIGGER_LEFT_SEEN  0x01u
#define TRIGGER_RIGHT_SEEN 0x02u

typedef struct {
    bool connected;
    INPUT_STATE state;
    bool rumble_left;
    bool rumble_right;
} PlayerInput;

typedef struct {
    uint16_t seen_buttons;
    uint8_t seen_triggers;
    uint8_t max_left_trigger;
    uint8_t max_right_trigger;

    bool calibrated;
    bool calibration_triggered;
    bool waiting_for_start_release;
    bool drift_sample_finished;
    uint32_t start_hold_frames;
    uint32_t drift_sample_frames_left;

    int16_t center_lx;
    int16_t center_ly;
    int16_t center_rx;
    int16_t center_ry;
    int32_t peak_left_delta;
    int32_t peak_right_delta;
} PlayerDiagnostics;

static void draw_text(int32_t x, int32_t y, uint32_t scale, uint32_t color, const char *text)
{
    xsdk_text_draw(x, y, scale, color, text);
}

static int32_t abs_i32(int32_t value)
{
    return value < 0 ? -value : value;
}

static int32_t max_i32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

static int axis_percent(int32_t value)
{
    if (value < 0)
        return (int)(value * 100 / 32768);
    return (int)(value * 100 / 32767);
}

static uint32_t count_bits16(uint16_t value)
{
    uint32_t count = 0;
    while (value != 0u) {
        count += (uint32_t)(value & 1u);
        value = (uint16_t)(value >> 1);
    }
    return count;
}

static uint32_t tests_completed(const PlayerDiagnostics *diagnostics)
{
    return count_bits16(diagnostics->seen_buttons) +
           (uint32_t)((diagnostics->seen_triggers & TRIGGER_LEFT_SEEN) != 0u) +
           (uint32_t)((diagnostics->seen_triggers & TRIGGER_RIGHT_SEEN) != 0u);
}

static void set_rumble(uint32_t player, bool left_enabled, bool right_enabled)
{
    const INPUT_VIBRATION vibration = {
        (uint16_t)(left_enabled ? 0x9000u : 0u),
        (uint16_t)(right_enabled ? 0x9000u : 0u),
    };
    const uint32_t result = XamInputSetState(player, 0, &vibration);
    if (result != ERROR_SUCCESS) {
        xsdk_log_write("PadCheck: vibration P%u failed: 0x%08X", (unsigned)(player + 1u), (unsigned)result);
    }
}

static void update_diagnostics(PlayerDiagnostics *diagnostics, const INPUT_GAMEPAD *pad)
{
    const bool start_pressed = (pad->buttons & INPUT_GAMEPAD_START) != 0u;
    const bool back_pressed = (pad->buttons & INPUT_GAMEPAD_BACK) != 0u;

    diagnostics->seen_buttons |= (uint16_t)(pad->buttons & BUTTON_TEST_MASK);

    if (pad->left_trigger >= TRIGGER_DETECT_THRESHOLD)
        diagnostics->seen_triggers |= TRIGGER_LEFT_SEEN;
    if (pad->right_trigger >= TRIGGER_DETECT_THRESHOLD)
        diagnostics->seen_triggers |= TRIGGER_RIGHT_SEEN;

    if (pad->left_trigger > diagnostics->max_left_trigger)
        diagnostics->max_left_trigger = pad->left_trigger;
    if (pad->right_trigger > diagnostics->max_right_trigger)
        diagnostics->max_right_trigger = pad->right_trigger;

    /* Hold START with both sticks untouched to capture each stick's resting centre. */
    if (start_pressed && !back_pressed) {
        if (diagnostics->start_hold_frames < CALIBRATION_HOLD_FRAMES)
            ++diagnostics->start_hold_frames;

        if (diagnostics->start_hold_frames >= CALIBRATION_HOLD_FRAMES &&
            !diagnostics->calibration_triggered) {
            diagnostics->center_lx = pad->thumb_lx;
            diagnostics->center_ly = pad->thumb_ly;
            diagnostics->center_rx = pad->thumb_rx;
            diagnostics->center_ry = pad->thumb_ry;
            diagnostics->peak_left_delta = 0;
            diagnostics->peak_right_delta = 0;
            diagnostics->drift_sample_frames_left = DRIFT_SAMPLE_FRAMES;
            diagnostics->calibrated = true;
            diagnostics->drift_sample_finished = false;
            diagnostics->waiting_for_start_release = true;
            diagnostics->calibration_triggered = true;
            xsdk_log_write("PadCheck: controller calibration captured");
        }
    } else {
        diagnostics->start_hold_frames = 0;
        diagnostics->calibration_triggered = false;
        if (diagnostics->waiting_for_start_release) {
            diagnostics->waiting_for_start_release = false;
        }
    }

    if (diagnostics->calibrated && !diagnostics->waiting_for_start_release &&
        diagnostics->drift_sample_frames_left > 0u) {
        const int32_t left_dx = abs_i32((int32_t)pad->thumb_lx - diagnostics->center_lx);
        const int32_t left_dy = abs_i32((int32_t)pad->thumb_ly - diagnostics->center_ly);
        const int32_t right_dx = abs_i32((int32_t)pad->thumb_rx - diagnostics->center_rx);
        const int32_t right_dy = abs_i32((int32_t)pad->thumb_ry - diagnostics->center_ry);

        diagnostics->peak_left_delta = max_i32(diagnostics->peak_left_delta, max_i32(left_dx, left_dy));
        diagnostics->peak_right_delta = max_i32(diagnostics->peak_right_delta, max_i32(right_dx, right_dy));
        --diagnostics->drift_sample_frames_left;
        if (diagnostics->drift_sample_frames_left == 0u)
            diagnostics->drift_sample_finished = true;
    }
}

static void draw_player_status(int32_t x, int32_t y, uint32_t player,
                               const PlayerInput *input, uint32_t selected)
{
    char line[56];
    const uint32_t color = !input->connected ? COLOR_RED :
                           (player == selected ? COLOR_CYAN : COLOR_GREEN);
    const char *state = !input->connected ? "SEM CONTROLE" :
                        (player == selected ? "SELECIONADO" : "CONECTADO");
    (void)snprintf(line, sizeof(line), "P%u  %s  %s", (unsigned)(player + 1u),
                   input->connected ? "ONLINE" : "OFFLINE", state);
    draw_text(x, y, 1, color, line);
}

static void draw_button(int32_t x, int32_t y, const char *name,
                        uint16_t current, const PlayerDiagnostics *diagnostics, uint16_t mask)
{
    char line[32];
    const bool pressed = (current & mask) != 0u;
    const bool seen = (diagnostics->seen_buttons & mask) != 0u;
    const char *state = pressed ? "PRESSIONADO" : (seen ? "OK" : "--");
    const uint32_t color = pressed ? COLOR_GREEN : (seen ? COLOR_CYAN : COLOR_MUTED);
    (void)snprintf(line, sizeof(line), "%-9s [%s]", name, state);
    draw_text(x, y, 1, color, line);
}

static void draw_button_compact(int32_t x, int32_t y, const char *name,
                                uint16_t current, const PlayerDiagnostics *diagnostics, uint16_t mask)
{
    char line[24];
    const bool pressed = (current & mask) != 0u;
    const bool seen = (diagnostics->seen_buttons & mask) != 0u;
    (void)snprintf(line, sizeof(line), "%-7s:%s", name, pressed ? "ON" : (seen ? "OK" : "--"));
    draw_text(x, y, 1, pressed ? COLOR_GREEN : (seen ? COLOR_CYAN : COLOR_MUTED), line);
}

static void draw_trigger(int32_t x, int32_t y, const char *name, uint8_t value,
                         uint8_t maximum, bool seen)
{
    char line[72];
    char bar[15];
    int filled = ((int)value * 14 + 127) / 255;
    if (filled < 0)
        filled = 0;
    if (filled > 14)
        filled = 14;
    for (int i = 0; i < 14; ++i)
        bar[i] = (i < filled) ? '#' : '.';
    bar[14] = '\0';
    (void)snprintf(line, sizeof(line), "%s [%s] %3u/255  MAX %3u", name, bar,
                   (unsigned)value, (unsigned)maximum);
    draw_text(x, y, 1, seen ? COLOR_GREEN : COLOR_MUTED, line);
}

static void draw_trigger_compact(int32_t x, int32_t y, const char *name, uint8_t value,
                                 uint8_t maximum, bool seen)
{
    char line[48];
    char bar[9];
    int filled = ((int)value * 8 + 127) / 255;
    if (filled < 0)
        filled = 0;
    if (filled > 8)
        filled = 8;
    for (int i = 0; i < 8; ++i)
        bar[i] = (i < filled) ? '#' : '.';
    bar[8] = '\0';
    (void)snprintf(line, sizeof(line), "%s [%s] %3u max%3u", name, bar,
                   (unsigned)value, (unsigned)maximum);
    draw_text(x, y, 1, seen ? COLOR_GREEN : COLOR_MUTED, line);
}

static const char *drift_status(const PlayerDiagnostics *diagnostics)
{
    if (!diagnostics->calibrated)
        return "CALIBRACAO PENDENTE";
    if (diagnostics->waiting_for_start_release)
        return "SOLTE START PARA INICIAR A AMOSTRA";
    if (!diagnostics->drift_sample_finished)
        return "MEDINDO... NAO TOQUE NOS ANALOGICOS";
    if (diagnostics->peak_left_delta > DRIFT_THRESHOLD ||
        diagnostics->peak_right_delta > DRIFT_THRESHOLD)
        return "MOVIMENTO DETECTADO - VERIFIQUE POSSIVEL DRIFT";
    return "CENTRO ESTAVEL DURANTE ESTA AMOSTRA";
}

static uint32_t drift_status_color(const PlayerDiagnostics *diagnostics)
{
    if (!diagnostics->calibrated || !diagnostics->drift_sample_finished)
        return COLOR_YELLOW;
    if (diagnostics->peak_left_delta > DRIFT_THRESHOLD ||
        diagnostics->peak_right_delta > DRIFT_THRESHOLD)
        return COLOR_ORANGE;
    return COLOR_GREEN;
}

static void draw_full_screen(uint32_t width, uint32_t height,
                             const PlayerInput inputs[PLAYER_COUNT],
                             const PlayerDiagnostics diagnostics[PLAYER_COUNT],
                             uint32_t selected, uint32_t exit_frames)
{
    char line[120];
    const PlayerInput *player = &inputs[selected];
    const PlayerDiagnostics *diag = &diagnostics[selected];
    const INPUT_GAMEPAD *pad = &player->state.gamepad;
    const uint16_t buttons = player->connected ? pad->buttons : 0u;
    const uint8_t lt = player->connected ? pad->left_trigger : 0u;
    const uint8_t rt = player->connected ? pad->right_trigger : 0u;
    const uint32_t completed = tests_completed(diag);

    xsdk_gpu_set_target(width, height);
    draw_text(40, 20, 2, COLOR_CYAN, APP_NAME "  |  DIAGNOSTICO DE CONTROLES");
    (void)snprintf(line, sizeof(line), "Versao %s  |  Homebrew open source  |  4 jogadores", APP_VERSION);
    draw_text(42, 64, 1, COLOR_MUTED, line);

    draw_player_status(42, 102, 0, &inputs[0], selected);
    draw_player_status(344, 102, 1, &inputs[1], selected);
    draw_player_status(646, 102, 2, &inputs[2], selected);
    draw_player_status(948, 102, 3, &inputs[3], selected);

    (void)snprintf(line, sizeof(line), "DETALHES DO P%u", (unsigned)(selected + 1u));
    draw_text(42, 145, 1, COLOR_YELLOW, line);
    if (player->connected)
        draw_text(260, 145, 1, COLOR_GREEN, "CONTROLE CONECTADO");
    else
        draw_text(260, 145, 1, COLOR_RED, "AGUARDANDO CONTROLE");

    draw_text(42, 184, 1, COLOR_YELLOW, "BOTOES DIGITAIS");
    draw_button(42, 218, "A", buttons, diag, INPUT_GAMEPAD_A);
    draw_button(188, 218, "B", buttons, diag, INPUT_GAMEPAD_B);
    draw_button(334, 218, "X", buttons, diag, INPUT_GAMEPAD_X);
    draw_button(480, 218, "Y", buttons, diag, INPUT_GAMEPAD_Y);
    draw_button(42, 254, "LB", buttons, diag, INPUT_GAMEPAD_LEFT_SHOULDER);
    draw_button(188, 254, "RB", buttons, diag, INPUT_GAMEPAD_RIGHT_SHOULDER);
    draw_button(334, 254, "L3", buttons, diag, INPUT_GAMEPAD_LEFT_THUMB);
    draw_button(480, 254, "R3", buttons, diag, INPUT_GAMEPAD_RIGHT_THUMB);
    draw_button(42, 290, "BACK", buttons, diag, INPUT_GAMEPAD_BACK);
    draw_button(188, 290, "START", buttons, diag, INPUT_GAMEPAD_START);
    draw_button(334, 290, "CIMA", buttons, diag, INPUT_GAMEPAD_DPAD_UP);
    draw_button(480, 290, "BAIXO", buttons, diag, INPUT_GAMEPAD_DPAD_DOWN);
    draw_button(42, 326, "ESQUERDA", buttons, diag, INPUT_GAMEPAD_DPAD_LEFT);
    draw_button(260, 326, "DIREITA", buttons, diag, INPUT_GAMEPAD_DPAD_RIGHT);
    draw_text(42, 366, 1, COLOR_DARK, "GUIDE/HOME e controlado pelo sistema do Xbox.");

    draw_text(690, 184, 1, COLOR_YELLOW, "ANALOGICOS - LEITURA AO VIVO");
    if (player->connected) {
        (void)snprintf(line, sizeof(line), "ESQ  X:%+6d (%+4d%%)  Y:%+6d (%+4d%%)",
                       (int)pad->thumb_lx, axis_percent(pad->thumb_lx),
                       (int)pad->thumb_ly, axis_percent(pad->thumb_ly));
    } else {
        (void)snprintf(line, sizeof(line), "ESQ  X:     0 (  0%%)  Y:     0 (  0%%)");
    }
    draw_text(690, 218, 1, COLOR_WHITE, line);
    if (player->connected) {
        (void)snprintf(line, sizeof(line), "DIR  X:%+6d (%+4d%%)  Y:%+6d (%+4d%%)",
                       (int)pad->thumb_rx, axis_percent(pad->thumb_rx),
                       (int)pad->thumb_ry, axis_percent(pad->thumb_ry));
    } else {
        (void)snprintf(line, sizeof(line), "DIR  X:     0 (  0%%)  Y:     0 (  0%%)");
    }
    draw_text(690, 252, 1, COLOR_WHITE, line);

    if (diag->calibrated && player->connected) {
        const int32_t left_dx = (int32_t)pad->thumb_lx - diag->center_lx;
        const int32_t left_dy = (int32_t)pad->thumb_ly - diag->center_ly;
        const int32_t right_dx = (int32_t)pad->thumb_rx - diag->center_rx;
        const int32_t right_dy = (int32_t)pad->thumb_ry - diag->center_ry;
        (void)snprintf(line, sizeof(line), "DELTA ESQ X:%+6ld Y:%+6ld",
                       (long)left_dx, (long)left_dy);
        draw_text(690, 286, 1, COLOR_CYAN, line);
        (void)snprintf(line, sizeof(line), "DELTA DIR X:%+6ld Y:%+6ld",
                       (long)right_dx, (long)right_dy);
        draw_text(690, 316, 1, COLOR_CYAN, line);
        (void)snprintf(line, sizeof(line), "PICO DA AMOSTRA - ESQ:%5ld  DIR:%5ld",
                       (long)diag->peak_left_delta, (long)diag->peak_right_delta);
        draw_text(690, 350, 1, COLOR_WHITE, line);
    } else {
        draw_text(690, 286, 1, COLOR_MUTED, "DELTA: calibre o centro para comparar.");
        draw_text(690, 316, 1, COLOR_MUTED, "PICO DA AMOSTRA: --");
    }

    draw_text(690, 394, 1, COLOR_YELLOW, "GATILHOS ANALOGICOS");
    draw_trigger(690, 428, "LT", lt, diag->max_left_trigger,
                 (diag->seen_triggers & TRIGGER_LEFT_SEEN) != 0u);
    draw_trigger(690, 462, "RT", rt, diag->max_right_trigger,
                 (diag->seen_triggers & TRIGGER_RIGHT_SEEN) != 0u);

    (void)snprintf(line, sizeof(line), "ENTRADAS TESTADAS: %u/%u (%u%%)",
                   (unsigned)completed, (unsigned)TOTAL_TEST_COUNT,
                   (unsigned)(completed * 100u / TOTAL_TEST_COUNT));
    draw_text(42, 420, 1, completed == TOTAL_TEST_COUNT ? COLOR_GREEN : COLOR_CYAN, line);
    draw_text(42, 454, 1, completed == TOTAL_TEST_COUNT ? COLOR_GREEN : COLOR_MUTED,
              completed == TOTAL_TEST_COUNT ? "TODAS AS ENTRADAS FORAM ACIONADAS" :
                                              "APERTE CADA BOTAO E OS DOIS GATILHOS");

    draw_text(42, 510, 1, COLOR_YELLOW, "TESTE DE POSSIVEL DRIFT");
    draw_text(42, 544, 1, drift_status_color(diag), drift_status(diag));
    draw_text(42, 578, 1, COLOR_MUTED,
              "Calibrar: segure START por 0,75s com sticks parados; solte e nao toque por 3s.");
    draw_text(42, 612, 1, COLOR_MUTED,
              "Pico acima de 3500 = movimento detectado; pode ser toque, nao prova defeito.");

    draw_text(42, (int32_t)height - 46, 1, COLOR_CYAN,
              "1o controle: direcional ESQ/DIR troca jogador | X/Y vibra | START+BACK sai");
    if (exit_frames != 0u) {
        (void)snprintf(line, sizeof(line), "SAINDO... mantenha START+BACK (%u/%u)",
                       (unsigned)exit_frames, (unsigned)EXIT_HOLD_FRAMES);
        draw_text((int32_t)width - 390, (int32_t)height - 46, 1, COLOR_YELLOW, line);
    }
}

static void draw_compact_screen(uint32_t width, uint32_t height,
                                const PlayerInput inputs[PLAYER_COUNT],
                                const PlayerDiagnostics diagnostics[PLAYER_COUNT],
                                uint32_t selected, uint32_t exit_frames)
{
    char line[112];
    const PlayerInput *player = &inputs[selected];
    const PlayerDiagnostics *diag = &diagnostics[selected];
    const INPUT_GAMEPAD *pad = &player->state.gamepad;
    const uint16_t buttons = player->connected ? pad->buttons : 0u;
    const uint8_t lt = player->connected ? pad->left_trigger : 0u;
    const uint8_t rt = player->connected ? pad->right_trigger : 0u;
    const uint32_t completed = tests_completed(diag);

    xsdk_gpu_set_target(width, height);
    draw_text(16, 10, 1, COLOR_CYAN, APP_NAME "  v" APP_VERSION);
    (void)snprintf(line, sizeof(line), "P1:%s P2:%s P3:%s P4:%s | DETALHE P%u",
                   inputs[0].connected ? "ON" : "--",
                   inputs[1].connected ? "ON" : "--",
                   inputs[2].connected ? "ON" : "--",
                   inputs[3].connected ? "ON" : "--", (unsigned)(selected + 1u));
    draw_text(16, 34, 1, COLOR_WHITE, line);

    draw_button_compact(16, 68, "A", buttons, diag, INPUT_GAMEPAD_A);
    draw_button_compact(170, 68, "B", buttons, diag, INPUT_GAMEPAD_B);
    draw_button_compact(324, 68, "X", buttons, diag, INPUT_GAMEPAD_X);
    draw_button_compact(478, 68, "Y", buttons, diag, INPUT_GAMEPAD_Y);
    draw_button_compact(16, 92, "LB", buttons, diag, INPUT_GAMEPAD_LEFT_SHOULDER);
    draw_button_compact(170, 92, "RB", buttons, diag, INPUT_GAMEPAD_RIGHT_SHOULDER);
    draw_button_compact(324, 92, "L3", buttons, diag, INPUT_GAMEPAD_LEFT_THUMB);
    draw_button_compact(478, 92, "R3", buttons, diag, INPUT_GAMEPAD_RIGHT_THUMB);
    draw_button_compact(16, 116, "BACK", buttons, diag, INPUT_GAMEPAD_BACK);
    draw_button_compact(170, 116, "START", buttons, diag, INPUT_GAMEPAD_START);
    draw_button_compact(324, 116, "CIMA", buttons, diag, INPUT_GAMEPAD_DPAD_UP);
    draw_button_compact(478, 116, "BAIXO", buttons, diag, INPUT_GAMEPAD_DPAD_DOWN);
    draw_button_compact(16, 140, "ESQ", buttons, diag, INPUT_GAMEPAD_DPAD_LEFT);
    draw_button_compact(170, 140, "DIR", buttons, diag, INPUT_GAMEPAD_DPAD_RIGHT);

    if (player->connected) {
        (void)snprintf(line, sizeof(line), "ANALOG ESQ X:%+6d Y:%+6d | DIR X:%+6d Y:%+6d",
                       (int)pad->thumb_lx, (int)pad->thumb_ly,
                       (int)pad->thumb_rx, (int)pad->thumb_ry);
    } else {
        (void)snprintf(line, sizeof(line), "ANALOG ESQ X:     0 Y:     0 | DIR X:     0 Y:     0");
    }
    draw_text(16, 178, 1, COLOR_WHITE, line);

    draw_trigger_compact(16, 210, "LT", lt, diag->max_left_trigger,
                         (diag->seen_triggers & TRIGGER_LEFT_SEEN) != 0u);
    draw_trigger_compact(324, 210, "RT", rt, diag->max_right_trigger,
                         (diag->seen_triggers & TRIGGER_RIGHT_SEEN) != 0u);

    (void)snprintf(line, sizeof(line), "ENTRADAS: %u/%u (%u%%)",
                   (unsigned)completed, (unsigned)TOTAL_TEST_COUNT,
                   (unsigned)(completed * 100u / TOTAL_TEST_COUNT));
    draw_text(16, 246, 1, completed == TOTAL_TEST_COUNT ? COLOR_GREEN : COLOR_CYAN, line);

    if (diag->calibrated) {
        (void)snprintf(line, sizeof(line), "PICO DRIFT ESQ:%ld DIR:%ld",
                       (long)diag->peak_left_delta, (long)diag->peak_right_delta);
        draw_text(16, 274, 1, COLOR_WHITE, line);
    } else {
        draw_text(16, 274, 1, COLOR_YELLOW, "DRIFT: segure START por 0,75s para calibrar");
    }
    draw_text(16, 302, 1, drift_status_color(diag), drift_status(diag));
    draw_text(16, 334, 1, COLOR_MUTED, "Amostra: solte START e nao toque nos sticks por 3s.");
    draw_text(16, 358, 1, COLOR_MUTED, "3500+ indica movimento, nao confirma defeito.");
    draw_text(16, 390, 1, COLOR_CYAN, "1o controle: direcional ESQ/DIR troca | X/Y vibra | START+BACK sai");

    if (exit_frames != 0u) {
        (void)snprintf(line, sizeof(line), "SAINDO %u/%u", (unsigned)exit_frames,
                       (unsigned)EXIT_HOLD_FRAMES);
        draw_text(16, (int32_t)height - 28, 1, COLOR_YELLOW, line);
    }
}

int main(void)
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t exit_frames = 0;
    uint32_t selected_player = 0;
    uint32_t last_input_result[PLAYER_COUNT] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    uint16_t previous_navigation_buttons = 0;
    bool running = true;

    PlayerInput inputs[PLAYER_COUNT] = { 0 };
    PlayerDiagnostics diagnostics[PLAYER_COUNT] = { 0 };

    xsdk_log_write("PadCheck 360 %s starting", APP_VERSION);

    NTSTATUS status = xsdk_gpu_open();
    if (!NT_SUCCESS(status)) {
        xsdk_log_write("PadCheck: xsdk_gpu_open failed: 0x%08X", (unsigned)status);
        return 1;
    }

    status = xsdk_present_open(XSDK_DISPLAY_720P, FRAME_BUFFERS, SWAP_INTERVAL, &width, &height);
    if (!NT_SUCCESS(status)) {
        xsdk_log_write("PadCheck: xsdk_present_open failed: 0x%08X", (unsigned)status);
        (void)xsdk_gpu_close();
        return 1;
    }

    status = xsdk_text_open();
    if (!NT_SUCCESS(status)) {
        xsdk_log_write("PadCheck: xsdk_text_open failed: 0x%08X", (unsigned)status);
        xsdk_present_close();
        (void)xsdk_gpu_close();
        return 1;
    }

    xsdk_log_write("PadCheck: display %ux%u", (unsigned)width, (unsigned)height);

    while (running) {
        bool any_exit_combo = false;

        for (uint32_t player = 0; player < PLAYER_COUNT; ++player) {
            INPUT_STATE state = { 0 };
            const uint32_t result = XamInputGetState(player, 0, &state);
            const bool connected = result == ERROR_SUCCESS;

            if (result != last_input_result[player]) {
                xsdk_log_write("PadCheck: P%u input result 0x%08X",
                               (unsigned)(player + 1u), (unsigned)result);
                last_input_result[player] = result;
            }

            inputs[player].connected = connected;
            inputs[player].state = connected ? state : (INPUT_STATE){ 0 };

            if (connected) {
                update_diagnostics(&diagnostics[player], &inputs[player].state.gamepad);
                const uint16_t current = inputs[player].state.gamepad.buttons;
                if ((current & (INPUT_GAMEPAD_START | INPUT_GAMEPAD_BACK)) ==
                    (INPUT_GAMEPAD_START | INPUT_GAMEPAD_BACK)) {
                    any_exit_combo = true;
                }

                const bool request_left = (current & INPUT_GAMEPAD_Y) != 0u;
                const bool request_right = (current & INPUT_GAMEPAD_X) != 0u;
                if (request_left != inputs[player].rumble_left ||
                    request_right != inputs[player].rumble_right) {
                    set_rumble(player, request_left, request_right);
                    inputs[player].rumble_left = request_left;
                    inputs[player].rumble_right = request_right;
                }
            } else if (inputs[player].rumble_left || inputs[player].rumble_right) {
                set_rumble(player, false, false);
                inputs[player].rumble_left = false;
                inputs[player].rumble_right = false;
            }
        }

        /* The first connected controller's D-pad switches the detailed panel. */
        uint32_t navigation_player = PLAYER_COUNT;
        for (uint32_t player = 0; player < PLAYER_COUNT; ++player) {
            if (inputs[player].connected) {
                navigation_player = player;
                break;
            }
        }
        if (navigation_player < PLAYER_COUNT) {
            const uint16_t nav_buttons = inputs[navigation_player].state.gamepad.buttons;
            const bool right_pressed = (nav_buttons & INPUT_GAMEPAD_DPAD_RIGHT) != 0u;
            const bool left_pressed = (nav_buttons & INPUT_GAMEPAD_DPAD_LEFT) != 0u;
            const bool right_edge = right_pressed &&
                                    (previous_navigation_buttons & INPUT_GAMEPAD_DPAD_RIGHT) == 0u;
            const bool left_edge = left_pressed &&
                                   (previous_navigation_buttons & INPUT_GAMEPAD_DPAD_LEFT) == 0u;
            if (right_edge)
                selected_player = (selected_player + 1u) % PLAYER_COUNT;
            else if (left_edge)
                selected_player = (selected_player + PLAYER_COUNT - 1u) % PLAYER_COUNT;
            previous_navigation_buttons = nav_buttons;
        } else {
            previous_navigation_buttons = 0;
        }

        if (any_exit_combo) {
            if (exit_frames < EXIT_HOLD_FRAMES)
                ++exit_frames;
        } else {
            exit_frames = 0;
        }

        uint32_t address = 0;
        status = xsdk_present_begin(&address, PRESENT_TIMEOUT_MS);
        if (!NT_SUCCESS(status)) {
            xsdk_log_write("PadCheck: xsdk_present_begin failed: 0x%08X", (unsigned)status);
            break;
        }

        if (width < 1000u || height < 600u)
            draw_compact_screen(width, height, inputs, diagnostics, selected_player, exit_frames);
        else
            draw_full_screen(width, height, inputs, diagnostics, selected_player, exit_frames);

        xsdk_gpu_resolve(address, width, height, COLOR_BACKGROUND);
        xsdk_present_end();

        if (exit_frames >= EXIT_HOLD_FRAMES)
            running = false;
    }

    for (uint32_t player = 0; player < PLAYER_COUNT; ++player) {
        if (inputs[player].rumble_left || inputs[player].rumble_right)
            set_rumble(player, false, false);
    }

    xsdk_text_close();
    xsdk_present_close();
    status = xsdk_gpu_close();
    if (!NT_SUCCESS(status))
        xsdk_log_write("PadCheck: xsdk_gpu_close failed: 0x%08X", (unsigned)status);

    xsdk_log_write("PadCheck: closed");
    return 0;
}
