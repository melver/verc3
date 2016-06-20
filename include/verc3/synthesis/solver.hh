/*
 * Copyright (c) 2016, The University of Edinburgh
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

#ifndef VERC3_SYNTHESIS_SOLVER_HH_
#define VERC3_SYNTHESIS_SOLVER_HH_

#include <future>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "verc3/command.hh"
#include "verc3/synthesis/enumerate.hh"

namespace verc3 {
namespace synthesis {

extern synthesis::RangeEnumerate g_range_enumerate;
extern thread_local synthesis::RangeEnumerate t_range_enumerate;

template <class TransitionSystem>
class Solver {
 public:
  explicit Solver(TransitionSystem&& ts) : ts_(std::move(ts)) {
    command_.eval()->set_trace_on_error(false);
    command_.eval()->unset_monitor();
  }

  auto operator()(
      const core::StateQueue<typename TransitionSystem::State>& start_states,
      synthesis::RangeEnumerate start, std::size_t num_candidates) {
    // assume t_range_enumerate is thread_local!
    assert(t_range_enumerate.states().empty());
    assert(num_candidates > 0);
    std::vector<synthesis::RangeEnumerate> result;
    t_range_enumerate = std::move(start);

    do {
      ++enumerated_candidates_;
      try {
        command_(start_states, &ts_);
        VLOG(0) << "SOLUTION @ total discovered "
                << t_range_enumerate.combinations()
                << " combinations | visited states: "
                << command_.eval()->num_visited_states() << " | "
                << t_range_enumerate;
        result.push_back(t_range_enumerate);
      } catch (const core::Error& e) {
      }
    } while (t_range_enumerate.Advance() && --num_candidates != 0);

    t_range_enumerate.Clear();
    return result;
  }

  std::size_t enumerated_candidates() const { return enumerated_candidates_; }

 private:
  ModelCheckerCommand<TransitionSystem> command_;
  TransitionSystem ts_;
  std::size_t enumerated_candidates_ = 0;
};

template <class TransitionSystem, class Func>
inline std::vector<synthesis::RangeEnumerate> ParallelSolve(
    const core::StateQueue<typename TransitionSystem::State>& start_states,
    Func transition_system_factory, std::size_t num_threads = 1,
    std::size_t min_per_thread_variants = 100) {
  std::vector<Solver<TransitionSystem>> solvers;
  std::vector<std::future<std::vector<synthesis::RangeEnumerate>>> futures;
  std::vector<synthesis::RangeEnumerate> result;

  // Reset any previous state; this implies this function can not be called
  // concurrently.
  g_range_enumerate.Clear();

  for (std::size_t i = 0; i < num_threads; ++i) {
    solvers.emplace_back(transition_system_factory(start_states.front()));
  }

  std::size_t enumerated_candidates = 0;
  do {
    // Get copy of current g_range_enumerate, as other threads might start
    // modifying it while we launch new threads.
    auto cur_range_enumerate = g_range_enumerate;

    // Set all existing elements to max before new ones are discovered by
    // starting the threads.
    g_range_enumerate.SetMax();

    if (g_range_enumerate.combinations() == 0) {
      auto solns =
          solvers.front()(start_states, std::move(cur_range_enumerate), 1);
      std::move(solns.begin(), solns.end(), std::back_inserter(result));
    } else {
      std::size_t per_thread_variants =
          (cur_range_enumerate.combinations() - enumerated_candidates) /
          num_threads;
      if (per_thread_variants < min_per_thread_variants) {
        per_thread_variants = min_per_thread_variants;
      }

      for (std::size_t i = 0; i < solvers.size(); ++i) {
        std::size_t this_from_candidate =
            (enumerated_candidates + per_thread_variants * i);
        std::size_t this_to_candidate =
            this_from_candidate + per_thread_variants >
                    cur_range_enumerate.combinations()
                ? cur_range_enumerate.combinations()
                : this_from_candidate + per_thread_variants;
        VLOG(0) << "Dispatching thread for candidates [" << this_from_candidate
                << ", " << this_to_candidate << ") ...";

        // mutable lambda, so that we move start_from, rather than copy.
        futures.emplace_back(std::async(std::launch::async, [
          &solver = solvers[i], &start_states, start_from = cur_range_enumerate,
          per_thread_variants
        ]() mutable {
          return solver(start_states, std::move(start_from),
                        per_thread_variants);
        }));

        if (!cur_range_enumerate.Advance(per_thread_variants)) break;
      }

      for (auto& future : futures) {
        auto solns = future.get();
        std::move(solns.begin(), solns.end(), std::back_inserter(result));
      }

      futures.clear();
    }

    enumerated_candidates = 0;
    for (const auto& solver : solvers) {
      enumerated_candidates += solver.enumerated_candidates();
    }

    VLOG(0) << "Enumerated " << enumerated_candidates << " candidates of "
            << g_range_enumerate.combinations()
            << " discovered possible candidates.";

    assert(enumerated_candidates <= g_range_enumerate.combinations());
  } while (g_range_enumerate.Advance());

  return result;
}

}  // namespace synthesis
}  // namespace verc3

#define SYNTHESIZE(opt, local_name, ...)                       \
  do {                                                         \
    static decltype(opt) local_name(#local_name, opt);         \
    local_name.Register(&verc3::synthesis::g_range_enumerate); \
    local_name.GetCurrent(verc3::synthesis::t_range_enumerate, \
                          true)(__VA_ARGS__);                  \
  } while (0);

#endif /* VERC3_SYNTHESIS_SOLVER_HH_ */

/* vim: set ts=2 sts=2 sw=2 et : */
