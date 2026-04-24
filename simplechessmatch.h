#include "gamemanager.h"
#include <boost/program_options.hpp>
#include <fstream>
#include <cmath>
#include <iomanip>
#include <algorithm>
#ifdef WIN32
#include <conio.h>
#else
#include <signal.h>
#endif
#ifdef __linux__
#include <termios.h>
#endif

#define MAX_THREADS 32
#define MAX_RETRIES_PER_PAIR 5

struct PairRecord {
   game_result g1 = UNFINISHED;
   game_result g2 = UNFINISHED;
};

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
   bool m_engines_shut_down;
   fstream m_results_log_file;

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

   enum SPRT_Decision {
      SPRT_NONE = -1,
      SPRT_H0 = 0,
      SPRT_H1 = 1
   };
   SPRT_Decision m_sprt_decision;

   vector<PairRecord> m_pair_records;
   vector<string> m_fens_list;
   uint m_engine1_wins_total;
   uint m_engine2_wins_total;
   uint m_draws_total;
   uint m_illegal_moves_total;
   int m_penta[5]; // Indices: 0=LL, 1=LD, 2=WL/DD, 3=WD, 4=WW
   int m_completed_pairs;

public:
   MatchManager(void);
   ~MatchManager(void);
   void cleanup(void);
   void main_loop(void);
   int initialize(void);
   int load_all_engines(void);
   void set_engine_options(Engine *engine);
   void send_engine_custom_commands(Engine *engine);
   void print_results(bool clear_screen = true);
   void shut_down_all_engines(void);

private:
   bool match_completed(void);
   bool new_game_can_start(void);
   uint num_games_in_progress(void);
   bool get_next_game(string &fen, bool &swap_sides, uint &pair_id);
   void update_stats_from_records(void);
   bool check_for_crashes(void);
   
   // Fishtest Statistical LLR Functions
   double secular(const double a[5], const double p[5]);
   void MLE_expected(const double a[5], const double p[5], double s, double p_MLE[5]);
   void MLE_t_value(const double a[5], const double p_hat[5], double ref, double t_target, double p_MLE[5]);
   double LLR_logistic(const double p_hat[5], double s0, double s1);
   double LLR_normalized(const double p_hat[5], double nelo0, double nelo1);
};