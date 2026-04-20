#include "simplechessmatch.h"

namespace po = boost::program_options;

struct options_info options;
MatchManager match_mgr;

// Helper function to convert elo to expected score
double elo_to_score(double elo) {
    return 1.0 / (1.0 + pow(10.0, -elo / 400.0));
}

int main(int argc, char* argv[])
{
   cout << "simplechessmatch\n";

#ifdef WIN32
   SetConsoleCtrlHandler(ctrl_c_handler, TRUE);
#else
   struct sigaction sig_handler;
   sig_handler.sa_handler = ctrl_c_handler;
   sigemptyset(&sig_handler.sa_mask);
   sig_handler.sa_flags = 0;
   sigaction(SIGINT, &sig_handler, NULL);
   // Prevent crashing on Linux if writing to a broken pipe after an engine crash
   signal(SIGPIPE, SIG_IGN);
#endif

   if (parse_cmd_line_options(argc, argv) == 0)
      return 0;

   if (match_mgr.initialize() == 0)
      return 0;

   cout << "loading engines...\n";

   if (match_mgr.load_all_engines() == 0)
   {
      match_mgr.shut_down_all_engines();
      match_mgr.cleanup();
      return 0;
   }

   cout << "engines loaded.\n";

   for (uint i = 0; i < options.num_threads; i++)
   {
      match_mgr.set_engine_options(&(match_mgr.m_game_mgr[i].m_engine1));
      match_mgr.send_engine_custom_commands(&(match_mgr.m_game_mgr[i].m_engine1));
      match_mgr.set_engine_options(&(match_mgr.m_game_mgr[i].m_engine2));
      match_mgr.send_engine_custom_commands(&(match_mgr.m_game_mgr[i].m_engine2));
   }

   match_mgr.main_loop();

   match_mgr.shut_down_all_engines();

   // Do not clear the screen on final print so any crash output remains readable
   match_mgr.print_results(false);

   match_mgr.cleanup();

   cout << "Exiting.\n";

   return 0;
}

MatchManager::MatchManager(void)
{
   m_engines_shut_down = false;
   m_game_mgr = nullptr;
   m_thread = nullptr;
   m_sprt_enabled = false;
   m_sprt_test_finished = false;
   m_sprt_llr = 0.0;
   m_sprt_lower_bound = 0.0;
   m_sprt_upper_bound = 0.0;
   m_sprt_elo0 = 0.0;
   m_sprt_elo1 = 0.0;
   m_sprt_alpha = 0.0;
   m_sprt_beta = 0.0;
   m_sprt_decision = SPRT_NONE;
   m_engine1_wins_total = 0;
   m_engine2_wins_total = 0;
   m_draws_total = 0;
   m_illegal_moves_total = 0;
   m_completed_pairs = 0;
   for (int i = 0; i < 5; i++) m_penta[i] = 0;
}

MatchManager::~MatchManager(void)
{
}

void MatchManager::cleanup(void)
{
   if (m_results_log_file.is_open())
      m_results_log_file.close();

   for (uint i = 0; i < options.num_threads; i++)
      if (m_thread[i].joinable())
         m_thread[i].join();

   delete[] m_game_mgr;
   delete[] m_thread;
}

void MatchManager::main_loop(void)
{
#if defined(WIN32) || defined(__linux__)
   cout << "\n***** Press any key to exit and terminate match *****\n\n";
#else
   cout << "\n***** Press Ctrl-C to exit and terminate match *****\n\n";
#endif

   while (!match_completed())
   {
      // --- Pass 1: Join any completed threads and record their results. ---
      bool should_abort = false;

      for (uint i = 0; i < options.num_threads; i++)
      {
         if (!m_game_mgr[i].m_thread_running && m_thread[i].joinable())
         {
            m_thread[i].join();

            bool was_disconnected = m_game_mgr[i].m_engine_disconnected.exchange(false);
            bool was_error        = m_game_mgr[i].m_error.exchange(false);

            game_result final_res = m_game_mgr[i].m_final_result;
            uint pid = m_game_mgr[i].m_pair_id;
            bool swap_sides = m_game_mgr[i].m_swap_sides;

            bool is_disconnect   = was_disconnected;
            bool is_recoverable  = was_error && options.continue_on_error && !is_disconnect;

            // Write result to log if the game finished validly, OR if it's a
            // recoverable error we want to skip over (like illegal move with --continue).
            if (m_game_mgr[i].m_is_valid_game || is_recoverable)
            {
               if (!swap_sides) m_pair_records[pid].g1 = final_res;
               else             m_pair_records[pid].g2 = final_res;

               if (m_results_log_file.is_open()) {
                  m_results_log_file << pid << "," << (swap_sides ? 1 : 0) << "," << (int)final_res << "\n";
                  m_results_log_file.flush();
               }

               update_stats_from_records();
            }
            else
            {
               // It's an unrecoverable error. We do NOT record it to the file.
               // It stays IN_PROGRESS/UNFINISHED, so when the app crashes out, it will
               // be retried naturally on the next run with --resume.
               should_abort = true;
            }
         }
      }

      // If a crash happened during Pass 1, abort gracefully.
      if (should_abort) {
         cout << "\n[!] Crash or error detected! Aborting match.\n";
         cout << "Resume from where it left off by adding: --resume results_log.txt\n";
         return;
      }

      // --- Pass 2: Abort if any thread is frozen/unresponsive. ---
      // This catches threads that are alive but stuck (haven't joined yet).
      if (check_for_crashes()) {
         cout << "\n[!] Engine unresponsive detected! Aborting match.\n";
         cout << "Resume from where it left off by adding: --resume results_log.txt\n";
         return;
      }

      // --- Pass 3: Dispatch new games to idle threads. ---
      // An idle thread is one where m_thread_running==false AND the thread object 
      // is no longer joinable (already joined in Pass 1). Enforces thread limits correctly.
      for (uint i = 0; i < options.num_threads; i++)
      {
         if (!m_game_mgr[i].m_thread_running && !m_thread[i].joinable())
         {
            if (m_sprt_enabled && m_sprt_test_finished) break;
            if (!new_game_can_start()) break;

            string fen;
            bool swap_sides;
            uint pair_id;

            if (!get_next_game(fen, swap_sides, pair_id))
               continue;

            m_game_mgr[i].m_fen = fen;
            m_game_mgr[i].m_swap_sides = swap_sides;
            m_game_mgr[i].m_pair_id = pair_id;

            m_game_mgr[i].m_thread_running = true;
            m_thread[i] = thread(&GameManager::game_runner, &m_game_mgr[i]);
         }
      }

      print_results(true);

      if (_kbhit())
      {
         cout << "\nKey pressed, terminating match.\n";
         return;
      }

      this_thread::sleep_for(50ms);
   }
}

bool MatchManager::match_completed(void)
{
   if (m_sprt_enabled && m_sprt_test_finished) return (num_games_in_progress() == 0);

   uint total_done = 0;
   for (uint i = 0; i < m_pair_records.size(); i++) {
      if (m_pair_records[i].g1 != UNFINISHED && m_pair_records[i].g1 != IN_PROGRESS) total_done++;
      if (m_pair_records[i].g2 != UNFINISHED && m_pair_records[i].g2 != IN_PROGRESS) total_done++;
   }
   return (total_done >= options.num_games_to_play && num_games_in_progress() == 0);
}

bool MatchManager::new_game_can_start(void)
{
   if (m_sprt_enabled && m_sprt_test_finished) return false;

   // Only check whether there is a pair slot waiting to be filled.
   // The thread-count cap is enforced structurally by Pass 3 of main_loop,
   // which only enters this function for threads that are already idle.
   int num_pairs = options.num_games_to_play / 2;
   for (int i = 0; i < num_pairs; i++) {
      if (m_pair_records[i].g1 == UNFINISHED || m_pair_records[i].g2 == UNFINISHED)
         return true;
   }
   return false;
}

uint MatchManager::num_games_in_progress(void)
{
   uint games = 0;
   for (uint i = 0; i < options.num_threads; i++)
      if (m_game_mgr[i].m_thread_running)
         games++;
   return games;
}

bool MatchManager::get_next_game(string &fen, bool &swap_sides, uint &pair_id)
{
   int num_pairs = options.num_games_to_play / 2;
   for (int i = 0; i < num_pairs; i++) {
      if (m_pair_records[i].g1 == UNFINISHED) {
         m_pair_records[i].g1 = IN_PROGRESS; // in-memory sentinel; never written to disk
         fen        = (i < (int)m_fens_list.size()) ? m_fens_list[i] : "";
         swap_sides = false;
         pair_id    = i;
         return true;
      }
      if (m_pair_records[i].g2 == UNFINISHED) {
         m_pair_records[i].g2 = IN_PROGRESS; // in-memory sentinel; never written to disk
         fen        = (i < (int)m_fens_list.size()) ? m_fens_list[i] : "";
         swap_sides = true;
         pair_id    = i;
         return true;
      }
   }
   return false;
}

void MatchManager::update_stats_from_records(void)
{
   uint e1_wins = 0, e2_wins = 0, draws = 0;
   uint illegal_moves = 0;
   int penta[5] = {0};
   int completed_pairs = 0;

   for (uint pid = 0; pid < m_pair_records.size(); pid++) {

      if (m_pair_records[pid].g1 == ERROR_ILLEGAL_MOVE) illegal_moves++;
      if (m_pair_records[pid].g2 == ERROR_ILLEGAL_MOVE) illegal_moves++;

      bool g1_done = (m_pair_records[pid].g1 == WHITE_WIN || m_pair_records[pid].g1 == BLACK_WIN || m_pair_records[pid].g1 == DRAW);
      bool g2_done = (m_pair_records[pid].g2 == WHITE_WIN || m_pair_records[pid].g2 == BLACK_WIN || m_pair_records[pid].g2 == DRAW);

      if (g1_done) {
         // g1 is played with engine1=white, engine2=black (swap_sides=false)
         if (m_pair_records[pid].g1 == WHITE_WIN) e1_wins++;
         else if (m_pair_records[pid].g1 == BLACK_WIN) e2_wins++;
         else draws++;
      }
      if (g2_done) {
         // g2 is played with engine1=black, engine2=white (swap_sides=true)
         if (m_pair_records[pid].g2 == BLACK_WIN) e1_wins++;
         else if (m_pair_records[pid].g2 == WHITE_WIN) e2_wins++;
         else draws++;
      }

      if (g1_done && g2_done) {
         double e1_score = 0.0;
         if (m_pair_records[pid].g1 == WHITE_WIN) e1_score += 1.0;
         else if (m_pair_records[pid].g1 == DRAW)  e1_score += 0.5;

         if (m_pair_records[pid].g2 == BLACK_WIN) e1_score += 1.0;
         else if (m_pair_records[pid].g2 == DRAW)  e1_score += 0.5;

         if      (e1_score == 0.0) penta[0]++;
         else if (e1_score == 0.5) penta[1]++;
         else if (e1_score == 1.0) penta[2]++;
         else if (e1_score == 1.5) penta[3]++;
         else if (e1_score == 2.0) penta[4]++;

         completed_pairs++;
      }
   }

   m_engine1_wins_total = e1_wins;
   m_engine2_wins_total = e2_wins;
   m_draws_total        = draws;
   m_completed_pairs    = completed_pairs;
   m_illegal_moves_total = illegal_moves;
   for (int k = 0; k < 5; k++) m_penta[k] = penta[k];

   // --- Compute LLR and lock in SPRT decision ---
   if (m_sprt_enabled && !m_sprt_test_finished && m_completed_pairs > 0) {
      double sum_P = 0.0;
      double sum_P2 = 0.0;
      for (int k = 0; k < 5; ++k) {
         double P = k * 0.5;
         double count = m_penta[k];
         sum_P  += count * P;
         sum_P2 += count * P * P;
      }
      double mean_P = sum_P / m_completed_pairs;
      double var_P  = (sum_P2 / m_completed_pairs) - (mean_P * mean_P);
      double score  = mean_P / 2.0;

      double E0 = elo_to_score(m_sprt_elo0);
      double E1 = elo_to_score(m_sprt_elo1);

      if (var_P > 1e-9) {
         m_sprt_llr = (4.0 * m_completed_pairs / var_P) * (E1 - E0) * (score - (E0 + E1) / 2.0);
      } else {
         m_sprt_llr = 0.0;
      }

      // Lock in the decision as soon as a boundary is crossed
      if (m_sprt_llr >= m_sprt_upper_bound) {
         m_sprt_test_finished = true;
         m_sprt_decision = SPRT_H1;
      } else if (m_sprt_llr <= m_sprt_lower_bound) {
         m_sprt_test_finished = true;
         m_sprt_decision = SPRT_H0;
      }
   }
}

bool MatchManager::check_for_crashes(void)
{
   for (uint i = 0; i < options.num_threads; i++) {
      // Only check for alive but unresponsive/frozen threads.
      // Disconnects and normal errors are handled directly in Pass 1.
      if (m_game_mgr[i].is_engine_unresponsive())
      {
         return true;
      }
   }
   return false;
}

int MatchManager::initialize(void)
{
   if (options.engine_file_name_1.empty() || options.engine_file_name_2.empty())
   {
      cout << "Error: must specify two engines\n";
      return 0;
   }

   m_sprt_enabled = options.sprt_enabled;
   if (m_sprt_enabled) {
      m_sprt_elo0 = options.sprt_elo0;
      m_sprt_elo1 = options.sprt_elo1;
      m_sprt_alpha = options.sprt_alpha;
      m_sprt_beta = options.sprt_beta;
      m_sprt_lower_bound = log(m_sprt_beta / (1.0 - m_sprt_alpha));
      m_sprt_upper_bound = log((1.0 - m_sprt_beta) / m_sprt_alpha);
      m_sprt_llr = 0.0;
      m_sprt_test_finished = false;
      cout << "SPRT test enabled with elo0=" << m_sprt_elo0 << ", elo1=" << m_sprt_elo1
           << ", alpha=" << m_sprt_alpha << ", beta=" << m_sprt_beta << "\n";
      cout << "SPRT bounds: [" << m_sprt_lower_bound << ", " << m_sprt_upper_bound << "]\n";
   }

   if (!options.fens_filename.empty())
   {
      ifstream fens_file(options.fens_filename);
      if (!fens_file.is_open())
      {
         cout << "Error: could not open FEN file " << options.fens_filename << "\n";
         return 0;
      }
      string line;
      while (getline(fens_file, line)) {
         if (!line.empty()) m_fens_list.push_back(line);
      }
      fens_file.close();

      if (options.num_games_to_play > m_fens_list.size() * 2) {
         options.num_games_to_play = (uint)(m_fens_list.size() * 2);
         cout << "Adjusted total games to " << options.num_games_to_play << " to match the number of provided FENs.\n";
      }
   }

   if (options.num_games_to_play % 2 != 0) {
      options.num_games_to_play++;
      cout << "Adjusted total games to " << options.num_games_to_play << " to ensure complete game pairs.\n";
   }

   int num_pairs = options.num_games_to_play / 2;
   m_pair_records.resize(num_pairs);

   // Load previous results if resuming
   if (!options.resume_filename.empty()) {
      ifstream res_file(options.resume_filename);
      if (res_file.is_open()) {
         string line;
         int lines_loaded = 0;
         int lines_skipped = 0;
         while (getline(res_file, line)) {
            if (line.empty() || line[0] == '#') continue;
            try {
               stringstream ss(line);
               string token;

               if (!getline(ss, token, ',')) throw runtime_error("missing pair_id");
               int pair_id = stoi(token);

               if (!getline(ss, token, ',')) throw runtime_error("missing swap_sides");
               int swap_sides = stoi(token);

               if (!getline(ss, token, ',')) throw runtime_error("missing result");
               int res_int = stoi(token);

               // Only accept valid terminal states. IN_PROGRESS is never written to disk,
               // so encountering it here means a corrupted file; skip it.
               if (res_int < 0 || res_int >= IN_PROGRESS)
                  throw runtime_error("result value out of range");

               game_result res = (game_result)res_int;

               if (pair_id >= 0 && pair_id < num_pairs) {
                  if (swap_sides == 0) m_pair_records[pair_id].g1 = res;
                  else                 m_pair_records[pair_id].g2 = res;
                  lines_loaded++;
                  
                  // If both games in the pair are now loaded, evaluate stats immediately
                  // to catch historical SPRT bounds crossings.
                  if (m_sprt_enabled && m_pair_records[pair_id].g1 != UNFINISHED && m_pair_records[pair_id].g2 != UNFINISHED) {
                     update_stats_from_records();
                  }
               }
            } catch (const exception &e) {
               cout << "Warning: skipping malformed line in resume file (\"" << line << "\"): " << e.what() << "\n";
               lines_skipped++;
            }
         }
         res_file.close();
         cout << "Resumed from " << options.resume_filename << ": loaded " << lines_loaded << " results";
         if (lines_skipped > 0) cout << ", skipped " << lines_skipped << " bad lines";
         cout << ".\n";
      } else {
         cout << "Warning: could not open resume file " << options.resume_filename << ".\n";
      }
   }

   update_stats_from_records();

   // Open log file: append when resuming, truncate on a fresh run.
   if (!options.resume_filename.empty()) {
      m_results_log_file.open(options.resume_filename, ios::out | ios::app);
   } else {
      // Warn the user if a log file from a previous run exists and --resume was not specified.
      ifstream check_existing("results_log.txt");
      if (check_existing.is_open()) {
         check_existing.close();
         cout << "Warning: results_log.txt already exists from a previous run. It will be overwritten.\n";
         cout << "         To resume instead, add: --resume results_log.txt\n";
      }
      m_results_log_file.open("results_log.txt", ios::out | ios::trunc);
      if (!m_results_log_file.is_open())
         cout << "Warning: could not open results_log.txt for writing.\n";
   }

   m_game_mgr = new GameManager[options.num_threads];
   m_thread    = new thread[options.num_threads];

   return 1;
}

int MatchManager::load_all_engines(void)
{
   for (uint i = 0; i < options.num_threads; i++)
   {
      if (m_game_mgr[i].m_engine1.load_engine(options.engine_file_name_1, i * 2 + 1, FIRST, options.uci_1) == 0)
      {
         cout << "failed to load engine " << options.engine_file_name_1 << "\n";
         return 0;
      }
      if (m_game_mgr[i].m_engine2.load_engine(options.engine_file_name_2, i * 2 + 2, SECOND, options.uci_2) == 0)
      {
         cout << "failed to load engine " << options.engine_file_name_2 << "\n";
         return 0;
      }
   }
   return 1;
}

void MatchManager::shut_down_all_engines(void)
{
   if (m_engines_shut_down)
      return;

   m_engines_shut_down = true;

   for (uint i = 0; i < options.num_threads; i++)
   {
      m_game_mgr[i].m_engine1.send_quit_cmd();
      m_game_mgr[i].m_engine2.send_quit_cmd();
   }

   cout << "shutting down engines...\n";

   int num_engines_running;
   for (int x = 0; x < 10; x++)
   {
      this_thread::sleep_for(50ms);
      num_engines_running = 0;
      for (uint i = 0; i < options.num_threads; i++)
         num_engines_running += (m_game_mgr[i].m_engine1.is_running() + m_game_mgr[i].m_engine2.is_running());
      if (num_engines_running == 0)
         return;
   }

   for (uint i = 0; i < options.num_threads; i++)
   {
      m_game_mgr[i].m_engine1.force_exit();
      m_game_mgr[i].m_engine2.force_exit();
   }
}

void MatchManager::set_engine_options(Engine *engine)
{
   if ((engine->m_number == FIRST) && (options.mem_size_1 != 0))
   {
      if (engine->m_uci)
         engine->send_engine_cmd("setoption name Hash value " + to_string(options.mem_size_1));
      else
         engine->send_engine_cmd("memory " + to_string(options.mem_size_1));
   }
   if ((engine->m_number == SECOND) && (options.mem_size_2 != 0))
   {
      if (engine->m_uci)
         engine->send_engine_cmd("setoption name Hash value " + to_string(options.mem_size_2));
      else
         engine->send_engine_cmd("memory " + to_string(options.mem_size_2));
   }

   if ((engine->m_number == FIRST) && (options.num_cores_1 != 0))
   {
      if (engine->m_uci)
         engine->send_engine_cmd("setoption name Threads value " + to_string(options.num_cores_1));
      else
         engine->send_engine_cmd("cores " + to_string(options.num_cores_1));
   }
   if ((engine->m_number == SECOND) && (options.num_cores_2 != 0))
   {
      if (engine->m_uci)
         engine->send_engine_cmd("setoption name Threads value " + to_string(options.num_cores_2));
      else
         engine->send_engine_cmd("cores " + to_string(options.num_cores_2));
   }
}

void MatchManager::send_engine_custom_commands(Engine *engine)
{
   if (engine->m_number == FIRST)
   {
      for (size_t i = 0; i < options.custom_commands_1.size(); i++)
         engine->send_engine_cmd(options.custom_commands_1[i]);
   }
   else
   {
      for (size_t i = 0; i < options.custom_commands_2.size(); i++)
         engine->send_engine_cmd(options.custom_commands_2[i]);
   }
}

void MatchManager::print_results(bool clear_screen)
{
   uint engine1_wins = m_engine1_wins_total;
   uint engine2_wins = m_engine2_wins_total;
   uint draws = m_draws_total;
   uint illegal_move_games = m_illegal_moves_total;
   uint engine1_losses_on_time = 0;
   uint engine2_losses_on_time = 0;

   for (uint i = 0; i < options.num_threads; i++)
   {
      engine1_losses_on_time += m_game_mgr[i].m_engine1_losses_on_time;
      engine2_losses_on_time += m_game_mgr[i].m_engine2_losses_on_time;
   }

   int N_games = engine1_wins + engine2_wins + draws;
   int N_pairs = m_completed_pairs;

   // Track total events (completed games + errors) to detect when a redraw is needed.
   int current_events = N_games + (int)illegal_move_games;
   static int last_events = -1;
   if (clear_screen && current_events == last_events)
      return;
   last_events = current_events;

   stringstream ss_output;
   ss_output << "Engine1: " << options.engine_file_name_1 << " vs Engine2: " << options.engine_file_name_2 << "\n\n";

   if (N_pairs == 0) {
      ss_output << "No game pairs completed yet." << endl;
   } else {
      double sum_P = 0.0;
      double sum_P2 = 0.0;
      for (int k = 0; k < 5; ++k) {
         double P     = k * 0.5;
         double count = m_penta[k];
         sum_P  += count * P;
         sum_P2 += count * P * P;
      }
      double mean_P = sum_P / N_pairs;
      double var_P  = (sum_P2 / N_pairs) - (mean_P * mean_P);
      double score  = mean_P / 2.0;

      ss_output << fixed << setprecision(2);

      // Elo
      if (score <= 1e-9 || score >= 1.0 - 1e-9) {
         ss_output << "Elo   | " << (score > 0.5 ? "+inf" : "-inf") << endl;
      } else {
         double var_per_game            = var_P / 4.0;
         double std_error_of_mean_score = sqrt(var_per_game / N_pairs);
         double elo_per_score           = 400.0 / (score * (1.0 - score) * log(10.0));
         double std_error_of_elo        = elo_per_score * std_error_of_mean_score;
         double elo_margin              = 1.96 * std_error_of_elo;
         double elo_diff                = -400.0 * log10(1.0 / score - 1.0);
         ss_output << "Elo   | " << elo_diff << " +- " << elo_margin << " (95%)" << endl;
      }

      // SPRT
      if (m_sprt_enabled) {
         stringstream tc_ss;
         if (options.tc_fixed_time_move_ms > 0) {
            tc_ss << fixed << setprecision(2) << (float)options.tc_fixed_time_move_ms / 1000.0 << "s";
         } else {
            tc_ss << options.tc_ms / 1000 << "+" << fixed << setprecision(2) << (float)options.tc_inc_ms / 1000.0;
         }
         ss_output << "SPRT  | " << options.sprt_elo1 << " " << tc_ss.str()
                   << " Threads=" << options.num_threads << " Hash=" << options.mem_size_1 << "MB" << endl;

         // Read the pre-calculated, frozen value
         ss_output << "LLR   | " << m_sprt_llr << " (" << m_sprt_lower_bound << ", " << m_sprt_upper_bound << ") ["
                   << m_sprt_elo0 << ", " << m_sprt_elo1 << "]" << endl;
      }

      // Games & Penta
      ss_output << "Games | N: " << N_games << " W: " << engine1_wins << " L: " << engine2_wins << " D: " << draws << endl;
      ss_output << "Penta | " << m_penta[0] << " " << m_penta[1] << " " << m_penta[2] << " " << m_penta[3] << " " << m_penta[4] << endl;

      stringstream ss;
      if (illegal_move_games != 0)
         ss << "  [Illegal Moves: " << illegal_move_games << "]";
      if ((engine1_losses_on_time != 0) || (engine2_losses_on_time != 0))
         ss << "  [Timeouts: " << engine1_losses_on_time << " / " << engine2_losses_on_time << "]";
      if (ss.str().length() > 0)
         ss_output << "Info  |" << ss.str() << endl;

      if (m_sprt_enabled && m_sprt_test_finished) {
         ss_output << "\nSPRT test finished: ";
         if (m_sprt_decision == SPRT_H1)
            ss_output << "H1 accepted (Engine 1 is stronger)." << endl;
         else if (m_sprt_decision == SPRT_H0)
            ss_output << "H0 accepted (elo is within bounds)." << endl;
      }
   }

   string output_str = ss_output.str();

   // Write current summary to results.txt (always overwrite)
   ofstream results_file("results.txt", ios::out | ios::trunc);
   if (results_file.is_open())
   {
      results_file << output_str;
      results_file.close();
   }

   if (clear_screen)
   {
      #ifdef WIN32
        system("cls");
      #else
        cout << "\033[2J\033[1;1H";
      #endif
   }
   else
   {
      cout << "\n";
   }

   cout << output_str;
}

int parse_cmd_line_options(int argc, char* argv[])
{
   try
   {
      po::options_description desc("Command line options");
      desc.add_options()
         ("help",       "print help message")
         ("e1",         po::value<string>(&options.engine_file_name_1), "first engine's file name")
         ("e2",         po::value<string>(&options.engine_file_name_2), "second engine's file name")
         ("x1",         "first engine uses xboard protocol. (UCI is the default protocol.)")
         ("x2",         "second engine uses xboard protocol. (UCI is the default protocol.)")
         ("cores1",     po::value<uint>(&options.num_cores_1)->default_value(1), "first engine number of cores")
         ("cores2",     po::value<uint>(&options.num_cores_2)->default_value(1), "second engine number of cores")
         ("mem1",       po::value<uint>(&options.mem_size_1)->default_value(128), "first engine memory usage (MB)")
         ("mem2",       po::value<uint>(&options.mem_size_2)->default_value(128), "second engine memory usage (MB)")
         ("custom1",    po::value<vector<string>>(&options.custom_commands_1), "first engine custom command. e.g. --custom1 \"setoption name Style value Risky\"")
         ("custom2",    po::value<vector<string>>(&options.custom_commands_2), "second engine custom command. Note: --custom1 and --custom2 can be used more than once in the command line.")
         ("debug1",     "enable debug for first engine")
         ("debug2",     "enable debug for second engine")
         ("tc",         po::value<uint>(&options.tc_ms)->default_value(10000), "time control base time (ms)")
         ("inc",        po::value<uint>(&options.tc_inc_ms)->default_value(100), "time control increment (ms)")
         ("fixed",      po::value<uint>(&options.tc_fixed_time_move_ms)->default_value(0), "time control fixed time per move (ms). This must be set to 0, unless engines should simply use a fixed amount of time per move.")
         ("margin",     po::value<uint>(&options.margin_ms)->default_value(50), "An engine loses on time if its clock goes below zero for this amount of time (ms).")
         ("games",      po::value<uint>(&options.num_games_to_play)->default_value(1000000), "total number of games to play")
         ("threads",    po::value<uint>(&options.num_threads)->default_value(1), "number of concurrent games to run")
         ("maxmoves",   po::value<uint>(&options.max_moves)->default_value(1000), "maximum number of moves per game (total) before adjudicating draw regardless of scores")
         ("earlywin",   "adjudicate win result early if both engines report mate scores")
         ("earlydraw",  "adjudicate draw result early if both engine scores are in range (-drawscore <= score <= drawscore) for a total of drawmoves moves")
         ("drawscore",  po::value<uint>(&options.draw_score)->default_value(25), "drawscore (centipawns) value for \"earlydraw\" setting")
         ("drawmoves",  po::value<uint>(&options.draw_moves)->default_value(20), "drawmoves value for \"earlydraw\" setting")
         ("fens",       po::value<string>(&options.fens_filename), "file containing FENs for opening positions (one FEN per line)")
         ("variant",    po::value<string>(&options.variant), "variant name")
         ("4pc",        "enable 4 player chess (teams) mode")
         ("continue",   "continue match if error occurs (e.g. illegal move)")
         ("pmoves",     "print out all moves")
         // SPRT options
         ("sprt",       "Enable SPRT test. Test stops when bounds are reached.")
         ("sprt-elo0",  po::value<double>(&options.sprt_elo0)->default_value(0.0), "SPRT H0 (null hypothesis) Elo.")
         ("sprt-elo1",  po::value<double>(&options.sprt_elo1)->default_value(5.0), "SPRT H1 (alternative hypothesis) Elo.")
         ("sprt-alpha", po::value<double>(&options.sprt_alpha)->default_value(0.05), "SPRT alpha (type I error).")
         ("sprt-beta",  po::value<double>(&options.sprt_beta)->default_value(0.05), "SPRT beta (type II error).")
         ("resume",     po::value<string>(&options.resume_filename), "resume match from the specified log file (e.g., results_log.txt)")
         ;

      po::variables_map var_map;
      po::store(po::parse_command_line(argc, argv, desc), var_map);
      po::notify(var_map);

      if (var_map.count("help"))
      {
         cout << desc << "\n";
         return 0;
      }

      options.uci_1             = (var_map.count("x1") == 0);
      options.uci_2             = (var_map.count("x2") == 0);
      options.debug_1           = (var_map.count("debug1") != 0);
      options.debug_2           = (var_map.count("debug2") != 0);
      options.continue_on_error = (var_map.count("continue") != 0);
      options.print_moves       = (var_map.count("pmoves") != 0);
      options.fourplayerchess   = (var_map.count("4pc") != 0);
      options.early_win         = (var_map.count("earlywin") != 0);
      options.early_draw        = (var_map.count("earlydraw") != 0);
      options.sprt_enabled      = (var_map.count("sprt") != 0);
   }
   catch (exception &e)
   {
      cerr << "error: " << e.what() << "\n";
      return 0;
   }
   catch (...)
   {
      cerr << "error processing command line options\n";
      return 0;
   }

   if (options.num_threads > MAX_THREADS)
      options.num_threads = MAX_THREADS;
   if (options.num_threads > options.num_games_to_play)
      options.num_threads = options.num_games_to_play;

   return 1;
}

#ifndef WIN32
#ifdef __linux__
// Linux _kbhit code from https://www.flipcode.com/archives/_kbhit_for_Linux.shtml (by Morgan McGuire)
int _kbhit(void)
{
   static const int STDIN = 0;
   static bool initialized = false;

   if (!initialized)
   {
      // Use termios to turn off line buffering
      termios term;
      tcgetattr(STDIN, &term);
      term.c_lflag &= ~ICANON;
      tcsetattr(STDIN, TCSANOW, &term);
      setbuf(stdin, NULL);
      initialized = true;
   }

   int bytesWaiting;
   ioctl(STDIN, FIONREAD, &bytesWaiting);
   return bytesWaiting;
}
#else
// _kbhit not implemented
int _kbhit(void)
{
   return 0;
}
#endif
#endif

#ifdef WIN32
BOOL WINAPI ctrl_c_handler(DWORD fdwCtrlType)
{
   match_mgr.shut_down_all_engines();
   return true;
}
#else
void ctrl_c_handler(int s)
{
   match_mgr.shut_down_all_engines();
}
#endif