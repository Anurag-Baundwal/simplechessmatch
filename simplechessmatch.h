#include "gamemanager.h"
#include <boost/program_options.hpp>
#include <fstream>
#include <cmath>
#include <iomanip>
#include <algorithm> // For std::max
#ifdef WIN32
#include <conio.h>
#else
#include <signal.h>
#endif
#ifdef __linux__
#include <termios.h>
#endif

#define MAX_THREADS 32

int parse_cmd_line_options(int argc, char* argv[]);
#ifdef WIN32
BOOL WINAPI ctrl_c_handler(DWORD fdwCtrlType);
#else
void ctrl_c_handler(int s);
int _kbhit(void);
#endif

class MatchManager
{
public:
   GameManager *m_game_mgr;

private:
   thread *m_thread;
   uint m_total_games_started;
   bool m_engines_shut_down;
   fstream m_FENs_file;
   fstream m_pgn_file;

   // SPRT related members
   bool m_sprt_enabled;
   double m_sprt_elo0;
   double m_sprt_elo1;
   double m_sprt_alpha;
   double m_sprt_beta;
   double m_sprt_lower_bound;
   double m_sprt_upper_bound;
   double m_sprt_llr;
   bool m_sprt_test_finished;

public:
   MatchManager(void);
   ~MatchManager(void);
   void cleanup(void);
   void main_loop(void);
   int initialize(void);
   int load_all_engines(void);
   void set_engine_options(Engine *engine);
   void send_engine_custom_commands(Engine *engine);
   void print_results(void);
   void save_pgn(void);
   void shut_down_all_engines(void);

private:
   bool match_completed(void);
   bool new_game_can_start(void);
   uint num_games_in_progress(void);
   int get_next_fen(string &fen);
};