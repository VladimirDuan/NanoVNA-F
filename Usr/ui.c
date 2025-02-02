/*
 * Copyright (c) 2014-2015, TAKAHASHI Tomohiro (TTRFTECH) edy555@gmail.com
 * All rights reserved.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * The software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */
#include "cmsis_os.h"
#include "board.h"
#include "system.h"
#include "touch_ctp.h"
#include "touch_rtp.h"
#include "nanovna.h"
#include <stdlib.h>
#include <string.h>

uistat_t uistat = {
 // digit: 6,
 // current_trace: 0
 6, 0, 0, 0
};

#define NO_EVENT                    0
#define EVT_BUTTON_SINGLE_CLICK     0x01
#define EVT_BUTTON_DOUBLE_CLICK     0x02
#define EVT_BUTTON_DOWN_LONG        0x04
#define EVT_UP                      0x10
#define EVT_DOWN                    0x20
#define EVT_REPEAT                  0x40

#define BUTTON_DOWN_LONG_TICKS      500  /* 1sec  */
#define BUTTON_DOUBLE_TICKS         500  /* 500ms */
#define BUTTON_REPEAT_TICKS         100  /* 100ms */
#define BUTTON_DEBOUNCE_TICKS       20

/* lever switch assignment */
#define BIT_UP1                     K_DOWN_Pin
#define BIT_PUSH                    K_PUSH_Pin
#define BIT_DOWN1                   K_UP_Pin

#define READ_PORT                   ((uint16_t)GPIOC->IDR)
#define BUTTON_MASK                 0x0380  // PC9 PC8 PC7

static uint16_t last_button = 0; // 0b0000;
static uint32_t last_button_down_ticks;
static uint32_t last_button_repeat_ticks;
static int8_t inhibit_until_release = FALSE;

enum { OP_NONE = 0, OP_LEVER, OP_TOUCH };
uint8_t operation_requested = OP_NONE;

uint8_t previous_marker = 0;

enum {
  UI_NORMAL, UI_MENU, UI_NUMERIC, UI_KEYPAD
};

enum {
  KM_START, KM_STOP, KM_CENTER, KM_SPAN, KM_CW, KM_SCALE, KM_REFPOS, KM_EDELAY
};

uint8_t ui_mode = UI_NORMAL;
uint8_t keypad_mode;
int8_t selection = 0;

typedef struct {
  uint8_t type;
  char *label;
  char *label_cn;
  const void *reference;
} menuitem_t;

int8_t last_touch_status = FALSE;
int16_t last_touch_x;
int16_t last_touch_y;

#define EVT_TOUCH_NONE 0
#define EVT_TOUCH_DOWN 1
#define EVT_TOUCH_PRESSED 2
#define EVT_TOUCH_RELEASED 3

int awd_count;
//int touch_x, touch_y;

#define NUMINPUT_LEN 10

#define KP_CONTINUE 0
#define KP_DONE 1
#define KP_CANCEL 2

char kp_buf[11];
int8_t kp_index = 0;

void draw_frequencies(void);

void ui_mode_normal(void);
void ui_mode_menu(void);
void ui_mode_numeric(int _keypad_mode);
void ui_mode_keypad(int _keypad_mode);
void draw_menu(void);
void leave_ui_mode(void);
void erase_menu_buttons(void);
void ui_process_keypad(void);
void ui_process_numeric(void);
void touch_position(int *x, int *y);

static void menu_push_submenu(const menuitem_t *submenu);

static int btn_check(void)
{
  int cur_button = READ_PORT & BUTTON_MASK;
  int changed = last_button ^ cur_button;
  int status = 0;
  uint32_t ticks = chVTGetSystemTime();
  if (changed & (BIT_PUSH)) {
    if (ticks - last_button_down_ticks >= BUTTON_DEBOUNCE_TICKS) {
      if (cur_button & (BIT_PUSH)) {
        // button released
        status |= EVT_BUTTON_SINGLE_CLICK;
        if (inhibit_until_release) {
          status = 0;
          inhibit_until_release = FALSE;
        }
      }
      last_button_down_ticks = ticks;
    }
  }

  if (changed & (BIT_UP1)) {
    if ((cur_button & (BIT_UP1))
        && (ticks >= last_button_down_ticks + BUTTON_DEBOUNCE_TICKS)) {
      status |= EVT_UP;
    }
    last_button_down_ticks = ticks;
  }
  if (changed & (BIT_DOWN1)) {
    if ((cur_button & (BIT_DOWN1))
        && (ticks >= last_button_down_ticks + BUTTON_DEBOUNCE_TICKS)) {
      status |= EVT_DOWN;
    }
    last_button_down_ticks = ticks;
  }
  last_button = cur_button;

  return status;
}

static int btn_wait_release(void)
{
  while (TRUE) {
    int cur_button = READ_PORT & BUTTON_MASK;
    int changed = last_button ^ cur_button;
    uint32_t ticks = chVTGetSystemTime();
    int status = 0;

    if (!inhibit_until_release) {
      if ((cur_button & (BIT_PUSH))
          && ticks - last_button_down_ticks >= BUTTON_DOWN_LONG_TICKS) {
        inhibit_until_release = TRUE;
        return EVT_BUTTON_DOWN_LONG;
      }
      if ((changed & (BIT_PUSH))
          && ticks - last_button_down_ticks < BUTTON_DOWN_LONG_TICKS) {
        return EVT_BUTTON_SINGLE_CLICK;
      }
    }

    if (changed) {
      // finished
      last_button = cur_button;
      last_button_down_ticks = ticks;
      inhibit_until_release = FALSE;
      return 0;
    }

    if (ticks - last_button_down_ticks >= BUTTON_DOWN_LONG_TICKS
        && ticks - last_button_repeat_ticks >= BUTTON_REPEAT_TICKS) {
      if (cur_button & (BIT_DOWN1)) {
        status |= EVT_DOWN | EVT_REPEAT;
      }
      if (cur_button & (BIT_UP1)) {
        status |= EVT_UP | EVT_REPEAT;
      }
      last_button_repeat_ticks = ticks;
      return status;
    }
  }
}

void bubble_sort(uint16_t arr[], int len)
{
  uint16_t temp;
  int i, j;
  for (i=0; i<len-1; i++) {     /* 外循环为排序趟数，len个数进行len-1趟 */
    for (j=0; j<len-1-i; j++) { /* 内循环为每趟比较的次数，第i趟比较len-i次 */
      if (arr[j] > arr[j+1]) {  /* 相邻元素比较，若逆序则交换（升序为左大于右，降序反之） */
        temp = arr[j];
        arr[j] = arr[j+1];
        arr[j+1] = temp;
      }
    }
  }
}

#define READ_TIMES 5  // 读取次数
int
touch_measure_y(void)
{
  // return GUI_TOUCH_X_MeasureX();
  uint8_t i = 0;
  uint16_t buf[READ_TIMES] = {0};
  uint16_t avg = 0;
  while(i < READ_TIMES) {
    buf[i] = TPReadY();
    i ++;
  }
  bubble_sort(buf, i);
  avg = (buf[1]+buf[2]+buf[3])/3;
  return avg;
}

int
touch_measure_x(void)
{
  // return GUI_TOUCH_X_MeasureY();
  uint8_t i = 0;
  uint16_t buf[READ_TIMES] = {0};
  uint16_t avg = 0;
  while(i < READ_TIMES) {
    buf[i] = TPReadX();
    i ++;
  }
  bubble_sort(buf, i);
  avg = (buf[1]+buf[2]+buf[3])/3;
  return avg;
}

void
touch_prepare_sense(void)
{

}

void
touch_start_watchdog(void)  // 开启 ADC 模拟看门狗
{
  touch_prepare_sense();
  /* 开启中断 */
  __HAL_GPIO_EXTI_CLEAR_IT(TP_IRQ_Pin);
  HAL_NVIC_ClearPendingIRQ(EXTI9_5_IRQn);
  HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
}

int
touch_status(void)
{
  touch_prepare_sense();
  // return (!CTP_INT_IN());
  chThdSleepMilliseconds(20);
  if (touch_measure_x() < 4040) {
    return 1;
  } else {
    return 0;
  }
}

int touch_check(void)
{
  // uint8_t buf[7];
  int stat = touch_status();    // 读取触摸状态
  if (stat) {                   // 触摸已按下
    /*
    chThdSleepMilliseconds(10);
    int x = touch_measure_x();  // 得到坐标值
    int y = touch_measure_y();  // 得到坐标值
    //if (touch_status()) {
      last_touch_x = x;
      last_touch_y = y;
    //}
    touch_prepare_sense();
    */
    /*
    osDelay(10);                // 10ms 读取一次
    ctp_readreg(0, (uint8_t *)&buf, 7);
    if ((buf[2]&0x0F) == 1) {
        last_touch_x = (uint16_t)(buf[5] & 0x0F)<<8 | (uint16_t)buf[6];
        last_touch_x /= 2;
        if (last_touch_x > LCD_WIDTH) {
            last_touch_x = LCD_WIDTH;
        }
        last_touch_y = 479 - ((uint16_t)(buf[3] & 0x0F)<<8 | (uint16_t)buf[4]);
        last_touch_y /= 2;
        if (last_touch_y > LCD_HEIGHT) {
            last_touch_y = LCD_HEIGHT;
        }
    } else {
        return EVT_TOUCH_NONE;  // 数据不对，退出
    }
    */
    int x = touch_measure_x();  // 读取 n 次，得到坐标值
    int y = touch_measure_y();  // 读取 n 次，得到坐标值
    //if (touch_status()) {
      last_touch_x = x;
      last_touch_y = y;
    //}
    touch_prepare_sense();
  }

  if (stat != last_touch_status) {
    last_touch_status = stat;
    if (stat) {  // 触摸按下
      return EVT_TOUCH_PRESSED;   // 之前为 0，现在为 1
    } else {     // 触摸释放
      return EVT_TOUCH_RELEASED;  // 之前为 1，现在为 0
    }
  } else {
    if (stat) 
      return EVT_TOUCH_DOWN;      // 长按
    else
      return EVT_TOUCH_NONE;      // 无效
  }
}

void touch_wait_release(void)
{
  int status;
  /* wait touch release */
  do {
    status = touch_check();
  } while(status != EVT_TOUCH_RELEASED);
}

extern void nt35510_line_x2(int, int, int, int, int);

void
touch_cal_exec(void)  // 触摸校准
{
  int status;
  int x1, x2, y1, y2;

  HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);

  nt35510_fill_x2(0, 0, LCD_WIDTH, LCD_HEIGHT, 0);
  nt35510_line_x2(0, 31, 63, 31, 0xffff);
  nt35510_line_x2(31, 0, 31, 63, 0xffff);

  do {
    status = touch_check();
  } while(status != EVT_TOUCH_RELEASED);
  x1 = last_touch_x;
  y1 = last_touch_y;

  nt35510_fill_x2(0, 0, LCD_WIDTH, LCD_HEIGHT, 0);
  nt35510_line_x2(LCD_WIDTH-64, LCD_HEIGHT-32, LCD_WIDTH-1, LCD_HEIGHT-32, 0xffff);
  nt35510_line_x2(LCD_WIDTH-32, LCD_HEIGHT-64, LCD_WIDTH-32, LCD_HEIGHT-1, 0xffff);

  do {
    status = touch_check();
  } while(status != EVT_TOUCH_RELEASED);
  x2 = last_touch_x;
  y2 = last_touch_y;

  config.touch_cal[0] = x1;
  config.touch_cal[1] = y1;
  config.touch_cal[2] = (x2 - x1) * 16 / (LCD_WIDTH  - 64);
  config.touch_cal[3] = (y2 - y1) * 16 / (LCD_HEIGHT - 64);

  // dbprintf("x1 y1 x2 y2: %d %d, %d %d\r\n", x1, y1, x2, y2);

  redraw_frame();
  request_to_redraw_grid();
  //redraw_all();

  touch_start_watchdog();
}

void
touch_draw_test(void)
{
  int status;
  int x0, y0;
  int x1, y1;

  HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);

  do {
    status = touch_check();
  } while(status != EVT_TOUCH_PRESSED);    // 无键则死等
  touch_position(&x0, &y0);                // 获取坐标

  // dbprintf("Event 0, X:%3d Y:%3d\r\n", x0, y0);

  do {
    status = touch_check();
    touch_position(&x1, &y1);              // 获取坐标
    
    nt35510_line_x2(x0, y0, x1, y1, 0xffff);  // 画线
    // dbprintf("Event 1, X:%3d Y:%3d\r\n", x1, y1);

    x0 = x1;
    y0 = y1;
    osDelay(50);
  } while(status != EVT_TOUCH_RELEASED);   // 按下则死等

  // dbprintf("Event 2, X:%3d Y:%3d\r\n", x0, y0);

  touch_start_watchdog();
}

void
touch_position(int *x, int *y)  // 真正的获取坐标
{
  // *x = (last_touch_x - config.touch_cal[0]) * 16 / config.touch_cal[2];
  // *y = (last_touch_y - config.touch_cal[1]) * 16 / config.touch_cal[3];
  // *x = last_touch_x;
  // *y = last_touch_y;

  if (last_touch_x >= config.touch_cal[0]) {
    *x = (last_touch_x - config.touch_cal[0]) * 16 / config.touch_cal[2] + 31;
  } else {
    *x = 31 - (config.touch_cal[0] - last_touch_x) * 16 / config.touch_cal[2];
  }
  if (*x > LCD_WIDTH-1) {
    *x = LCD_WIDTH-1;
  }

  if (last_touch_y >= config.touch_cal[1]) {
    *y = (last_touch_y - config.touch_cal[1]) * 16 / config.touch_cal[3] + 31;
  } else {
    *y = 31 - (config.touch_cal[1] - last_touch_y) * 16 / config.touch_cal[3];
  }
  if (*y > LCD_HEIGHT-1) {
    *y = LCD_HEIGHT-1;
  }
}

// type of menu item 
enum {
  MT_NONE,
  MT_BLANK,
  MT_SUBMENU,
  MT_CALLBACK,
  MT_CANCEL,
  MT_CLOSE
};

typedef void (*menuaction_cb_t)(int item);


static void menu_move_back(void);


static void
menu_calop_cb(int item)
{
  switch (item) {
  case 0: // OPEN
    cal_collect(CAL_OPEN);
    break;
  case 1: // SHORT
    cal_collect(CAL_SHORT);
    break;
  case 2: // LOAD
    cal_collect(CAL_LOAD);
    break;
  case 3: // ISOLN
    cal_collect(CAL_ISOLN);
    break;
  case 4: // THRU
    cal_collect(CAL_THRU);
    break;
  }
  selection = item+1;
  draw_cal_status();
  draw_menu();
}

static void
menu_caldone_cb(int item)
{
  extern const menuitem_t menu_save[];
  //extern const menuitem_t menu_cal[];
  (void)item;
  cal_done();
  draw_cal_status();
  menu_move_back();
  menu_push_submenu(menu_save);
}

static void
menu_cal2_cb(int item)
{
  switch (item) {
  case 1: // RESET
    cal_status = 0;
    break;
  case 2: // CORRECTION
    // toggle applying correction
    if (cal_status)
      cal_status ^= CALSTAT_APPLY;
    draw_menu();
    break;
  }
  draw_cal_status();
  //menu_move_back();
}

static void
menu_recall_cb(int item)
{
  if (item < 0 || item >= 5)
    return;
  if (caldata_recall(item) == 0) {
    config.default_loadcal = item;
    config_save();
    menu_move_back();
    ui_mode_normal();
    update_grid();
    draw_cal_status();
  }
}

static void
menu_save_cb(int item)
{
  if (item < 0 || item >= 5)
    return;
  if (caldata_save(item) == 0) {
    config.default_loadcal = item;
    config_save();
    menu_move_back();
    ui_mode_normal();
    draw_cal_status();
  }
}

static void
menu_trace_cb(int item)
{
  extern const menuitem_t menu_trace_op[];

  if (item < 0 || item >= 4)
    return;
  if (trace[item].enabled) {
    uistat.current_trace = item;
    menu_push_submenu(menu_trace_op);
  } else {
    trace[item].enabled = TRUE;
    uistat.current_trace = item;
    menu_move_back();
    request_to_redraw_grid();
    ui_mode_normal();
    //redraw_all();
  }
}

static void
menu_format_cb(int item)
{
  switch (item) {
  case 0:
    set_trace_type(uistat.current_trace, TRC_LOGMAG);
    break;
  case 1:
    set_trace_type(uistat.current_trace, TRC_PHASE);
    break;
  case 2:
    set_trace_type(uistat.current_trace, TRC_DELAY);
    break;
  case 3:
    set_trace_type(uistat.current_trace, TRC_SMITH);
    break;
  case 4:
    set_trace_type(uistat.current_trace, TRC_SWR);
    break;
  }

  request_to_redraw_grid();
  ui_mode_normal();
  //redraw_all();
}

static void
menu_format2_cb(int item)
{
  switch (item) {
  case 0:
    set_trace_type(uistat.current_trace, TRC_POLAR);
    break;
  case 1:
    set_trace_type(uistat.current_trace, TRC_LINEAR);
    break;
  }

  request_to_redraw_grid();
  ui_mode_normal();
}

static void
menu_channel_cb(int item)
{
  if (item < 0 || item >= 2)
    return;
  set_trace_channel(uistat.current_trace, item);
  menu_move_back();
  ui_mode_normal();
}

static void
menu_touchcal_cb(int item)
{
  touch_cal_exec();
  config_save();
  menu_move_back();
  ui_mode_normal();
}

static void
menu_langset_cb(int item)
{
  if (item < 0 || item >= 2)
    return;
  config.lang = item;
  config_save();
  menu_move_back();
  ui_mode_normal();
}
/*
static void 
choose_active_marker(void)
{
  int i;
  for (i = 0; i < 4; i++)
    if (markers[i].enabled) {
      active_marker = i;
      return;
    }
  active_marker = -1;
} */

static void 
choose_active_trace(void)
{
  int i;
  for (i = 0; i < 4; i++)
    if (trace[i].enabled) {
      uistat.current_trace = i;
      return;
    }
}

static void
menu_trace_op_cb(int item)
{
  (void)item;
  int t;
  switch (item) {
  case 0: // OFF
    if (uistat.current_trace >= 0) {
      trace[uistat.current_trace].enabled = FALSE;
      choose_active_trace();
    }
    break;

  case 1: // SINGLE
    for (t = 0; t < 4; t++)
      if (uistat.current_trace != t) {
        trace[t].enabled = FALSE;
      }
    break;
  }
  menu_move_back();
  request_to_redraw_grid();
  ui_mode_normal();
  //redraw_all();
}

static void
menu_scale_cb(int item)
{
  int status;
  status = btn_wait_release();
  if (status & EVT_BUTTON_DOWN_LONG) {
    ui_mode_keypad(KM_SCALE + item);
    ui_process_keypad();
  } else {
    ui_mode_numeric(KM_SCALE + item);
    ui_process_numeric();
  }
}

static void
menu_stimulus_cb(int item)
{
  int status;
  switch (item) {
  case 0: /* START */
  case 1: /* STOP */
  case 2: /* CENTER */
  case 3: /* SPAN */
  case 4: /* CW */
    status = btn_wait_release();
    if (status & EVT_BUTTON_DOWN_LONG) {
      ui_mode_keypad(item);
      ui_process_keypad();
    } else {
      ui_mode_numeric(item);
      ui_process_numeric();
    }
    break;
  case 5: /* PAUSE */
    toggle_sweep();
    // menu_move_back();
    // ui_mode_normal();
    draw_menu();
    break;
  }
}


static int32_t
get_marker_frequency(int marker)
{
  if (marker < 0 || marker >= 4)
    return -1;
  if (!markers[marker].enabled)
    return -1;
  return frequencies[markers[marker].index];
}

static void
menu_marker_op_cb(int item)
{
  int32_t freq = get_marker_frequency(active_marker);
  if (freq < 0)
    return; // no active marker

  switch (item) {
  case 1: /* MARKER->START */
    set_sweep_frequency(ST_START, freq);
    break;
  case 2: /* MARKER->STOP */
    set_sweep_frequency(ST_STOP, freq);
    break;
  case 3: /* MARKER->CENTER */
    set_sweep_frequency(ST_CENTER, freq);
    break;
  case 4: /* MARKERS->SPAN */
    {
      if (previous_marker == active_marker)
        return;
      int32_t freq2 = get_marker_frequency(previous_marker);
      if (freq2 < 0)
        return;
      if (freq > freq2) {
        freq2 = freq;
        freq = get_marker_frequency(previous_marker);
      }
      set_sweep_frequency(ST_START, freq);
      set_sweep_frequency(ST_STOP, freq2);
#if 0
      int32_t span = (freq - freq2) * 2;
      if (span < 0) span = -span;
      set_sweep_frequency(ST_SPAN, span);
#endif
    }
    break;
  }
  ui_mode_normal();
  draw_cal_status();
  //redraw_all();
}

static void
menu_marker_sel_cb(int item)
{
  if (item >= 0 && item < 4) {
    if (active_marker == item) {
      markers[active_marker].enabled = FALSE;
      active_marker = previous_marker;
      previous_marker = 0;
      //choose_active_marker();
    } else {
      previous_marker = active_marker;
      active_marker = item;
      markers[active_marker].enabled = TRUE;
    }
  } else if (item == 4) {
    // ALL OFF
    markers[0].enabled = FALSE;
    markers[1].enabled = FALSE;
    markers[2].enabled = FALSE;
    markers[3].enabled = FALSE;
    previous_marker = 0;
    active_marker = -1;
  } 

  if (active_marker >= 0)
    redraw_marker(active_marker, TRUE);
  ui_mode_normal();
}

const menuitem_t menu_calop[] = {
  { MT_CALLBACK, "OPEN", "\x52\x61", menu_calop_cb },
  { MT_CALLBACK, "SHORT", "\x60\x61", menu_calop_cb },
  { MT_CALLBACK, "LOAD", "\x62\x63", menu_calop_cb },
  { MT_CALLBACK, "ISOLN", "\x64\x65", menu_calop_cb },
  { MT_CALLBACK, "THRU", "\x66\x17", menu_calop_cb },
  { MT_CALLBACK, "DONE", "\x67\x68", menu_caldone_cb },
  { MT_CANCEL, S_LARROW" BACK", "\x11\x12", NULL },
  { MT_NONE, NULL, NULL } // sentinel
};

const menuitem_t menu_save[] = {
  { MT_CALLBACK, "SAVE 0", "\x57\x59\x30", menu_save_cb },
  { MT_CALLBACK, "SAVE 1", "\x57\x59\x31", menu_save_cb },
  { MT_CALLBACK, "SAVE 2", "\x57\x59\x32", menu_save_cb },
  { MT_CALLBACK, "SAVE 3", "\x57\x59\x33", menu_save_cb },
  { MT_CALLBACK, "SAVE 4", "\x57\x59\x34", menu_save_cb },
  { MT_CANCEL, S_LARROW" BACK", "\x11\x12", NULL },
  { MT_NONE, NULL, NULL } // sentinel
};

const menuitem_t menu_cal[] = {
  { MT_SUBMENU, "CALIBRATE", "\x52\x45\x07\x08", menu_calop },
  { MT_CALLBACK, "RESET", "\x53\x0A\x07\x08", menu_cal2_cb },
  { MT_CALLBACK, "CORRECTION", "\x54\x55\x07\x08", menu_cal2_cb },
  { MT_CANCEL, S_LARROW" BACK", "\x11\x12", NULL },
  { MT_NONE, NULL, NULL } // sentinel
};

const menuitem_t menu_trace_op[] = {
  { MT_CALLBACK, "OFF", "\x0D\x0E", menu_trace_op_cb },
  { MT_CALLBACK, "SINGLE", "\x4D\x01", menu_trace_op_cb },
  { MT_CANCEL, S_LARROW" BACK", "\x11\x12", NULL },
  { MT_NONE, NULL, NULL } // sentinel
};

const menuitem_t menu_trace[] = {
  { MT_CALLBACK, "TRACE 0", "\x0F\x10\x30", menu_trace_cb },
  { MT_CALLBACK, "TRACE 1", "\x0F\x10\x31", menu_trace_cb },
  { MT_CALLBACK, "TRACE 2", "\x0F\x10\x32", menu_trace_cb },
  { MT_CALLBACK, "TRACE 3", "\x0F\x10\x33", menu_trace_cb },
  { MT_CANCEL, S_LARROW" BACK", "\x11\x12", NULL },
  { MT_NONE, NULL, NULL } // sentinel
};

const menuitem_t menu_format2[] = {
  { MT_CALLBACK, "POLAR", "\x27\x28\x29", menu_format2_cb },
  { MT_CALLBACK, "LINEAR", "\x19\x1A\x6F\x70", menu_format2_cb },
  { MT_CANCEL, S_LARROW" BACK", "\x11\x12", NULL },
  { MT_NONE, NULL, NULL } // sentinel
};

const menuitem_t menu_format[] = {
  { MT_CALLBACK, "LOGMAG", "\x19\x1A\x6D\x6E", menu_format_cb },
  { MT_CALLBACK, "PHASE", "\x1B\x1A\x2A", menu_format_cb },
  { MT_CALLBACK, "DELAY", "\x1C\x1D\x1E", menu_format_cb },
  { MT_CALLBACK, "SMITH", "\x1F\x20\x21", menu_format_cb },
  { MT_CALLBACK, "SWR", "\x22\x23\x24", menu_format_cb },
  { MT_SUBMENU, S_RARROW" MORE", "\x25\x26", menu_format2 },  
  //{ MT_CALLBACK, "LINEAR", menu_format_cb },
  //{ MT_CALLBACK, "SWR", menu_format_cb },
  { MT_CANCEL, S_LARROW" BACK", "\x11\x12", NULL },
  { MT_NONE, NULL, NULL } // sentinel
};

const menuitem_t menu_scale[] = {
  { MT_CALLBACK, "SCALE/DIV", "\x15\x16\x3A\x2B", menu_scale_cb },
  { MT_CALLBACK, "\2REFERENCE\0POSITION", "\x2C\x2D\x2E\x2F", menu_scale_cb },
  { MT_CALLBACK, "\2ELECTRICAL\0DELAY", "\x3B\x1D\x1E", menu_scale_cb },
  { MT_CANCEL, S_LARROW" BACK", "\x11\x12", NULL },
  { MT_NONE, NULL, NULL } // sentinel
};


const menuitem_t menu_channel[] = {
  { MT_CALLBACK, "\2S11\0REFLECT", "\x69\x6A\x3C\x3D", menu_channel_cb },
  { MT_CALLBACK, "\2S21\0THROUGH", "\x6B\x6C\x3E\x3F", menu_channel_cb },
  { MT_CANCEL, S_LARROW" BACK", "\x11\x12", NULL },
  { MT_NONE, NULL, NULL } // sentinel
};

const menuitem_t menu_langset[] = {
  { MT_CALLBACK, "ENGLISH", "\x72\x5F", menu_langset_cb },
  { MT_CALLBACK, "CHINESE", "\x5E\x5F", menu_langset_cb },
  { MT_CANCEL, S_LARROW" BACK", "\x11\x12", NULL },
  { MT_NONE, NULL, NULL } // sentinel
};

const menuitem_t menu_display[] = {
  { MT_SUBMENU, "TRACE", "\x0F\x10", menu_trace },
  { MT_SUBMENU, "FORMAT", "\x13\x14", menu_format },
  { MT_SUBMENU, "SCALE", "\x15\x16", menu_scale },
  { MT_SUBMENU, "CHANNEL", "\x17\x18", menu_channel },
  { MT_SUBMENU, "LANGSET",  "\x5C\x5D", menu_langset },
  { MT_CALLBACK, "TOUCHCAL", "\x5A\x5B\x07\x08", menu_touchcal_cb },
  { MT_CANCEL, S_LARROW" BACK", "\x11\x12", NULL },
  { MT_NONE, NULL, NULL } // sentinel
};

const menuitem_t menu_stimulus[] = {
  { MT_CALLBACK, "START", "\x44\x45", menu_stimulus_cb },
  { MT_CALLBACK, "STOP", "\x46\x47", menu_stimulus_cb },
  { MT_CALLBACK, "CENTER", "\x48\x49", menu_stimulus_cb },
  { MT_CALLBACK, "SPAN", "\x4A\x4B", menu_stimulus_cb },
  { MT_CALLBACK, "CW FREQ", "\x4D\x1A", menu_stimulus_cb },
  { MT_CALLBACK, "\2PAUSE\0SWEEP", "\x4E\x4F\x50\x51", menu_stimulus_cb },
  { MT_CANCEL, S_LARROW" BACK", "\x11\x12", NULL },
  { MT_NONE, NULL, NULL } // sentinel
};

const menuitem_t menu_marker_sel[] = {
  { MT_CALLBACK, "MARKER 1", "\x03\x04\x31", menu_marker_sel_cb },
  { MT_CALLBACK, "MARKER 2", "\x03\x04\x32", menu_marker_sel_cb },
  { MT_CALLBACK, "MARKER 3", "\x03\x04\x33", menu_marker_sel_cb },
  { MT_CALLBACK, "MARKER 4", "\x03\x04\x34", menu_marker_sel_cb },
  { MT_CALLBACK, "ALL OFF", "\x0D\x0E\x42\x43", menu_marker_sel_cb },
  { MT_CANCEL, S_LARROW" BACK", "\x11\x12", NULL },
  { MT_NONE, NULL, NULL } // sentinel
};

const menuitem_t menu_marker[] = {
  { MT_SUBMENU, "\2SELECT\0MARKER", "\x40\x41\x03\x04", menu_marker_sel },
  { MT_CALLBACK, S_RARROW"START", "\x4C\x44\x45", menu_marker_op_cb },
  { MT_CALLBACK, S_RARROW"STOP", "\x4C\x46\x47", menu_marker_op_cb },
  { MT_CALLBACK, S_RARROW"CENTER", "\x4C\x48\x49", menu_marker_op_cb },
  { MT_CALLBACK, S_RARROW"SPAN", "\x4C\x4A\x4B", menu_marker_op_cb },
  { MT_CANCEL, S_LARROW" BACK", "\x11\x12", NULL },
  { MT_NONE, NULL, NULL } // sentinel
};

const menuitem_t menu_recall[] = {
  { MT_CALLBACK, "RECALL 0", "\x56\x41\x30", menu_recall_cb },
  { MT_CALLBACK, "RECALL 1", "\x56\x41\x31", menu_recall_cb },
  { MT_CALLBACK, "RECALL 2", "\x56\x41\x32", menu_recall_cb },
  { MT_CALLBACK, "RECALL 3", "\x56\x41\x33", menu_recall_cb },
  { MT_CALLBACK, "RECALL 4", "\x56\x41\x34", menu_recall_cb },
  { MT_CANCEL, S_LARROW" BACK", "\x11\x12", NULL },
  { MT_NONE, NULL, NULL } // sentinel
};

const menuitem_t menu_recall_save[] = {
  { MT_SUBMENU, "RECALL", "\x74\x56", menu_recall },
  { MT_SUBMENU, "SAVE", "\x73\x57", menu_save },
  { MT_CANCEL, S_LARROW" BACK", "\x11\x12", NULL },
  { MT_NONE, NULL, NULL } // sentinel
};
  
const menuitem_t menu_top[] = {
  { MT_SUBMENU, "DISPLAY", "\x01\x02", menu_display },
  { MT_SUBMENU, "MARKER", "\x03\x04", menu_marker },
  { MT_SUBMENU, "STIMULUS", "\x05\x06", menu_stimulus },
  { MT_SUBMENU, "CAL", "\x07\x08", menu_cal },
  { MT_SUBMENU, "\2RECALL\0SAVE", "\2\x74\x56\0\x73\x57", menu_recall_save },
  { MT_CLOSE, "CLOSE", "\x0D\x0E", NULL },
  { MT_NONE, NULL, NULL } // sentinel
};

#define MENU_STACK_DEPTH_MAX 4
uint8_t menu_current_level = 0;
const menuitem_t *menu_stack[4] = {
  menu_top, NULL, NULL, NULL
};

static void
ensure_selection(void)
{
  const menuitem_t *menu = menu_stack[menu_current_level];
  int i;
  for (i = 0; menu[i].type != MT_NONE; i++)
    ;
  if (selection >= i)
    selection = i-1;
}

static void menu_move_back(void)
{
  if (menu_current_level == 0)
    return;
  menu_current_level--;
  ensure_selection();
  erase_menu_buttons();
  draw_menu();
}

static void menu_push_submenu(const menuitem_t *submenu)
{
  if (menu_current_level < MENU_STACK_DEPTH_MAX-1)
    menu_current_level++;
  menu_stack[menu_current_level] = submenu;
  ensure_selection();
  erase_menu_buttons();
  draw_menu();
}

/*
static void menu_move_top(void)
{
  if (menu_current_level == 0)
    return;
  menu_current_level = 0;
  ensure_selection();
  erase_menu_buttons();
  draw_menu();
}
*/

void menu_invoke(int item)
{
  const menuitem_t *menu = menu_stack[menu_current_level];
  menu = &menu[item];

  switch (menu->type) {
  case MT_NONE:
  case MT_BLANK:
  case MT_CLOSE:
    ui_mode_normal();
    break;

  case MT_CANCEL:
    menu_move_back();
    break;

  case MT_CALLBACK: {
    menuaction_cb_t cb = (menuaction_cb_t)menu->reference;
    if (cb == NULL)
      return;
    (*cb)(item);
    break;
  }

  case MT_SUBMENU:
    menu_push_submenu((const menuitem_t*)menu->reference);
    break;
  }
}

#define KP_X(x) (48*(x) + 2 + (LCD_WIDTH-64-192))
#define KP_Y(y) (48*(y) + 2)

#define KP_PERIOD 10
#define KP_MINUS 11
#define KP_X1 12
#define KP_K 13
#define KP_M 14
#define KP_G 15
#define KP_BS 16
#define KP_INF 17
#define KP_DB 18

typedef struct {
  uint16_t x, y;
  int8_t c;
} keypads_t;

const keypads_t *keypads;
uint8_t keypads_last_index;

const keypads_t keypads_freq[] = {
  { KP_X(1), KP_Y(3), KP_PERIOD },
  { KP_X(0), KP_Y(3), 0 },
  { KP_X(0), KP_Y(2), 1 },
  { KP_X(1), KP_Y(2), 2 },
  { KP_X(2), KP_Y(2), 3 },
  { KP_X(0), KP_Y(1), 4 },
  { KP_X(1), KP_Y(1), 5 },
  { KP_X(2), KP_Y(1), 6 },
  { KP_X(0), KP_Y(0), 7 },
  { KP_X(1), KP_Y(0), 8 },
  { KP_X(2), KP_Y(0), 9 },
  { KP_X(3), KP_Y(0), KP_G },
  { KP_X(3), KP_Y(1), KP_M },
  { KP_X(3), KP_Y(2), KP_K },
  { KP_X(3), KP_Y(3), KP_X1 },
  { KP_X(2), KP_Y(3), KP_BS },
  { 0, 0, -1 }
};

const keypads_t keypads_scale[] = {
  { KP_X(1), KP_Y(3), KP_PERIOD },
  { KP_X(0), KP_Y(3), 0 },
  { KP_X(0), KP_Y(2), 1 },
  { KP_X(1), KP_Y(2), 2 },
  { KP_X(2), KP_Y(2), 3 },
  { KP_X(0), KP_Y(1), 4 },
  { KP_X(1), KP_Y(1), 5 },
  { KP_X(2), KP_Y(1), 6 },
  { KP_X(0), KP_Y(0), 7 },
  { KP_X(1), KP_Y(0), 8 },
  { KP_X(2), KP_Y(0), 9 },
  { KP_X(3), KP_Y(3), KP_X1 },
  { KP_X(2), KP_Y(3), KP_BS },
  { 0, 0, -1 }
};

const keypads_t *keypads_mode_tbl[] = {
  keypads_freq, // start
  keypads_freq, // stop
  keypads_freq, // center
  keypads_freq, // span
  keypads_freq, // cw freq
  keypads_scale, // scale
  keypads_scale, // respos
  keypads_scale // electrical delay
};

const char *keypad_mode_label[] = {
  "START", "STOP", "CENTER", "SPAN", "CW FREQ", "SCALE", "REFPOS", "EDELAY"
};


void
draw_keypad(void)
{
  int i = 0;
  while (keypads[i].x) {
    uint16_t bg = config.menu_normal_color;
    if (i == selection)
      bg = config.menu_active_color;
    nt35510_fill_x2(keypads[i].x, keypads[i].y, 44, 44, bg);
    nt35510_drawfont_x2(keypads[i].c, &NF20x24, keypads[i].x+12, keypads[i].y+10, 0x0000, bg);
    i++;
  }
}

void
draw_numeric_area_frame(void)
{
    nt35510_fill_x2(0, 208, LCD_WIDTH, 32, 0xffff);
    #if USE_ILI_LCD
    nt35510_drawstring_5x7_x2(keypad_mode_label[keypad_mode], 10, 220, 0x0000, 0xffff);
    #else
    nt35510_drawstring_5x7(keypad_mode_label[keypad_mode], 10, 220, 0x0000, 0xffff);
    #endif
}

void
draw_numeric_input(const char *buf)
{
  int i = 0;
  int x = 64;  // 数字条开始位置
  int focused = FALSE;
  const uint16_t xsim[] = { 0, 0, 8, 0, 0, 8, 0, 0, 0, 0 };
  for (i = 0; i < 10 && buf[i]; i++) {
    uint16_t fg = 0x0000;
    uint16_t bg = 0xffff;
    int c = buf[i];
    if (c == '.')
      c = KP_PERIOD;
    else if (c == '-')
      c = KP_MINUS;
    else if (c >= '0' && c <= '9')
      c = c - '0';
    else
      c = -1;

    if (uistat.digit == 8-i) {
      fg = BRG556(128,255,128);
      focused = TRUE;
      if (uistat.digit_mode)
        bg = 0x0000;
    }

    if (c >= 0)
      nt35510_drawfont_x2(c, &NF20x24, x, 208+4, fg, bg);
    else if (focused)
      nt35510_drawfont_x2(0, &NF20x24, x, 208+4, fg, bg);
    else
      nt35510_fill_x2(x, 208+4, 20, 24, bg);
      
    x += 20;
    if (xsim[i] > 0) {
      //nt35510_fill_x2(x, 208+4, xsim[i], 20, bg);
      x += xsim[i];
    }
  }
}

static int
menu_is_multiline(const char *label, const char **l1, const char **l2)
{
  if (label[0] != '\2')
    return FALSE;

  *l1 = &label[1];
  *l2 = &label[1] + strlen(&label[1]) + 1;
  return TRUE;
}

static void
menu_item_modify_attribute(const menuitem_t *menu, int item,
                           uint16_t *fg, uint16_t *bg)
{
  if (menu == menu_trace && item < 4) {
    *bg = config.trace_color[item];
  } else if (menu == menu_calop) {
    if ((item == 0 && ((cal_status & CALSTAT_OPEN) || (cal_status & CALSTAT_ES)))
      || (item == 1 && ((cal_status & CALSTAT_SHORT) || (cal_status & CALSTAT_ER)))
        || (item == 2 && (cal_status & CALSTAT_LOAD))
        || (item == 3 && (cal_status & CALSTAT_ISOLN))
        || (item == 4 && ((cal_status & CALSTAT_THRU) || (cal_status & CALSTAT_ET)))) {
      *bg = 0x0000;
      *fg = 0xffff;
    }
  } else if (menu == menu_stimulus) {
    if (item == 5 /* PAUSE */ && !sweep_enabled) {
      *bg = 0x0000;
      *fg = 0xffff;
    }
  } else if (menu == menu_cal) {
    if (item == 2 /* CORRECTION */ && (cal_status & CALSTAT_APPLY)) {
      *bg = 0x0000;
      *fg = 0xffff;
    }
  }
}

void
draw_menu_buttons(const menuitem_t *menu)
{
  int i = 0;
  for (i = 0; i < 7; i++) {
    const char *l1, *l2;
    if (menu[i].type == MT_NONE)
      break;
    if (menu[i].type == MT_BLANK) 
      continue;
    int y = 32*i;
    uint16_t bg = config.menu_normal_color;
    uint16_t fg = 0x0000;
    // focus only in MENU mode but not in KEYPAD mode
    if (ui_mode == UI_MENU && i == selection)
      bg = config.menu_active_color;
    nt35510_fill_x2(LCD_WIDTH-60, y, 60, 30, bg);
    
    menu_item_modify_attribute(menu, i, &fg, &bg);
    #if USE_ILI_LCD
    if (menu_is_multiline(menu[i].label, &l1, &l2)) {
      nt35510_drawstring_5x7_x2(l1, LCD_WIDTH-54, y+8, fg, bg);
      nt35510_drawstring_5x7_x2(l2, LCD_WIDTH-54, y+15, fg, bg);
    } else {
      nt35510_drawstring_5x7_x2(menu[i].label, LCD_WIDTH-54, y+12, fg, bg);
    }
    #else
    if (config.lang == LANG_CN)
    {
        if (menu_is_multiline(menu[i].label_cn, &l1, &l2)) {
          nt35510_drawhz24x24(l1, LCD_WIDTH-54, y+4, fg, bg);
          nt35510_drawhz24x24(l2, LCD_WIDTH-54, y+16, fg, bg);
        } else {
          nt35510_drawhz24x24(menu[i].label_cn, LCD_WIDTH-54, y+8, fg, bg);
        }
    } else {
        if (menu_is_multiline(menu[i].label, &l1, &l2)) {
          nt35510_drawstring_5x7(l1, LCD_WIDTH-54, y+8, fg, bg);
          nt35510_drawstring_5x7(l2, LCD_WIDTH-54, y+15, fg, bg);
        } else {
          nt35510_drawstring_5x7(menu[i].label, LCD_WIDTH-54, y+12, fg, bg);
        }
    }
    #endif
  }
}

void
menu_select_touch(int i)
{
  selection = i;
  draw_menu();
  touch_wait_release();
  selection = -1;
  menu_invoke(i);
}

void
menu_apply_touch(void)
{
  int touch_x, touch_y;
  const menuitem_t *menu = menu_stack[menu_current_level];
  int i;

  touch_position(&touch_x, &touch_y);
  for (i = 0; i < 7; i++) {
    if (menu[i].type == MT_NONE)
      break;
    if (menu[i].type == MT_BLANK) 
      continue;
    int y = 32*i;
    if (y-2 < touch_y && touch_y < y+30+2  // 菜单宽度30，从上至下
        && LCD_WIDTH-60 < touch_x) {       // 菜单位于右侧
      menu_select_touch(i);
      return;
    }
  }

  touch_wait_release();
  ui_mode_normal();
}

void
draw_menu(void)
{
  draw_menu_buttons(menu_stack[menu_current_level]);
}

void
erase_menu_buttons(void)
{
  uint16_t bg = 0;
  nt35510_fill_x2(LCD_WIDTH-60, 0, 60, 32*7, bg);
}

void
erase_numeric_input(void)
{
  uint16_t bg = 0;
  nt35510_fill_x2(0, LCD_HEIGHT-32, LCD_WIDTH, 32, bg);
}

void
leave_ui_mode()
{
  if (ui_mode == UI_MENU) {
    request_to_draw_cells_behind_menu();
    erase_menu_buttons();
  } else if (ui_mode == UI_NUMERIC) {
    request_to_draw_cells_behind_numeric_input();
    erase_numeric_input();
    draw_frequencies();
  }
}

void
fetch_numeric_target(void)
{
  switch (keypad_mode) {
  case KM_START:
    uistat.value = get_sweep_frequency(ST_START);
    break;
  case KM_STOP:
    uistat.value = get_sweep_frequency(ST_STOP);
    break;
  case KM_CENTER:
    uistat.value = get_sweep_frequency(ST_CENTER);
    break;
  case KM_SPAN:
    uistat.value = get_sweep_frequency(ST_SPAN);
    break;
  case KM_CW:
    uistat.value = get_sweep_frequency(ST_CW);
    break;
  case KM_SCALE:
    uistat.value = get_trace_scale(uistat.current_trace) * 1000;
    break;
  case KM_REFPOS:
    uistat.value = get_trace_refpos(uistat.current_trace) * 1000;
    break;
  case KM_EDELAY:
    uistat.value = get_electrical_delay();
    break;
  }
}

void set_numeric_value(void)
{
  switch (keypad_mode) {
  case KM_START:
    set_sweep_frequency(ST_START, uistat.value);
    break;
  case KM_STOP:
    set_sweep_frequency(ST_STOP, uistat.value);
    break;
  case KM_CENTER:
    set_sweep_frequency(ST_CENTER, uistat.value);
    break;
  case KM_SPAN:
    set_sweep_frequency(ST_SPAN, uistat.value);
    break;
  case KM_CW:
    set_sweep_frequency(ST_CW, uistat.value);
    break;
  case KM_SCALE:
    set_trace_scale(uistat.current_trace, uistat.value / 1000.0);
    break;
  case KM_REFPOS:
    set_trace_refpos(uistat.current_trace, uistat.value / 1000.0);
    break;
  case KM_EDELAY:
    set_electrical_delay(uistat.value);
    break;
  }
}

void
draw_numeric_area(void)
{
  char buf[10];
  chsnprintf(buf, sizeof buf, "%9d", uistat.value);
  draw_numeric_input(buf);
}


void
ui_mode_menu(void)
{
  if (ui_mode == UI_MENU) 
    return;

  ui_mode = UI_MENU;
  /* narrowen plotting area */
#if USE_ILI_LCD
  area_width = AREA_WIDTH_NORMAL - (64-8);
#else
  // 有效的刷新面积减去按钮的宽度
  area_width = AREA_WIDTH_NORMAL - (64-8-16);
#endif
  area_height = HEIGHT;
  ensure_selection();
  draw_menu();
}

void
ui_mode_numeric(int _keypad_mode)
{
  if (ui_mode == UI_NUMERIC) 
    return;

  leave_ui_mode();
  
  // keypads array
  keypad_mode = _keypad_mode;
  ui_mode = UI_NUMERIC;
  area_width = AREA_WIDTH_NORMAL;
  area_height = LCD_HEIGHT-32;//HEIGHT - 32;

  draw_numeric_area_frame();
  fetch_numeric_target();
  draw_numeric_area();
}

void
ui_mode_keypad(int _keypad_mode)
{
  if (ui_mode == UI_KEYPAD) 
    return;

  // keypads array
  keypad_mode = _keypad_mode;
  keypads = keypads_mode_tbl[_keypad_mode];
  int i;
  for (i = 0; keypads[i+1].c >= 0; i++)
    ;
  keypads_last_index = i;

  ui_mode = UI_KEYPAD;
#if USE_ILI_LCD
  area_width = AREA_WIDTH_NORMAL - (64-8);
#else
  area_width = AREA_WIDTH_NORMAL - (64-8-16);
#endif
  area_height = HEIGHT - 32;
  draw_menu();
  draw_keypad();
  draw_numeric_area_frame();
  draw_numeric_input("");
}

void
ui_mode_normal(void)
{
  if (ui_mode == UI_NORMAL) 
    return;

  area_width = AREA_WIDTH_NORMAL;
  area_height = HEIGHT;
  leave_ui_mode();
  ui_mode = UI_NORMAL;
}

void
ui_process_normal(void)
{
  int status = btn_check();
  if (status != 0) {
    if (status & EVT_BUTTON_SINGLE_CLICK) {
      ui_mode_menu();
    } else {
      do {
        if (active_marker >= 0 && markers[active_marker].enabled) {
          if ((status & EVT_DOWN) && markers[active_marker].index > 0) {
            markers[active_marker].index--;
            markers[active_marker].frequency = frequencies[markers[active_marker].index];
            redraw_marker(active_marker, FALSE);
          }
          if ((status & EVT_UP) && markers[active_marker].index < 100) {
            markers[active_marker].index++;
            markers[active_marker].frequency = frequencies[markers[active_marker].index];
            redraw_marker(active_marker, FALSE);
          }
        }
        status = btn_wait_release();
      } while (status != 0);
      if (active_marker >= 0)
        redraw_marker(active_marker, TRUE);
    }
  }
}

void
ui_process_menu(void)
{
  int status = btn_check();
  if (status != 0) {
    if (status & EVT_BUTTON_SINGLE_CLICK) {
      menu_invoke(selection);
    } else {
      do {
        if (status & EVT_UP
            && menu_stack[menu_current_level][selection+1].type != MT_NONE) {
          selection++;
          draw_menu();
        }
        if (status & EVT_DOWN
            && selection > 0) {
          selection--;
          draw_menu();
        }
        status = btn_wait_release();
      } while (status != 0);
    }
  }
}

/* 数字条间隔 20 20 20 8 20 20 20 8 20 20 20 20 */
uint16_t
get_postion(uint8_t d)
{
  switch(d)
  {
  case 0:
    return 64;
  case 1:
    return 84;
  case 2:
    return 104;
  case 3:
    return 132;
  case 4:
    return 152;
  case 5:
    return 172;
  case 6:
    return 200;
  case 7:
    return 220;
  case 8:
    return 240;
  case 9:
    return 260;
  default:
    return 64;
  }
}

int
keypad_click(int key) 
{
  int c = keypads[key].c;
  if (c >= KP_X1 && c <= KP_G) {
    int n = c - KP_X1;
    int scale = 1;
    while (n-- > 0)
      scale *= 1000;
    /* numeric input done */
    double value = my_atof(kp_buf) * scale;
    switch (keypad_mode) {
    case KM_START:
      set_sweep_frequency(ST_START, (int)value);
      break;
    case KM_STOP:
      set_sweep_frequency(ST_STOP, (int)value);
      break;
    case KM_CENTER:
      set_sweep_frequency(ST_CENTER, (int)value);
      break;
    case KM_SPAN:
      set_sweep_frequency(ST_SPAN, (int)value);
      break;
    case KM_CW:
      set_sweep_frequency(ST_CW, (int)value);
      break;
    case KM_SCALE:
      set_trace_scale(uistat.current_trace, (float)value);
      break;
    case KM_REFPOS:
      set_trace_refpos(uistat.current_trace, (float)value);
      break;
    case KM_EDELAY:
      set_electrical_delay((float)value);
      break;
    }

    return KP_DONE;
  } else if (c <= 9 && kp_index < NUMINPUT_LEN)
    kp_buf[kp_index++] = '0' + c;
  else if (c == KP_PERIOD && kp_index < NUMINPUT_LEN) {
    // check period in former input
    int j;
    for (j = 0; j < kp_index && kp_buf[j] != '.'; j++)
      ;
    // append period if there are no period
    if (kp_index == j)
      kp_buf[kp_index++] = '.';
  } else if (c == KP_BS) {
    if (kp_index == 0) {
      return KP_CANCEL;
    }
    --kp_index;
    nt35510_fill_x2(get_postion(kp_index), 208+4, 20, 24, 0xffff);
  }
  kp_buf[kp_index] = '\0';
  draw_numeric_input(kp_buf);
  return KP_CONTINUE;
}

int
keypad_apply_touch(void)
{
  int touch_x, touch_y;
  int i = 0;

  touch_position(&touch_x, &touch_y);

  while (keypads[i].x) {
    if (keypads[i].x-2 < touch_x && touch_x < keypads[i].x+44+2
        && keypads[i].y-2 < touch_y && touch_y < keypads[i].y+44+2) {
      // draw focus
      selection = i;
      draw_keypad();
      touch_wait_release();
      // erase focus
      selection = -1;
      draw_keypad();
      return i;
    }
    i++;
  }
  return -1;
}

/* 数字条间隔 20 20 20 8 20 20 20 8 20 20 20 20 */
uint8_t
get_digit(uint16_t x)
{
  if (x < 84) {
    return 9;
  }
  if (x > 84 && x < 104) {
    return 8;
  }
  if (x > 104 && x < 128) {
    return 7;
  }

  if (x > 128 && x < 152) {
    return 6;
  }
  if (x > 152 && x < 172) {
    return 5;
  }
  if (x > 172 && x < 196) {
    return 4;
  }

  if (x > 196 && x < 220) {
    return 3;
  }
  if (x > 220 && x < 240) {
    return 2;
  }
  return 1;
}

void
numeric_apply_touch(void)
{
  int touch_x, touch_y;
  int i = 0;
  int step;
  touch_position(&touch_x, &touch_y);

  if (touch_y < LCD_HEIGHT-40) {
    ui_mode_normal();
    return;
  }
  
  if (touch_x < 64) {  // 按下左下角，确认输入
    set_numeric_value();
    ui_mode_normal();
    return;
  }
  
  if (touch_x > 64+9*20+8+8) {
    ui_mode_keypad(keypad_mode);  // 按下数字条后部，弹出大键盘
    ui_process_keypad();
    return;
  }
  if (touch_y < LCD_HEIGHT-20) {
    step = 1;
  } else {
    step = -1;
  }
  
  // i = 9 - (touch_x - 64) / 20;
  i = get_digit(touch_x) - 1;
  uistat.digit = i;
  uistat.digit_mode = TRUE;
  for (i = uistat.digit; i > 0; i--)
    step *= 10;
  uistat.value += step;
  draw_numeric_area();
  
  touch_wait_release();
  uistat.digit_mode = FALSE;
  draw_numeric_area();
  
  return;
}

void
ui_process_numeric(void)
{
  int status = btn_check();

  if (status != 0) {
    if (status == EVT_BUTTON_SINGLE_CLICK) {
      status = btn_wait_release();
      if (uistat.digit_mode) {
        if (status & (EVT_BUTTON_SINGLE_CLICK | EVT_BUTTON_DOWN_LONG)) {
          uistat.digit_mode = FALSE;
          draw_numeric_area();
        }
      } else {
        if (status & EVT_BUTTON_DOWN_LONG) {
          uistat.digit_mode = TRUE;
          draw_numeric_area();
        } else if (status & EVT_BUTTON_SINGLE_CLICK) {
          set_numeric_value();
          ui_mode_normal();
        }
      }
    } else {
      do {
        if (uistat.digit_mode) {
          if (status & EVT_DOWN) {
            if (uistat.digit < 8) {
              uistat.digit++;
              draw_numeric_area();
            } else {
              goto exit;
            }
          }
          if (status & EVT_UP) {
            if (uistat.digit > 0) {
              uistat.digit--;
              draw_numeric_area();
            } else {
              goto exit;
            }
          }
        } else {
          int32_t step = 1;
          int n;
          for (n = uistat.digit; n > 0; n--)
            step *= 10;
          if (status & EVT_DOWN) {
            uistat.value += step;
            draw_numeric_area();
          }
          if (status & EVT_UP) {
            uistat.value -= step;
            draw_numeric_area();
          }
        }
        status = btn_wait_release();
      } while (status != 0);
    }
  }

  return;

 exit:
  // cancel operation
  ui_mode_normal();
}

void
ui_process_keypad(void)
{
  int status;

  HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);

  kp_index = 0;
  while (TRUE) {
    status = btn_check();
    if (status & (EVT_UP|EVT_DOWN)) {
      int s = status;
      do {
        if (s & EVT_UP) {
          selection--;
          if (selection < 0)
            selection = keypads_last_index;
          draw_keypad();
        }
        if (s & EVT_DOWN) {
          selection++;
          if (keypads[selection].c < 0) {
            // reaches to tail
            selection = 0;
          }
          draw_keypad();
        }
        s = btn_wait_release();
      } while (s != 0);
    }

    if (status == EVT_BUTTON_SINGLE_CLICK) {
      if (keypad_click(selection))
        /* exit loop on done or cancel */
        break; 
    }

    status = touch_check();
    if (status == EVT_TOUCH_PRESSED) {
      int key = keypad_apply_touch();
      if (key >= 0 && keypad_click(key))
        /* exit loop on done or cancel */
        break; 
    }
  }

  redraw_frame();
  request_to_redraw_grid();
  ui_mode_normal();
  //redraw_all();
  touch_start_watchdog();
}

void
ui_process_lever(void)
{
  switch (ui_mode) {
  case UI_NORMAL:
    ui_process_normal();
    break;    
  case UI_MENU:
    ui_process_menu();
    break;    
  case UI_NUMERIC:
    ui_process_numeric();
    break;    
  case UI_KEYPAD:
    ui_process_keypad();
    break;    
  }
}


void drag_marker(int t, int m)
{
  int status;
  /* wait touch release */
  do {
    int touch_x, touch_y;
    int index;
    touch_position(&touch_x, &touch_y);
    touch_x -= OFFSETX;
    touch_y -= OFFSETY;
    index = search_nearest_index(touch_x, touch_y, t);
    if (index >= 0) {
      markers[m].index = index;
      markers[m].frequency = frequencies[index];
      redraw_marker(m, TRUE);
    }

    status = touch_check();
  } while(status != EVT_TOUCH_RELEASED);
}

static int 
sq_distance(int x0, int y0)
{
  return x0*x0 + y0*y0;
}

/*
=======================================
    拾取标记点
=======================================
*/
int
touch_pickup_marker(void)
{
  int touch_x, touch_y;
  int m, t;
  touch_position(&touch_x, &touch_y);
  touch_x -= OFFSETX;  // -15
  touch_y -= OFFSETY;  // -0

  for (m = 0; m < 4; m++) {
    if (!markers[m].enabled)
      continue;

    for (t = 0; t < 4; t++) {
      int x, y;
      if (!trace[t].enabled)
        continue;

      marker_position(m, t, &x, &y);

      if (sq_distance(x - touch_x, y - touch_y) < 20*20) {  // 直线距离 20像素
        if (active_marker != m) {
          previous_marker = active_marker;
          active_marker = m;
          redraw_marker(active_marker, TRUE);
        }
        // select trace
        uistat.current_trace = t;
        
        // drag marker until release
        drag_marker(t, m);  // 拖动标记点
        return TRUE;
      }
    }
  }

  return FALSE;
}

void
ui_process_touch(void)
{
  awd_count++;
  HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);

  int status = touch_check();
  if (status == EVT_TOUCH_PRESSED || status == EVT_TOUCH_DOWN) {
    switch (ui_mode) {
    case UI_NORMAL:
      // 是否拖动标记
      if (touch_pickup_marker()) {
        break;
      }
      // 否则打开菜单
      touch_wait_release();

      // switch menu mode
      selection = -1;
      ui_mode_menu();  // 打开菜单
      break;

    case UI_MENU:      // 设置菜单
      menu_apply_touch();
      break;

    case UI_NUMERIC:   // 数字条模式
      numeric_apply_touch();
      break;
    }
  }
  touch_start_watchdog();
}

void
ui_process(void)
{
  switch (operation_requested) {
  case OP_LEVER:  // 波轮
    ui_process_lever();
    break;
  case OP_TOUCH:  // 触摸
    ui_process_touch();
    break;
  }
  operation_requested = OP_NONE;
  /*
  if (!CTP_INT_IN()) {
      ui_process_touch();
  }
  */
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if ((GPIO_Pin == K_UP_Pin) || 
    (GPIO_Pin == K_PUSH_Pin) || 
    (GPIO_Pin == K_DOWN_Pin))
  {
    operation_requested = OP_LEVER;
    // beep_open(100);
  }

  if (GPIO_Pin == GPIO_PIN_6) {
    operation_requested = OP_TOUCH;
    g_TP_Irq = 1;
  }
}

void
test_touch(int *x, int *y)
{
  HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);

  *x = touch_measure_x();
  *y = touch_measure_y();

  touch_start_watchdog();
}

void
handle_touch_interrupt(void)
{
  operation_requested = OP_TOUCH;
}

void
ui_init()
{
  nt35510_drawstring(&font_12x24,  "NanoVNA-F "APP_VERSION,            304, 200, BRG556(0,0,255), 0x0000);
  nt35510_drawstring(&font_12x24,  "Handheld Vector Network Analyzer", 208, 224, BRG556(0,0,255), 0x0000);
  nt35510_drawstring(&font_12x24,  "www.nanovna-f.com",                300, 248, BRG556(0,0,255), 0x0000);
}
