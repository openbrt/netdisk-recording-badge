#pragma once
void kuku_endurance_start(void);
// USB setup only, private endpoint/token kept in RAM and cleared on restart.
int kuku_endurance_arm(const char *url, const char *token);
int kuku_endurance_off(void);
void kuku_endurance_status(void);
