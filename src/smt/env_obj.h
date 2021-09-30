/******************************************************************************
 * Top contributors (to current version):
 *   Aina Niemetz
 *
 * This file is part of the cvc5 project.
 *
 * Copyright (c) 2009-2021 by the authors listed in the file AUTHORS
 * in the top-level source directory and their institutional affiliations.
 * All rights reserved.  See the file COPYING in the top-level source
 * directory for licensing information.
 * ****************************************************************************
 *
 * The base class for everything that nees access to the environment (Env)
 * instance, which gives access to global utilities available to internal code.
 */

#include "cvc5_private.h"

#ifndef CVC5__SMT__ENV_OBJ_H
#define CVC5__SMT__ENV_OBJ_H

#include <memory>

#include "expr/node.h"

namespace cvc5 {

class Env;
class LogicInfo;
class NodeManager;
class Options;
class StatisticsRegistry;

namespace context {
class Context;
class UserContext;
}  // namespace context

class EnvObj
{
 protected:
  /** Constructor. */
  EnvObj(Env& env);
  EnvObj() = delete;
  /** Destructor.  */
  virtual ~EnvObj() {}

  /**
   * Rewrite a node.
   * This is a wrapper around theory::Rewriter::rewrite via Env.
   */
  Node rewrite(TNode node) const;
  /**
   * Extended rewrite a node.
   * This is a wrapper around theory::Rewriter::extendedRewrite via Env.
   */
  Node extendedRewrite(TNode node, bool aggr = true) const;
  /**
   * Evaluate node n under the substitution args -> vals.
   * This is a wrapper about theory::Rewriter::evaluate via Env.
   */
  Node evaluate(TNode n,
                const std::vector<Node>& args,
                const std::vector<Node>& vals,
                bool useRewriter = true) const;
  /** Same as above, with a visited cache. */
  Node evaluate(TNode n,
                const std::vector<Node>& args,
                const std::vector<Node>& vals,
                const std::unordered_map<Node, Node>& visited,
                bool useRewriter = true) const;

  /** Get the current logic information. */
  const LogicInfo& logicInfo() const;

  /** Get the options object (const version only) via Env. */
  const Options& options() const;

  /** Get a pointer to the Context via Env. */
  context::Context* context() const;

  /** Get a pointer to the UserContext via Env. */
  context::UserContext* userContext() const;

  /** Get the statistics registry via Env. */
  StatisticsRegistry& statisticsRegistry() const;

  /** The associated environment. */
  Env& d_env;
};

}  // namespace cvc5
#endif