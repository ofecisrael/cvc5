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
 * Implementation of LFSC node conversion
 */

#include "proof/lfsc/lfsc_node_converter.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

#include "expr/aci_norm.h"
#include "expr/array_store_all.h"
#include "expr/cardinality_constraint.h"
#include "expr/dtype.h"
#include "expr/dtype_cons.h"
#include "expr/sequence.h"
#include "expr/skolem_manager.h"
#include "printer/smt2/smt2_printer.h"
#include "theory/builtin/generic_op.h"
#include "theory/bv/theory_bv_utils.h"
#include "theory/datatypes/datatypes_rewriter.h"
#include "theory/strings/word.h"
#include "theory/uf/function_const.h"
#include "theory/uf/theory_uf_rewriter.h"
#include "util/bitvector.h"
#include "util/finite_field_value.h"
#include "util/floatingpoint.h"
#include "util/iand.h"
#include "util/rational.h"
#include "util/regexp.h"
#include "util/string.h"

using namespace cvc5::internal::kind;

namespace cvc5::internal {
namespace proof {

LfscNodeConverter::LfscNodeConverter(NodeManager* nm) : NodeConverter(nm)
{
  d_arrow = nm->mkSortConstructor("arrow", 2);

  d_sortType = nm->mkSort("sortType");
  // the embedding of arrow into Node, which is binary constructor over sorts
  TypeNode anfType = nm->mkFunctionType({d_sortType, d_sortType}, d_sortType);
  d_typeAsNode[d_arrow] =
      getSymbolInternal(Kind::FUNCTION_TYPE, anfType, "arrow");

  TypeNode intType = nm->integerType();
  TypeNode arrType = nm->mkFunctionType({d_sortType, d_sortType}, d_sortType);
  d_typeKindToNodeCons[Kind::ARRAY_TYPE] =
      getSymbolInternal(Kind::FUNCTION_TYPE, arrType, "Array");
  TypeNode bvType = nm->mkFunctionType(intType, d_sortType);
  d_typeKindToNodeCons[Kind::BITVECTOR_TYPE] =
      getSymbolInternal(Kind::FUNCTION_TYPE, bvType, "BitVec");
  TypeNode fpType = nm->mkFunctionType({intType, intType}, d_sortType);
  d_typeKindToNodeCons[Kind::FLOATINGPOINT_TYPE] =
      getSymbolInternal(Kind::FUNCTION_TYPE, fpType, "FloatingPoint");
  TypeNode setType = nm->mkFunctionType(d_sortType, d_sortType);
  d_typeKindToNodeCons[Kind::SET_TYPE] =
      getSymbolInternal(Kind::FUNCTION_TYPE, setType, "Set");
  d_typeKindToNodeCons[Kind::BAG_TYPE] =
      getSymbolInternal(Kind::FUNCTION_TYPE, setType, "Bag");
  d_typeKindToNodeCons[Kind::SEQUENCE_TYPE] =
      getSymbolInternal(Kind::FUNCTION_TYPE, setType, "Seq");
}

Node LfscNodeConverter::preConvert(Node n)
{
  // match is not supported in LFSC syntax, we eliminate it at pre-order
  // traversal, which avoids type-checking errors during conversion, since e.g.
  // match case nodes are required but cannot be preserved
  if (n.getKind() == Kind::MATCH)
  {
    return theory::datatypes::DatatypesRewriter::expandMatch(n);
  }
  return n;
}

Node LfscNodeConverter::postConvert(Node n)
{
  Kind k = n.getKind();
  // we eliminate MATCH at preConvert above
  Assert(k != Kind::MATCH);
  if (k == Kind::ASCRIPTION_TYPE)
  {
    // dummy node, return it
    return n;
  }
  TypeNode tn = n.getType();
  Trace("lfsc-term-process-debug")
      << "postConvert " << n << " " << k << std::endl;
  if (k == Kind::BOUND_VARIABLE)
  {
    if (d_symbols.find(n) != d_symbols.end())
    {
      // ignore internally generated symbols
      return n;
    }
    // bound variable v is (bvar x T)
    TypeNode intType = d_nm->integerType();
    Node x = d_nm->mkConstInt(Rational(getOrAssignIndexForBVar(n)));
    Node tc = typeAsNode(convertType(tn));
    TypeNode ftype = d_nm->mkFunctionType({intType, d_sortType}, tn);
    Node bvarOp = getSymbolInternal(k, ftype, "bvar");
    return mkApplyUf(bvarOp, {x, tc});
  }
  else if (k == Kind::RAW_SYMBOL)
  {
    // ignore internally generated symbols
    return n;
  }
  else if (k == Kind::SKOLEM || k == Kind::DUMMY_SKOLEM
           || k == Kind::DT_SYGUS_EVAL)
  {
    // constructors/selectors are represented by skolems, which are defined
    // symbols
    if (tn.isDatatypeConstructor() || tn.isDatatypeSelector()
        || tn.isDatatypeTester() || tn.isDatatypeUpdater())
    {
      // note these are not converted to their user named (cvc.) symbols here,
      // to avoid type errors when constructing terms for postConvert
      return n;
    }
    // skolems v print as their original forms
    // v is (skolem W) where W is the original or original form of v
    Node wi = SkolemManager::getUnpurifiedForm(n);
    if (!wi.isNull() && wi != n)
    {
      Trace("lfsc-term-process-debug")
          << "...original form " << wi << std::endl;
      wi = convert(wi);
      Trace("lfsc-term-process-debug")
          << "...converted original for " << wi << std::endl;
      TypeNode ftype = d_nm->mkFunctionType(tn, tn);
      Node skolemOp = getSymbolInternal(k, ftype, "skolem");
      return mkApplyUf(skolemOp, {wi});
    }
    // might be a skolem function
    Node ns = maybeMkSkolemFun(n);
    if (!ns.isNull())
    {
      return ns;
    }
    // Otherwise, it is an uncategorized skolem, must use a fresh variable.
    // This case will only apply for terms originating from places with no
    // proof support. Note it is not added as a declared variable, instead it
    // is used as (var N T) throughout.
    TypeNode intType = d_nm->integerType();
    TypeNode varType = d_nm->mkFunctionType({intType, d_sortType}, tn);
    Node var = mkInternalSymbol("var", varType);
    Node index = d_nm->mkConstInt(Rational(getOrAssignIndexForFVar(n)));
    Node tc = typeAsNode(convertType(tn));
    return mkApplyUf(var, {index, tc});
  }
  else if (n.isVar())
  {
    d_declVars.insert(n);
    return mkInternalSymbol(getNameForUserNameOf(n), tn);
  }
  else if (k == Kind::CARDINALITY_CONSTRAINT)
  {
    Trace("lfsc-term-process-debug")
        << "...convert cardinality constraint" << std::endl;
    const CardinalityConstraint& cc =
        n.getOperator().getConst<CardinalityConstraint>();
    Node tnn = typeAsNode(convertType(cc.getType()));
    Node ub = d_nm->mkConstInt(Rational(cc.getUpperBound()));
    TypeNode tnc = d_nm->mkFunctionType({tnn.getType(), ub.getType()},
                                        d_nm->booleanType());
    Node fcard = getSymbolInternal(k, tnc, "fmf.card");
    return mkApplyUf(fcard, {tnn, ub});
  }
  else if (k == Kind::APPLY_UF)
  {
    return convert(theory::uf::TheoryUfRewriter::getHoApplyForApplyUf(n));
  }
  else if (k == Kind::APPLY_CONSTRUCTOR || k == Kind::APPLY_SELECTOR
           || k == Kind::APPLY_TESTER || k == Kind::APPLY_UPDATER)
  {
    // must add to declared types
    const DType& dt = DType::datatypeOf(n.getOperator());
    d_declTypes.insert(dt.getTypeNode());
    // must convert other kinds of apply to functions, since we convert to
    // HO_APPLY
    Node opc = getOperatorOfTerm(n, true);
    if (n.getNumChildren() == 0)
    {
      return opc;
    }
    return postConvert(mkApplyUf(opc, std::vector<Node>(n.begin(), n.end())));
  }
  else if (k == Kind::HO_APPLY)
  {
    std::vector<TypeNode> argTypes;
    argTypes.push_back(n[0].getType());
    argTypes.push_back(n[1].getType());
    TypeNode tnh = d_nm->mkFunctionType(argTypes, tn);
    Node hconstf = getSymbolInternal(k, tnh, "apply");
    return mkApplyUf(hconstf, {n[0], n[1]});
  }
  else if (k == Kind::CONST_RATIONAL || k == Kind::CONST_INTEGER)
  {
    TypeNode tnv = d_nm->mkFunctionType(tn, tn);
    Node rconstf;
    Node arg;
    Rational r = n.getConst<Rational>();
    if (tn.isInteger())
    {
      rconstf = getSymbolInternal(k, tnv, "int");
      if (r.sgn() == -1)
      {
        // use LFSC syntax for mpz negation
        Node mpzn = getSymbolInternal(k, d_nm->mkFunctionType(tn, tn), "~");
        arg = mkApplyUf(mpzn, {d_nm->mkConstInt(r.abs())});
      }
      else
      {
        arg = n;
      }
    }
    else
    {
      rconstf = getSymbolInternal(k, tnv, "real");
      // ensure rationals are printed properly here using mpq syntax
      // Note that inconvieniently, LFSC uses (non-sexpr) syntax n/m for
      // constant rationals, hence we must use a string
      std::stringstream ss;
      ss << "__LFSC_TMP" << r.getNumerator().abs() << "/" << r.getDenominator();
      arg = mkInternalSymbol(ss.str(), tn);
      // negative (~ n/m)
      if (r.sgn() == -1)
      {
        Node mpzn = getSymbolInternal(k, d_nm->mkFunctionType(tn, tn), "~");
        arg = mkApplyUf(mpzn, {arg});
      }
    }
    return mkApplyUf(rconstf, {arg});
  }
  else if (k == Kind::CONST_BITVECTOR)
  {
    TypeNode btn = d_nm->booleanType();
    TypeNode tnv = d_nm->mkFunctionType(btn, tn);
    BitVector bv = n.getConst<BitVector>();
    Node ret = convertBitVector(bv);
    Node bconstf = getSymbolInternal(k, tnv, "bv");
    return mkApplyUf(bconstf, {ret});
  }
  else if (k == Kind::CONST_FLOATINGPOINT)
  {
    BitVector s, e, i;
    n.getConst<FloatingPoint>().getIEEEBitvectors(s, e, i);
    Node sn = convert(d_nm->mkConst(s));
    Node en = convert(d_nm->mkConst(e));
    Node in = convert(d_nm->mkConst(i));
    TypeNode tnv =
        d_nm->mkFunctionType({sn.getType(), en.getType(), in.getType()}, tn);
    Node bconstf = getSymbolInternal(k, tnv, "fp");
    return mkApplyUf(bconstf, {sn, en, in});
  }
  else if (k == Kind::CONST_FINITE_FIELD)
  {
    const FiniteFieldValue& ffv = n.getConst<FiniteFieldValue>();
    Node v = convert(d_nm->mkConstInt(ffv.getValue()));
    Node fs = convert(d_nm->mkConstInt(ffv.getFieldSize()));
    TypeNode tnv = d_nm->mkFunctionType({v.getType(), fs.getType()}, tn);
    Node ffconstf = getSymbolInternal(k, tnv, "ff.value");
    return mkApplyUf(ffconstf, {v, fs});
  }
  else if (k == Kind::CONST_STRING)
  {
    //"" is emptystr
    //"A" is (char 65)
    //"ABC" is (str.++ (char 65) (str.++ (char 66) (str.++ (char 67) emptystr)))
    std::vector<Node> charVec;
    getCharVectorInternal(n, charVec);
    Assert(!charVec.empty());
    if (charVec.size() == 1)
    {
      // handles empty string and singleton character
      return charVec[0];
    }
    std::reverse(charVec.begin(), charVec.end());
    Node ret = postConvert(getNullTerminator(d_nm, Kind::STRING_CONCAT, tn));
    for (size_t i = 0, size = charVec.size(); i < size; i++)
    {
      ret = d_nm->mkNode(Kind::STRING_CONCAT, charVec[i], ret);
    }
    return ret;
  }
  else if (k == Kind::CONST_SEQUENCE)
  {
    const std::vector<Node>& charVec = n.getConst<Sequence>().getVec();
    TypeNode etype = d_nm->mkFunctionType(d_sortType, tn);
    Node ret = getSymbolInternal(k, etype, "seq.empty");
    ret = mkApplyUf(ret, {typeAsNode(convertType(tn))});
    std::vector<Node> vecu;
    for (size_t i = 0, size = charVec.size(); i < size; i++)
    {
      Node u =
          d_nm->mkNode(Kind::SEQ_UNIT, postConvert(charVec[size - (i + 1)]));
      if (size == 1)
      {
        // singleton case
        return u;
      }
      ret = d_nm->mkNode(Kind::STRING_CONCAT, u, ret);
    }
    return ret;
  }
  else if (k == Kind::STORE_ALL)
  {
    Node t = typeAsNode(convertType(tn));
    TypeNode caRetType = d_nm->mkFunctionType(tn.getArrayConstituentType(), tn);
    TypeNode catype = d_nm->mkFunctionType(d_sortType, caRetType);
    Node bconstf = getSymbolInternal(k, catype, "array_const");
    Node f = mkApplyUf(bconstf, {t});
    ArrayStoreAll storeAll = n.getConst<ArrayStoreAll>();
    return mkApplyUf(f, {convert(storeAll.getValue())});
  }
  else if (k == Kind::GEQ || k == Kind::GT || k == Kind::LEQ || k == Kind::LT
           || k == Kind::SUB || k == Kind::DIVISION || k == Kind::DIVISION_TOTAL
           || k == Kind::INTS_DIVISION || k == Kind::INTS_DIVISION_TOTAL
           || k == Kind::INTS_MODULUS || k == Kind::INTS_MODULUS_TOTAL
           || k == Kind::NEG || k == Kind::POW
           || k == Kind::FLOATINGPOINT_COMPONENT_NAN
           || k == Kind::FLOATINGPOINT_COMPONENT_INF
           || k == Kind::FLOATINGPOINT_COMPONENT_ZERO
           || k == Kind::FLOATINGPOINT_COMPONENT_SIGN
           || k == Kind::FLOATINGPOINT_COMPONENT_EXPONENT
           || k == Kind::FLOATINGPOINT_COMPONENT_SIGNIFICAND
           || k == Kind::ROUNDINGMODE_BITBLAST
           || GenericOp::isIndexedOperatorKind(k))
  {
    // must give special names to SMT-LIB operators with arithmetic subtyping
    // note that SUB is not n-ary
    // get the macro-apply version of the operator
    Node opc = getOperatorOfTerm(n, true);
    return mkApplyUf(opc, std::vector<Node>(n.begin(), n.end()));
  }
  else if (k == Kind::SET_EMPTY || k == Kind::SET_UNIVERSE
           || k == Kind::BAG_EMPTY)
  {
    Node t = typeAsNode(convertType(tn));
    TypeNode etype = d_nm->mkFunctionType(d_sortType, tn);
    Node ef = getSymbolInternal(
        k,
        etype,
        k == Kind::SET_EMPTY
            ? "set.empty"
            : (k == Kind::SET_UNIVERSE ? "set.universe" : "bag.empty"));
    return mkApplyUf(ef, {t});
  }
  else if (n.isClosure())
  {
    // (forall ((x1 T1) ... (xn Tk)) P) is
    // ((forall x1 T1) ((forall x2 T2) ... ((forall xk Tk) P))). We use
    // SEXPR to do this, which avoids the need for indexed operators.
    Node ret = n[1];
    Node cop = getOperatorOfClosure(n, true);
    Node pcop = getOperatorOfClosure(n, true, true);
    for (size_t i = 0, nchild = n[0].getNumChildren(); i < nchild; i++)
    {
      size_t ii = (nchild - 1) - i;
      Node v = n[0][ii];
      // use the partial operator for variables except the last one.  This
      // avoids type errors in internal representation of LFSC terms.
      Node vop = getOperatorOfBoundVar(ii == 0 ? cop : pcop, v);
      ret = mkApplyUf(vop, {ret});
    }
    // notice that intentionally we drop annotations here
    return ret;
  }
  else if (k == Kind::FUNCTION_ARRAY_CONST)
  {
    // must convert to lambda and then run the conversion
    Node lam = theory::uf::FunctionConst::toLambda(n);
    Assert(!lam.isNull());
    return convert(lam);
  }
  else if (k == Kind::REGEXP_LOOP)
  {
    // ((_ re.loop n1 n2) t) is ((re.loop n1 n2) t)
    TypeNode intType = d_nm->integerType();
    TypeNode relType =
        d_nm->mkFunctionType({intType, intType}, d_nm->mkFunctionType(tn, tn));
    Node rop = getSymbolInternal(
        k, relType, printer::smt2::Smt2Printer::smtKindString(k));
    RegExpLoop op = n.getOperator().getConst<RegExpLoop>();
    Node n1 = d_nm->mkConstInt(Rational(op.d_loopMinOcc));
    Node n2 = d_nm->mkConstInt(Rational(op.d_loopMaxOcc));
    return mkApplyUf(mkApplyUf(rop, {n1, n2}), {n[0]});
  }
  else if (k == Kind::BITVECTOR_FROM_BOOLS)
  {
    TypeNode btn = d_nm->booleanType();
    // (from_bools t1 ... tn) is
    // (from_bools t1 (from_bools t2 ... (from_bools tn emptybv)))
    // where notice that each from_bools has a different type
    Node curr = getNullTerminator(d_nm, Kind::BITVECTOR_CONCAT, tn);
    for (size_t i = 0, nchild = n.getNumChildren(); i < nchild; ++i)
    {
      TypeNode bvt = d_nm->mkBitVectorType(i + 1);
      TypeNode ftype = d_nm->mkFunctionType({btn, curr.getType()}, bvt);
      Node bbt = getSymbolInternal(k, ftype, "from_bools");
      curr = mkApplyUf(bbt, {n[nchild - (i + 1)], curr});
    }
    return curr;
  }
  else if (k == Kind::SEP_NIL)
  {
    Node tnn = typeAsNode(convertType(tn));
    TypeNode ftype = d_nm->mkFunctionType(d_sortType, tn);
    Node s = getSymbolInternal(k, ftype, "sep.nil");
    return mkApplyUf(s, {tnn});
  }
  else if (NodeManager::isNAryKind(k) && n.getNumChildren() >= 2)
  {
    size_t nchild = n.getNumChildren();
    Assert(n.getMetaKind() != metakind::PARAMETERIZED);
    // convert all n-ary applications to binary
    std::vector<Node> children(n.begin(), n.end());
    // distinct is special case
    if (k == Kind::DISTINCT)
    {
      // DISTINCT(x1,...,xn) --->
      // AND(DISTINCT(x1,x2), AND(,..., AND(,..,DISTINCT(x_{n-1},x_n))))
      Node ret = d_nm->mkNode(k, children[0], children[1]);
      for (unsigned i = 0; i < nchild; i++)
        for (unsigned j = i + 1; j < nchild; j++)
        {
          if (i != 0 && j != 1)
          {
            ret = d_nm->mkNode(
                Kind::AND, ret, d_nm->mkNode(k, children[i], children[j]));
          }
        }
      Trace("lfsc-term-process-debug") << "n: " << n << std::endl
                                       << "ret: " << ret << std::endl;
      return ret;
    }
    std::reverse(children.begin(), children.end());
    // Add the null-terminator. This is done to disambiguate the number
    // of children for term with n-ary operators. In particular note that
    // (or A B C (or D E)) has representation:
    //   (or A (or B (or C (or (or D E) false))))
    // This makes the AST above distinguishable from (or A B C D E),
    // which otherwise would both have representation:
    //   (or A (or B (or C (or D E))))
    Node nullTerm = getNullTerminator(d_nm, k, tn);
    // Most operators simply get binarized
    Node ret;
    size_t istart = 0;
    if (nullTerm.isNull())
    {
      ret = children[0];
      istart = 1;
    }
    else
    {
      // must convert recursively, since nullTerm may have subterms.
      ret = convert(nullTerm);
    }
    // check whether we are also changing the operator name, in which case
    // we build a binary uninterpreted function opc
    bool isArithOp =
        (k == Kind::ADD || k == Kind::MULT || k == Kind::NONLINEAR_MULT);
    std::stringstream arithOpName;
    if (isArithOp)
    {
      // currently allow subtyping
      arithOpName << "a.";
      arithOpName << printer::smt2::Smt2Printer::smtKindString(k);
    }
    // now, iterate over children and make binary conversion
    for (size_t i = istart, npchild = children.size(); i < npchild; i++)
    {
      if (isArithOp)
      {
        // Arithmetic operators must deal with permissive type rules for
        // ADD, MULT, NONLINEAR_MULT. We use the properly typed operator to
        // avoid debug failures.
        TypeNode tn1 = children[i].getType();
        TypeNode tn2 = ret.getType();
        TypeNode ftype = d_nm->mkFunctionType({tn1, tn2}, tn);
        Node opc = getSymbolInternal(k, ftype, arithOpName.str());
        ret = mkApplyUf(opc, {children[i], ret});
      }
      else
      {
        ret = d_nm->mkNode(k, children[i], ret);
      }
    }
    Trace("lfsc-term-process-debug")
        << "...return n-ary conv " << ret << std::endl;
    return ret;
  }
  return n;
}

Node LfscNodeConverter::mkApplyUf(Node op, const std::vector<Node>& args) const
{
  std::vector<Node> aargs;
  if (op.isVar())
  {
    aargs.push_back(op);
  }
  else
  {
    // Note that dag threshold is disabled for printing operators.
    std::stringstream ss;
    options::ioutils::applyOutputLanguage(ss, Language::LANG_SMTLIB_V2_6);
    options::ioutils::applyDagThresh(ss, 0);
    ss << op;
    Node opv = NodeManager::mkRawSymbol(ss.str(), op.getType());
    aargs.push_back(opv);
  }
  aargs.insert(aargs.end(), args.begin(), args.end());
  return d_nm->mkNode(Kind::APPLY_UF, aargs);
}

TypeNode LfscNodeConverter::preConvertType(TypeNode tn)
{
  if (tn.getKind() == Kind::TUPLE_TYPE)
  {
    // Must collect the tuple type here, since at post-order traversal, the
    // type has been modified and no longer maintains the mapping to its
    // datatype encoding.
    d_declTypes.insert(tn);
  }
  return tn;
}

TypeNode LfscNodeConverter::postConvertType(TypeNode tn)
{
  TypeNode cur = tn;
  Node tnn;
  Kind k = tn.getKind();
  Trace("lfsc-term-process-debug")
      << "postConvertType " << tn << " " << tn.getNumChildren() << " " << k
      << std::endl;
  if (k == Kind::FUNCTION_TYPE)
  {
    // (-> T1 ... Tn T) is (arrow T1 .... (arrow Tn T))
    std::vector<TypeNode> argTypes = tn.getArgTypes();
    // also make the node embedding of the type
    Node arrown = d_typeAsNode[d_arrow];
    Assert(!arrown.isNull());
    cur = tn.getRangeType();
    tnn = typeAsNode(cur);
    for (std::vector<TypeNode>::reverse_iterator it = argTypes.rbegin();
         it != argTypes.rend();
         ++it)
    {
      std::vector<TypeNode> aargs;
      aargs.push_back(*it);
      aargs.push_back(cur);
      cur = d_nm->mkSort(d_arrow, aargs);
      tnn = mkApplyUf(arrown, {typeAsNode(*it), tnn});
    }
  }
  else if (k == Kind::BITVECTOR_TYPE)
  {
    tnn = d_typeKindToNodeCons[k];
    Node w = d_nm->mkConstInt(Rational(tn.getBitVectorSize()));
    tnn = mkApplyUf(tnn, {w});
  }
  else if (k == Kind::FLOATINGPOINT_TYPE)
  {
    tnn = d_typeKindToNodeCons[k];
    Node e = d_nm->mkConstInt(Rational(tn.getFloatingPointExponentSize()));
    Node s = d_nm->mkConstInt(Rational(tn.getFloatingPointSignificandSize()));
    tnn = mkApplyUf(tnn, {e, s});
  }
  else if (k == Kind::TUPLE_TYPE)
  {
    // special case: tuples must be distinguished by their arity
    size_t nargs = tn.getNumChildren();
    if (nargs > 0)
    {
      std::vector<TypeNode> types;
      std::vector<TypeNode> convTypes;
      std::vector<Node> targs;
      for (size_t i = 0; i < nargs; i++)
      {
        TypeNode tnc = tn[i];
        types.push_back(d_sortType);
        convTypes.push_back(tnc);
        targs.push_back(typeAsNode(tnc));
      }
      TypeNode ftype = d_nm->mkFunctionType(types, d_sortType);
      // must distinguish by arity
      std::stringstream ss;
      ss << "Tuple_" << nargs;
      tnn = mkApplyUf(getSymbolInternal(k, ftype, ss.str()), targs);
      // we are changing its name, we must make a sort constructor
      cur = d_nm->mkSortConstructor(ss.str(), nargs);
      cur = d_nm->mkSort(cur, convTypes);
    }
    else
    {
      // no need to convert type for tuples of size 0,
      // type as node is simple
      tnn = getSymbolInternal(k, d_sortType, "UnitTuple");
    }
  }
  else if (tn.getNumChildren() == 0)
  {
    Assert(!tn.isTuple());
    // an uninterpreted sort, or an uninstantiatied (maybe parametric) datatype
    d_declTypes.insert(tn);
    std::stringstream ss;
    options::ioutils::applyOutputLanguage(ss, Language::LANG_SMTLIB_V2_6);
    tn.toStream(ss);
    if (tn.isUninterpretedSortConstructor())
    {
      std::string s = getNameForUserNameOfInternal(tn.getId(), ss.str());
      tnn = getSymbolInternal(k, d_sortType, s, false);
      cur =
          d_nm->mkSortConstructor(s, tn.getUninterpretedSortConstructorArity());
    }
    else if (tn.isUninterpretedSort() || tn.isDatatype())
    {
      std::string s = getNameForUserNameOfInternal(tn.getId(), ss.str());
      tnn = getSymbolInternal(k, d_sortType, s, false);
      cur = d_nm->mkSort(s);
    }
    else
    {
      // all other builtin type constants, e.g. Int
      tnn = getSymbolInternal(k, d_sortType, ss.str());
    }
  }
  else
  {
    // to build the type-as-node, must convert the component types
    std::vector<Node> targs;
    std::vector<TypeNode> types;
    for (const TypeNode& tnc : tn)
    {
      targs.push_back(typeAsNode(tnc));
      types.push_back(d_sortType);
    }
    Node op;
    if (k == Kind::PARAMETRIC_DATATYPE)
    {
      // note we don't add to declared types here, since the parametric
      // datatype is traversed and will be declared as a type constructor
      // erase first child, which repeats the datatype
      targs.erase(targs.begin(), targs.begin() + 1);
      types.erase(types.begin(), types.begin() + 1);
      TypeNode ftype = d_nm->mkFunctionType(types, d_sortType);
      // the operator has been converted; it is no longer a datatype, thus
      // we must print to get its name.
      std::stringstream ss;
      ss << tn[0];
      op = getSymbolInternal(k, ftype, ss.str());
    }
    else if (k == Kind::INSTANTIATED_SORT_TYPE)
    {
      // We don't add to declared types here. The type constructor is already
      // added to declare types when processing the children of this.
      // Also, similar to PARAMETRIC_DATATYPE, the type constructor
      // should be erased from children.
      targs.erase(targs.begin(), targs.begin() + 1);
      types.erase(types.begin(), types.begin() + 1);
      TypeNode ftype = d_nm->mkFunctionType(types, d_sortType);
      std::string name = tn.getUninterpretedSortConstructor().getName();
      op = getSymbolInternal(k, ftype, name, false);
    }
    else if (k == Kind::NULLABLE_TYPE)
    {
      TypeNode ftype = d_nm->mkFunctionType(d_sortType, d_sortType);
      op = getSymbolInternal(k, ftype, "Nullable", false);
    }
    else
    {
      std::map<Kind, Node>::iterator it = d_typeKindToNodeCons.find(k);
      if (it != d_typeKindToNodeCons.end())
      {
        op = it->second;
      }
    }
    if (!op.isNull())
    {
      tnn = mkApplyUf(op, targs);
    }
    else
    {
      AlwaysAssert(false);
    }
  }
  Assert(!tnn.isNull());
  Trace("lfsc-term-process-debug") << "...type as node: " << tnn << std::endl;
  d_typeAsNode[cur] = tnn;
  return cur;
}

std::string LfscNodeConverter::getNameForUserName(const std::string& name,
                                                  size_t variant)
{
  // For user name X, we do cvc.Y, where Y contains an escaped version of X.
  // Specifically, since LFSC does not allow these characters in identifier
  // bodies: "() \t\n\f;", each is replaced with an escape sequence "\xXX"
  // where XX is the zero-padded hex representation of the code point. "\\" is
  // also escaped.
  //
  // See also: https://github.com/cvc5/LFSC/blob/master/src/lexer.flex#L24
  //
  // The "cvc." prefix ensures we do not clash with LFSC definitions.
  //
  // The escaping ensures that all names are valid LFSC identifiers.
  std::stringstream ss;
  ss << "cvc";
  if (variant != 0)
  {
    ss << variant;
  }
  ss << ".";
  std::string sanitized = ss.str();
  size_t found = sanitized.size();
  sanitized += name;
  // The following sanitizes symbols that are not allowed in LFSC identifiers
  // here, e.g.
  //   |a b|
  // is converted to:
  //   cvc.a\x20b
  // where "20" is the hex unicode value of " ".
  do
  {
    found = sanitized.find_first_of("() \t\n\f\\;", found);
    if (found != std::string::npos)
    {
      // Emit hex sequence
      std::stringstream seq;
      seq << "\\x" << std::setbase(16) << std::setfill('0') << std::setw(2)
          << static_cast<size_t>(sanitized[found]);
      sanitized.replace(found, 1, seq.str());
      // increment found over the escape
      found += 3;
    }
  } while (found != std::string::npos);
  return sanitized;
}

std::string LfscNodeConverter::getNameForUserNameOf(Node v)
{
  std::string name = v.getName();
  return getNameForUserNameOfInternal(v.getId(), name);
}

std::string LfscNodeConverter::getNameForUserNameOfInternal(
    uint64_t id, const std::string& name)
{
  std::vector<uint64_t>& syms = d_userSymbolList[name];
  size_t variant = 0;
  std::vector<uint64_t>::iterator itr =
      std::find(syms.begin(), syms.end(), id);
  if (itr != syms.cend())
  {
    variant = std::distance(syms.begin(), itr);
  }
  else
  {
    variant = syms.size();
    syms.push_back(id);
  }
  return getNameForUserName(name, variant);
}

bool LfscNodeConverter::shouldTraverse(Node n)
{
  Kind k = n.getKind();
  // don't convert bound variable or instantiation pattern list directly
  if (k == Kind::BOUND_VAR_LIST || k == Kind::INST_PATTERN_LIST)
  {
    return false;
  }
  // should not traverse internal applications
  if (k == Kind::APPLY_UF)
  {
    if (d_symbols.find(n.getOperator()) != d_symbols.end())
    {
      return false;
    }
  }
  return true;
}

Node LfscNodeConverter::maybeMkSkolemFun(Node k, bool macroApply)
{
  SkolemManager* sm = d_nm->getSkolemManager();
  SkolemId sfi = SkolemId::NONE;
  Node cacheVal;
  TypeNode tn = k.getType();
  if (sm->isSkolemFunction(k, sfi, cacheVal))
  {
    if (sfi == SkolemId::RE_UNFOLD_POS_COMPONENT)
    {
      // a skolem corresponding to a regular expression unfolding component
      // should print as (skolem_re_unfold_pos t R n) where the skolem is the
      // n^th component for the unfolding of (str.in_re t R).
      TypeNode strType = d_nm->stringType();
      TypeNode reType = d_nm->regExpType();
      TypeNode intType = d_nm->integerType();
      TypeNode reut = d_nm->mkFunctionType({strType, reType, intType}, strType);
      Node sk = getSymbolInternal(k.getKind(), reut, "skolem_re_unfold_pos");
      Assert(!cacheVal.isNull() && cacheVal.getKind() == Kind::SEXPR
             && cacheVal.getNumChildren() == 3);
      // third value is mpz, which is not converted
      return mkApplyUf(sk,
          {convert(cacheVal[0]), convert(cacheVal[1]), cacheVal[2]});
    }
  }
  return Node::null();
}

Node LfscNodeConverter::typeAsNode(TypeNode tni) const
{
  // should always exist in the cache, as we always run types through
  // postConvertType before calling this method.
  std::map<TypeNode, Node>::const_iterator it = d_typeAsNode.find(tni);
  AlwaysAssert(it != d_typeAsNode.end()) << "Missing typeAsNode " << tni;
  return it->second;
}

Node LfscNodeConverter::mkInternalSymbol(const std::string& name,
                                         TypeNode tn,
                                         bool useRawSym)
{
  // use raw symbol so that it is never quoted
  Node sym = useRawSym ? NodeManager::mkRawSymbol(name, tn)
                       : NodeManager::mkBoundVar(name, tn);
  d_symbols.insert(sym);
  return sym;
}

Node LfscNodeConverter::getSymbolInternalFor(Node n,
                                             const std::string& name,
                                             bool useRawSym)
{
  return getSymbolInternal(n.getKind(), n.getType(), name, useRawSym);
}

Node LfscNodeConverter::getSymbolInternal(Kind k,
                                          TypeNode tn,
                                          const std::string& name,
                                          bool useRawSym)
{
  std::tuple<Kind, TypeNode, std::string> key(k, tn, name);
  std::map<std::tuple<Kind, TypeNode, std::string>, Node>::iterator it =
      d_symbolsMap.find(key);
  if (it != d_symbolsMap.end())
  {
    return it->second;
  }
  Node sym = mkInternalSymbol(name, tn, useRawSym);
  d_symbolToBuiltinKind[sym] = k;
  d_symbolsMap[key] = sym;
  return sym;
}

void LfscNodeConverter::getCharVectorInternal(Node c, std::vector<Node>& chars)
{
  Assert(c.getKind() == Kind::CONST_STRING);
  const std::vector<unsigned>& vec = c.getConst<String>().getVec();
  if (vec.size() == 0)
  {
    Node ec = getSymbolInternalFor(c, "emptystr");
    chars.push_back(ec);
    return;
  }
  TypeNode tnc = d_nm->mkFunctionType(d_nm->integerType(), c.getType());
  Node aconstf = getSymbolInternal(Kind::CONST_STRING, tnc, "char");
  for (unsigned i = 0, size = vec.size(); i < size; i++)
  {
    Node cc = mkApplyUf(aconstf, {d_nm->mkConstInt(Rational(vec[i]))});
    chars.push_back(cc);
  }
}

Node LfscNodeConverter::convertBitVector(const BitVector& bv)
{
  TypeNode btn = d_nm->booleanType();
  TypeNode btnv = d_nm->mkFunctionType({btn, btn}, btn);
  size_t w = bv.getSize();
  Node ret = getSymbolInternal(Kind::CONST_BITVECTOR, btn, "bvn");
  Node b0 = getSymbolInternal(Kind::CONST_BITVECTOR, btn, "b0");
  Node b1 = getSymbolInternal(Kind::CONST_BITVECTOR, btn, "b1");
  Node bvc = getSymbolInternal(Kind::CONST_BITVECTOR, btnv, "bvc");
  for (size_t i = 0; i < w; i++)
  {
    Node arg = bv.isBitSet((w - 1) - i) ? b1 : b0;
    ret = mkApplyUf(bvc, {arg, ret});
  }
  return ret;
}

Node LfscNodeConverter::getNullTerminator(NodeManager* nm, Kind k, TypeNode tn)
{
  Node nullTerm;
  switch (k)
  {
    case Kind::REGEXP_CONCAT:
      // the language containing only the empty string, which has special
      // syntax in LFSC
      nullTerm = getSymbolInternal(k, tn, "re.empty");
      break;
    case Kind::BITVECTOR_CONCAT:
    {
      // the null terminator of bitvector concat is a dummy variable of
      // bit-vector type with zero width, regardless of the type of the overall
      // concat.
      TypeNode bvz = d_nm->mkBitVectorType(0);
      nullTerm = getSymbolInternal(k, bvz, "emptybv");
    }
    break;
    default:
      // no special handling, or not null terminated
      break;
  }
  if (!nullTerm.isNull())
  {
    return nullTerm;
  }
  // otherwise, fall back to standard utility
  return expr::getNullTerminator(nm, k, tn);
}

Kind LfscNodeConverter::getBuiltinKindForInternalSymbol(Node op) const
{
  std::map<Node, Kind>::const_iterator it = d_symbolToBuiltinKind.find(op);
  if (it != d_symbolToBuiltinKind.end())
  {
    return it->second;
  }
  return Kind::UNDEFINED_KIND;
}

Node LfscNodeConverter::getOperatorOfTerm(Node n, bool macroApply)
{
  Assert(n.hasOperator());
  Assert(!n.isClosure());
  Kind k = n.getKind();
  std::stringstream opName;
  Trace("lfsc-term-process-debug2")
      << "getOperatorOfTerm " << n << " " << k << " "
      << (n.getMetaKind() == metakind::PARAMETERIZED) << " "
      << GenericOp::isIndexedOperatorKind(k) << std::endl;
  if (n.getMetaKind() == metakind::PARAMETERIZED)
  {
    Node op = n.getOperator();
    std::vector<Node> indices;
    if (GenericOp::isIndexedOperatorKind(k))
    {
      indices = GenericOp::getIndicesForOperator(k, n.getOperator());
      // we must convert the name of indices on updaters and testers
      if (k == Kind::APPLY_UPDATER || k == Kind::APPLY_TESTER)
      {
        Assert(indices.size() == 1);
        // must convert to user name
        TypeNode intType = d_nm->integerType();
        indices[0] =
            getSymbolInternal(k, intType, getNameForUserNameOf(indices[0]));
      }
    }
    else if (op.getType().isFunction())
    {
      return op;
    }
    // note other kinds of functions (e.g. selectors and testers)
    std::vector<TypeNode> argTypes;
    for (const Node& nc : n)
    {
      argTypes.push_back(nc.getType());
    }
    TypeNode ftype = n.getType();
    if (!argTypes.empty())
    {
      ftype = d_nm->mkFunctionType(argTypes, ftype);
    }
    Node ret;
    if (GenericOp::isIndexedOperatorKind(k))
    {
      std::vector<TypeNode> itypes;
      for (const Node& i : indices)
      {
        itypes.push_back(i.getType());
      }
      if (!itypes.empty())
      {
        ftype = d_nm->mkFunctionType(itypes, ftype);
      }
      if (!macroApply)
      {
        if (k != Kind::APPLY_UPDATER && k != Kind::APPLY_TESTER)
        {
          opName << "f_";
        }
      }
      // must avoid overloading for to_fp variants
      if (k == Kind::FLOATINGPOINT_TO_FP_FROM_FP)
      {
        opName << "to_fp_fp";
      }
      else if (k == Kind::FLOATINGPOINT_TO_FP_FROM_IEEE_BV)
      {
        opName << "to_fp_ieee_bv";
      }
      else if (k == Kind::FLOATINGPOINT_TO_FP_FROM_SBV)
      {
        opName << "to_fp_sbv";
      }
      else if (k == Kind::FLOATINGPOINT_TO_FP_FROM_REAL)
      {
        opName << "to_fp_real";
      }
      else if (k == Kind::BITVECTOR_BIT)
      {
        opName << "bit";
      }
      else if (k==Kind::DIVISIBLE)
      {
        opName << "a.divisible";
      }
      else
      {
        opName << printer::smt2::Smt2Printer::smtKindString(k);
      }
    }
    else if (k == Kind::APPLY_CONSTRUCTOR)
    {
      unsigned index = DType::indexOf(op);
      const DType& dt = DType::datatypeOf(op);
      // get its variable name
      opName << getNameForUserNameOf(dt[index].getConstructor());
    }
    else if (k == Kind::APPLY_SELECTOR)
    {
      ret = maybeMkSkolemFun(op, macroApply);
      if (ret.isNull())
      {
        unsigned index = DType::indexOf(op);
        const DType& dt = DType::datatypeOf(op);
        unsigned cindex = DType::cindexOf(op);
        opName << getNameForUserNameOf(dt[cindex][index].getSelector());
      }
    }
    else if (k == Kind::SET_SINGLETON || k == Kind::BAG_MAKE
             || k == Kind::SEQ_UNIT)
    {
      if (!macroApply)
      {
        opName << "f_";
      }
      opName << printer::smt2::Smt2Printer::smtKindString(k);
    }
    else
    {
      opName << op;
    }
    if (ret.isNull())
    {
      Trace("lfsc-term-process-debug2") << "...default symbol" << std::endl;
      ret = getSymbolInternal(k, ftype, opName.str());
    }
    // if indexed, apply to index
    if (!indices.empty())
    {
      ret = mkApplyUf(ret, indices);
    }
    Trace("lfsc-term-process-debug2") << "...return " << ret << std::endl;
    return ret;
  }
  std::vector<TypeNode> argTypes;
  for (const Node& nc : n)
  {
    argTypes.push_back(nc.getType());
  }
  // we only use binary operators
  if (NodeManager::isNAryKind(k))
  {
    argTypes.resize(2);
  }
  TypeNode tn = n.getType();
  TypeNode ftype = d_nm->mkFunctionType(argTypes, tn);
  // most functions are called f_X where X is the SMT-LIB name, if we are
  // getting the macroApply variant, then we don't prefix with `f_`.
  if (!macroApply)
  {
    opName << "f_";
  }
  if (k == Kind::FLOATINGPOINT_COMPONENT_NAN
      || k == Kind::FLOATINGPOINT_COMPONENT_INF
      || k == Kind::FLOATINGPOINT_COMPONENT_ZERO
      || k == Kind::FLOATINGPOINT_COMPONENT_SIGN
      || k == Kind::FLOATINGPOINT_COMPONENT_EXPONENT
      || k == Kind::FLOATINGPOINT_COMPONENT_SIGNIFICAND
      || k == Kind::ROUNDINGMODE_BITBLAST)
  {
    // remove @fp.
    std::string str = printer::smt2::Smt2Printer::smtKindString(k);
    opName << str.substr(4);
  }
  else
  {
    // all arithmetic kinds must explicitly deal with real vs int subtyping
    if (k == Kind::ADD || k == Kind::MULT || k == Kind::NONLINEAR_MULT
        || k == Kind::GEQ || k == Kind::GT || k == Kind::LEQ || k == Kind::LT
        || k == Kind::SUB || k == Kind::DIVISION || k == Kind::DIVISION_TOTAL
        || k == Kind::INTS_DIVISION || k == Kind::INTS_DIVISION_TOTAL
        || k == Kind::INTS_MODULUS || k == Kind::INTS_MODULUS_TOTAL
        || k == Kind::NEG || k == Kind::POW)
    {
      // currently allow subtyping
      opName << "a.";
    }
    if (k == Kind::NEG)
    {
      opName << "u";
    }
    opName << printer::smt2::Smt2Printer::smtKindString(k);
  }
  return getSymbolInternal(k, ftype, opName.str());
}

Node LfscNodeConverter::getOperatorOfClosure(Node q,
                                             bool macroApply,
                                             bool isPartial)
{
  TypeNode retType = isPartial ? q[1].getType() : q.getType();
  TypeNode bodyType = d_nm->mkFunctionType(q[1].getType(), retType);
  // We permit non-flat function types here
  // intType is used here for variable indices
  TypeNode intType = d_nm->integerType();
  TypeNode ftype = d_nm->mkFunctionType({intType, d_sortType}, bodyType);
  Kind k = q.getKind();
  std::stringstream opName;
  if (!macroApply)
  {
    opName << "f_";
  }
  opName << printer::smt2::Smt2Printer::smtKindString(k);
  return getSymbolInternal(k, ftype, opName.str());
}

Node LfscNodeConverter::getOperatorOfBoundVar(Node cop, Node v)
{
  Node x = d_nm->mkConstInt(Rational(getOrAssignIndexForBVar(v)));
  Node tc = typeAsNode(convertType(v.getType()));
  return mkApplyUf(cop, {x, tc});
}

size_t LfscNodeConverter::getOrAssignIndexForFVar(Node fv)
{
  std::map<Node, size_t>::iterator it = d_fvarIndex.find(fv);
  if (it != d_fvarIndex.end())
  {
    return it->second;
  }
  size_t id = d_fvarIndex.size();
  d_fvarIndex[fv] = id;
  return id;
}

size_t LfscNodeConverter::getOrAssignIndexForBVar(Node bv)
{
  Assert(bv.isVar());
  std::map<Node, size_t>::iterator it = d_bvarIndex.find(bv);
  if (it != d_bvarIndex.end())
  {
    return it->second;
  }
  size_t id = d_bvarIndex.size();
  d_bvarIndex[bv] = id;
  return id;
}

const std::unordered_set<Node>& LfscNodeConverter::getDeclaredSymbols() const
{
  return d_declVars;
}

const std::unordered_set<TypeNode>& LfscNodeConverter::getDeclaredTypes() const
{
  return d_declTypes;
}

}  // namespace proof
}  // namespace cvc5::internal
