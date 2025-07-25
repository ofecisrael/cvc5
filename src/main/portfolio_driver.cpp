/******************************************************************************
 * Top contributors (to current version):
 *   Gereon Kremer, Daniel Larraz, Andrew Reynolds
 *
 * This file is part of the cvc5 project.
 *
 * Copyright (c) 2009-2025 by the authors listed in the file AUTHORS
 * in the top-level source directory and their institutional affiliations.
 * All rights reserved.  See the file COPYING in the top-level source
 * directory for licensing information.
 * ****************************************************************************
 *
 */
#include "main/portfolio_driver.h"

#if HAVE_SYS_WAIT_H
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cvc5/cvc5.h>

#include <chrono>
#include <cstdlib>
#include <optional>
#include <thread>

#include "base/check.h"
#include "base/exception.h"
#include "base/output.h"
#include "main/command_executor.h"
#include "parser/commands.h"
#include "parser/command_status.h"

using namespace cvc5::parser;

namespace cvc5::main {

enum SolveStatus : int
{
  STATUS_SOLVED = 0,
  STATUS_UNSOLVED = 1,
};

bool ExecutionContext::solveContinuous(parser::InputParser* parser,
                                       bool stopAtSetLogic,
                                       bool stopAtCheckSat)
{
  Command cmd;
  bool interrupted = false;
  bool status = true;
  while (status)
  {
    if (interrupted)
    {
      solver().getDriverOptions().out() << CommandInterrupted();
      d_executor->reset();
      break;
    }
    cmd = parser->nextCommand();
    if (cmd.isNull())
    {
      break;
    }
    Cmd* cc = cmd.d_cmd.get();
    if (stopAtCheckSat)
    {
      if (dynamic_cast<CheckSatCommand*>(cc) != nullptr)
      {
        d_hasReadCheckSat = true;
        break;
      }
    }
    status = d_executor->doCommand(&cmd);
    if (cc->interrupted() && status == 0)
    {
      interrupted = true;
      break;
    }
    if (dynamic_cast<QuitCommand*>(cc) != nullptr)
    {
      break;
    }
    if (stopAtSetLogic)
    {
      auto* slc = dynamic_cast<SetBenchmarkLogicCommand*>(cc);
      if (slc != nullptr)
      {
        d_logic = slc->getLogic();
        break;
      }
    }
  }
  return status;
}

bool ExecutionContext::continueAfterSolving(parser::InputParser* parser)
{
  Command cmd;
  bool interrupted = false;
  bool status = true;
  while (status)
  {
    if (interrupted)
    {
      solver().getDriverOptions().out() << CommandInterrupted();
      d_executor->reset();
      break;
    }
    cmd = parser->nextCommand();
    if (cmd.isNull())
    {
      break;
    }
    Cmd* cc = cmd.d_cmd.get();
    if (dynamic_cast<GetModelCommand*>(cc) != nullptr)
    {
      try
      {
        std::string result = solver().getModel(d_sorts, d_terms);
        solver().getDriverOptions().out() << result;
        status = true;
      }
      catch (std::exception& e)
      {
        status = false;
      }
    }
    else if (dynamic_cast<GetUnsatCoreCommand*>(cc) != nullptr)
    {
      try
      {
        std::vector<cvc5::Term> core = solver().getUnsatCore();
        std::ostream& out = solver().getDriverOptions().out();
        out << "(" << std::endl;
        for (const cvc5::Term& t : core)
        {
          auto it = d_named_terms.find(t);
          if (it != d_named_terms.end())
          {
            out << it->second << std::endl;
          }
        }
        out << ")" << std::endl;
        status = true;
      }
      catch (std::exception& e)
      {
        status = false;
      }
    }
    else
    {
      status = d_executor->doCommand(&cmd);
    }
    if (cc->interrupted() && status == 0)
    {
      interrupted = true;
      break;
    }
    if (dynamic_cast<QuitCommand*>(cc) != nullptr)
    {
      break;
    }
  }
  return status;
}

bool ExecutionContext::runCheckSatCommand()
{
  std::shared_ptr<Cmd> cmd(new CheckSatCommand());
  Command command(cmd);
  return d_executor->doCommand(&command);
}

void ExecutionContext::storeDeclarationsAndNamedTerms()
{
  SymbolManager* sm = d_executor->getSymbolManager();
  d_sorts = sm->getDeclaredSorts();
  d_terms = sm->getDeclaredTerms();
  d_named_terms = sm->getNamedTerms();
}

bool ExecutionContext::runResetCommand()
{
  std::shared_ptr<Cmd> cmd(new ResetCommand());
  Command command(cmd);
  return d_executor->doCommand(&command);
}

std::vector<Command> ExecutionContext::parseCommands(
    parser::InputParser* parser)
{
  std::vector<Command> res;
  while (true)
  {
    Command cmd = parser->nextCommand();
    if (cmd.isNull())
    {
      break;
    }
    res.emplace_back(cmd);
    if (dynamic_cast<QuitCommand*>(cmd.d_cmd.get()) != nullptr)
    {
      break;
    }
  }
  return res;
}

bool ExecutionContext::solveCommands(std::vector<Command>& cmds)
{
  bool interrupted = false;
  bool status = true;
  for (Command& cmd : cmds)
  {
    if (interrupted)
    {
      solver().getDriverOptions().out() << CommandInterrupted();
      d_executor->reset();
      break;
    }

    status = d_executor->doCommand(&cmd);
    Cmd* cc = cmd.d_cmd.get();
    if (cc->interrupted() && status == 0)
    {
      interrupted = true;
      break;
    }

    if (dynamic_cast<QuitCommand*>(cc) != nullptr)
    {
      break;
    }
  }
  return status;
}

#if HAVE_SYS_WAIT_H

namespace {

/**
 * Provides a convenient wrapper for POSIX pipes in the context of forking.
 * The implemented mechanism is using a pipe to buffer the (standard or error)
 * output of a child process and optionally copy it to the respective output of
 * the parent process. This wrapper closely follows
 * http://www.microhowto.info/howto/capture_the_output_of_a_child_process_in_c.html
 */
class Pipe
{
 public:
  /** Open a new pipe */
  void open()
  {
    if (pipe(d_pipe) == -1)
    {
      throw internal::Exception("Unable to open pipe for child process");
    }
  }
  /**
   * Redirects the given file descriptor fd into this pipe using dup2 and closes
   * both ends of the pipe. This method should be called within the child
   * process after forking to redirect standard out or error out into the pipe.
   */
  void dup(int fd)
  {
    // dup2 may get interrupted by a signal. If this happens the error is EINTR
    // and we can simply try again.
    while ((dup2(d_pipe[1], fd) == -1) && (errno == EINTR))
    {
    }
    close(d_pipe[0]);
    close(d_pipe[1]);
  }
  /**
   * Close the input of this pipe. This method should be called within the
   * parent process after forking.
   */
  void closeIn() { close(d_pipe[1]); }
  /**
   * Copy the content of the pipe into the given output stream. This method
   * should be called within the parent process after the child process has
   * terminated.
   */
  void flushTo(std::ostream& os)
  {
    char buf[4096];
    while (true)
    {
      ssize_t cnt = read(d_pipe[0], buf, sizeof(buf));
      if (cnt == -1)
      {
        if (errno == EINTR)
        {
          continue;
        }
        else
        {
          throw internal::Exception("Unable to read from pipe");
        }
      }
      else if (cnt == 0)
      {
        break;
      }
      os.write(buf, cnt);
    }
  }

 private:
  int d_pipe[2];
};

void printPortfolioConfig(Solver& solver, PortfolioConfig& config)
{
  bool dry_run = solver.getOption("portfolio-dry-run") == "true";
  if (dry_run || solver.isOutputOn("portfolio"))
  {
    std::ostream& out = (dry_run) ? solver.getDriverOptions().out()
                                  : solver.getOutput("portfolio");
    out << "(portfolio \"" << config.toOptionString() << "\"";
    out << " :timeout " << config.d_timeout;
    out << ")" << std::endl;
  }
}

/**
 * Manages running portfolio configurations until one has solved the input
 * problem. Depending on --portfolio-jobs runs multiple jobs in parallel.
 */
class PortfolioProcessPool
{
  enum class JobState
  {
    PENDING,
    RUNNING,
    DONE
  };
  /**
   * A job, consisting of the configuration, the pids of the worker and timeout
   * process, the stderr and stdout pipes and the job state.
   * Initially, a job is created but not started and all properties except for
   * the configuration have their default value. Then starting a job, the state
   * ich changed to RUNNING and the pids and pipes have their proper values. If
   * no timeout is enforced, the timeout pid remains unchanged. After the job
   * has finished, checkResults() eventually analyzes the jobs result and
   * changes the state to DONE.
   */
  struct Job
  {
    PortfolioConfig d_config;
    pid_t d_worker = -1;
    pid_t d_timeout = -1;
    Pipe d_errPipe;
    Pipe d_outPipe;
    JobState d_state = JobState::PENDING;
  };

 public:
  PortfolioProcessPool(ExecutionContext& ctx, parser::InputParser* parser, uint64_t timeout)
      : d_ctx(ctx),
        d_parser(parser),
        d_maxJobs(ctx.solver().getOptionInfo("portfolio-jobs").uintValue()),
        d_timeout(timeout)
  {
  }

  bool run(PortfolioStrategy& strategy)
  {
    for (const auto& s : strategy.d_strategies)
    {
      d_jobs.emplace_back(Job{s});
    }

    // While there are jobs to be run or jobs still running
    while (d_nextJob < d_jobs.size() || d_running > 0)
    {
      // Check if any job was successful
      if (checkResults())
      {
        return true;
      }

      // While we can start jobs right now
      while (d_nextJob < d_jobs.size() && d_running < d_maxJobs)
      {
        startNextJob();
      }

      if (d_running > 0)
      {
        int wstatus = 0;
        pid_t child = wait(&wstatus);
        if (checkResults(child, wstatus))
        {
          return true;
        }
      }
    }
    if (checkResults()) return true;

    return false;
  }

 private:
  void startNextJob()
  {
    Assert(d_nextJob < d_jobs.size());
    Job& job = d_jobs[d_nextJob];
    Trace("portfolio") << "Starting " << job.d_config << std::endl;
    printPortfolioConfig(d_ctx.solver(), job.d_config);

    // Set up pipes to capture output of worker
    job.d_errPipe.open();
    job.d_outPipe.open();
    // Start the worker process
    job.d_worker = fork();
    if (job.d_worker == -1)
    {
      throw internal::Exception("Unable to fork");
    }
    if (job.d_worker == 0)
    {
      job.d_errPipe.dup(STDERR_FILENO);
      job.d_outPipe.dup(STDOUT_FILENO);

      std::vector<cvc5::Term> assertions = d_ctx.solver().getAssertions();
      std::string logic = d_ctx.solver().getLogic();

      std::string produceUnsatCoresValue =
          d_ctx.solver().getOption("produce-unsat-cores");
      std::string produceModelsValue =
          d_ctx.solver().getOption("produce-models");
      d_ctx.storeDeclarationsAndNamedTerms();

      d_ctx.runResetCommand();

      d_ctx.solver().setOption("produce-unsat-cores", produceUnsatCoresValue);
      d_ctx.solver().setOption("produce-models", produceModelsValue);
      job.d_config.applyOptions(d_ctx.solver());
      d_ctx.solver().setLogic(logic);

      for (Term& t : assertions)
      {
        d_ctx.solver().assertFormula(t);
      }
      // 0 = solved, 1 = not solved
      SolveStatus rc = SolveStatus::STATUS_UNSOLVED;
      if (d_ctx.runCheckSatCommand())
      // if (d_ctx.solveCommands(d_commands))
      {
        Result res = d_ctx.d_executor->getResult();
        d_ctx.continueAfterSolving(d_parser);
        if (res.isSat() || res.isUnsat())
        {
          rc = SolveStatus::STATUS_SOLVED;
        }
      }
      _exit(rc);
    }
    job.d_errPipe.closeIn();
    job.d_outPipe.closeIn();

    // Start the timeout process
    if (d_timeout > 0 && job.d_config.d_timeout > 0)
    {
      job.d_timeout = fork();
      if (job.d_timeout == 0)
      {
        auto duration = std::chrono::duration<double, std::milli>(
            job.d_config.d_timeout * d_timeout);
        std::this_thread::sleep_for(duration);
        kill(job.d_worker, SIGKILL);
        _exit(0);
      }
    }

    ++d_nextJob;
    ++d_running;
    job.d_state = JobState::RUNNING;
  }

  /**
   * Check whether some process terminated and solved the input. If so,
   * forward the child process output to the main out and return true.
   * Otherwise return false.
   * If child and status are given, only the job with this particular worker is
   * considered and status is assumed to be the wstatus reported by waitpid.
   */
  bool checkResults(pid_t child = -1, int status = 0)
  {
    // check d_jobs for items where worker has terminated and timeout != -1
    for (auto& job : d_jobs)
    {
      // has not been started yet
      if (job.d_state == JobState::PENDING) continue;
      // has already been analyzed
      if (job.d_state == JobState::DONE) continue;
      // was given an explicit child, but this is not it.
      if (child != -1 && job.d_worker != child) continue;

      int wstatus = 0;
      pid_t res = 0;
      if (child == -1)
      {
        res = waitpid(job.d_worker, &wstatus, WNOHANG);
        // has not terminated yet
        if (res == 0) continue;
        if (res == -1) continue;
      }
      else
      {
        res = child;
        wstatus = status;
      }
      // mark as analyzed
      Trace("portfolio") << "Finished " << job.d_config << std::endl;
      // terminate the corresponding timeout process if it is still running
      if (job.d_timeout > 0)
      {
        kill(job.d_timeout, SIGKILL);
      }
      job.d_state = JobState::DONE;
      --d_running;
      // check if exited normally
      if (WIFSIGNALED(wstatus))
      {
        continue;
      }
      if (WIFEXITED(wstatus))
      {
        if (WEXITSTATUS(wstatus) == SolveStatus::STATUS_SOLVED)
        {
          Trace("portfolio") << "Successful!" << std::endl;
          if (d_ctx.solver().isOutputOn("portfolio"))
          {
            std::ostream& out = d_ctx.solver().getOutput("portfolio");
            out << "(portfolio-success \"" << job.d_config.toOptionString()
                << "\")" << std::endl;
          }
          job.d_errPipe.flushTo(std::cerr);
          job.d_outPipe.flushTo(std::cout);
          return true;
        }
      }
    }
    return false;
  }

  ExecutionContext& d_ctx;
  parser::InputParser* d_parser;
  /** All jobs. */
  std::vector<Job> d_jobs;
  /** The id of the next job to be started within d_jobs */
  size_t d_nextJob = 0;
  /** The number of currently running jobs */
  size_t d_running = 0;
  const uint64_t d_maxJobs;
  const uint64_t d_timeout;
};

}  // namespace

#endif

bool PortfolioDriver::solve(std::unique_ptr<CommandExecutor>& executor)
{
  ExecutionContext ctx{executor.get()};
  Solver& solver = ctx.solver();
  bool use_portfolio = solver.getOption("use-portfolio") == "true";
  if (!use_portfolio)
  {
    return ctx.solveContinuous(d_parser, false);
  }
#if HAVE_SYS_WAIT_H
  ctx.solveContinuous(d_parser, true);

  if (!ctx.d_logic)
  {
    return ctx.solveContinuous(d_parser, false);
  }

  bool dry_run = solver.getOption("portfolio-dry-run") == "true";

  bool incremental_solving = solver.getOption("incremental") == "true";
  PortfolioStrategy strategy = getStrategy(incremental_solving, *ctx.d_logic);
  Assert(!strategy.d_strategies.empty()) << "The portfolio strategy should never be empty.";
  if (strategy.d_strategies.size() == 1)
  {
    PortfolioConfig& config = strategy.d_strategies.front();
    printPortfolioConfig(ctx.solver(), config);
    if (dry_run) return true;
    config.applyOptions(solver);
    return ctx.solveContinuous(d_parser, false);
  }

  uint64_t total_timeout = ctx.solver().getOptionInfo("tlimit").uintValue();
  if (total_timeout == 0)
  {
    total_timeout = 1'200'000; // miliseconds
  }

  if (dry_run)
  {
    for (PortfolioConfig& config : strategy.d_strategies)
    {
      printPortfolioConfig(ctx.solver(), config);
    }
    return true;
  }

  bool uninterrupted = ctx.solveContinuous(d_parser, false, true);
  if (uninterrupted && ctx.d_hasReadCheckSat)
  {
    PortfolioProcessPool pool(ctx, d_parser, total_timeout);  // ctx.parseCommands(d_parser));
    bool solved = pool.run(strategy);
    if (!solved)
    {
      std::cout << "unknown" << std::endl;
    }
    return solved;
  }
  return uninterrupted;
#else
  Warning() << "Can't run portfolio without <sys/wait.h>.";
  return ctx.solveContinuous(d_parser, false);
#endif
}

std::string PortfolioConfig::toOptionString() const
{
  std::stringstream ss;
  bool firstTime = true;
  for (const std::pair<std::string, std::string>& o : d_options)
  {
    if (firstTime)
    {
      firstTime = false;
    }
    else
    {
      ss << " ";
    }
    ss << "--";
    if (o.second == "true")
    {
      ss << o.first;
    }
    else if (o.second == "false")
    {
      ss << "no-" << o.first;
    }
    else
    {
      ss << o.first << "=" << o.second;
    }
  }
  return ss.str();
}

std::ostream& operator<<(std::ostream& os, const PortfolioConfig& config)
{
  for (const auto& o : config.d_options)
  {
    os << o.first << "=" << o.second << " ";
  }
  os << "timeout=" << config.d_timeout;
  return os;
}

/**
 * Check if the first string (the logic) is one of the remaining strings.
 * Used to have a reasonably concise syntax to check the current logic against a
 * lengthy list.
 */
template <typename... T>
bool isOneOf(const std::string& logic, T&&... list)
{
  return ((logic == list) || ...);
}

PortfolioStrategy PortfolioDriver::getStrategy(bool incremental_solving,
                                               const std::string& logic)
{
  if (incremental_solving)
  {
    return getIncrementalStrategy(logic);
  }
  else
  {
    return getNonIncrementalStrategy(logic);
  }
}

PortfolioStrategy PortfolioDriver::getIncrementalStrategy(
    const std::string& logic)
{
  PortfolioStrategy s;
  if (isOneOf(logic, "QF_AUFLIA"))
  {
    s.add().unset("arrays-eager-index").set("arrays-eager-lemmas");
  }
  else if (isOneOf(logic, "QF_BV"))
  {
    s.add().set("bitblast", "eager");
  }
  else
  {
    s.add();
  }
  return s;
}

PortfolioStrategy PortfolioDriver::getNonIncrementalStrategy(
    const std::string& logic)
{
  PortfolioStrategy s;
  if (isOneOf(logic, "QF_LRA"))
  {
    s.add(0.166666667)
        .set("miplib-trick")
        .set("miplib-trick-subs", "4")
        .set("use-approx")
        .set("lemmas-on-replay-failure")
        .set("replay-early-close-depth", "4")
        .set("replay-lemma-reject-cut", "128")
        .set("replay-reject-cut", "512")
        .set("unconstrained-simp")
        .set("use-soi");
    s.add(0.583333333)
        .unset("restrict-pivots")
        .set("use-soi")
        .set("new-prop")
        .set("unconstrained-simp");
    s.add();
  }
  else if (isOneOf(logic, "QF_LIA"))
  {
    // same as QF_LRA but add --pb-rewrites
    s.add(0.95)
        .set("miplib-trick")
        .set("miplib-trick-subs", "4")
        .set("use-approx")
        .set("lemmas-on-replay-failure")
        .set("replay-early-close-depth", "4")
        .set("replay-lemma-reject-cut", "128")
        .set("replay-reject-cut", "512")
        .set("unconstrained-simp")
        .set("use-soi")
        .set("pb-rewrites")
        .set("ite-simp")
        .set("simp-ite-compress");
    s.add();
  }
  else if (isOneOf(logic, "QF_NIA"))
  {
    s.add(0.35)
        .set("nl-ext-tplanes")
        .set("decision", "justification");
    s.add(0.05)
        .set("nl-ext-tplanes")
        .set("decision", "internal");
    s.add(0.05)
        .unset("nl-ext-tplanes")
        .set("decision", "internal");
    s.add(0.05)
        .unset("arith-brab")
        .set("nl-ext-tplanes")
        .set("decision", "internal");
    // totals to more than 100%, but smaller bit-widths usually fail quickly
    s.add(0.25)
        .set("solve-int-as-bv", "2")
        .set("bitblast", "eager");
    s.add(0.25)
        .set("solve-int-as-bv", "4")
        .set("bitblast", "eager");
    s.add(0.25)
        .set("solve-int-as-bv", "8")
        .set("bitblast", "eager");
    s.add(0.25)
        .set("solve-int-as-bv", "16")
        .set("bitblast", "eager");
    s.add(0.5)
        .set("solve-int-as-bv", "32")
        .set("bitblast", "eager");
    s.add().set("nl-ext-tplanes").set("decision", "internal");
  }
  else if (isOneOf(logic, "QF_NRA"))
  {
    s.add(0.5).set("decision", "justification");
    s.add(0.25)
        .set("decision", "internal")
        .unset("nl-cov")
        .set("nl-ext", "full")
        .set("nl-ext-tplanes");
    s.add().set("decision", "internal").set("nl-ext", "none");
  }
  else if (isOneOf(logic,
                   "ALIA",
                   "AUFLIA",
                   "AUFLIRA",
                   "AUFNIRA",
                   "UF",
                   "UFBVLIA",
                   "UFBVFP",
                   "UFIDL",
                   "UFLIA",
                   "UFLRA",
                   "UFNIA",
                   "UFDT",
                   "UFDTLIA",
                   "UFDTLIRA",
                   "AUFDTLIA",
                   "AUFDTLIRA",
                   "AUFBV",
                   "AUFBVDTLIA",
                   "AUFBVFP",
                   "AUFNIA",
                   "UFFPDTLIRA",
                   "UFFPDTNIRA"))
  {
    // initial runs
    s.add(0.025).set("simplification", "none").set("enum-inst");
    s.add(0.025).unset("e-matching").set("enum-inst");
    s.add(0.025)
        .unset("e-matching")
        .set("enum-inst")
        .set("enum-inst-sum");
    // trigger selections
    s.add(0.025).set("relevant-triggers").set("enum-inst");
    s.add(0.025).set("trigger-sel", "max").set("enum-inst");
    s.add(0.025)
        .set("multi-trigger-when-single")
        .set("enum-inst");
    s.add(0.025)
        .set("multi-trigger-when-single")
        .set("multi-trigger-priority")
        .set("enum-inst");
    s.add(0.025).set("multi-trigger-cache").set("enum-inst");
    s.add(0.025).unset("multi-trigger-linear").set("enum-inst");
    // other
    s.add(0.025).set("pre-skolem-quant", "on").set("enum-inst");
    s.add(0.025).set("inst-when", "full").set("enum-inst");
    s.add(0.025)
        .unset("e-matching")
        .unset("cbqi")
        .set("enum-inst");
    s.add(0.025).set("enum-inst").set("quant-ind");
    s.add(0.025)
        .set("decision", "internal")
        .set("simplification", "none")
        .unset("inst-no-entail")
        .unset("cbqi")
        .set("enum-inst");
    s.add(0.025)
        .set("decision", "internal")
        .set("enum-inst")
        .set("enum-inst-sum");
    s.add(0.025).set("term-db-mode", "relevant").set("enum-inst");
    s.add(0.025).set("enum-inst-interleave").set("enum-inst");
    s.add(0.025).set("preregister-mode", "lazy").set("enum-inst");
    // finite model find
    s.add(0.025).set("finite-model-find").set("fmf-mbqi", "none");
    s.add(0.025)
        .set("finite-model-find")
        .set("decision", "internal");
    s.add(0.025)
        .set("finite-model-find")
        .set("macros-quant")
        .set("macros-quant-mode", "all");
    s.add(0.05).set("finite-model-find").set("e-matching");
    s.add(0.05).set("mbqi");
    // long runs
    s.add(0.15)
        .set("finite-model-find")
        .set("decision", "internal");
    s.add().set("enum-inst");
  }
  else if (isOneOf(logic, "UFBV"))
  {
    // most problems in UFBV are essentially BV
    s.add(0.125).set("sygus-inst");
    s.add(0.125).set("mbqi").unset("cegqi").unset("sygus-inst");
    s.add(0.25)
        .set("enum-inst")
        .set("cegqi-nested-qe")
        .set("decision", "internal");
    s.add(0.25)
        .set("mbqi-enum")
        .unset("cegqi")
        .unset("sygus-inst");
    s.add(0.025)
        .set("enum-inst")
        .unset("cegqi-innermost")
        .set("global-negate");
    ;
    s.add().set("finite-model-find");
  }
  else if (isOneOf(logic, "ABV", "BV"))
  {
    s.add(0.066666667).set("sygus-inst");
    s.add(0.066666667).set("mbqi").unset("cegqi").unset("sygus-inst");
    s.add(0.25)
        .set("mbqi-enum")
        .unset("cegqi")
        .unset("sygus-inst");
    s.add(0.25)
        .set("enum-inst")
        .set("cegqi-nested-qe")
        .set("decision", "internal");
    s.add(0.025).set("enum-inst").unset("cegqi-bv");
    s.add(0.025)
        .set("enum-inst")
        .set("cegqi-bv-ineq", "eq-slack");
    s.add(0.066666667).set("enum-inst").unset("cegqi-innermost").set("global-negate");
    s.add().set("enum-inst");
  }
  else if (isOneOf(logic, "ABVFP", "ABVFPLRA", "BVFP", "FP", "NIA", "NRA", "BVFPLRA"))
  {
    s.add(0.25)
        .set("mbqi-enum")
        .unset("cegqi")
        .unset("sygus-inst");
    s.add(0.25).set("enum-inst").set("nl-ext-tplanes");
    s.add(0.05).set("mbqi").unset("cegqi").unset("sygus-inst");
    s.add().set("sygus-inst");
  }
  else if (isOneOf(logic, "LIA", "LRA"))
  {
    s.add(0.025).set("enum-inst");
    s.add(0.25).set("enum-inst").set("cegqi-nested-qe");
    s.add(0.025).set("mbqi").unset("cegqi").unset("sygus-inst");
    s.add(0.025)
        .set("mbqi-enum")
        .unset("cegqi")
        .unset("sygus-inst");
    s.add()
        .set("enum-inst")
        .set("cegqi-nested-qe")
        .set("decision", "internal");
  }
  else if (isOneOf(logic, "QF_AUFBV"))
  {
    s.add(0.5);
    s.add().set("decision", "stoponly");
  }
  else if (isOneOf(logic, "QF_ABV"))
  {
    s.add(0.41666667)
        .set("ite-simp")
        .set("simp-with-care")
        .set("repeat-simp");
    s.add();
  }
  else if (isOneOf(logic, "QF_BV"))
  {
    s.add().set("bitblast", "eager").set("bv-assert-input");
  }
  else if (isOneOf(logic, "QF_UFBV"))
  {
    s.add(0.75).set("bitblast", "eager").set("bv-assert-input");
    s.add();
  }
  else if (isOneOf(logic, "QF_AUFLIA"))
  {
    s.add()
        .unset("arrays-eager-index")
        .set("arrays-eager-lemmas")
        .set("decision", "justification");
  }
  else if (isOneOf(logic, "QF_AX"))
  {
    s.add()
        .unset("arrays-eager-index")
        .set("arrays-eager-lemmas")
        .set("decision", "internal");
  }
  else if (isOneOf(logic, "QF_AUFNIA"))
  {
    s.add()
        .set("decision", "justification")
        .unset("arrays-eager-index")
        .set("arrays-eager-lemmas");
  }
  else if (isOneOf(logic, "QF_ALIA"))
  {
    s.add(0.116666667).set("decision", "justification");
    s.add()
        .set("decision", "stoponly")
        .unset("arrays-eager-index")
        .set("arrays-eager-lemmas");
  }
  else if (isOneOf(logic, "QF_S", "QF_SLIA"))
  {
    s.add(0.25)
        .set("strings-exp")
        .set("strings-fmf")
        .unset("jh-rlv-order");
    s.add().set("strings-exp").unset("jh-rlv-order");
  }
  else
  {
    s.add();
  }
  return s;
}

}  // namespace cvc5::main
