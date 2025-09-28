#ifndef APP_H
#define APP_H

extern void app_init();
extern void app_on_encoder_change(int delta);
extern void app_tick();
extern void app_on_click();

#endif // APP_H
