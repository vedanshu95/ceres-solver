// Ceres Solver - A fast non-linear least squares minimizer
// Copyright 2010, 2011, 2012 Google Inc. All rights reserved.
// http://code.google.com/p/ceres-solver/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// * Neither the name of Google Inc. nor the names of its contributors may be
//   used to endorse or promote products derived from this software without
//   specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Author: keir@google.com (Keir Mierle)

#include "ceres/solver_impl.h"

#include <iostream>  // NOLINT
#include <numeric>
#include "ceres/evaluator.h"
#include "ceres/gradient_checking_cost_function.h"
#include "ceres/iteration_callback.h"
#include "ceres/levenberg_marquardt_strategy.h"
#include "ceres/linear_solver.h"
#include "ceres/map_util.h"
#include "ceres/minimizer.h"
#include "ceres/parameter_block.h"
#include "ceres/problem.h"
#include "ceres/problem_impl.h"
#include "ceres/program.h"
#include "ceres/residual_block.h"
#include "ceres/schur_ordering.h"
#include "ceres/stringprintf.h"
#include "ceres/trust_region_minimizer.h"

namespace ceres {
namespace internal {
namespace {

// Callback for updating the user's parameter blocks. Updates are only
// done if the step is successful.
class StateUpdatingCallback : public IterationCallback {
 public:
  StateUpdatingCallback(Program* program, double* parameters)
      : program_(program), parameters_(parameters) {}

  CallbackReturnType operator()(const IterationSummary& summary) {
    if (summary.step_is_successful) {
      program_->StateVectorToParameterBlocks(parameters_);
      program_->CopyParameterBlockStateToUserState();
    }
    return SOLVER_CONTINUE;
  }

 private:
  Program* program_;
  double* parameters_;
};

// Callback for logging the state of the minimizer to STDERR or STDOUT
// depending on the user's preferences and logging level.
class LoggingCallback : public IterationCallback {
 public:
  explicit LoggingCallback(bool log_to_stdout)
      : log_to_stdout_(log_to_stdout) {}

  ~LoggingCallback() {}

  CallbackReturnType operator()(const IterationSummary& summary) {
    const char* kReportRowFormat =
        "% 4d: f:% 8e d:% 3.2e g:% 3.2e h:% 3.2e "
        "rho:% 3.2e mu:% 3.2e li:% 3d it:% 3.2e tt:% 3.2e";
    string output = StringPrintf(kReportRowFormat,
                                 summary.iteration,
                                 summary.cost,
                                 summary.cost_change,
                                 summary.gradient_max_norm,
                                 summary.step_norm,
                                 summary.relative_decrease,
                                 summary.trust_region_radius,
                                 summary.linear_solver_iterations,
                                 summary.iteration_time_in_seconds,
                                 summary.cumulative_time_in_seconds);
    if (log_to_stdout_) {
      cout << output << endl;
    } else {
      VLOG(1) << output;
    }
    return SOLVER_CONTINUE;
  }

 private:
  const bool log_to_stdout_;
};

}  // namespace

void SolverImpl::Minimize(const Solver::Options& options,
                          Program* program,
                          Evaluator* evaluator,
                          LinearSolver* linear_solver,
                          double* parameters,
                          Solver::Summary* summary) {
  Minimizer::Options minimizer_options(options);
  LoggingCallback logging_callback(options.minimizer_progress_to_stdout);
  if (options.logging_type != SILENT) {
    minimizer_options.callbacks.insert(minimizer_options.callbacks.begin(),
                                       &logging_callback);
  }

  StateUpdatingCallback updating_callback(program, parameters);
  if (options.update_state_every_iteration) {
    // This must get pushed to the front of the callbacks so that it is run
    // before any of the user callbacks.
    minimizer_options.callbacks.insert(minimizer_options.callbacks.begin(),
                                       &updating_callback);
  }

  minimizer_options.evaluator = evaluator;
  scoped_ptr<SparseMatrix> jacobian(evaluator->CreateJacobian());
  minimizer_options.jacobian = jacobian.get();

  TrustRegionStrategy::Options trust_region_strategy_options;
  trust_region_strategy_options.linear_solver = linear_solver;
  trust_region_strategy_options.initial_radius =
      options.initial_trust_region_radius;
  trust_region_strategy_options.max_radius = options.max_trust_region_radius;
  trust_region_strategy_options.lm_min_diagonal = options.lm_min_diagonal;
  trust_region_strategy_options.lm_max_diagonal = options.lm_max_diagonal;
  trust_region_strategy_options.trust_region_strategy_type =
      options.trust_region_strategy_type;
  scoped_ptr<TrustRegionStrategy> strategy(
      TrustRegionStrategy::Create(trust_region_strategy_options));
  minimizer_options.trust_region_strategy = strategy.get();

  TrustRegionMinimizer minimizer;
  time_t minimizer_start_time = time(NULL);
  minimizer.Minimize(minimizer_options, parameters, summary);
  summary->minimizer_time_in_seconds = time(NULL) - minimizer_start_time;
}

void SolverImpl::Solve(const Solver::Options& original_options,
                       ProblemImpl* original_problem_impl,
                       Solver::Summary* summary) {
  time_t solver_start_time = time(NULL);
  Solver::Options options(original_options);
  Program* original_program = original_problem_impl->mutable_program();
  ProblemImpl* problem_impl = original_problem_impl;
  // Reset the summary object to its default values.
  *CHECK_NOTNULL(summary) = Solver::Summary();


#ifndef CERES_USE_OPENMP
  if (options.num_threads > 1) {
    LOG(WARNING)
        << "OpenMP support is not compiled into this binary; "
        << "only options.num_threads=1 is supported. Switching"
        << "to single threaded mode.";
    options.num_threads = 1;
  }
  if (options.num_linear_solver_threads > 1) {
    LOG(WARNING)
        << "OpenMP support is not compiled into this binary; "
        << "only options.num_linear_solver_threads=1 is supported. Switching"
        << "to single threaded mode.";
    options.num_linear_solver_threads = 1;
  }
#endif

  summary->linear_solver_type_given = options.linear_solver_type;
  summary->num_eliminate_blocks_given = original_options.num_eliminate_blocks;
  summary->num_threads_given = original_options.num_threads;
  summary->num_linear_solver_threads_given =
      original_options.num_linear_solver_threads;
  summary->ordering_type = original_options.ordering_type;

  summary->num_parameter_blocks = problem_impl->NumParameterBlocks();
  summary->num_parameters = problem_impl->NumParameters();
  summary->num_residual_blocks = problem_impl->NumResidualBlocks();
  summary->num_residuals = problem_impl->NumResiduals();

  summary->num_threads_used = options.num_threads;
  summary->sparse_linear_algebra_library =
      options.sparse_linear_algebra_library;
  summary->trust_region_strategy_type = options.trust_region_strategy_type;

  // Evaluate the initial cost, residual vector and the jacobian
  // matrix if requested by the user. The initial cost needs to be
  // computed on the original unpreprocessed problem, as it is used to
  // determine the value of the "fixed" part of the objective function
  // after the problem has undergone reduction.
  Evaluator::Evaluate(
      original_program,
      options.num_threads,
      &(summary->initial_cost),
      options.return_initial_residuals ? &summary->initial_residuals : NULL,
      options.return_initial_gradient ? &summary->initial_gradient : NULL,
      options.return_initial_jacobian ? &summary->initial_jacobian : NULL);
   original_program->SetParameterBlockStatePtrsToUserStatePtrs();

  // If the user requests gradient checking, construct a new
  // ProblemImpl by wrapping the CostFunctions of problem_impl inside
  // GradientCheckingCostFunction and replacing problem_impl with
  // gradient_checking_problem_impl.
  scoped_ptr<ProblemImpl> gradient_checking_problem_impl;
  // Save the original problem impl so we don't use the gradient
  // checking one when computing the residuals.
  if (options.check_gradients) {
    VLOG(1) << "Checking Gradients";
    gradient_checking_problem_impl.reset(
        CreateGradientCheckingProblemImpl(
            problem_impl,
            options.numeric_derivative_relative_step_size,
            options.gradient_check_relative_precision));

    // From here on, problem_impl will point to the GradientChecking version.
    problem_impl = gradient_checking_problem_impl.get();
  }

  // Create the three objects needed to minimize: the transformed program, the
  // evaluator, and the linear solver.

  scoped_ptr<Program> reduced_program(
      CreateReducedProgram(&options, problem_impl, &summary->fixed_cost, &summary->error));
  if (reduced_program == NULL) {
    return;
  }

  summary->num_parameter_blocks_reduced = reduced_program->NumParameterBlocks();
  summary->num_parameters_reduced = reduced_program->NumParameters();
  summary->num_residual_blocks_reduced = reduced_program->NumResidualBlocks();
  summary->num_residuals_reduced = reduced_program->NumResiduals();

  scoped_ptr<LinearSolver>
      linear_solver(CreateLinearSolver(&options, &summary->error));
  summary->linear_solver_type_used = options.linear_solver_type;
  summary->preconditioner_type = options.preconditioner_type;
  summary->num_eliminate_blocks_used = options.num_eliminate_blocks;
  summary->num_linear_solver_threads_used = options.num_linear_solver_threads;

  if (linear_solver == NULL) {
    return;
  }

  if (!MaybeReorderResidualBlocks(options,
                                  reduced_program.get(),
                                  &summary->error)) {
    return;
  }

  scoped_ptr<Evaluator> evaluator(
      CreateEvaluator(options, reduced_program.get(), &summary->error));
  if (evaluator == NULL) {
    return;
  }

  // The optimizer works on contiguous parameter vectors; allocate some.
  Vector parameters(reduced_program->NumParameters());

  // Collect the discontiguous parameters into a contiguous state vector.
  reduced_program->ParameterBlocksToStateVector(parameters.data());

  time_t minimizer_start_time = time(NULL);
  summary->preprocessor_time_in_seconds =
      minimizer_start_time - solver_start_time;

  // Run the optimization.
  Minimize(options,
           reduced_program.get(),
           evaluator.get(),
           linear_solver.get(),
           parameters.data(),
           summary);

  // If the user aborted mid-optimization or the optimization
  // terminated because of a numerical failure, then return without
  // updating user state.
  if (summary->termination_type == USER_ABORT ||
      summary->termination_type == NUMERICAL_FAILURE) {
    return;
  }

  time_t post_process_start_time = time(NULL);

  // Push the contiguous optimized parameters back to the user's parameters.
  reduced_program->StateVectorToParameterBlocks(parameters.data());
  reduced_program->CopyParameterBlockStateToUserState();

  // Evaluate the final cost, residual vector and the jacobian
  // matrix if requested by the user.
  Evaluator::Evaluate(
      original_program,
      options.num_threads,
      &summary->final_cost,
      options.return_final_residuals ? &summary->final_residuals : NULL,
      options.return_final_gradient ? &summary->final_gradient : NULL,
      options.return_final_jacobian ? &summary->final_jacobian : NULL);

  // Ensure the program state is set to the user parameters on the way out.
  original_program->SetParameterBlockStatePtrsToUserStatePtrs();
  // Stick a fork in it, we're done.
  summary->postprocessor_time_in_seconds = time(NULL) - post_process_start_time;
}

// Strips varying parameters and residuals, maintaining order, and updating
// num_eliminate_blocks.
bool SolverImpl::RemoveFixedBlocksFromProgram(Program* program,
                                              int* num_eliminate_blocks,
                                              double* fixed_cost,
                                              string* error) {
  int original_num_eliminate_blocks = *num_eliminate_blocks;
  vector<ParameterBlock*>* parameter_blocks =
      program->mutable_parameter_blocks();

  scoped_array<double> residual_block_evaluate_scratch;
  if (fixed_cost != NULL) {
    residual_block_evaluate_scratch.reset(
        new double[program->MaxScratchDoublesNeededForEvaluate()]);
    *fixed_cost = 0.0;
  }

  // Mark all the parameters as unused. Abuse the index member of the parameter
  // blocks for the marking.
  for (int i = 0; i < parameter_blocks->size(); ++i) {
    (*parameter_blocks)[i]->set_index(-1);
  }

  // Filter out residual that have all-constant parameters, and mark all the
  // parameter blocks that appear in residuals.
  {
    vector<ResidualBlock*>* residual_blocks =
        program->mutable_residual_blocks();
    int j = 0;
    for (int i = 0; i < residual_blocks->size(); ++i) {
      ResidualBlock* residual_block = (*residual_blocks)[i];
      int num_parameter_blocks = residual_block->NumParameterBlocks();

      // Determine if the residual block is fixed, and also mark varying
      // parameters that appear in the residual block.
      bool all_constant = true;
      for (int k = 0; k < num_parameter_blocks; k++) {
        ParameterBlock* parameter_block = residual_block->parameter_blocks()[k];
        if (!parameter_block->IsConstant()) {
          all_constant = false;
          parameter_block->set_index(1);
        }
      }

      if (!all_constant) {
        (*residual_blocks)[j++] = (*residual_blocks)[i];
      } else if (fixed_cost != NULL) {
        // The residual is constant and will be removed, so its cost is
        // added to the variable fixed_cost.
        double cost = 0.0;
        if (!residual_block->Evaluate(
              &cost, NULL, NULL, residual_block_evaluate_scratch.get())) {
          *error = StringPrintf("Evaluation of the residual %d failed during "
                                "removal of fixed residual blocks.", i);
          return false;
        }
        *fixed_cost += cost;
      }
    }
    residual_blocks->resize(j);
  }

  // Filter out unused or fixed parameter blocks, and update
  // num_eliminate_blocks as necessary.
  {
    vector<ParameterBlock*>* parameter_blocks =
        program->mutable_parameter_blocks();
    int j = 0;
    for (int i = 0; i < parameter_blocks->size(); ++i) {
      ParameterBlock* parameter_block = (*parameter_blocks)[i];
      if (parameter_block->index() == 1) {
        (*parameter_blocks)[j++] = parameter_block;
      } else if (i < original_num_eliminate_blocks) {
        (*num_eliminate_blocks)--;
      }
    }
    parameter_blocks->resize(j);
  }

  CHECK(((program->NumResidualBlocks() == 0) &&
         (program->NumParameterBlocks() == 0)) ||
        ((program->NumResidualBlocks() != 0) &&
         (program->NumParameterBlocks() != 0)))
      << "Congratulations, you found a bug in Ceres. Please report it.";
  return true;
}

Program* SolverImpl::CreateReducedProgram(Solver::Options* options,
                                          ProblemImpl* problem_impl,
                                          double* fixed_cost,
                                          string* error) {
  Program* original_program = problem_impl->mutable_program();
  scoped_ptr<Program> transformed_program(new Program(*original_program));

  if (options->ordering_type == USER &&
      !ApplyUserOrdering(*problem_impl,
                         options->ordering,
                         transformed_program.get(),
                         error)) {
    return NULL;
  }

  if (options->ordering_type == SCHUR && options->num_eliminate_blocks != 0) {
    *error = "Can't specify SCHUR ordering and num_eliminate_blocks "
        "at the same time; SCHUR ordering determines "
        "num_eliminate_blocks automatically.";
    return NULL;
  }

  if (options->ordering_type == SCHUR && options->ordering.size() != 0) {
    *error = "Can't specify SCHUR ordering type and the ordering "
        "vector at the same time; SCHUR ordering determines "
        "a suitable parameter ordering automatically.";
    return NULL;
  }

  int num_eliminate_blocks = options->num_eliminate_blocks;

  if (!RemoveFixedBlocksFromProgram(transformed_program.get(),
                                    &num_eliminate_blocks,
                                    fixed_cost,
                                    error)) {
    return NULL;
  }

  if (transformed_program->NumParameterBlocks() == 0) {
    LOG(WARNING) << "No varying parameter blocks to optimize; "
                 << "bailing early.";
    return transformed_program.release();
  }

  if (options->ordering_type == SCHUR) {
    vector<ParameterBlock*> schur_ordering;
    num_eliminate_blocks = ComputeSchurOrdering(*transformed_program,
                                                &schur_ordering);
    CHECK_EQ(schur_ordering.size(), transformed_program->NumParameterBlocks())
        << "Congratulations, you found a Ceres bug! Please report this error "
        << "to the developers.";

    // Replace the transformed program's ordering with the schur ordering.
    swap(*transformed_program->mutable_parameter_blocks(), schur_ordering);
  }
  options->num_eliminate_blocks = num_eliminate_blocks;
  CHECK_GE(options->num_eliminate_blocks, 0)
      << "Congratulations, you found a Ceres bug! Please report this error "
      << "to the developers.";

  // Since the transformed program is the "active" program, and it is mutated,
  // update the parameter offsets and indices.
  transformed_program->SetParameterOffsetsAndIndex();
  return transformed_program.release();
}

LinearSolver* SolverImpl::CreateLinearSolver(Solver::Options* options,
                                             string* error) {
  if (options->trust_region_strategy_type == DOGLEG) {
    if (options->linear_solver_type == ITERATIVE_SCHUR ||
        options->linear_solver_type == CGNR) {
      *error = "DOGLEG only supports exact factorization based linear "
               "solvers. If you want to use an iterative solver please "
               "use LEVENBERG_MARQUARDT as the trust_region_strategy_type";
      return NULL;
    }
  }

#ifdef CERES_NO_SUITESPARSE
  if (options->linear_solver_type == SPARSE_NORMAL_CHOLESKY &&
      options->sparse_linear_algebra_library == SUITE_SPARSE) {
    *error = "Can't use SPARSE_NORMAL_CHOLESKY with SUITESPARSE because "
             "SuiteSparse was not enabled when Ceres was built.";
    return NULL;
  }
#endif

#ifdef CERES_NO_CXSPARSE
  if (options->linear_solver_type == SPARSE_NORMAL_CHOLESKY &&
      options->sparse_linear_algebra_library == CX_SPARSE) {
    *error = "Can't use SPARSE_NORMAL_CHOLESKY with CXSPARSE because "
             "CXSparse was not enabled when Ceres was built.";
    return NULL;
  }
#endif


  if (options->linear_solver_max_num_iterations <= 0) {
    *error = "Solver::Options::linear_solver_max_num_iterations is 0.";
    return NULL;
  }
  if (options->linear_solver_min_num_iterations <= 0) {
    *error = "Solver::Options::linear_solver_min_num_iterations is 0.";
    return NULL;
  }
  if (options->linear_solver_min_num_iterations >
      options->linear_solver_max_num_iterations) {
    *error = "Solver::Options::linear_solver_min_num_iterations > "
        "Solver::Options::linear_solver_max_num_iterations.";
    return NULL;
  }

  LinearSolver::Options linear_solver_options;
  linear_solver_options.min_num_iterations =
        options->linear_solver_min_num_iterations;
  linear_solver_options.max_num_iterations =
      options->linear_solver_max_num_iterations;
  linear_solver_options.type = options->linear_solver_type;
  linear_solver_options.preconditioner_type = options->preconditioner_type;
  linear_solver_options.sparse_linear_algebra_library =
      options->sparse_linear_algebra_library;
  linear_solver_options.use_block_amd = options->use_block_amd;

#ifdef CERES_NO_SUITESPARSE
  if (linear_solver_options.preconditioner_type == SCHUR_JACOBI) {
    *error =  "SCHUR_JACOBI preconditioner not suppored. Please build Ceres "
        "with SuiteSparse support.";
    return NULL;
  }

  if (linear_solver_options.preconditioner_type == CLUSTER_JACOBI) {
    *error =  "CLUSTER_JACOBI preconditioner not suppored. Please build Ceres "
        "with SuiteSparse support.";
    return NULL;
  }

  if (linear_solver_options.preconditioner_type == CLUSTER_TRIDIAGONAL) {
    *error =  "CLUSTER_TRIDIAGONAL preconditioner not suppored. Please build "
        "Ceres with SuiteSparse support.";
    return NULL;
  }
#endif

  linear_solver_options.num_threads = options->num_linear_solver_threads;
  linear_solver_options.num_eliminate_blocks =
      options->num_eliminate_blocks;

  if ((linear_solver_options.num_eliminate_blocks == 0) &&
      IsSchurType(linear_solver_options.type)) {
#if defined(CERES_NO_SUITESPARSE) && defined(CERES_NO_CXSPARSE)
    LOG(INFO) << "No elimination block remaining switching to DENSE_QR.";
    linear_solver_options.type = DENSE_QR;
#else
    LOG(INFO) << "No elimination block remaining "
              << "switching to SPARSE_NORMAL_CHOLESKY.";
    linear_solver_options.type = SPARSE_NORMAL_CHOLESKY;
#endif
  }

#if defined(CERES_NO_SUITESPARSE) && defined(CERES_NO_CXSPARSE)
  if (linear_solver_options.type == SPARSE_SCHUR) {
    *error = "Can't use SPARSE_SCHUR because neither SuiteSparse nor"
             "CXSparse was enabled when Ceres was compiled.";
    return NULL;
  }
#endif

  // The matrix used for storing the dense Schur complement has a
  // single lock guarding the whole matrix. Running the
  // SchurComplementSolver with multiple threads leads to maximum
  // contention and slowdown. If the problem is large enough to
  // benefit from a multithreaded schur eliminator, you should be
  // using a SPARSE_SCHUR solver anyways.
  if ((linear_solver_options.num_threads > 1) &&
      (linear_solver_options.type == DENSE_SCHUR)) {
    LOG(WARNING) << "Warning: Solver::Options::num_linear_solver_threads = "
                 << options->num_linear_solver_threads
                 << " with DENSE_SCHUR will result in poor performance; "
                 << "switching to single-threaded.";
    linear_solver_options.num_threads = 1;
  }

  options->linear_solver_type = linear_solver_options.type;
  options->num_linear_solver_threads = linear_solver_options.num_threads;

  return LinearSolver::Create(linear_solver_options);
}

bool SolverImpl::ApplyUserOrdering(const ProblemImpl& problem_impl,
                                   vector<double*>& ordering,
                                   Program* program,
                                   string* error) {
  if (ordering.size() != program->NumParameterBlocks()) {
    *error = StringPrintf("User specified ordering does not have the same "
                          "number of parameters as the problem. The problem"
                          "has %d blocks while the ordering has %ld blocks.",
                          program->NumParameterBlocks(),
                          ordering.size());
    return false;
  }

  // Ensure that there are no duplicates in the user's ordering.
  {
    vector<double*> ordering_copy(ordering);
    sort(ordering_copy.begin(), ordering_copy.end());
    if (unique(ordering_copy.begin(), ordering_copy.end())
        != ordering_copy.end()) {
      *error = "User specified ordering contains duplicates.";
      return false;
    }
  }

  vector<ParameterBlock*>* parameter_blocks =
      program->mutable_parameter_blocks();

  fill(parameter_blocks->begin(),
       parameter_blocks->end(),
       static_cast<ParameterBlock*>(NULL));

  const ProblemImpl::ParameterMap& parameter_map = problem_impl.parameter_map();
  for (int i = 0; i < ordering.size(); ++i) {
    ProblemImpl::ParameterMap::const_iterator it =
        parameter_map.find(ordering[i]);
    if (it == parameter_map.end()) {
      *error = StringPrintf("User specified ordering contains a pointer "
                            "to a double that is not a parameter block in the "
                            "problem. The invalid double is at position %d "
                            " in options.ordering.", i);
      return false;
    }
    (*parameter_blocks)[i] = it->second;
  }
  return true;
}

// Find the minimum index of any parameter block to the given residual.
// Parameter blocks that have indices greater than num_eliminate_blocks are
// considered to have an index equal to num_eliminate_blocks.
int MinParameterBlock(const ResidualBlock* residual_block,
                      int num_eliminate_blocks) {
  int min_parameter_block_position = num_eliminate_blocks;
  for (int i = 0; i < residual_block->NumParameterBlocks(); ++i) {
    ParameterBlock* parameter_block = residual_block->parameter_blocks()[i];
    if (!parameter_block->IsConstant()) {
      CHECK_NE(parameter_block->index(), -1)
          << "Did you forget to call Program::SetParameterOffsetsAndIndex()? "
          << "This is a Ceres bug; please contact the developers!";
      min_parameter_block_position = std::min(parameter_block->index(),
                                              min_parameter_block_position);
    }
  }
  return min_parameter_block_position;
}

// Reorder the residuals for program, if necessary, so that the residuals
// involving each E block occur together. This is a necessary condition for the
// Schur eliminator, which works on these "row blocks" in the jacobian.
bool SolverImpl::MaybeReorderResidualBlocks(const Solver::Options& options,
                                            Program* program,
                                            string* error) {
  // Only Schur types require the lexicographic reordering.
  if (!IsSchurType(options.linear_solver_type)) {
    return true;
  }

  CHECK_NE(0, options.num_eliminate_blocks)
        << "Congratulations, you found a Ceres bug! Please report this error "
        << "to the developers.";

  // Create a histogram of the number of residuals for each E block. There is an
  // extra bucket at the end to catch all non-eliminated F blocks.
  vector<int> residual_blocks_per_e_block(options.num_eliminate_blocks + 1);
  vector<ResidualBlock*>* residual_blocks = program->mutable_residual_blocks();
  vector<int> min_position_per_residual(residual_blocks->size());
  for (int i = 0; i < residual_blocks->size(); ++i) {
    ResidualBlock* residual_block = (*residual_blocks)[i];
    int position = MinParameterBlock(residual_block,
                                     options.num_eliminate_blocks);
    min_position_per_residual[i] = position;
    DCHECK_LE(position, options.num_eliminate_blocks);
    residual_blocks_per_e_block[position]++;
  }

  // Run a cumulative sum on the histogram, to obtain offsets to the start of
  // each histogram bucket (where each bucket is for the residuals for that
  // E-block).
  vector<int> offsets(options.num_eliminate_blocks + 1);
  std::partial_sum(residual_blocks_per_e_block.begin(),
                   residual_blocks_per_e_block.end(),
                   offsets.begin());
  CHECK_EQ(offsets.back(), residual_blocks->size())
      << "Congratulations, you found a Ceres bug! Please report this error "
      << "to the developers.";

  CHECK(find(residual_blocks_per_e_block.begin(),
             residual_blocks_per_e_block.end() - 1, 0) !=
        residual_blocks_per_e_block.end())
      << "Congratulations, you found a Ceres bug! Please report this error "
      << "to the developers.";

  // Fill in each bucket with the residual blocks for its corresponding E block.
  // Each bucket is individually filled from the back of the bucket to the front
  // of the bucket. The filling order among the buckets is dictated by the
  // residual blocks. This loop uses the offsets as counters; subtracting one
  // from each offset as a residual block is placed in the bucket. When the
  // filling is finished, the offset pointerts should have shifted down one
  // entry (this is verified below).
  vector<ResidualBlock*> reordered_residual_blocks(
      (*residual_blocks).size(), static_cast<ResidualBlock*>(NULL));
  for (int i = 0; i < residual_blocks->size(); ++i) {
    int bucket = min_position_per_residual[i];

    // Decrement the cursor, which should now point at the next empty position.
    offsets[bucket]--;

    // Sanity.
    CHECK(reordered_residual_blocks[offsets[bucket]] == NULL)
        << "Congratulations, you found a Ceres bug! Please report this error "
        << "to the developers.";

    reordered_residual_blocks[offsets[bucket]] = (*residual_blocks)[i];
  }

  // Sanity check #1: The difference in bucket offsets should match the
  // histogram sizes.
  for (int i = 0; i < options.num_eliminate_blocks; ++i) {
    CHECK_EQ(residual_blocks_per_e_block[i], offsets[i + 1] - offsets[i])
        << "Congratulations, you found a Ceres bug! Please report this error "
        << "to the developers.";
  }
  // Sanity check #2: No NULL's left behind.
  for (int i = 0; i < reordered_residual_blocks.size(); ++i) {
    CHECK(reordered_residual_blocks[i] != NULL)
        << "Congratulations, you found a Ceres bug! Please report this error "
        << "to the developers.";
  }

  // Now that the residuals are collected by E block, swap them in place.
  swap(*program->mutable_residual_blocks(), reordered_residual_blocks);
  return true;
}

Evaluator* SolverImpl::CreateEvaluator(const Solver::Options& options,
                                       Program* program,
                                       string* error) {
  Evaluator::Options evaluator_options;
  evaluator_options.linear_solver_type = options.linear_solver_type;
  evaluator_options.num_eliminate_blocks = options.num_eliminate_blocks;
  evaluator_options.num_threads = options.num_threads;
  return Evaluator::Create(evaluator_options, program, error);
}

}  // namespace internal
}  // namespace ceres
