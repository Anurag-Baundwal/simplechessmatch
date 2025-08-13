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
   match_mgr.print_results(); // Final print
   match_mgr.save_pgn();

   match_mgr.cleanup();

   cout << "Exiting.\n";

   return 0;
}

MatchManager::MatchManager(void)
{
   m_total_games_started = 0;
   m_engines_shut_down = false;
   m_game_mgr = nullptr;
   m_thread = nullptr;
   m_sprt_enabled = false;
   m_sprt_test_finished = false;
   m_sprt_llr = 0.0;
}

MatchManager::~MatchManager(void)
{
}

void MatchManager::cleanup(void)
{
   if (m_FENs_file.is_open())
      m_FENs_file.close();
   if (m_pgn_file.is_open())
      m_pgn_file.close();
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
   string fen;
   bool swap_sides = false;

#if defined(WIN32) || defined(__linux__)
   cout << "\n***** Press any key to exit and terminate match *****\n\n";
#else
   cout << "\n***** Press Ctrl-C to exit and terminate match *****\n\n";
#endif

   while (!match_completed())
   {
      for (uint i = 0; i < options.num_threads; i++)
      {
         if (!new_game_can_start())
            break;
         if (m_game_mgr[i].m_thread_running == 0)
         {
            if (m_thread[i].joinable())
               m_thread[i].join();

            if (!swap_sides)
               if (get_next_fen(fen) == 0)
               {
                  // If we run out of FENs but not games, stop the match
                  options.num_games_to_play = m_total_games_started;
                  break; 
               }
            m_game_mgr[i].m_fen = fen;
            m_game_mgr[i].m_swap_sides = swap_sides;
            swap_sides = !swap_sides;

            m_game_mgr[i].m_thread_running = true;
            m_thread[i] = thread(&GameManager::game_runner, &m_game_mgr[i]);
            m_total_games_started++;
         }
      }

      // This loop waits for threads to finish and prints updates
      bool all_threads_busy_or_match_done = !new_game_can_start() || (m_total_games_started >= options.num_games_to_play);
      while (all_threads_busy_or_match_done && !match_completed())
      {
         this_thread::sleep_for(500ms);
         print_results();
         save_pgn();
         if (_kbhit())
         {
            cout << "\nKey pressed, terminating match.\n";
            return;
         }
         for (uint i = 0; i < options.num_threads; i++)
            if (m_game_mgr[i].m_engine_disconnected || m_game_mgr[i].is_engine_unresponsive() || (!options.continue_on_error && m_game_mgr[i].m_error))
               return;
        
         // Re-check condition to break out of this waiting loop
         all_threads_busy_or_match_done = !new_game_can_start() || (m_total_games_started >= options.num_games_to_play);
      }
      // Brief sleep in the outer loop to avoid busy-waiting when threads are finishing
      this_thread::sleep_for(50ms);
   }
}

bool MatchManager::match_completed(void)
{
   if (m_sprt_enabled && m_sprt_test_finished)
      return (num_games_in_progress() == 0);
   return ((m_total_games_started >= options.num_games_to_play) && (num_games_in_progress() == 0));
}

bool MatchManager::new_game_can_start(void)
{
   if (m_sprt_enabled && m_sprt_test_finished) return false;
   return ((m_total_games_started < options.num_games_to_play) && (num_games_in_progress() < options.num_threads));
}

uint MatchManager::num_games_in_progress(void)
{
   uint games = 0;
   for (uint i = 0; i < options.num_threads; i++)
      if (m_game_mgr[i].m_thread_running)
         games++;
   return games;
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
      m_FENs_file.open(options.fens_filename, ios::in);
      if (!m_FENs_file.is_open())
      {
         cout << "Error: could not open FEN file " << options.fens_filename << "\n";
         return 0;
      }
   }

   if (!options.pgn_filename.empty() && !options.pgn4_filename.empty())
   {
      cout << "Error: must not choose both PGN and PGN4\n";
      return 0;
   }

   if (!options.pgn_filename.empty() || !options.pgn4_filename.empty())
   {
      options.pgn4_format = options.pgn_filename.empty();
      string filename = (options.pgn4_format) ? options.pgn4_filename : options.pgn_filename;
      m_pgn_file.open(filename, ios::out);
      if (!m_pgn_file.is_open())
      {
         cout << "Error: could not open PGN file " << filename << "\n";
         return 0;
      }
   }
   else
      options.pgn4_format = options.fourplayerchess;
   
   // Open results_log.txt in truncate mode to clear it on a new run.
   m_results_log_file.open("results_log.txt", ios::out | ios::trunc);
   if (!m_results_log_file.is_open())
   {
      cout << "Warning: could not open results_log.txt for writing.\n";
   }

   m_game_mgr = new GameManager[options.num_threads];
   m_thread = new thread[options.num_threads];

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

   // poll for all engines shut down, for up to 500 ms.
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

void MatchManager::print_results(void)
{
   uint engine1_wins, engine2_wins, draws, illegal_move_games, engine1_losses_on_time, engine2_losses_on_time;
   engine1_wins = engine2_wins = draws = illegal_move_games = engine1_losses_on_time = engine2_losses_on_time = 0;

   for (uint i = 0; i < options.num_threads; i++)
   {
      engine1_wins += m_game_mgr[i].m_engine1_wins;
      engine2_wins += m_game_mgr[i].m_engine2_wins;
      draws += m_game_mgr[i].m_draws;
      illegal_move_games += m_game_mgr[i].m_illegal_move_games;
      engine1_losses_on_time += m_game_mgr[i].m_engine1_losses_on_time;
      engine2_losses_on_time += m_game_mgr[i].m_engine2_losses_on_time;
   }
   
   int N = engine1_wins + engine2_wins + draws;

   static int last_total_games_completed = -1;
   int total_games_completed = m_total_games_started - num_games_in_progress();
   if (total_games_completed == last_total_games_completed && N > 0)
      return;
   last_total_games_completed = total_games_completed;

   // Update SPRT
   if (m_sprt_enabled && !m_sprt_test_finished && N > 0) {
       // Probabilities under H0 (elo = elo0)
       double E0 = elo_to_score(m_sprt_elo0);
       // Probabilities under H1 (elo = elo1)
       double E1 = elo_to_score(m_sprt_elo1);

       // Estimate the probability of a draw from the match data.
       // Add a small epsilon to avoid division by zero or log(0) if N=0 or draws=0
       double P_draw = (double)draws / N;

       // Probabilities of win, loss, draw for H0
       double w0 = E0 - P_draw / 2.0;
       double l0 = 1.0 - E0 - P_draw / 2.0;
       double d0 = P_draw;

       // Probabilities of win, loss, draw for H1
       double w1 = E1 - P_draw / 2.0;
       double l1 = 1.0 - E1 - P_draw / 2.0;
       double d1 = P_draw;

       // To avoid log(0) for w,l in case of 100% draw rate
       if (w0 <= 0) w0 = 1e-9;
       if (l0 <= 0) l0 = 1e-9;
       if (w1 <= 0) w1 = 1e-9;
       if (l1 <= 0) l1 = 1e-9;

       // Use the correct trinomial LLR formula.
       // We can ignore the draw term if we assume P(draw) is independent of elo, because log(d1/d0) = log(P_draw/P_draw) = log(1) = 0.
       // This is a common and safe assumption for small elo differences.
       m_sprt_llr = (double)engine1_wins * log(w1 / w0) +
                    (double)engine2_wins * log(l1 / l0);
       // The full formula would be:
       // m_sprt_llr = (double)engine1_wins * log(w1 / w0) +
       //              (double)engine2_wins * log(l1 / l0) +
       //              (double)draws * log(d1 / d0); // This term is 0 with our assumption.

       if (m_sprt_llr > m_sprt_upper_bound) {
           m_sprt_test_finished = true;
       } else if (m_sprt_llr < m_sprt_lower_bound) {
           m_sprt_test_finished = true;
       }
   }
   
   // --- Build the output string ---
   stringstream ss_output;

   ss_output << "Engine1: " << options.engine_file_name_1 << " vs Engine2: " << options.engine_file_name_2 << "\n\n";

   if (N == 0) {
       ss_output << "No games completed yet." << endl;
   } else {
       double score = (double)(engine1_wins + (double)draws / 2.0) / N;
   
       ss_output << fixed << setprecision(2);
   
       // Elo
       if (score <= 1e-9 || score >= 1.0 - 1e-9) {
           ss_output << "Elo   | " << (score > 0.5 ? "+inf" : "-inf") << endl;
       } else {
           double elo_diff = -400.0 * log10(1.0 / score - 1.0);
           double win_frac = (double)engine1_wins / N;
           double draw_frac = (double)draws / N;
           double variance_of_score = (win_frac + 0.25 * draw_frac - score * score);
           double std_error_of_mean_score = sqrt(variance_of_score / N);
           double elo_per_score = 400.0 / (score * (1.0 - score) * log(10.0));
           double std_error_of_elo = elo_per_score * std_error_of_mean_score;
           double elo_margin = 1.96 * std_error_of_elo;
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
          string tc_str = tc_ss.str();
      
          ss_output << "SPRT  | " << options.sprt_elo1 << " " << tc_str << " Threads=" << options.num_threads << " Hash=" << options.mem_size_1 << "MB" << endl;
          ss_output << "LLR   | " << m_sprt_llr << " (" << m_sprt_lower_bound << ", " << m_sprt_upper_bound << ") [" 
               << m_sprt_elo0 << ", " << m_sprt_elo1 << "]" << endl;
       }
   
       // Games
       ss_output << "Games | N: " << N << " W: " << engine1_wins << " L: " << engine2_wins << " D: " << draws << endl;
   
       // Other info
       stringstream ss;
       if (illegal_move_games != 0)
          ss << "  [Illegal Moves: " << illegal_move_games << "]";
       if ((engine1_losses_on_time != 0) || (engine2_losses_on_time != 0))
          ss << "  [Timeouts: " << engine1_losses_on_time << " / " << engine2_losses_on_time <<  "]";
       if (ss.str().length() > 0)
          ss_output << "Info  |" << ss.str() << endl;

       if (m_sprt_enabled && m_sprt_test_finished) {
            ss_output << "\nSPRT test finished: ";
            if(m_sprt_llr >= m_sprt_upper_bound)
                ss_output << "H1 accepted (engine 1 is stronger)." << endl;
            else
                ss_output << "H0 accepted (elo is within bounds)." << endl;
       }
   }
   
   string output_str = ss_output.str();

   // --- Write to results.txt (overwrite) ---
   ofstream results_file("results.txt", ios::out | ios::trunc);
   if (results_file.is_open())
   {
      results_file << output_str;
      results_file.close();
   }
   
   // --- Append to results_log.txt ---
   if (m_results_log_file.is_open())
   {
       static bool first_log_entry = true;
       if (!first_log_entry) {
           m_results_log_file << "\n-------------------------------------------------\n\n";
       }
       m_results_log_file << output_str;
       m_results_log_file.flush();
       first_log_entry = false;
   }

   // --- Clear screen and print to console ---
   #ifdef WIN32
     system("cls");
   #else
     cout << "\033[2J\033[1;1H";
   #endif

   cout << output_str;
}

int MatchManager::get_next_fen(string &fen)
{
   if (!m_FENs_file.is_open())
   {
      fen = "";
      return 1;
   }
   getline(m_FENs_file, fen);
   if (m_FENs_file.eof() && fen.empty())
   {
      cout << "Used all FENs.\n";
      return 0;
   }
   return 1;
}

void MatchManager::save_pgn(void)
{
   if (!m_pgn_file.is_open())
      return;

   for (uint i = 0; i < options.num_threads; i++)
   {
      if (m_game_mgr[i].m_pgn_valid.load(memory_order_acquire))
      {
         m_game_mgr[i].m_pgn_valid = false;
         m_pgn_file << m_game_mgr[i].m_pgn;
      }
   }
}

int parse_cmd_line_options(int argc, char* argv[])
{
   try
   {
      po::options_description desc("Command line options");
      desc.add_options()
         ("help",      "print help message")
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
         ("ponder1",    "enable extra ponder time for engine 1") // <-- NEW
         ("ponder2",    "enable extra ponder time for engine 2") // <-- NEW
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
         ("pgn",        po::value<string>(&options.pgn_filename), "save games in PGN format to specified file name\n(if file exists it will be overwritten)")
         ("pgn4",       po::value<string>(&options.pgn4_filename), "save games in PGN4 format to specified file name\n(if file exists it will be overwritten)")
         // SPRT options
         ("sprt", "Enable SPRT test. Test stops when bounds are reached.")
         ("sprt-elo0", po::value<double>(&options.sprt_elo0)->default_value(0.0), "SPRT H0 (null hypothesis) Elo.")
         ("sprt-elo1", po::value<double>(&options.sprt_elo1)->default_value(5.0), "SPRT H1 (alternative hypothesis) Elo.")
         ("sprt-alpha", po::value<double>(&options.sprt_alpha)->default_value(0.05), "SPRT alpha (type I error).")
         ("sprt-beta", po::value<double>(&options.sprt_beta)->default_value(0.05), "SPRT beta (type II error).")
         ;

      po::variables_map var_map;
      po::store(po::parse_command_line(argc, argv, desc), var_map);
      po::notify(var_map);

      if (var_map.count("help"))
      {
         cout << desc << "\n";
         return 0;
      }

      options.uci_1 = (var_map.count("x1") == 0);
      options.uci_2 = (var_map.count("x2") == 0);
      options.debug_1 = (var_map.count("debug1") != 0);
      options.debug_2 = (var_map.count("debug2") != 0);
      options.ponder1 = (var_map.count("ponder1") != 0); // <-- NEW
      options.ponder2 = (var_map.count("ponder2") != 0); // <-- NEW
      options.continue_on_error = (var_map.count("continue") != 0);
      options.print_moves = (var_map.count("pmoves") != 0);
      options.fourplayerchess = (var_map.count("4pc") != 0);
      options.early_win = (var_map.count("earlywin") != 0);
      options.early_draw = (var_map.count("earlydraw") != 0);
      options.sprt_enabled = (var_map.count("sprt") != 0);
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