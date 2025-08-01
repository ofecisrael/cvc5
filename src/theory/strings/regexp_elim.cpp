/******************************************************************************
 * Top contributors (to current version):
 *   Andrew Reynolds, Aina Niemetz, Daniel Larraz
 *
 * This file is part of the cvc5 project.
 *
 * Copyright (c) 2009-2025 by the authors listed in the file AUTHORS
 * in the top-level source directory and their institutional affiliations.
 * All rights reserved.  See the file COPYING in the top-level source
 * directory for licensing information.
 * ****************************************************************************
 *
 * Implementation of techniques for eliminating regular expressions.
 */

#include "theory/strings/regexp_elim.h"

#include "expr/bound_var_manager.h"
#include "options/strings_options.h"
#include "proof/proof_node_manager.h"
#include "smt/env.h"
#include "theory/rewriter.h"
#include "theory/strings/regexp_entail.h"
#include "theory/strings/theory_strings_utils.h"
#include "theory/strings/word.h"
#include "util/rational.h"
#include "util/string.h"

using namespace cvc5::internal::kind;

namespace cvc5::internal {
namespace theory {
namespace strings {

RegExpElimination::RegExpElimination(Env& env, bool isAgg, context::Context* c)
    : EnvObj(env),
      d_isAggressive(isAgg),
      d_epg(!env.isTheoryProofProducing()
                ? nullptr
                : new EagerProofGenerator(env, c, "RegExpElimination::epg"))
{
}

Node RegExpElimination::eliminate(Node atom, bool isAgg)
{
  Assert(atom.getKind() == Kind::STRING_IN_REGEXP);
  if (atom[1].getKind() == Kind::REGEXP_CONCAT)
  {
    return eliminateConcat(atom, isAgg);
  }
  else if (atom[1].getKind() == Kind::REGEXP_STAR)
  {
    return eliminateStar(atom, isAgg);
  }
  return Node::null();
}

TrustNode RegExpElimination::eliminateTrusted(Node atom)
{
  Node eatom = eliminate(atom, d_isAggressive);
  if (!eatom.isNull())
  {
    if (isProofEnabled())
    {
      ProofNodeManager* pnm = d_env.getProofNodeManager();
      Node eq = atom.eqNode(eatom);
      std::shared_ptr<ProofNode> pn =
          pnm->mkTrustedNode(TrustId::RE_ELIM, {}, {}, eq);
      d_epg->setProofFor(eq, pn);
      return TrustNode::mkTrustRewrite(atom, eatom, d_epg.get());
    }
    return TrustNode::mkTrustRewrite(atom, eatom, nullptr);
  }
  return TrustNode::null();
}

Node RegExpElimination::eliminateConcat(Node atom, bool isAgg)
{
  NodeManager* nm = atom.getNodeManager();
  BoundVarManager* bvm = nm->getBoundVarManager();
  Node x = atom[0];
  Node lenx = nm->mkNode(Kind::STRING_LENGTH, x);
  Node re = atom[1];
  Node zero = nm->mkConstInt(Rational(0));
  std::vector<Node> children;
  utils::getConcat(re, children);

  // If it can be reduced to memberships in fixed length regular expressions.
  // This includes concatenations where at most one child is of the form
  // (re.* re.allchar), which we abbreviate _* below, and all other children
  // have a fixed length.
  // The intuition why this is a "non-aggressive" rewrite is that membership
  // into fixed length regular expressions are easy to handle.
  // the index of _* in re
  unsigned pivotIndex = 0;
  bool hasPivotIndex = false;
  bool hasFixedLength = true;
  std::vector<Node> childLengths;
  std::vector<Node> childLengthsPostPivot;
  for (unsigned i = 0, size = children.size(); i < size; i++)
  {
    Node c = children[i];
    Node fl = RegExpEntail::getFixedLengthForRegexp(c);
    if (fl.isNull())
    {
      if (!hasPivotIndex && c.getKind() == Kind::REGEXP_STAR
          && c[0].getKind() == Kind::REGEXP_ALLCHAR)
      {
        hasPivotIndex = true;
        pivotIndex = i;
        // zero is used in sum below and is used for concat-fixed-len
        fl = zero;
      }
      else
      {
        hasFixedLength = false;
      }
    }
    if (!fl.isNull())
    {
      childLengths.push_back(fl);
      if (hasPivotIndex)
      {
        childLengthsPostPivot.push_back(fl);
      }
    }
  }
  Node lenSum = childLengths.size() > 1
                    ? nm->mkNode(Kind::ADD, childLengths)
                    : (childLengths.empty() ? zero : childLengths[0]);
  // if we have a fixed length
  if (hasFixedLength)
  {
    Assert(re.getNumChildren() == children.size());
    std::vector<Node> conc;
    conc.push_back(
        nm->mkNode(hasPivotIndex ? Kind::GEQ : Kind::EQUAL, lenx, lenSum));
    Node currEnd = zero;
    for (unsigned i = 0, size = childLengths.size(); i < size; i++)
    {
      if (hasPivotIndex && i == pivotIndex)
      {
        Node ppSum = childLengthsPostPivot.size() == 1
                         ? childLengthsPostPivot[0]
                         : nm->mkNode(Kind::ADD, childLengthsPostPivot);
        currEnd = nm->mkNode(Kind::SUB, lenx, ppSum);
      }
      else
      {
        Node curr =
            nm->mkNode(Kind::STRING_SUBSTR, x, currEnd, childLengths[i]);
        // We do not need to include memberships of the form
        //   (str.substr x n 1) in re.allchar
        // since we know that by construction, n < len( x ).
        if (re[i].getKind() != Kind::REGEXP_ALLCHAR)
        {
          Node currMem = nm->mkNode(Kind::STRING_IN_REGEXP, curr, re[i]);
          conc.push_back(currMem);
        }
        currEnd = nm->mkNode(Kind::ADD, currEnd, childLengths[i]);
      }
    }
    Node res = nm->mkNode(Kind::AND, conc);
    // For example:
    //   x in re.++(re.union(re.range("A", "J"), re.range("N", "Z")), "AB") -->
    //   len( x ) = 3 ^
    //   substr(x,0,1) in re.union(re.range("A", "J"), re.range("N", "Z")) ^
    //   substr(x,1,2) in "AB"
    // An example with a pivot index:
    //   x in re.++( "AB" ++ _* ++ "C" ) -->
    //   len( x ) >= 3 ^
    //   substr( x, 0, 2 ) in "AB" ^
    //   substr( x, len( x ) - 1, 1 ) in "C"
    return returnElim(atom, res, "concat-fixed-len");
  }

  // memberships of the form x in re.++ * s1 * ... * sn *, where * are
  // any number of repetitions (exact or indefinite) of re.allchar.
  Trace("re-elim-debug") << "Try re concat with gaps " << atom << std::endl;
  bool onlySigmasAndConsts = true;
  std::vector<Node> sep_children;
  std::vector<unsigned> gap_minsize;
  std::vector<bool> gap_exact;
  // the first gap is initially strict zero
  gap_minsize.push_back(0);
  gap_exact.push_back(true);
  for (const Node& c : children)
  {
    Trace("re-elim-debug") << "  " << c << std::endl;
    onlySigmasAndConsts = false;
    if (c.getKind() == Kind::STRING_TO_REGEXP)
    {
      onlySigmasAndConsts = true;
      sep_children.push_back(c[0]);
      // the next gap is initially strict zero
      gap_minsize.push_back(0);
      gap_exact.push_back(true);
    }
    else if (c.getKind() == Kind::REGEXP_STAR
             && c[0].getKind() == Kind::REGEXP_ALLCHAR)
    {
      // found a gap of any size
      onlySigmasAndConsts = true;
      gap_exact[gap_exact.size() - 1] = false;
    }
    else if (c.getKind() == Kind::REGEXP_ALLCHAR)
    {
      // add one to the minimum size of the gap
      onlySigmasAndConsts = true;
      gap_minsize[gap_minsize.size() - 1]++;
    }
    if (!onlySigmasAndConsts)
    {
      Trace("re-elim-debug") << "...cannot handle " << c << std::endl;
      break;
    }
  }
  // we should always rewrite concatenations that are purely re.allchar
  // and re.*( re.allchar ).
  Assert(!onlySigmasAndConsts || !sep_children.empty());
  if (onlySigmasAndConsts && !sep_children.empty())
  {
    bool canProcess = true;
    std::vector<Node> conj;
    // The following constructs a set of constraints that encodes that a
    // set of string terms are found, in order, in string x.
    // prev_end stores the current (symbolic) index in x that we are
    // searching.
    Node prev_end = zero;
    // the symbolic index we start searching, for each child in sep_children.
    std::vector<Node> prev_ends;
    unsigned gap_minsize_end = gap_minsize.back();
    bool gap_exact_end = gap_exact.back();
    std::vector<Node> non_greedy_find_vars;
    for (unsigned i = 0, size = sep_children.size(); i < size; i++)
    {
      if (gap_minsize[i] > 0)
      {
        // the gap to this child is at least gap_minsize[i]
        prev_end = nm->mkNode(
            Kind::ADD, prev_end, nm->mkConstInt(Rational(gap_minsize[i])));
      }
      prev_ends.push_back(prev_end);
      Node sc = sep_children[i];
      Node lensc = nm->mkNode(Kind::STRING_LENGTH, sc);
      if (gap_exact[i])
      {
        // if the gap is exact, it is a substring constraint
        Node curr = prev_end;
        Node ss = nm->mkNode(Kind::STRING_SUBSTR, x, curr, lensc);
        conj.push_back(ss.eqNode(sc));
        prev_end = nm->mkNode(Kind::ADD, curr, lensc);
      }
      else
      {
        // otherwise, we can use indexof to represent some next occurrence
        if (gap_exact[i + 1] && i + 1 != size)
        {
          if (!isAgg)
          {
            canProcess = false;
            break;
          }
          // if the gap after this one is strict, we need a non-greedy find
          // thus, we add a symbolic constant
          Node cacheVal =
              BoundVarManager::getCacheValue(atom, nm->mkConstInt(Rational(i)));
          TypeNode intType = nm->integerType();
          Node k = bvm->mkBoundVar(
              BoundVarId::STRINGS_RE_ELIM_CONCAT_INDEX, cacheVal, intType);
          non_greedy_find_vars.push_back(k);
          prev_end = nm->mkNode(Kind::ADD, prev_end, k);
        }
        Node curr = nm->mkNode(Kind::STRING_INDEXOF, x, sc, prev_end);
        Node idofFind = curr.eqNode(nm->mkConstInt(Rational(-1))).negate();
        conj.push_back(idofFind);
        prev_end = nm->mkNode(Kind::ADD, curr, lensc);
      }
    }

    if (canProcess)
    {
      // since sep_children is non-empty, conj is non-empty
      Assert(!conj.empty());
      // Process the last gap, if necessary.
      // Notice that if the last gap is not exact and its minsize is zero,
      // then the last indexof/substr constraint entails the following
      // constraint, so it is not necessary to add.
      // Below, we may write "A" for (str.to.re "A") and _ for re.allchar:
      Node cEnd = nm->mkConstInt(Rational(gap_minsize_end));
      if (gap_exact_end)
      {
        Assert(!sep_children.empty());
        // if it is strict, it corresponds to a substr case.
        // For example:
        //     x in (re.++ "A" (re.* _) "B" _ _) --->
        //        ... ^ "B" = substr( x, len( x ) - 3, 1 )  ^ ...
        Node sc = sep_children.back();
        Node lenSc = nm->mkNode(Kind::STRING_LENGTH, sc);
        Node loc =
            nm->mkNode(Kind::SUB, lenx, nm->mkNode(Kind::ADD, lenSc, cEnd));
        Node scc = sc.eqNode(nm->mkNode(Kind::STRING_SUBSTR, x, loc, lenSc));
        // We also must ensure that we fit. This constraint is necessary in
        // addition to the constraint above. Take this example:
        //     x in (re.++ "A" _ (re.* _) "B" _) --->
        //       substr( x, 0, 1 ) = "A" ^             // find "A"
        //       indexof( x, "B", 2 ) != -1 ^          // find "B" >=1 after "A"
        //       substr( x, len(x)-2, 1 ) = "B" ^      // "B" is at end - 2.
        //       indexof( x, "B", 2 ) <= len( x ) - 2
        // The last constaint ensures that the second and third constraints
        // may refer to the same "B". If it were not for the last constraint, it
        // would have been the case than "ABB" would be a model for x, where
        // the second constraint refers to the third position, and the third
        // constraint refers to the second position.
        //
        // With respect to the above example, the following is an optimization.
        // For that example, we instead produce:
        //     x in (re.++ "A" _ (re.* _) "B" _) --->
        //       substr( x, 0, 1 ) = "A" ^          // find "A"
        //       substr( x, len(x)-2, 1 ) = "B" ^   // "B" is at end - 2
        //       2 <= len( x ) - 2
        // The intuition is that above, there are two constraints that insist
        // that "B" is found, whereas we only need one. The last constraint
        // above says that the "B" we find at end-2 can be found >=1 after
        // the "A".
        conj.pop_back();
        Node fit = nm->mkNode(
            gap_exact[sep_children.size() - 1] ? Kind::EQUAL : Kind::LEQ,
            prev_ends.back(),
            loc);

        conj.push_back(scc);
        conj.push_back(fit);
      }
      else if (gap_minsize_end > 0)
      {
        // if it is non-strict, we are in a "greedy find" situtation where
        // we just need to ensure that the next occurrence fits.
        // For example:
        //     x in (re.++ "A" (re.* _) "B" _ _ (re.* _)) --->
        //        ... ^ indexof( x, "B", 1 ) + 2 <= len( x )
        Node fit =
            nm->mkNode(Kind::LEQ, nm->mkNode(Kind::ADD, prev_end, cEnd), lenx);
        conj.push_back(fit);
      }
      Node res = nm->mkAnd(conj);
      // process the non-greedy find variables
      if (!non_greedy_find_vars.empty())
      {
        std::vector<Node> children2;
        for (const Node& v : non_greedy_find_vars)
        {
          Node bound = nm->mkNode(Kind::AND,
                                  nm->mkNode(Kind::LEQ, zero, v),
                                  nm->mkNode(Kind::LT, v, lenx));
          children2.push_back(bound);
        }
        children2.push_back(res);
        Node body = nm->mkNode(Kind::AND, children2);
        Node bvl = nm->mkNode(Kind::BOUND_VAR_LIST, non_greedy_find_vars);
        res = utils::mkForallInternal(nm, bvl, body.negate()).negate();
      }
      // must also give a minimum length requirement
      res = nm->mkNode(Kind::AND, res, nm->mkNode(Kind::GEQ, lenx, lenSum));
      // Examples of this elimination:
      //   x in (re.++ "A" (re.* _) "B" (re.* _)) --->
      //     substr(x,0,1)="A" ^ indexof(x,"B",1)!=-1
      //   x in (re.++ (re.* _) "A" _ _ _ (re.* _) "B" _ _ (re.* _)) --->
      //     indexof(x,"A",0)!=-1 ^
      //     indexof( x, "B", indexof( x, "A", 0 ) + 1 + 3 ) != -1 ^
      //     indexof( x, "B", indexof( x, "A", 0 ) + 1 + 3 )+1+2 <= len(x) ^
      //     len(x) >= 7

      // An example of a non-greedy find:
      //   x in re.++( re.*( _ ), "A", _, "B", re.*( _ ) ) --->
      //     (exists k. 0 <= k < len( x ) ^
      //               indexof( x, "A", k ) != -1 ^
      //               substr( x, indexof( x, "A", k )+2, 1 ) = "B") ^
      //     len(x) >= 3
      return returnElim(atom, res, "concat-with-gaps");
    }
  }

  if (!isAgg)
  {
    return Node::null();
  }
  // only aggressive rewrites below here

  // if the first or last child is constant string, we can split the membership
  // into a conjunction of two memberships.
  Node sStartIndex = zero;
  Node sLength = lenx;
  std::vector<Node> sConstraints;
  std::vector<Node> rexpElimChildren;
  unsigned nchildren = children.size();
  Assert(nchildren > 1);
  for (unsigned r = 0; r < 2; r++)
  {
    unsigned index = r == 0 ? 0 : nchildren - 1;
    Node c = children[index];
    if (c.getKind() == Kind::STRING_TO_REGEXP)
    {
      Assert(children[index + (r == 0 ? 1 : -1)].getKind()
             != Kind::STRING_TO_REGEXP);
      Node s = c[0];
      Node lens = nm->mkNode(Kind::STRING_LENGTH, s);
      Node sss = r == 0 ? zero : nm->mkNode(Kind::SUB, lenx, lens);
      Node ss = nm->mkNode(Kind::STRING_SUBSTR, x, sss, lens);
      sConstraints.push_back(ss.eqNode(s));
      if (r == 0)
      {
        sStartIndex = lens;
      }
      else if (r == 1 && sConstraints.size() == 2)
      {
        // first and last children cannot overlap
        Node bound = nm->mkNode(Kind::GEQ, sss, sStartIndex);
        sConstraints.push_back(bound);
      }
      sLength = nm->mkNode(Kind::SUB, sLength, lens);
    }
    if (r == 1 && !sConstraints.empty())
    {
      // add the middle children
      for (unsigned i = 1; i < (nchildren - 1); i++)
      {
        rexpElimChildren.push_back(children[i]);
      }
    }
    if (c.getKind() != Kind::STRING_TO_REGEXP)
    {
      rexpElimChildren.push_back(c);
    }
  }
  if (!sConstraints.empty())
  {
    Node ss = nm->mkNode(Kind::STRING_SUBSTR, x, sStartIndex, sLength);
    Assert(!rexpElimChildren.empty());
    Node regElim = utils::mkConcat(rexpElimChildren, nm->regExpType());
    sConstraints.push_back(nm->mkNode(Kind::STRING_IN_REGEXP, ss, regElim));
    Node ret = nm->mkNode(Kind::AND, sConstraints);
    // e.g.
    // x in re.++( "A", R ) ---> substr(x,0,1)="A" ^ substr(x,1,len(x)-1) in R
    return returnElim(atom, ret, "concat-splice");
  }
  Assert(nchildren > 1);
  for (unsigned i = 0; i < nchildren; i++)
  {
    if (children[i].getKind() == Kind::STRING_TO_REGEXP)
    {
      Node s = children[i][0];
      Node lens = nm->mkNode(Kind::STRING_LENGTH, s);
      // there exists an index in this string such that the substring is this
      Node k;
      std::vector<Node> echildren;
      if (i == 0)
      {
        k = zero;
      }
      else if (i + 1 == nchildren)
      {
        k = nm->mkNode(Kind::SUB, lenx, lens);
      }
      else
      {
        Node cacheVal =
            BoundVarManager::getCacheValue(atom, nm->mkConstInt(Rational(i)));
        TypeNode intType = nm->integerType();
        k = bvm->mkBoundVar(
            BoundVarId::STRINGS_RE_ELIM_CONCAT_INDEX, cacheVal, intType);
        Node bound = nm->mkNode(
            Kind::AND,
            nm->mkNode(Kind::LEQ, zero, k),
            nm->mkNode(Kind::LEQ, k, nm->mkNode(Kind::SUB, lenx, lens)));
        echildren.push_back(bound);
      }
      Node substrEq = nm->mkNode(Kind::STRING_SUBSTR, x, k, lens).eqNode(s);
      echildren.push_back(substrEq);
      if (i > 0)
      {
        std::vector<Node> rprefix;
        rprefix.insert(rprefix.end(), children.begin(), children.begin() + i);
        Node rpn = utils::mkConcat(rprefix, nm->regExpType());
        Node substrPrefix =
            nm->mkNode(Kind::STRING_IN_REGEXP,
                       nm->mkNode(Kind::STRING_SUBSTR, x, zero, k),
                       rpn);
        echildren.push_back(substrPrefix);
      }
      if (i + 1 < nchildren)
      {
        std::vector<Node> rsuffix;
        rsuffix.insert(rsuffix.end(), children.begin() + i + 1, children.end());
        Node rps = utils::mkConcat(rsuffix, nm->regExpType());
        Node ks = nm->mkNode(Kind::ADD, k, lens);
        Node substrSuffix = nm->mkNode(
            Kind::STRING_IN_REGEXP,
            nm->mkNode(
                Kind::STRING_SUBSTR, x, ks, nm->mkNode(Kind::SUB, lenx, ks)),
            rps);
        echildren.push_back(substrSuffix);
      }
      Node body = nm->mkNode(Kind::AND, echildren);
      if (k.getKind() == Kind::BOUND_VARIABLE)
      {
        Node bvl = nm->mkNode(Kind::BOUND_VAR_LIST, k);
        body = utils::mkForallInternal(nm, bvl, body.negate()).negate();
      }
      // e.g. x in re.++( R1, "AB", R2 ) --->
      //  exists k.
      //    0 <= k <= (len(x)-2) ^
      //    substr( x, k, 2 ) = "AB" ^
      //    substr( x, 0, k ) in R1 ^
      //    substr( x, k+2, len(x)-(k+2) ) in R2
      return returnElim(atom, body, "concat-find");
    }
  }
  return Node::null();
}

Node RegExpElimination::eliminateStar(Node atom, bool isAgg)
{
  if (!isAgg)
  {
    return Node::null();
  }
  // only aggressive rewrites below here

  NodeManager* nm = atom.getNodeManager();
  BoundVarManager* bvm = nm->getBoundVarManager();
  Node x = atom[0];
  Node lenx = nm->mkNode(Kind::STRING_LENGTH, x);
  Node re = atom[1];
  Node zero = nm->mkConstInt(Rational(0));
  // for regular expression star,
  // if the period is a fixed constant, we can turn it into a bounded
  // quantifier
  std::vector<Node> disj;
  if (re[0].getKind() == Kind::REGEXP_UNION)
  {
    for (const Node& r : re[0])
    {
      disj.push_back(r);
    }
  }
  else
  {
    disj.push_back(re[0]);
  }
  bool lenOnePeriod = true;
  std::vector<Node> char_constraints;
  TypeNode intType = nm->integerType();
  Node index =
      bvm->mkBoundVar(BoundVarId::STRINGS_RE_ELIM_STAR_INDEX, atom, intType);
  Node substr_ch =
      nm->mkNode(Kind::STRING_SUBSTR, x, index, nm->mkConstInt(Rational(1)));
  // handle the case where it is purely characters
  for (const Node& r : disj)
  {
    Assert(r.getKind() != Kind::REGEXP_UNION);
    Assert(r.getKind() != Kind::REGEXP_ALLCHAR);
    lenOnePeriod = false;
    // lenOnePeriod is true if this regular expression is a single character
    // regular expression
    if (r.getKind() == Kind::STRING_TO_REGEXP)
    {
      Node s = r[0];
      if (s.isConst() && s.getConst<String>().size() == 1)
      {
        lenOnePeriod = true;
      }
    }
    else if (r.getKind() == Kind::REGEXP_RANGE)
    {
      lenOnePeriod = true;
    }
    if (!lenOnePeriod)
    {
      break;
    }
    else
    {
      Node regexp_ch = nm->mkNode(Kind::STRING_IN_REGEXP, substr_ch, r);
      char_constraints.push_back(regexp_ch);
    }
  }
  if (lenOnePeriod)
  {
    Assert(!char_constraints.empty());
    Node bound = nm->mkNode(Kind::AND,
                            nm->mkNode(Kind::LEQ, zero, index),
                            nm->mkNode(Kind::LT, index, lenx));
    Node conc = char_constraints.size() == 1
                    ? char_constraints[0]
                    : nm->mkNode(Kind::OR, char_constraints);
    Node body = nm->mkNode(Kind::OR, bound.negate(), conc);
    Node bvl = nm->mkNode(Kind::BOUND_VAR_LIST, index);
    Node res = utils::mkForallInternal(nm, bvl, body);
    // e.g.
    //   x in (re.* (re.union "A" "B" )) --->
    //   forall k. 0<=k<len(x) => (substr(x,k,1) in "A" OR substr(x,k,1) in "B")
    return returnElim(atom, res, "star-char");
  }
  // otherwise, for stars of constant length these are periodic
  if (disj.size() == 1)
  {
    Node r = disj[0];
    if (r.getKind() == Kind::STRING_TO_REGEXP)
    {
      Node s = r[0];
      if (s.isConst())
      {
        Node lens = nm->mkConstInt(Word::getLength(s));
        Assert(lens.getConst<Rational>().sgn() > 0);
        std::vector<Node> conj;
        // lens is a positive constant, so it is safe to use total div/mod here.
        Node bound = nm->mkNode(
            Kind::AND,
            nm->mkNode(Kind::LEQ, zero, index),
            nm->mkNode(Kind::LT,
                       index,
                       nm->mkNode(Kind::INTS_DIVISION_TOTAL, lenx, lens)));
        Node conc = nm->mkNode(Kind::STRING_SUBSTR,
                               x,
                               nm->mkNode(Kind::MULT, index, lens),
                               lens)
                        .eqNode(s);
        Node body = nm->mkNode(Kind::OR, bound.negate(), conc);
        Node bvl = nm->mkNode(Kind::BOUND_VAR_LIST, index);
        Node res = utils::mkForallInternal(nm, bvl, body);
        res = nm->mkNode(
            Kind::AND,
            nm->mkNode(Kind::INTS_MODULUS_TOTAL, lenx, lens).eqNode(zero),
            res);
        // e.g.
        //    x in ("abc")* --->
        //    forall k. 0 <= k < (len( x ) div 3) => substr(x,3*k,3) = "abc" ^
        //    len(x) mod 3 = 0
        return returnElim(atom, res, "star-constant");
      }
    }
  }
  return Node::null();
}

Node RegExpElimination::returnElim(Node atom, Node atomElim, const char* id)
{
  Trace("re-elim") << "re-elim: " << atom << " to " << atomElim << " by " << id
                   << "." << std::endl;
  return atomElim;
}
bool RegExpElimination::isProofEnabled() const
{
  return d_env.isTheoryProofProducing();
}

}  // namespace strings
}  // namespace theory
}  // namespace cvc5::internal
