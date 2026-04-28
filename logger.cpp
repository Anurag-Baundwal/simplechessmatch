#include "logger.h"
#include <mutex>

ofstream g_event_log;
ofstream g_debug1_log;
ofstream g_debug2_log;
ofstream g_moves_log;

std::mutex g_log_mutex;

void log_event(const string &msg)
{
   std::lock_guard<std::mutex> lock(g_log_mutex);
   if (g_event_log.is_open())
   {
      g_event_log << msg << "\n";
      g_event_log.flush();
   }
}

void log_debug(engine_number num, const string &msg)
{
   std::lock_guard<std::mutex> lock(g_log_mutex);
   if (num == FIRST && g_debug1_log.is_open())
   {
      g_debug1_log << msg << "\n";
      g_debug1_log.flush();
   }
   else if (num == SECOND && g_debug2_log.is_open())
   {
      g_debug2_log << msg << "\n";
      g_debug2_log.flush();
   }
}

void log_move(const string &msg)
{
   std::lock_guard<std::mutex> lock(g_log_mutex);
   if (g_moves_log.is_open())
   {
      g_moves_log << msg << "\n";
      g_moves_log.flush();
   }
}