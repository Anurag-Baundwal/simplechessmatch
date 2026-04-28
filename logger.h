#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <fstream>
#include "engine.h"

using namespace std;

extern ofstream g_event_log;
extern ofstream g_debug1_log;
extern ofstream g_debug2_log;
extern ofstream g_moves_log;

void log_event(const string &msg);
void log_debug(engine_number num, const string &msg);
void log_move(const string &msg);

#endif