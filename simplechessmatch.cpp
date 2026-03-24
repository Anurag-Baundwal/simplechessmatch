// simplechessmatch.cpp

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
   // This flag is used to differentiate a user-initiated stop (Ctrl+C) from an actual engine error.
   m_user_initiated_exit = false;
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

   // Wait for any remaining game threads to finish before exiting.
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
      // This loop identifies available game slots and starts new games.
      for (uint i = 0; i < options.num_threads; i++)
      {
         if (!new_game_can_start())
            break;
         
         if (m_game_mgr[i].m_thread_running == 0)
         {
            // If the thread slot is free, first clean up the previous game's resources.
            if (m_thread[i].joinable())
            {
               m_thread[i].join();

               // --- Pentanomial processing logic ---
               if (m_game_mgr[i].m_is_valid_game) {
                  double e1_score = 0.5;
                  if ((m_game_mgr[i].m_final_result == WHITE_WIN && !m_game_mgr[i].m_swap_sides) ||
                      (m_game_mgr[i].m_final_result == BLACK_WIN && m_game_mgr[i].m_swap_sides)) {
                     e1_score = 1.0;
                  } else if ((m_game_mgr[i].m_final_result == BLACK_WIN && !m_game_mgr[i].m_swap_sides) ||
                             (m_game_mgr[i].m_final_result == WHITE_WIN && m_game_mgr[i].m_swap_sides)) {
                     e1_score = 0.0;
                  }

                  uint pid = m_game_mgr[i].m_pair_id;
                  if (pid < m_pair_results.size()) {
                     m_pair_results[pid].engine1_score += e1_score;
                     m_pair_results[pid].finished_games++;

                     if (m_pair_results[pid].finished_games == 2) {
                        double total = m_pair_results[pid].engine1_score;
                        if (total == 0.0) m_penta[0]++;
                        else if (total == 0.5) m_penta[1]++;
                        else if (total == 1.0) m_penta[2]++;
                        else if (total == 1.5) m_penta[3]++;
                        else if (total == 2.0) m_penta[4]++;
                        
                        m_completed_pairs++;
                     }
                  }
               }
               // ------------------------------------

               // Return the cores used by the finished game to the available pool.
               return_cores_to_pool(m_game_mgr[i].m_core_for_engine1);
               m_game_mgr[i].m_core_for_engine1 = "";
               m_game_mgr[i].m_core_for_engine2 = "";
            }

            // --- Core allocation logic ---
            string shared_core_list;
            if (!allocate_cores_for_game(shared_core_list))
            {
                // Not enough cores for a game right now, try again in the next iteration.
                continue;
            }
            
            if (!swap_sides)
               if (get_next_fen(fen) == 0)
               {
                  // If we run out of FENs but not games, stop the match.
                  options.num_games_to_play = m_total_games_started;
                  // Return the cores we just took, as the game won't run.
                  return_cores_to_pool(shared_core_list);
                  break;
               }

            m_game_mgr[i].m_fen = fen;
            m_game_mgr[i].m_swap_sides = swap_sides;
            m_game_mgr[i].m_pair_id = m_total_games_started / 2;
            swap_sides = !swap_sides;
            
            m_game_mgr[i].m_core_for_engine1 = shared_core_list;
            m_game_mgr[i].m_core_for_engine2 = shared_core_list;

            m_game_mgr[i].m_thread_running = true;
            m_thread[i] = thread(&GameManager::game_runner, &m_game_mgr[i]);
            m_total_games_started++;
         }
      }

      // This loop waits for running games to finish and provides match status updates.
      bool all_threads_busy_or_match_done = !new_game_can_start() || (m_total_games_started >= options.num_games_to_play);
      while (all_threads_busy_or_match_done && !match_completed())
      {
         this_thread::sleep_for(500ms);
         print_results();
         save_pgn();

         if (_kbhit())
         {
            cout << "\nKey pressed, terminating match.\n";
            m_user_initiated_exit = true;
            return;
         }

         // Check all running games for unexpected errors.
         for (uint i = 0; i < options.num_threads; i++)
         {
             // Only treat a disconnect as an error if it wasn't caused by the user pressing Ctrl+C.
             if (!m_user_initiated_exit && (m_game_mgr[i].m_engine_disconnected || m_game_mgr[i].is_engine_unresponsive() || (!options.continue_on_error && m_game_mgr[i].m_error)))
             {
                 cout << "\nError detected in a game thread. Shutting down." << endl;
                 
                 // Shut down all engines first to prevent them from becoming orphaned processes.
                 shut_down_all_engines();  

                 // Join all threads and return their cores to the pool for a clean exit.
                 for (uint j = 0; j < options.num_threads; j++)
                 {
                     if (m_thread[j].joinable())
                     {
                         m_thread[j].join(); // Wait for the thread to exit.
                     }
                     // Return cores regardless of whether the thread was joined, as they were already allocated.
                     if (!m_game_mgr[j].m_core_for_engine1.empty())
                     {
                         return_cores_to_pool(m_game_mgr[j].m_core_for_engine1);
                         m_game_mgr[j].m_core_for_engine1.clear();
                         m_game_mgr[j].m_core_for_engine2.clear();
                     }
                 }
                 return; // Exit the main loop.
             }
          }
        
         // Re-check the condition to break out of this waiting loop.
         all_threads_busy_or_match_done = !new_game_can_start() || (m_total_games_started >= options.num_games_to_play);
      }
      
      // Brief sleep in the outer loop to avoid busy-waiting when threads are finishing.
      this_thread::sleep_for(50ms);
   }
}

bool MatchManager::allocate_cores_for_game(string& shared_core_list)
{
    lock_guard<mutex> lock(m_core_mutex);

    // For a fair match, both engines must use the same number of threads.
    if (options.num_cores_1 != options.num_cores_2) {
        cout << "Warning: For fair core allocation, --cores1 and --cores2 should be equal. Using --cores1 value." << endl;
    }
    uint cores_needed_per_game = options.num_cores_1;

    // Helper lambda to perform the allocation and build the core string.
    auto do_allocation = [&](uint p_cores, uint e_cores) {
        stringstream ss;
        
        // P-cores (physical first, then logical to prioritize performance)
        uint p_needed = p_cores;
        uint phys_to_take = min(p_needed, (uint)m_available_physical_p_cores.size());
        for (uint i = 0; i < phys_to_take; ++i) { ss << m_available_physical_p_cores.back() << ","; m_available_physical_p_cores.pop_back(); }
        p_needed -= phys_to_take;
        uint log_to_take = min(p_needed, (uint)m_available_logical_p_cores.size());
        for (uint i = 0; i < log_to_take; ++i) { ss << m_available_logical_p_cores.back() << ","; m_available_logical_p_cores.pop_back(); }
        
        // E-cores
        for (uint i = 0; i < e_cores; ++i) { ss << m_available_e_cores.back() << ","; m_available_e_cores.pop_back(); }
        
        shared_core_list = ss.str();
        if (!shared_core_list.empty()) shared_core_list.pop_back(); // Remove trailing comma
        return true;
    };

    // --- Allocation Strategy ---

    // Case 1: P/E cores are defined by user, apply tiered allocation.
    if (!m_p_core_list.empty() || !m_e_core_list.empty())
    {
        uint total_p_available = (uint)(m_available_physical_p_cores.size() + m_available_logical_p_cores.size());
        
        // Priority 1: Try to allocate from P-cores only.
        if (total_p_available >= cores_needed_per_game) {
            return do_allocation(cores_needed_per_game, 0);
        }

        // Priority 2: Try to allocate from E-cores only.
        if (m_available_e_cores.size() >= cores_needed_per_game) {
            return do_allocation(0, cores_needed_per_game);
        }

        // Priority 3: Try a mixed allocation of all available P-cores and remaining E-cores.
        if (total_p_available > 0) {
            uint e_cores_needed = cores_needed_per_game - total_p_available;
            if (m_available_e_cores.size() >= e_cores_needed) {
                return do_allocation(total_p_available, e_cores_needed);
            }
        }
    }

    // Case 2: No P/E cores specified, fall back to generic allocation.
    if (m_available_generic_cores.size() >= cores_needed_per_game)
    {
        stringstream ss;
        for (uint i = 0; i < cores_needed_per_game; ++i) { ss << m_available_generic_cores.back() << ","; m_available_generic_cores.pop_back(); }
        
        shared_core_list = ss.str();
        if (!shared_core_list.empty()) shared_core_list.pop_back();
        return true;
    }

    // If we reach here, no allocation was possible.
    return false;
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

void MatchManager::parse_core_list(const string& core_str, vector<int>& core_vec) {
    stringstream ss(core_str);
    string core_num_str;
    while (getline(ss, core_num_str, ',')) {
        try {
            core_vec.push_back(stoi(core_num_str));
        } catch (...) {
            cout << "Warning: Invalid core number '" << core_num_str << "' in core list." << endl;
        }
    }
}

void MatchManager::return_cores_to_pool(const string& core_list_str)
{
    if (core_list_str.empty()) return;

    lock_guard<mutex> lock(m_core_mutex);

    stringstream ss(core_list_str);
    string core_num_str;

    // Parse the comma-separated list of core numbers
    while (getline(ss, core_num_str, ','))
    {
        try {
            int core_num = stoi(core_num_str);

            // Correctly categorize and return the core to its original pool.
            if (find(m_p_core_list.begin(), m_p_core_list.end(), core_num) != m_p_core_list.end()) {
                if (core_num % 2 == 0) { // Physical P-cores are even
                    m_available_physical_p_cores.push_back(core_num);
                } else { // Logical P-cores are odd
                    m_available_logical_p_cores.push_back(core_num);
                }
            }
            else if (find(m_e_core_list.begin(), m_e_core_list.end(), core_num) != m_e_core_list.end()) {
                m_available_e_cores.push_back(core_num);
            }
            else {
                m_available_generic_cores.push_back(core_num);
            }
        }
        catch (...) {
            cout << "Warning: Could not convert core number '" << core_num_str << "' back to an integer while returning to pool." << endl;
        }
    }
}

bool MatchManager::load_state_from_resume_file()
{
    ifstream resume_file(options.resume_filename);
    if (!resume_file.is_open())
    {
        cout << "Error: could not open resume file " << options.resume_filename << endl;
        return false;
    }

    cout << "Resuming match from " << options.resume_filename << "..." << endl;

    string line;
    uint total_wins_from_file = 0, total_losses_from_file = 0, total_draws_from_file = 0;
    bool games_line_found = false;
    bool pentanomial_found = false;

    while (getline(resume_file, line))
    {
        // Skip empty lines or lines that don't start with "Games" or "Thread" or "Penta"
        if (line.rfind("Games", 0) != 0 && line.rfind("Thread", 0) != 0 && line.rfind("Penta", 0) != 0) {
            continue;
        }

        stringstream ss(line);
        string header;
        char separator;
        
        ss >> header;

        if (header == "Games")
        {
            string temp;
            uint n, w, l, d;
            ss >> separator >> temp >> n >> temp >> w >> temp >> l >> temp >> d;
            if (ss.fail()) {
                cout << "Warning: Could not parse 'Games' line: " << line << endl;
                continue;
            }
            total_wins_from_file = w;
            total_losses_from_file = l;
            total_draws_from_file = d;
            games_line_found = true;
        }
        else if (header == "Thread")
        {
            uint thread_id;
            string temp;
            uint n, w, l, d;
            ss >> thread_id >> separator >> temp >> n >> temp >> w >> temp >> l >> temp >> d;
            
            if (ss.fail()) {
                cout << "Warning: Could not parse 'Thread' line: " << line << endl;
                continue;
            }

            if (thread_id < options.num_threads)
            {
                m_game_mgr[thread_id].m_engine1_wins = w;
                m_game_mgr[thread_id].m_engine2_wins = l;
                m_game_mgr[thread_id].m_draws = d;
            }
            else
            {
                cout << "Warning: Thread ID " << thread_id << " from resume file is out of bounds for current thread count (" << options.num_threads << "). Ignoring." << endl;
            }
        }
        else if (header == "Penta")
        {
            ss >> separator >> m_penta[0] >> m_penta[1] >> m_penta[2] >> m_penta[3] >> m_penta[4];
            if (ss.fail()) {
                cout << "Warning: Could not parse 'Penta' line: " << line << endl;
                continue;
            }
            m_completed_pairs = m_penta[0] + m_penta[1] + m_penta[2] + m_penta[3] + m_penta[4];
            pentanomial_found = true;
        }
    }
    
    resume_file.close();
    
    uint wins_check = 0, losses_check = 0, draws_check = 0;
    for (uint i = 0; i < options.num_threads; ++i) {
        wins_check += m_game_mgr[i].m_engine1_wins.load(memory_order_relaxed);
        losses_check += m_game_mgr[i].m_engine2_wins.load(memory_order_relaxed);
        draws_check += m_game_mgr[i].m_draws.load(memory_order_relaxed);
    }
    
    if (games_line_found && (total_wins_from_file != wins_check || total_losses_from_file != losses_check || total_draws_from_file != draws_check)) {
        cout << "Warning: Totals from 'Games' line (" << total_wins_from_file << "W " << total_losses_from_file << "L " << total_draws_from_file << "D"
             << ") do not match sum of 'Thread' lines (" << wins_check << "W " << losses_check << "L " << draws_check << "D"
             << ").\nUsing sum of thread lines as the source of truth." << endl;
    }

    m_total_games_started = wins_check + losses_check + draws_check;

    if (m_total_games_started % 2 != 0) {
        // cout << "Warning: Resuming from an odd number of games. Realigning dispatch counter to protect pair integrity." << endl;
        // Snap to an even number so the next dispatched game starts a fresh pair properly.
        // (Note: This doesn't delete the game from the Win/Loss stats, it just resets the pair dispatcher).
        m_total_games_started -= 1;
    }
    
    if (m_total_games_started == 0 && !games_line_found) {
        cout << "Warning: No game data found in resume file. Starting a new match." << endl;
    } else {
        cout << "Resumed state: " << wins_check << " W / " << losses_check << " L / " << draws_check << " D (" << m_total_games_started << " games)" << endl;
    }

    if (m_total_games_started >= options.num_games_to_play) {
        cout << "Match already completed according to resume file. " << m_total_games_started << " games played out of " << options.num_games_to_play << "." << endl;
    } else if (m_total_games_started > 0) {
        cout << "Will play " << (options.num_games_to_play - m_total_games_started) << " more games." << endl;
    }
    
    if (!pentanomial_found && m_total_games_started > 0) {
        cout << "Warning: Resume file missing Pentanomial data. Synthesizing safe distribution from totals." << endl;
        
        // Since m_total_games_started may have been decremented by 1 to realign game pairs
        // when resuming from odd game counts, we must recalculate the true number of games played. 
        // If we divide by the adjusted value, our win/loss/draw probabilities will exceed 1.0 (100%).
        // We use the unadjusted sum here to synthesize an accurate pentanomial distribution.
        uint unadjusted_game_total = wins_check + losses_check + draws_check;
        
        double win_rate = (unadjusted_game_total > 0) ? (double)wins_check / unadjusted_game_total : 0.0;
        double loss_rate = (unadjusted_game_total > 0) ? (double)losses_check / unadjusted_game_total : 0.0;
        double draw_rate = (unadjusted_game_total > 0) ? (double)draws_check / unadjusted_game_total : 0.0;
        
        int pairs = m_total_games_started / 2;
        m_completed_pairs = pairs;
        
        m_penta[0] = (int)round(pairs * loss_rate * loss_rate);
        m_penta[1] = (int)round(pairs * 2 * loss_rate * draw_rate);
        m_penta[2] = (int)round(pairs * (2 * win_rate * loss_rate + draw_rate * draw_rate));
        m_penta[3] = (int)round(pairs * 2 * win_rate * draw_rate);
        m_penta[4] = (int)round(pairs * win_rate * win_rate);
        
        // Ensure total equals expected pairs
        int sum = m_penta[0] + m_penta[1] + m_penta[2] + m_penta[3] + m_penta[4];
        if (sum < pairs) m_penta[2] += (pairs - sum);
        else if (sum > pairs) m_penta[2] -= (sum - pairs);
    }
    return true;
}

int MatchManager::initialize(void)
{
   if (options.engine_file_name_1.empty() || options.engine_file_name_2.empty())
   {
      cout << "Error: must specify two engines\n";
      return 0;
   }

   // --- Core Initialization and Limiting Logic ---
   vector<int> p_cores_from_arg, e_cores_from_arg;
   vector<int> all_hw_cores;
   uint num_hw_cores = thread::hardware_concurrency();
   if (num_hw_cores > 0) {
       for (uint i = 0; i < num_hw_cores; ++i) {
           all_hw_cores.push_back(i);
       }
   }
   
   std::random_device rd;
   std::default_random_engine rng(rd());

   if (!options.pcores.empty()) {
       parse_core_list(options.pcores, p_cores_from_arg);
   }
   if (!options.ecores.empty()) {
       parse_core_list(options.ecores, e_cores_from_arg);
   }

   // Auto-detect E-cores if only P-cores are given, and vice-versa.
   if (!options.pcores.empty() && options.ecores.empty() && !all_hw_cores.empty()) {
       m_p_core_list = p_cores_from_arg;
       vector<int> p_cores_sorted = p_cores_from_arg;
       sort(p_cores_sorted.begin(), p_cores_sorted.end());
       sort(all_hw_cores.begin(), all_hw_cores.end());
       set_difference(all_hw_cores.begin(), all_hw_cores.end(),
                      p_cores_sorted.begin(), p_cores_sorted.end(),
                      back_inserter(m_e_core_list));
       cout << "Detected " << m_p_core_list.size() << " P-cores from argument and auto-detected " << m_e_core_list.size() << " E-cores." << endl;
   } else if (options.pcores.empty() && !options.ecores.empty() && !all_hw_cores.empty()) {
       m_e_core_list = e_cores_from_arg;
       vector<int> e_cores_sorted = e_cores_from_arg;
       sort(e_cores_sorted.begin(), e_cores_sorted.end());
       sort(all_hw_cores.begin(), all_hw_cores.end());
       set_difference(all_hw_cores.begin(), all_hw_cores.end(),
                      e_cores_sorted.begin(), e_cores_sorted.end(),
                      back_inserter(m_p_core_list));
       cout << "Detected " << m_e_core_list.size() << " E-cores from argument and auto-detected " << m_p_core_list.size() << " P-cores." << endl;
   } else {
       m_p_core_list = p_cores_from_arg;
       m_e_core_list = e_cores_from_arg;
       if (!m_p_core_list.empty() || !m_e_core_list.empty()) {
           cout << "Detected " << m_p_core_list.size() << " P-cores and " << m_e_core_list.size() << " E-cores from arguments." << endl;
       }
   }
   
   // Populate available core pools based on the final master lists.
   if (!m_p_core_list.empty() || !m_e_core_list.empty()) {
       for (int core : m_p_core_list) {
           if (core % 2 == 0) { // Physical P-cores are even
               m_available_physical_p_cores.push_back(core);
           } else { // Logical P-cores are odd
               m_available_logical_p_cores.push_back(core);
           }
       }
       m_available_e_cores = m_e_core_list;
   } else if (!all_hw_cores.empty()) {
       cout << "No specific cores provided. Using all " << all_hw_cores.size() << " available hardware logical processors as generic cores." << endl;
       m_available_generic_cores = all_hw_cores;
   }

   // Dynamically reduce thread count if not enough cores are available for the requested concurrency.
   uint cores_per_game = options.num_cores_1;
   uint total_cores_in_pool = (uint)(m_available_physical_p_cores.size() + m_available_logical_p_cores.size() + m_available_e_cores.size() + m_available_generic_cores.size());

   if (cores_per_game > 0 && total_cores_in_pool < options.num_threads * cores_per_game) {
       uint old_threads = options.num_threads;
       options.num_threads = total_cores_in_pool / cores_per_game;
       cout << "Warning: Not enough cores in the pool (" << total_cores_in_pool << ") for " << old_threads << " concurrent games requiring " << cores_per_game << " core(s) each. Reducing concurrent games to " << options.num_threads << "." << endl;
   }
   
   // Shuffle the available pools to randomize which specific cores get picked for each game.
   shuffle(m_available_physical_p_cores.begin(), m_available_physical_p_cores.end(), rng);
   shuffle(m_available_logical_p_cores.begin(), m_available_logical_p_cores.end(), rng);
   shuffle(m_available_e_cores.begin(), m_available_e_cores.end(), rng);
   shuffle(m_available_generic_cores.begin(), m_available_generic_cores.end(), rng);
   
   cout << "Final core pools ready. Physical P-cores: " << m_available_physical_p_cores.size() 
        << ", Logical P-cores: " << m_available_logical_p_cores.size()
        << ", E-cores: " << m_available_e_cores.size() 
        << ", Generic cores: " << m_available_generic_cores.size() << endl;

   if (options.num_games_to_play % 2 != 0) {
       options.num_games_to_play++;
       cout << "Adjusted total games to " << options.num_games_to_play << " to ensure complete game pairs." << endl;
   }
   m_pair_results.resize(options.num_games_to_play / 2 + 1);
   m_completed_pairs = 0;
   for (int i = 0; i < 5; i++) m_penta[i] = 0;

   m_game_mgr = new GameManager[options.num_threads];
   m_thread = new thread[options.num_threads];

   if (!options.resume_filename.empty()) {
       if (!load_state_from_resume_file()) {
           return 0; // Abort if loading state fails
       }
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
      // If resuming, skip already played FENs
      if (!options.resume_filename.empty() && m_total_games_started > 0) {
          uint fens_to_skip = m_total_games_started / 2;
          cout << "Skipping " << fens_to_skip << " FENs from the beginning of the file." << endl;
          string dummy;
          for (uint i = 0; i < fens_to_skip; ++i) {
              if (!getline(m_FENs_file, dummy)) {
                  cout << "Warning: Reached end of FEN file while skipping." << endl;
                  break;
              }
          }
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

      ios_base::openmode mode = ios::out;
      if (!options.resume_filename.empty() && m_total_games_started > 0) {
          mode |= ios::app; // Append if resuming
          cout << "Appending new games to " << filename << endl;
      } else {
          mode |= ios::trunc; // Overwrite if not resuming
      }
      
      m_pgn_file.open(filename, mode);
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

   // Poll for a graceful shutdown, giving engines a moment to comply.
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

   // If engines are still running, force their termination.
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
      engine1_wins += m_game_mgr[i].m_engine1_wins.load(memory_order_relaxed);
      engine2_wins += m_game_mgr[i].m_engine2_wins.load(memory_order_relaxed);
      draws += m_game_mgr[i].m_draws.load(memory_order_relaxed);
      illegal_move_games += m_game_mgr[i].m_illegal_move_games.load(memory_order_relaxed);
      engine1_losses_on_time += m_game_mgr[i].m_engine1_losses_on_time.load(memory_order_relaxed);
      engine2_losses_on_time += m_game_mgr[i].m_engine2_losses_on_time.load(memory_order_relaxed);
   }
   
   int N_games = engine1_wins + engine2_wins + draws;
   int N_pairs = m_completed_pairs;

   static int last_total_games_completed = -1;
   int total_games_completed = m_total_games_started - num_games_in_progress();
   if (total_games_completed == last_total_games_completed && N_games > 0)
      return;
   last_total_games_completed = total_games_completed;

   stringstream ss_output;
   ss_output << "Engine1: " << options.engine_file_name_1 << " vs Engine2: " << options.engine_file_name_2 << "\n\n";

   if (N_pairs == 0) {
       ss_output << "No game pairs completed yet." << endl;
   } else {
       double sum_P = 0.0;
       double sum_P2 = 0.0;
       for (int k = 0; k < 5; ++k) {
           double P = k * 0.5; // Pair score outcomes: 0.0, 0.5, 1.0, 1.5, 2.0
           double count = m_penta[k];
           sum_P += count * P;
           sum_P2 += count * P * P;
       }
       double mean_P = sum_P / N_pairs;
       double var_P = (sum_P2 / N_pairs) - (mean_P * mean_P);
       double score = mean_P / 2.0;

       ss_output << fixed << setprecision(2);

       // Elo
       if (score <= 1e-9 || score >= 1.0 - 1e-9) {
           ss_output << "Elo   | " << (score > 0.5 ? "+inf" : "-inf") << endl;
       } else {
           double var_per_game = var_P / 4.0;
           double std_error_of_mean_score = sqrt(var_per_game / N_pairs);
           double elo_per_score = 400.0 / (score * (1.0 - score) * log(10.0));
           double std_error_of_elo = elo_per_score * std_error_of_mean_score;
           double elo_margin = 1.96 * std_error_of_elo;
           double elo_diff = -400.0 * log10(1.0 / score - 1.0);
           ss_output << "Elo   | " << elo_diff << " +- " << elo_margin << " (95%)" << endl;
       }

       // SPRT
       if (m_sprt_enabled) {
          stringstream tc_ss;
          tc_ss << fixed << setprecision(2);
          if (options.tc_fixed_time_move_ms_1 > 0 || options.tc_fixed_time_move_ms_2 > 0) {
            float time1_s = ((options.tc_fixed_time_move_ms_1 > 0) ? options.tc_fixed_time_move_ms_1 : options.tc_fixed_time_move_ms) / 1000.0f;
            float time2_s = ((options.tc_fixed_time_move_ms_2 > 0) ? options.tc_fixed_time_move_ms_2 : options.tc_fixed_time_move_ms) / 1000.0f;
            tc_ss << time1_s << "s vs " << time2_s << "s";
          } else if (options.tc_fixed_time_move_ms > 0) {
              tc_ss << (float)options.tc_fixed_time_move_ms / 1000.0 << "s";
          } else {
              tc_ss << options.tc_ms / 1000 << "+" << (float)options.tc_inc_ms / 1000.0;
          }
          string tc_str = tc_ss.str();
      
          ss_output << "SPRT  | " << options.sprt_elo1 << " " << tc_str << " Threads=" << options.num_threads << " Hash=" << options.mem_size_1 << "MB" << endl;

          // LLR calculation using Brownian Motion approximation (Cutechess standard)
          double E0 = elo_to_score(m_sprt_elo0);
          double E1 = elo_to_score(m_sprt_elo1);
          if (var_P > 1e-9) {
              m_sprt_llr = (4.0 * N_pairs / var_P) * (E1 - E0) * (score - (E0 + E1) / 2.0);
          } else {
              m_sprt_llr = 0.0;
          }

          ss_output << "LLR   | " << m_sprt_llr << " (" << m_sprt_lower_bound << ", " << m_sprt_upper_bound << ") [" 
               << m_sprt_elo0 << ", " << m_sprt_elo1 << "]" << endl;

          if (!m_sprt_test_finished && m_sprt_llr > m_sprt_upper_bound) m_sprt_test_finished = true;
          else if (!m_sprt_test_finished && m_sprt_llr < m_sprt_lower_bound) m_sprt_test_finished = true;
       }

       // Games
       ss_output << "Games | N: " << N_games << " W: " << engine1_wins << " L: " << engine2_wins << " D: " << draws << endl;
       ss_output << "Penta | " << m_penta[0] << " " << m_penta[1] << " " << m_penta[2] << " " << m_penta[3] << " " << m_penta[4] << endl;
       ss_output << endl;

       // Per-thread stats
       for (uint i = 0; i < options.num_threads; i++)
       {
           uint thread_wins = m_game_mgr[i].m_engine1_wins.load(memory_order_relaxed);
           uint thread_losses = m_game_mgr[i].m_engine2_wins.load(memory_order_relaxed);
           uint thread_draws = m_game_mgr[i].m_draws.load(memory_order_relaxed);
           uint thread_n = thread_wins + thread_losses + thread_draws;

           ss_output << "Thread " << i << " | N: " << thread_n
               << " W: " << thread_wins
               << " L: " << thread_losses
               << " D: " << thread_draws << endl;
       }
       
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
         ("resume",    po::value<string>(&options.resume_filename), "Resume a match from a results.txt file")
         ("e1",         po::value<string>(&options.engine_file_name_1), "first engine's file name")
         ("e2",         po::value<string>(&options.engine_file_name_2), "second engine's file name")
         ("x1",         "first engine uses xboard protocol. (UCI is the default protocol.)")
         ("x2",         "second engine uses xboard protocol. (UCI is the default protocol.)")
         ("cores1",     po::value<uint>(&options.num_cores_1)->default_value(1), "first engine number of cores/threads")
         ("cores2",     po::value<uint>(&options.num_cores_2)->default_value(1), "second engine number of cores/threads")
         ("mem1",       po::value<uint>(&options.mem_size_1)->default_value(128), "first engine memory usage (MB)")
         ("mem2",       po::value<uint>(&options.mem_size_2)->default_value(128), "second engine memory usage (MB)")
         ("custom1",    po::value<vector<string>>(&options.custom_commands_1), "first engine custom command. e.g. --custom1 \"setoption name Style value Risky\"")
         ("custom2",    po::value<vector<string>>(&options.custom_commands_2), "second engine custom command. Note: --custom1 and --custom2 can be used more than once in the command line.")
         ("pcores",     po::value<string>(&options.pcores), "CPU affinity P-cores (e.g., \"0,1,2,3\")")
         ("ecores",     po::value<string>(&options.ecores), "CPU affinity E-cores (e.g., \"4,5,6,7\")")
         ("debug1",     "enable debug for first engine")
         ("debug2",     "enable debug for second engine")
         ("ponder1",    "enable extra ponder time for engine 1")
         ("ponder2",    "enable extra ponder time for engine 2")
         ("tc",         po::value<uint>(&options.tc_ms)->default_value(10000), "time control base time (ms)")
         ("inc",        po::value<uint>(&options.tc_inc_ms)->default_value(100), "time control increment (ms)")
         ("fixed",      po::value<uint>(&options.tc_fixed_time_move_ms)->default_value(0), "time control fixed time per move (ms). This must be set to 0, unless engines should simply use a fixed amount of time per move.")
         ("fixed1",     po::value<uint>(&options.tc_fixed_time_move_ms_1)->default_value(0), "fixed time per move for engine 1 (ms). Overrides --fixed.")
         ("fixed2",     po::value<uint>(&options.tc_fixed_time_move_ms_2)->default_value(0), "fixed time per move for engine 2 (ms). Overrides --fixed.")
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
      options.ponder1 = (var_map.count("ponder1") != 0);
      options.ponder2 = (var_map.count("ponder2") != 0);
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

// Signal handler for Ctrl+C.
// This must only contain async-safe operations. Setting an atomic flag is safe.
#ifdef WIN32
BOOL WINAPI ctrl_c_handler(DWORD fdwCtrlType)
{
   match_mgr.m_user_initiated_exit = true;
   match_mgr.shut_down_all_engines();
   return true;
}
#else
void ctrl_c_handler(int s)
{
   match_mgr.m_user_initiated_exit = true;
   match_mgr.shut_down_all_engines();
}
#endif