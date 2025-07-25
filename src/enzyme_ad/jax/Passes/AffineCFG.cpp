#include "mlir/Analysis/SliceAnalysis.h"
#include "mlir/Dialect/Affine/Analysis/AffineAnalysis.h"
#include "mlir/Dialect/Affine/Analysis/LoopAnalysis.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Affine/Passes.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/IRMapping.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Interfaces/FunctionInterfaces.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "src/enzyme_ad/jax/Passes/AffineUtils.h"
#include "src/enzyme_ad/jax/Passes/Passes.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Support/Debug.h"

#include "src/enzyme_ad/jax/Utils.h"
#include "llvm/ADT/MapVector.h"

#include <deque>
#include <isl/set.h>
#include <numeric>

#define DEBUG_TYPE "affine-cfg"

namespace mlir {
namespace enzyme {
#define GEN_PASS_DEF_AFFINECFG
#include "src/enzyme_ad/jax/Passes/Passes.h.inc"
} // namespace enzyme
} // namespace mlir

using namespace mlir;
using namespace mlir::arith;
using namespace mlir::affine;
using namespace mlir::enzyme;

void populateAffineParallelizationPattern(MLIRContext &context,
                                          RewritePatternSet &patterns);

Region *getLocalAffineScope(Operation *op) {
  auto curOp = op;
  while (auto parentOp = curOp->getParentOp()) {
    if (parentOp->hasTrait<OpTrait::AffineScope>()) {
      return curOp->getParentRegion();
    }
    curOp = parentOp;
  }
  return nullptr;
}

bool isValidSymbolInt(Value value, bool recur = true);
bool isValidSymbolInt(Operation *defOp, bool recur) {
  Attribute operandCst;
  if (matchPattern(defOp, m_Constant(&operandCst)))
    return true;

  if (recur) {
    if (isa<SelectOp, IndexCastOp, IndexCastUIOp, AddIOp, MulIOp, DivSIOp,
            DivUIOp, RemSIOp, RemUIOp, SubIOp, CmpIOp, TruncIOp, ExtUIOp,
            ExtSIOp>(defOp))
      if (llvm::all_of(defOp->getOperands(), [&](Value v) {
            bool b = isValidSymbolInt(v, recur);
            // if (!b)
            //	LLVM_DEBUG(llvm::dbgs() << "illegal isValidSymbolInt: "
            //<< value << " due to " << v << "\n");
            return b;
          }))
        return true;
    if (auto ifOp = dyn_cast<scf::IfOp>(defOp)) {
      if (isValidSymbolInt(ifOp.getCondition(), recur)) {
        if (llvm::all_of(
                ifOp.thenBlock()->without_terminator(),
                [&](Operation &o) { return isValidSymbolInt(&o, recur); }) &&
            llvm::all_of(
                ifOp.elseBlock()->without_terminator(),
                [&](Operation &o) { return isValidSymbolInt(&o, recur); }))
          return true;
      }
    }
    if (auto ifOp = dyn_cast<affine::AffineIfOp>(defOp)) {
      if (llvm::all_of(ifOp.getOperands(),
                       [&](Value o) { return isValidSymbolInt(o, recur); }))
        if (llvm::all_of(
                ifOp.getThenBlock()->without_terminator(),
                [&](Operation &o) { return isValidSymbolInt(&o, recur); }) &&
            llvm::all_of(
                ifOp.getElseBlock()->without_terminator(),
                [&](Operation &o) { return isValidSymbolInt(&o, recur); }))
          return true;
    }
  }
  return false;
}

// isValidSymbol, even if not index
bool isValidSymbolInt(Value value, bool recur) {
  // Check that the value is a top level value.
  if (affine::isTopLevelValue(value))
    return true;

  if (auto *defOp = value.getDefiningOp()) {
    if (isValidSymbolInt(defOp, recur))
      return true;
    return affine::isValidSymbol(value, getLocalAffineScope(defOp));
  }

  return false;
}

struct AffineApplyNormalizer {
  AffineApplyNormalizer(AffineMap map, ArrayRef<Value> operands,
                        PatternRewriter *rewriter, DominanceInfo *DI);

  /// Returns the AffineMap resulting from normalization.
  AffineMap getAffineMap() { return affineMap; }

  SmallVector<Value, 8> getOperands() {
    SmallVector<Value, 8> res(reorderedDims);
    res.append(concatenatedSymbols.begin(), concatenatedSymbols.end());
    return res;
  }

private:
  /// Helper function to insert `v` into the coordinate system of the current
  /// AffineApplyNormalizer. Returns the AffineDimExpr with the corresponding
  /// renumbered position.
  AffineDimExpr renumberOneDim(Value v);

  /// Maps of Value to position in `affineMap`.
  DenseMap<Value, unsigned> dimValueToPosition;

  /// Ordered dims and symbols matching positional dims and symbols in
  /// `affineMap`.
  SmallVector<Value, 8> reorderedDims;
  SmallVector<Value, 8> concatenatedSymbols;

  AffineMap affineMap;
};

static bool isAffineForArg(Value val) {
  if (!isa<BlockArgument>(val))
    return false;
  Operation *parentOp = cast<BlockArgument>(val).getOwner()->getParentOp();
  return (
      isa_and_nonnull<affine::AffineForOp, affine::AffineParallelOp>(parentOp));
}

static bool legalCondition(Value en, bool dim = false) {
  if (en.getDefiningOp<affine::AffineApplyOp>())
    return true;

  if (!dim && !isValidSymbolInt(en, /*recur*/ false)) {
    if (isValidIndex(en) || isValidSymbolInt(en, /*recur*/ true)) {
      return true;
    }
  }

  while (auto ic = en.getDefiningOp<IndexCastOp>())
    en = ic.getIn();

  while (auto ic = en.getDefiningOp<IndexCastUIOp>())
    en = ic.getIn();

  if ((en.getDefiningOp<AddIOp>() || en.getDefiningOp<SubIOp>() ||
       en.getDefiningOp<MulIOp>() || en.getDefiningOp<RemUIOp>() ||
       en.getDefiningOp<RemSIOp>()) &&
      (en.getDefiningOp()->getOperand(1).getDefiningOp<ConstantIntOp>() ||
       en.getDefiningOp()->getOperand(1).getDefiningOp<ConstantIndexOp>()))
    return true;
  // if (auto IC = dyn_cast_or_null<IndexCastOp>(en.getDefiningOp())) {
  //	if (!outer || legalCondition(IC.getOperand(), false)) return true;
  //}
  if (!dim)
    if (auto BA = dyn_cast<BlockArgument>(en)) {
      if (isa<affine::AffineForOp, affine::AffineParallelOp>(
              BA.getOwner()->getParentOp()))
        return true;
    }
  return false;
}

bool isNonTopLevelPureSymbol(Value value) {
  if (auto *defOp = value.getDefiningOp()) {
    if (!isPure(defOp))
      return false;

    auto region = getLocalAffineScope(defOp);
    Attribute operandCst;
    if (!matchPattern(defOp, m_Constant(&operandCst)) &&
        !affine::isValidSymbol(value, region))
      return false;
    if (defOp->getNumOperands() != 0)
      return false;
    if (defOp->getParentRegion() == region)
      return false;
    return true;
  }
  return false;
}

/// The AffineNormalizer composes AffineApplyOp recursively. Its purpose is to
/// keep a correspondence between the mathematical `map` and the `operands` of
/// a given affine::AffineApplyOp. This correspondence is maintained by
/// iterating over the operands and forming an `auxiliaryMap` that can be
/// composed mathematically with `map`. To keep this correspondence in cases
/// where symbols are produced by affine.apply operations, we perform a local
/// rewrite of symbols as dims.
///
/// Rationale for locally rewriting symbols as dims:
/// ================================================
/// The mathematical composition of AffineMap must always concatenate symbols
/// because it does not have enough information to do otherwise. For example,
/// composing `(d0)[s0] -> (d0 + s0)` with itself must produce
/// `(d0)[s0, s1] -> (d0 + s0 + s1)`.
///
/// The result is only equivalent to `(d0)[s0] -> (d0 + 2 * s0)` when
/// applied to the same mlir::Value for both s0 and s1.
/// As a consequence mathematical composition of AffineMap always concatenates
/// symbols.
///
/// When AffineMaps are used in affine::AffineApplyOp however, they may specify
/// composition via symbols, which is ambiguous mathematically. This corner case
/// is handled by locally rewriting such symbols that come from
/// affine::AffineApplyOp into dims and composing through dims.
/// TODO: Composition via symbols comes at a significant code
/// complexity. Alternatively we should investigate whether we want to
/// explicitly disallow symbols coming from affine.apply and instead force the
/// user to compose symbols beforehand. The annoyances may be small (i.e. 1 or 2
/// extra API calls for such uses, which haven't popped up until now) and the
/// benefit potentially big: simpler and more maintainable code for a
/// non-trivial, recursive, procedure.
AffineApplyNormalizer::AffineApplyNormalizer(AffineMap map,
                                             ArrayRef<Value> operands,
                                             PatternRewriter *rewriter,
                                             DominanceInfo *DI) {
  assert(map.getNumInputs() == operands.size() &&
         "number of operands does not match the number of map inputs");

  LLVM_DEBUG(map.print(llvm::dbgs() << "\nInput map: "));

  SmallVector<Value, 8> addedValues;

  llvm::SmallSet<unsigned, 1> symbolsToPromote;

  unsigned numDims = map.getNumDims();

  SmallVector<AffineExpr, 8> dimReplacements;
  SmallVector<AffineExpr, 8> symReplacements;

  SmallVector<SmallVectorImpl<Value> *> opsTodos;
  auto replaceOp = [&](Operation *oldOp, Operation *newOp) {
    for (auto [oldV, newV] :
         llvm::zip(oldOp->getResults(), newOp->getResults()))
      for (auto ops : opsTodos)
        for (auto &op : *ops)
          if (op == oldV)
            op = newV;
  };

  SmallVector<Operation **> operationContext;
  std::function<Value(Value, bool)> fix = [&](Value v,
                                              bool index) -> Value /*legal*/ {
    bool ntop = isNonTopLevelPureSymbol(v);
    if (!ntop && isValidSymbolInt(v, /*recur*/ false)) {
      return v;
    }
    if (index && isAffineForArg(v)) {
      return v;
    }
    auto *op = v.getDefiningOp();
    if (!op)
      return nullptr;
    if (!op)
      llvm::errs() << v << "\n";
    assert(op);
    if (!isReadOnly(op)) {
      return nullptr;
    }
    Operation *front = nullptr;
    operationContext.push_back(&front);
    if (front)
      assert(front->getBlock());
    SmallVector<Value> ops;
    opsTodos.push_back(&ops);
    std::function<void(Operation *)> getAllOps = [&](Operation *todo) {
      assert(todo->getBlock());
      for (auto v : todo->getOperands()) {
        if (llvm::all_of(op->getRegions(), [&](Region &r) {
              return !r.isAncestor(v.getParentRegion());
            }))
          ops.push_back(v);
      }
      for (auto &r : todo->getRegions()) {
        for (auto &b : r.getBlocks())
          for (auto &o2 : b.without_terminator())
            getAllOps(&o2);
      }
    };

    if (front)
      assert(front->getBlock());
    getAllOps(op);

    if (front)
      assert(front->getBlock());

    for (auto o : ops) {
      if (front)
        assert(front->getBlock());
      Operation *next;
      if (auto *op = o.getDefiningOp()) {
        assert(op->getBlock());
        if (front)
          assert(front->getBlock());
        if (Value nv = fix(o, index)) {
          op = nv.getDefiningOp();
        } else {
          operationContext.pop_back();
          return nullptr;
        }
        next = op->getNextNode();
        assert(next->getBlock());
        if (front)
          assert(front->getBlock());
      } else {
        auto BA = cast<BlockArgument>(o);
        if (index && isAffineForArg(BA)) {
        } else if (!isValidSymbolInt(o, /*recur*/ false)) {
          operationContext.pop_back();
          return nullptr;
        }
        next = &BA.getOwner()->front();
        assert(next->getBlock());
        if (front)
          assert(front->getBlock());
      }
      if (front)
        assert(front->getBlock());
      if (next)
        assert(next->getBlock());
      if (front == nullptr)
        front = next;
      else if (DI && DI->dominates(front, next))
        front = next;
      if (front)
        assert(front->getBlock());
    }
    if (!front && ntop) {
      auto region = getLocalAffineScope(op);
      front = &region->front().front();
    }
    opsTodos.pop_back();
    if (!front)
      op->dump();
    assert(front);
    if (!rewriter) {
      operationContext.pop_back();
      assert(isValidSymbolInt(op->getResult(0), /*recur*/ false));
      return op->getResult(0);
    } else {
      PatternRewriter::InsertionGuard B(*rewriter);
      rewriter->setInsertionPoint(front);
      if (front)
        assert(front->getBlock());
      auto cloned = rewriter->clone(*op);
      replaceOp(op, cloned);
      if (front)
        assert(front->getBlock());
      for (auto op_ptr : operationContext) {
        if (*op_ptr == op) {
          *op_ptr = cloned;
        }
      }
      rewriter->replaceOp(op, cloned->getResults());

      operationContext.pop_back();
      if (!isValidSymbolInt(cloned->getResult(0), /*recur*/ false)) {
        llvm::errs() << " clonedParent: "
                     << *cloned->getParentOfType<FunctionOpInterface>() << "\n";
        llvm::errs() << " cloned: " << *cloned << "\n";
        llvm_unreachable("busted");
      }
      return cloned->getResult(0);
    }
  };
  auto renumberOneSymbol = [&](Value v) {
    for (auto i : llvm::enumerate(addedValues)) {
      if (i.value() == v)
        return getAffineSymbolExpr(i.index(), map.getContext());
    }
    auto expr = getAffineSymbolExpr(addedValues.size(), map.getContext());
    addedValues.push_back(v);
    return expr;
  };

  // 2. Compose affine::AffineApplyOps and dispatch dims or symbols.
  for (unsigned i = 0, e = operands.size(); i < e; ++i) {
    auto t = operands[i];
    auto decast = t;
    while (true) {
      if (auto idx = decast.getDefiningOp<IndexCastOp>()) {
        decast = idx.getIn();
        continue;
      }
      if (auto idx = decast.getDefiningOp<IndexCastUIOp>()) {
        decast = idx.getIn();
        continue;
      }
      if (auto idx = decast.getDefiningOp<TruncIOp>()) {
        decast = idx.getIn();
        continue;
      }
      if (auto idx = decast.getDefiningOp<ExtUIOp>()) {
        decast = idx.getIn();
        continue;
      }
      if (auto idx = decast.getDefiningOp<ExtSIOp>()) {
        decast = idx.getIn();
        continue;
      }
      break;
    }

    if (!isValidSymbolInt(t, /*recur*/ false)) {
      t = decast;
    }

    // Only promote one at a time, lest we end up with two dimensions
    // multiplying each other.

    if (((!isValidSymbolInt(t, /*recur*/ false) &&
          (t.getDefiningOp<AddIOp>() || t.getDefiningOp<SubIOp>() ||
           (t.getDefiningOp<MulIOp>() &&
            ((isValidIndex(t.getDefiningOp()->getOperand(0)) &&
              isValidSymbolInt(t.getDefiningOp()->getOperand(1))) ||
             (isValidIndex(t.getDefiningOp()->getOperand(1)) &&
              isValidSymbolInt(t.getDefiningOp()->getOperand(0)))) &&
            !(fix(t.getDefiningOp()->getOperand(0), false) &&
              fix(t.getDefiningOp()->getOperand(1), false))

                ) ||
           ((t.getDefiningOp<DivUIOp>() || t.getDefiningOp<DivSIOp>()) &&
            (isValidIndex(t.getDefiningOp()->getOperand(0)) &&
             isValidSymbolInt(t.getDefiningOp()->getOperand(1))) &&
            (!(fix(t.getDefiningOp()->getOperand(0), false) &&
               fix(t.getDefiningOp()->getOperand(1), false)))) ||
           (t.getDefiningOp<DivSIOp>() &&
            (isValidIndex(t.getDefiningOp()->getOperand(0)) &&
             isValidSymbolInt(t.getDefiningOp()->getOperand(1)))) ||
           (t.getDefiningOp<DivUIOp>() &&
            (isValidIndex(t.getDefiningOp()->getOperand(0)) &&
             isValidSymbolInt(t.getDefiningOp()->getOperand(1)))) ||
           (t.getDefiningOp<RemUIOp>() &&
            (isValidIndex(t.getDefiningOp()->getOperand(0)) &&
             isValidSymbolInt(t.getDefiningOp()->getOperand(1)))) ||
           (t.getDefiningOp<RemSIOp>() &&
            (isValidIndex(t.getDefiningOp()->getOperand(0)) &&
             isValidSymbolInt(t.getDefiningOp()->getOperand(1)))) ||
           t.getDefiningOp<ConstantIntOp>() ||
           t.getDefiningOp<ConstantIndexOp>())) ||
         ((decast.getDefiningOp<AddIOp>() || decast.getDefiningOp<SubIOp>() ||
           decast.getDefiningOp<MulIOp>() || decast.getDefiningOp<RemUIOp>() ||
           decast.getDefiningOp<RemSIOp>() || decast.getDefiningOp<ShRUIOp>() ||
           decast.getDefiningOp<ShLIOp>()) &&
          (decast.getDefiningOp()
               ->getOperand(1)
               .getDefiningOp<ConstantIntOp>() ||
           decast.getDefiningOp()
               ->getOperand(1)
               .getDefiningOp<ConstantIndexOp>())))) {
      t = decast;
      LLVM_DEBUG(llvm::dbgs() << " Replacing: " << t << "\n");

      AffineMap affineApplyMap;
      SmallVector<Value, 8> affineApplyOperands;

      // llvm::dbgs() << "\nop to start: " << t << "\n";

      if (auto op = t.getDefiningOp<AddIOp>()) {
        affineApplyMap =
            AffineMap::get(0, 2,
                           getAffineSymbolExpr(0, op.getContext()) +
                               getAffineSymbolExpr(1, op.getContext()));
        affineApplyOperands.push_back(op.getLhs());
        affineApplyOperands.push_back(op.getRhs());
      } else if (auto op = t.getDefiningOp<SubIOp>()) {
        affineApplyMap =
            AffineMap::get(0, 2,
                           getAffineSymbolExpr(0, op.getContext()) -
                               getAffineSymbolExpr(1, op.getContext()));
        affineApplyOperands.push_back(op.getLhs());
        affineApplyOperands.push_back(op.getRhs());
      } else if (auto op = t.getDefiningOp<MulIOp>()) {
        if (auto ci = op.getRhs().getDefiningOp<ConstantIntOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1, getAffineSymbolExpr(0, op.getContext()) * ci.value());
          affineApplyOperands.push_back(op.getLhs());
        } else if (auto ci = op.getRhs().getDefiningOp<ConstantIndexOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1, getAffineSymbolExpr(0, op.getContext()) * ci.value());
          affineApplyOperands.push_back(op.getLhs());
        } else {
          affineApplyMap =
              AffineMap::get(0, 2,
                             getAffineSymbolExpr(0, op.getContext()) *
                                 getAffineSymbolExpr(1, op.getContext()));
          affineApplyOperands.push_back(op.getLhs());
          affineApplyOperands.push_back(op.getRhs());
        }
      } else if (auto op = t.getDefiningOp<DivSIOp>()) {
        if (auto ci = op.getRhs().getDefiningOp<ConstantIntOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1,
              getAffineSymbolExpr(0, op.getContext()).floorDiv(ci.value()));
          affineApplyOperands.push_back(op.getLhs());
        } else if (auto ci = op.getRhs().getDefiningOp<ConstantIndexOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1,
              getAffineSymbolExpr(0, op.getContext()).floorDiv(ci.value()));
          affineApplyOperands.push_back(op.getLhs());
        } else {
          affineApplyMap = AffineMap::get(
              0, 2,
              getAffineSymbolExpr(0, op.getContext())
                  .floorDiv(getAffineSymbolExpr(1, op.getContext())));
          affineApplyOperands.push_back(op.getLhs());
          affineApplyOperands.push_back(op.getRhs());
        }
      } else if (auto op = t.getDefiningOp<DivUIOp>()) {
        if (auto ci = op.getRhs().getDefiningOp<ConstantIntOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1,
              getAffineSymbolExpr(0, op.getContext()).floorDiv(ci.value()));
          affineApplyOperands.push_back(op.getLhs());
        } else if (auto ci = op.getRhs().getDefiningOp<ConstantIndexOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1,
              getAffineSymbolExpr(0, op.getContext()).floorDiv(ci.value()));
          affineApplyOperands.push_back(op.getLhs());
        } else {
          affineApplyMap = AffineMap::get(
              0, 2,
              getAffineSymbolExpr(0, op.getContext())
                  .floorDiv(getAffineSymbolExpr(1, op.getContext())));
          affineApplyOperands.push_back(op.getLhs());
          affineApplyOperands.push_back(op.getRhs());
        }
      } else if (auto op = t.getDefiningOp<RemSIOp>()) {
        if (auto ci = op.getRhs().getDefiningOp<ConstantIntOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1, getAffineSymbolExpr(0, op.getContext()) % ci.value());
          affineApplyOperands.push_back(op.getLhs());
        } else if (auto ci = op.getRhs().getDefiningOp<ConstantIndexOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1, getAffineSymbolExpr(0, op.getContext()) % ci.value());
          affineApplyOperands.push_back(op.getLhs());
        } else {
          affineApplyMap =
              AffineMap::get(0, 2,
                             getAffineSymbolExpr(0, op.getContext()) %
                                 getAffineSymbolExpr(1, op.getContext()));
          affineApplyOperands.push_back(op.getLhs());
          affineApplyOperands.push_back(op.getRhs());
        }
      } else if (auto op = t.getDefiningOp<RemUIOp>()) {
        if (auto ci = op.getRhs().getDefiningOp<ConstantIntOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1, getAffineSymbolExpr(0, op.getContext()) % ci.value());
          affineApplyOperands.push_back(op.getLhs());
        } else if (auto ci = op.getRhs().getDefiningOp<ConstantIndexOp>()) {
          affineApplyMap = AffineMap::get(
              0, 1, getAffineSymbolExpr(0, op.getContext()) % ci.value());
          affineApplyOperands.push_back(op.getLhs());
        } else {
          affineApplyMap =
              AffineMap::get(0, 2,
                             getAffineSymbolExpr(0, op.getContext()) %
                                 getAffineSymbolExpr(1, op.getContext()));
          affineApplyOperands.push_back(op.getLhs());
          affineApplyOperands.push_back(op.getRhs());
        }
      } else if (auto op = t.getDefiningOp<ShRUIOp>()) {

        APInt iattr;
        if (!matchPattern(op.getRhs(), m_ConstantInt(&iattr))) {
          llvm_unreachable("shr rhs needed to be constant int");
        }

        affineApplyMap =
            AffineMap::get(0, 1,
                           getAffineSymbolExpr(0, op.getContext())
                               .floorDiv(1 << iattr.getZExtValue()));
        affineApplyOperands.push_back(op.getLhs());
      } else if (auto op = t.getDefiningOp<ShLIOp>()) {

        APInt iattr;
        if (!matchPattern(op.getRhs(), m_ConstantInt(&iattr))) {
          llvm_unreachable("shl rhs needed to be constant int");
        }

        affineApplyMap =
            AffineMap::get(0, 1,
                           getAffineSymbolExpr(0, op.getContext()) *
                               (1 << iattr.getZExtValue()));
        affineApplyOperands.push_back(op.getLhs());
      } else if (auto op = t.getDefiningOp<ConstantIntOp>()) {
        affineApplyMap = AffineMap::get(
            0, 0, getAffineConstantExpr(op.value(), op.getContext()));
      } else if (auto op = t.getDefiningOp<ConstantIndexOp>()) {
        affineApplyMap = AffineMap::get(
            0, 0, getAffineConstantExpr(op.value(), op.getContext()));
      } else {
        llvm_unreachable("");
      }

      SmallVector<AffineExpr, 0> dimRemapping;
      unsigned numOtherSymbols = affineApplyOperands.size();
      SmallVector<AffineExpr, 2> symRemapping(numOtherSymbols);
      for (unsigned idx = 0; idx < numOtherSymbols; ++idx) {
        symRemapping[idx] = renumberOneSymbol(affineApplyOperands[idx]);
      }
      affineApplyMap = affineApplyMap.replaceDimsAndSymbols(
          dimRemapping, symRemapping, reorderedDims.size(), addedValues.size());

      LLVM_DEBUG(affineApplyMap.print(
          llvm::dbgs() << "\nRenumber into current normalizer: "));

      if (i >= numDims)
        symReplacements.push_back(affineApplyMap.getResult(0));
      else
        dimReplacements.push_back(affineApplyMap.getResult(0));

    } else if (isAffineForArg(t)) {
      if (i >= numDims)
        symReplacements.push_back(renumberOneDim(t));
      else
        dimReplacements.push_back(renumberOneDim(t));
    } else if (t.getDefiningOp<affine::AffineApplyOp>()) {
      auto affineApply = t.getDefiningOp<affine::AffineApplyOp>();
      // a. Compose affine.apply operations.
      LLVM_DEBUG(affineApply->print(
          llvm::dbgs() << "\nCompose affine::AffineApplyOp recursively: "));
      AffineMap affineApplyMap = affineApply.getAffineMap();
      SmallVector<Value, 8> affineApplyOperands(
          affineApply.getOperands().begin(), affineApply.getOperands().end());

      SmallVector<AffineExpr, 0> dimRemapping(affineApplyMap.getNumDims());

      for (size_t i = 0; i < affineApplyMap.getNumDims(); ++i) {
        assert(i < affineApplyOperands.size());
        dimRemapping[i] = renumberOneDim(affineApplyOperands[i]);
      }
      unsigned numOtherSymbols = affineApplyOperands.size();
      SmallVector<AffineExpr, 2> symRemapping(numOtherSymbols -
                                              affineApplyMap.getNumDims());
      for (unsigned idx = 0; idx < symRemapping.size(); ++idx) {
        symRemapping[idx] = renumberOneSymbol(
            affineApplyOperands[idx + affineApplyMap.getNumDims()]);
      }
      affineApplyMap = affineApplyMap.replaceDimsAndSymbols(
          dimRemapping, symRemapping, reorderedDims.size(), addedValues.size());

      LLVM_DEBUG(
          affineApplyMap.print(llvm::dbgs() << "\nAffine apply fixup map: "));

      if (i >= numDims)
        symReplacements.push_back(affineApplyMap.getResult(0));
      else
        dimReplacements.push_back(affineApplyMap.getResult(0));
    } else {
      if (!isValidSymbolInt(t, /*recur*/ false)) {
        if (t.getDefiningOp()) {
          if ((t = fix(t, false))) {
            if (!isValidSymbolInt(t, /*recur*/ false)) {
              llvm::errs()
                  << " op: "
                  << *t.getDefiningOp()->getParentOfType<FunctionOpInterface>()
                  << "\n";
              llvm::errs() << " failed to move:" << t
                           << " to become valid symbol\n";
              llvm_unreachable("cannot move");
            }
          } else
            llvm_unreachable("cannot move");
        } else
          llvm_unreachable("cannot move2");
      }
      if (i < numDims) {
        // b. The mathematical composition of AffineMap composes dims.
        dimReplacements.push_back(renumberOneDim(t));
      } else {
        // c. The mathematical composition of AffineMap concatenates symbols.
        //    Note that the map composition will put symbols already present
        //    in the map before any symbols coming from the auxiliary map, so
        //    we insert them before any symbols that are due to renumbering,
        //    and after the proper symbols we have seen already.
        symReplacements.push_back(renumberOneSymbol(t));
      }
    }
  }
  for (auto v : addedValues)
    concatenatedSymbols.push_back(v);

  // Create the new map by replacing each symbol at pos by the next new dim.
  unsigned numNewDims = reorderedDims.size();
  unsigned numNewSymbols = addedValues.size();
  assert(dimReplacements.size() == map.getNumDims());
  assert(symReplacements.size() == map.getNumSymbols());
  auto auxillaryMap = map.replaceDimsAndSymbols(
      dimReplacements, symReplacements, numNewDims, numNewSymbols);
  LLVM_DEBUG(auxillaryMap.print(llvm::dbgs() << "\nRewritten map: "));

  affineMap = auxillaryMap; // simplifyAffineMap(auxillaryMap);

  LLVM_DEBUG(affineMap.print(llvm::dbgs() << "\nSimplified result: "));
  LLVM_DEBUG(llvm::dbgs() << "\n");
}

AffineDimExpr AffineApplyNormalizer::renumberOneDim(Value v) {
  DenseMap<Value, unsigned>::iterator iterPos;
  bool inserted = false;
  std::tie(iterPos, inserted) =
      dimValueToPosition.insert(std::make_pair(v, dimValueToPosition.size()));
  if (inserted) {
    reorderedDims.push_back(v);
  }
  return cast<AffineDimExpr>(getAffineDimExpr(iterPos->second, v.getContext()));
}

static void composeAffineMapAndOperands(AffineMap *map,
                                        SmallVectorImpl<Value> *operands,
                                        PatternRewriter *rewriter,
                                        DominanceInfo *DI) {
  AffineApplyNormalizer normalizer(*map, *operands, rewriter, DI);
  auto normalizedMap = normalizer.getAffineMap();
  auto normalizedOperands = normalizer.getOperands();
  affine::canonicalizeMapAndOperands(&normalizedMap, &normalizedOperands);
  normalizedMap = recreateExpr(normalizedMap);
  *map = normalizedMap;
  *operands = normalizedOperands;
  assert(*map);
}

bool need(AffineMap *map, SmallVectorImpl<Value> *operands) {
  assert(map->getNumInputs() == operands->size());
  for (size_t i = 0; i < map->getNumInputs(); ++i) {
    auto v = (*operands)[i];
    if (legalCondition(v, i < map->getNumDims()))
      return true;
  }
  return false;
}
bool need(IntegerSet *map, SmallVectorImpl<Value> *operands) {
  for (size_t i = 0; i < map->getNumInputs(); ++i) {
    auto v = (*operands)[i];
    if (legalCondition(v, i < map->getNumDims()))
      return true;
  }
  return false;
}

void fully2ComposeAffineMapAndOperands(
    PatternRewriter *builder, AffineMap *map, SmallVectorImpl<Value> *operands,
    DominanceInfo *DI, SmallVectorImpl<Operation *> *insertedOps = nullptr) {
  IRMapping indexMap;
  if (builder)
    for (auto op : *operands) {
      SmallVector<IndexCastOp> attempt;
      auto idx0 = op.getDefiningOp<IndexCastOp>();
      attempt.push_back(idx0);
      if (!idx0)
        continue;

      for (auto &u : idx0.getIn().getUses()) {
        if (auto idx = dyn_cast<IndexCastOp>(u.getOwner()))
          if (DI->dominates((Operation *)idx, &*builder->getInsertionPoint()))
            attempt.push_back(idx);
      }

      for (auto idx : attempt) {
        if (affine::isValidSymbol(idx)) {
          indexMap.map(idx.getIn(), idx);
          break;
        }
      }
    }
  assert(map->getNumInputs() == operands->size());
  while (need(map, operands)) {
    composeAffineMapAndOperands(map, operands, builder, DI);
    assert(map->getNumInputs() == operands->size());
  }
  *map = simplifyAffineMap(*map);
  if (builder)
    for (auto &op : *operands) {
      if (!op.getType().isIndex()) {
        Operation *toInsert;
        if (auto *o = op.getDefiningOp())
          toInsert = o->getNextNode();
        else {
          auto BA = cast<BlockArgument>(op);
          toInsert = &BA.getOwner()->front();
        }

        if (auto v = indexMap.lookupOrNull(op))
          op = v;
        else {
          if (insertedOps) {
            OpBuilder builder(toInsert);
            auto inserted = builder.create<IndexCastOp>(
                op.getLoc(), builder.getIndexType(), op);
            op = inserted->getResult(0);
            insertedOps->push_back(inserted);
          } else {
            PatternRewriter::InsertionGuard B(*builder);
            builder->setInsertionPoint(toInsert);
            auto inserted = builder->create<IndexCastOp>(
                op.getLoc(), builder->getIndexType(), op);
            op = inserted->getResult(0);
          }
        }
      }
    }
}

void fully2ComposeAffineMapAndOperands(
    PatternRewriter &builder, AffineMap *map, SmallVectorImpl<Value> *operands,
    DominanceInfo &DI, SmallVectorImpl<Operation *> *insertedOps) {
  fully2ComposeAffineMapAndOperands(&builder, map, operands, &DI, insertedOps);
}

void fully2ComposeIntegerSetAndOperands(
    PatternRewriter &builder, IntegerSet *set, SmallVectorImpl<Value> *operands,
    DominanceInfo &DI, SmallVectorImpl<Operation *> *insertedOps = nullptr) {
  IRMapping indexMap;
  for (auto op : *operands) {
    SmallVector<IndexCastOp> attempt;
    auto idx0 = op.getDefiningOp<IndexCastOp>();
    attempt.push_back(idx0);
    if (!idx0)
      continue;

    for (auto &u : idx0.getIn().getUses()) {
      if (auto idx = dyn_cast<IndexCastOp>(u.getOwner()))
        if (DI.dominates((Operation *)idx, &*builder.getInsertionPoint()))
          attempt.push_back(idx);
    }

    for (auto idx : attempt) {
      if (affine::isValidSymbol(idx)) {
        indexMap.map(idx.getIn(), idx);
        break;
      }
    }
  }
  auto map = AffineMap::get(set->getNumDims(), set->getNumSymbols(),
                            set->getConstraints(), set->getContext());
  while (need(&map, operands)) {
    composeAffineMapAndOperands(&map, operands, &builder, &DI);
  }
  map = simplifyAffineMap(map);
  *set = IntegerSet::get(map.getNumDims(), map.getNumSymbols(),
                         map.getResults(), set->getEqFlags());
  for (auto &op : *operands) {
    if (!op.getType().isIndex()) {
      Operation *toInsert;
      if (auto *o = op.getDefiningOp())
        toInsert = o->getNextNode();
      else {
        auto BA = cast<BlockArgument>(op);
        toInsert = &BA.getOwner()->front();
      }

      if (auto v = indexMap.lookupOrNull(op))
        op = v;
      else {
        if (insertedOps) {
          OpBuilder builder(toInsert);
          auto inserted = builder.create<IndexCastOp>(
              op.getLoc(), builder.getIndexType(), op);
          op = inserted->getResult(0);
          insertedOps->push_back(inserted);
        } else {
          PatternRewriter::InsertionGuard B(builder);
          builder.setInsertionPoint(toInsert);
          auto inserted = builder.create<IndexCastOp>(
              op.getLoc(), builder.getIndexType(), op);
          op = inserted->getResult(0);
        }
      }
    }
  }
}

namespace {
struct AffineCFGPass : public enzyme::impl::AffineCFGBase<AffineCFGPass> {
  void runOnOperation() override;
};
} // namespace

static void setLocationAfter(PatternRewriter &b, mlir::Value val) {
  if (val.getDefiningOp()) {
    auto it = val.getDefiningOp()->getIterator();
    it++;
    b.setInsertionPoint(val.getDefiningOp()->getBlock(), it);
  }
  if (auto bop = dyn_cast<mlir::BlockArgument>(val))
    b.setInsertionPoint(bop.getOwner(), bop.getOwner()->begin());
}

template <typename T> struct IndexCastMovement : public OpRewritePattern<T> {
  using OpRewritePattern<T>::OpRewritePattern;

  LogicalResult matchAndRewrite(T op,
                                PatternRewriter &rewriter) const override {
    if (op.use_empty()) {
      rewriter.eraseOp(op);
      return success();
    }

    mlir::Value val = op.getOperand();
    if (auto bop = dyn_cast<mlir::BlockArgument>(val)) {
      if (op.getOperation()->getBlock() != bop.getOwner()) {
        op.getOperation()->moveBefore(bop.getOwner(), bop.getOwner()->begin());
        return success();
      }
      return failure();
    }

    if (val.getDefiningOp()) {
      if (op.getOperation()->getBlock() != val.getDefiningOp()->getBlock()) {
        auto it = val.getDefiningOp()->getIterator();
        op.getOperation()->moveAfter(val.getDefiningOp()->getBlock(), it);
      }
      return failure();
    }
    return failure();
  }
};

/*
struct SimplfyIntegerCastMath : public OpRewritePattern<IndexCastOp> {
  using OpRewritePattern<IndexCastOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(IndexCastOp op,
                                PatternRewriter &rewriter) const override {
    if (op.use_empty()) {
      rewriter.eraseOp(op);
      return success();
    }
    if (auto iadd = op.getOperand().getDefiningOp<AddIOp>()) {
      PatternRewriter b(rewriter);
      setLocationAfter(b, iadd.getOperand(0));
      PatternRewriter b2(rewriter);
      setLocationAfter(b2, iadd.getOperand(1));
      rewriter.replaceOpWithNewOp<AddIOp>(
          op,
          b.create<IndexCastOp>(op.getLoc(), op.getType(), iadd.getOperand(0)),
          b2.create<IndexCastOp>(op.getLoc(), op.getType(),
                                 iadd.getOperand(1)));
      return success();
    }
    if (auto iadd = op.getOperand().getDefiningOp<SubIOp>()) {
      PatternRewriter b(rewriter);
      setLocationAfter(b, iadd.getOperand(0));
      PatternRewriter b2(rewriter);
      setLocationAfter(b2, iadd.getOperand(1));
      rewriter.replaceOpWithNewOp<SubIOp>(
          op,
          b.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                       iadd.getOperand(0)),
          b2.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                        iadd.getOperand(1)));
      return success();
    }
    if (auto iadd = op.getOperand().getDefiningOp<MulIOp>()) {
      PatternRewriter b(rewriter);
      setLocationAfter(b, iadd.getOperand(0));
      PatternRewriter b2(rewriter);
      setLocationAfter(b2, iadd.getOperand(1));
      rewriter.replaceOpWithNewOp<MulIOp>(
          op,
          b.create<IndexCastOp>(op.getLoc(), op.getType(), iadd.getOperand(0)),
          b2.create<IndexCastOp>(op.getLoc(), op.getType(),
                                 iadd.getOperand(1)));
      return success();
    }
    if (auto iadd = op.getOperand().getDefiningOp<DivUIOp>()) {
      PatternRewriter b(rewriter);
      setLocationAfter(b, iadd.getOperand(0));
      PatternRewriter b2(rewriter);
      setLocationAfter(b2, iadd.getOperand(1));
      rewriter.replaceOpWithNewOp<DivUIOp>(
          op,
          b.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                       iadd.getOperand(0)),
          b2.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                        iadd.getOperand(1)));
      return success();
    }
    if (auto iadd = op.getOperand().getDefiningOp<DivSIOp>()) {
      PatternRewriter b(rewriter);
      setLocationAfter(b, iadd.getOperand(0));
      PatternRewriter b2(rewriter);
      setLocationAfter(b2, iadd.getOperand(1));
      rewriter.replaceOpWithNewOp<DivSIOp>(
          op,
          b.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                       iadd.getOperand(0)),
          b2.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                        iadd.getOperand(1)));
      return success();
    }
    if (auto iadd = op.getOperand().getDefiningOp<RemUIOp>()) {
      PatternRewriter b(rewriter);
      setLocationAfter(b, iadd.getOperand(0));
      PatternRewriter b2(rewriter);
      setLocationAfter(b2, iadd.getOperand(1));
      rewriter.replaceOpWithNewOp<RemUIOp>(
          op,
          b.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                       iadd.getOperand(0)),
          b2.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                        iadd.getOperand(1)));
      return success();
    }
    if (auto iadd = op.getOperand().getDefiningOp<RemSIOp>()) {
      PatternRewriter b(rewriter);
      setLocationAfter(b, iadd.getOperand(0));
      PatternRewriter b2(rewriter);
      setLocationAfter(b2, iadd.getOperand(1));
      rewriter.replaceOpWithNewOp<RemSIOp>(
          op,
          b.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                       iadd.getOperand(0)),
          b2.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                        iadd.getOperand(1)));
      return success();
    }
    if (auto iadd = op.getOperand().getDefiningOp<SelectOp>()) {
      PatternRewriter b(rewriter);
      setLocationAfter(b, iadd.getTrueValue());
      PatternRewriter b2(rewriter);
      setLocationAfter(b2, iadd.getFalseValue());
      auto cond = iadd.getCondition();
      PatternRewriter b3(rewriter);
      setLocationAfter(b3, cond);
      if (auto cmp = iadd.getCondition().getDefiningOp<CmpIOp>()) {
        if (cmp.getLhs() == iadd.getTrueValue() &&
            cmp.getRhs() == iadd.getFalseValue()) {

          auto truev = b.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                                    iadd.getTrueValue());
          auto falsev = b2.create<arith::IndexCastOp>(op.getLoc(), op.getType(),
                                                      iadd.getFalseValue());
          cond = b3.create<CmpIOp>(cmp.getLoc(), cmp.getPredicate(), truev,
                                   falsev);
          rewriter.replaceOpWithNewOp<SelectOp>(op, cond, truev, falsev);
          return success();
        }
      }
    }
    return failure();
  }
};
*/

struct CanonicalizeAffineApply
    : public OpRewritePattern<affine::AffineApplyOp> {
  using OpRewritePattern<affine::AffineApplyOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(affine::AffineApplyOp affineOp,
                                PatternRewriter &rewriter) const override {

    SmallVector<Value, 4> mapOperands(affineOp.getMapOperands());
    auto map = affineOp.getMap();
    auto prevMap = map;

    auto *scope = getLocalAffineScope(affineOp)->getParentOp();
    DominanceInfo DI(scope);

    fully2ComposeAffineMapAndOperands(rewriter, &map, &mapOperands, DI);
    affine::canonicalizeMapAndOperands(&map, &mapOperands);
    map = removeDuplicateExprs(map);
    map = recreateExpr(map);

    if (map == prevMap)
      return failure();

    rewriter.replaceOpWithNewOp<affine::AffineApplyOp>(affineOp, map,
                                                       mapOperands);
    return success();
  }
};

template <typename T>
struct CanonicalizeIndexCast : public OpRewritePattern<T> {
  using OpRewritePattern<T>::OpRewritePattern;

  LogicalResult matchAndRewrite(T indexcastOp,
                                PatternRewriter &rewriter) const override {

    // Fold IndexCast(IndexCast(x)) -> x
    auto cast = indexcastOp.getOperand().template getDefiningOp<T>();
    if (cast && cast.getOperand().getType() == indexcastOp.getType()) {
      mlir::Value vals[] = {cast.getOperand()};
      rewriter.replaceOp(indexcastOp, vals);
      return success();
    }

    // Fold IndexCast(constant) -> constant
    // A little hack because we go through int.  Otherwise, the size
    // of the constant might need to change.
    if (auto cst =
            indexcastOp.getOperand().template getDefiningOp<ConstantIntOp>()) {
      rewriter.replaceOpWithNewOp<ConstantIndexOp>(indexcastOp, cst.value());
      return success();
    }
    return failure();
  }
};

/*
struct CanonicalizeAffineIf : public OpRewritePattern<affine::AffineIfOp> {
  using OpRewritePattern<affine::AffineIfOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(affine::AffineIfOp affineOp,
                                PatternRewriter &rewriter) const override {
    SmallVector<Value, 4> mapOperands(affineOp.mapOperands());
    auto map = affineOp.map();
    auto prevMap = map;
    fully2ComposeAffineMapAndOperands(&map, &mapOperands);
    affine::canonicalizeMapAndOperands(&map, &mapOperands);
    map = removeDuplicateExprs(map);
    map = recreateExpr(map);
    if (map == prevMap)
      return failure();
    rewriter.replaceOpWithNewOp<affine::AffineApplyOp>(affineOp, map,
mapOperands); return success();
  }
};
*/

bool isValidIndex(Value val) {
  if (val.getDefiningOp<affine::AffineApplyOp>())
    return true;

  if (isValidSymbolInt(val))
    return true;

  if (auto cast = val.getDefiningOp<IndexCastOp>())
    return isValidIndex(cast.getOperand());

  if (auto cast = val.getDefiningOp<IndexCastUIOp>())
    return isValidIndex(cast.getOperand());

  if (auto cast = val.getDefiningOp<TruncIOp>())
    return isValidIndex(cast.getOperand());

  if (auto cast = val.getDefiningOp<ExtSIOp>())
    return isValidIndex(cast.getOperand());

  if (auto cast = val.getDefiningOp<ExtUIOp>())
    return isValidIndex(cast.getOperand());

  if (auto bop = val.getDefiningOp<AddIOp>())
    return isValidIndex(bop.getOperand(0)) && isValidIndex(bop.getOperand(1));

  if (auto bop = val.getDefiningOp<MulIOp>())
    return (isValidIndex(bop.getOperand(0)) &&
            isValidSymbolInt(bop.getOperand(1))) ||
           (isValidIndex(bop.getOperand(1)) &&
            isValidSymbolInt(bop.getOperand(0)));

  if (auto bop = val.getDefiningOp<DivSIOp>())
    return (isValidIndex(bop.getOperand(0)) &&
            isValidSymbolInt(bop.getOperand(1)));

  if (auto bop = val.getDefiningOp<DivUIOp>())
    return (isValidIndex(bop.getOperand(0)) &&
            isValidSymbolInt(bop.getOperand(1)));

  if (auto bop = val.getDefiningOp<RemSIOp>()) {
    return (isValidIndex(bop.getOperand(0)) &&
            bop.getOperand(1).getDefiningOp<arith::ConstantOp>());
  }

  if (auto bop = val.getDefiningOp<RemUIOp>())
    return (isValidIndex(bop.getOperand(0)) &&
            bop.getOperand(1).getDefiningOp<arith::ConstantOp>());

  if (auto bop = val.getDefiningOp<SubIOp>())
    return isValidIndex(bop.getOperand(0)) && isValidIndex(bop.getOperand(1));

  if (auto bop = val.getDefiningOp<ShRUIOp>()) {
    return (isValidIndex(bop.getOperand(0)) &&
            bop.getOperand(1).getDefiningOp<arith::ConstantOp>());
  }

  if (auto bop = val.getDefiningOp<ShLIOp>()) {
    return (isValidIndex(bop.getOperand(0)) &&
            bop.getOperand(1).getDefiningOp<arith::ConstantOp>());
  }

  if (val.getDefiningOp<ConstantIndexOp>())
    return true;

  if (val.getDefiningOp<ConstantIntOp>())
    return true;

  if (auto ba = dyn_cast<BlockArgument>(val)) {
    auto *owner = ba.getOwner();
    assert(owner);

    auto *parentOp = owner->getParentOp();
    if (!parentOp) {
      owner->dump();
      llvm::errs() << " ba: " << ba << "\n";
    }
    assert(parentOp);
    if (isa<FunctionOpInterface>(parentOp))
      return true;
    if (auto af = dyn_cast<affine::AffineForOp>(parentOp))
      return af.getInductionVar() == ba;

    // TODO ensure not a reduced var
    if (isa<affine::AffineParallelOp>(parentOp))
      return true;

    if (isa<FunctionOpInterface>(parentOp))
      return true;
  }

  LLVM_DEBUG(llvm::dbgs() << "illegal isValidIndex: " << val << "\n");
  return false;
}

// returns legality
bool handleMinMax(Value start, SmallVectorImpl<Value> &out, bool &min,
                  bool &max) {

  SmallVector<Value> todo = {start};
  while (todo.size()) {
    auto cur = todo.back();
    todo.pop_back();
    if (isValidIndex(cur)) {
      out.push_back(cur);
      continue;
    } else if (auto selOp = cur.getDefiningOp<SelectOp>()) {
      // UB only has min of operands
      if (auto cmp = selOp.getCondition().getDefiningOp<CmpIOp>()) {
        if (cmp.getLhs() == selOp.getTrueValue() &&
            cmp.getRhs() == selOp.getFalseValue()) {
          todo.push_back(cmp.getLhs());
          todo.push_back(cmp.getRhs());
          if (cmp.getPredicate() == CmpIPredicate::sle ||
              cmp.getPredicate() == CmpIPredicate::slt) {
            min = true;
            continue;
          }
          if (cmp.getPredicate() == CmpIPredicate::sge ||
              cmp.getPredicate() == CmpIPredicate::sgt) {
            max = true;
            continue;
          }
        }
      }
    }
    return false;
  }
  return !(min && max);
}

bool handle(PatternRewriter &b, AffineIfOp ifOp, size_t idx,
            SmallVectorImpl<AffineExpr> &exprs, SmallVectorImpl<bool> &eqflags,
            SmallVectorImpl<ValueOrInt> &applies, bool negated) {

  auto tval =
      cast<AffineYieldOp>(ifOp.getThenBlock()->getTerminator()).getOperand(idx);
  auto fval =
      cast<AffineYieldOp>(ifOp.getThenBlock()->getTerminator()).getOperand(idx);
  if (!negated && matchPattern(tval, m_One()) && matchPattern(fval, m_Zero())) {
    auto iset = ifOp.getCondition();
    for (auto expr : iset.getConstraints()) {
      exprs.push_back(expr.shiftSymbols(iset.getNumSymbols(), applies.size()));
    }
    for (auto eq : iset.getEqFlags()) {
      eqflags.push_back(eq);
    }
    for (auto op : ifOp.getOperands()) {
      applies.emplace_back(op);
    }
    return true;
  }

  LLVM_DEBUG(llvm::dbgs() << "illegal handle cmp: " << ifOp << " - idx: " << idx
                          << "\n");
  return false;
}

bool handle(PatternRewriter &b, CmpIOp cmpi, SmallVectorImpl<AffineExpr> &exprs,
            SmallVectorImpl<bool> &eqflags,
            SmallVectorImpl<ValueOrInt> &applies, bool negated) {
  SmallVector<Value> lhs0;
  bool lhs_min = false;
  bool lhs_max = false;
  if (!handleMinMax(cmpi.getLhs(), lhs0, lhs_min, lhs_max)) {
    LLVM_DEBUG(llvm::dbgs() << "illegal lhs minmax: " << cmpi.getLhs() << " - "
                            << cmpi << "\n");
    return false;
  }
  assert(lhs0.size());
  SmallVector<Value> rhs0;
  bool rhs_min = false;
  bool rhs_max = false;
  if (!handleMinMax(cmpi.getRhs(), rhs0, rhs_min, rhs_max)) {
    LLVM_DEBUG(llvm::dbgs() << "illegal rhs minmax: " << cmpi.getRhs() << " - "
                            << cmpi << "\n");
    return false;
  }
  assert(rhs0.size());

  SmallVector<ValueOrInt> lhs;
  for (auto v : lhs0) {
    lhs.emplace_back(v);
  }
  SmallVector<ValueOrInt> rhs;
  for (auto v : rhs0) {
    rhs.emplace_back(v);
  }

  auto pred = cmpi.getPredicate();
  if (negated) {
    pred = arith::invertPredicate(pred);
  }

  if (lhs.size() == 1 && !lhs[0].isValue) {
    auto tmp = lhs;
    lhs = rhs;
    rhs = tmp;
    pred = swapPredicate(pred);
  }

  if (rhs.size() == 1 && !rhs[0].isValue && rhs[0] == 1) {
    switch (pred) {
    default:;
      break;
    // a u< 1 -> a == 0
    case CmpIPredicate::ult:
      rhs[0].i_val = 0;
      pred = CmpIPredicate::eq;
      break;
    // a u>= 1 -> a != 0
    case CmpIPredicate::uge:
      rhs[0].i_val = 0;
      pred = CmpIPredicate::ne;
      break;
    }
  }

  switch (pred) {
  case CmpIPredicate::eq: {
    if (lhs_min || lhs_max || rhs_min || rhs_max)
      return false;
    eqflags.push_back(true);

    applies.push_back(lhs[0]);
    applies.push_back(rhs[0]);
    AffineExpr dims[2] = {b.getAffineSymbolExpr(2 * exprs.size() + 0),
                          b.getAffineSymbolExpr(2 * exprs.size() + 1)};
    exprs.push_back(dims[0] - dims[1]);
  } break;

  case CmpIPredicate::ugt:
  case CmpIPredicate::uge:
    for (auto lhspack : lhs)
      if (!valueCmp(Cmp::GE, lhspack, 0)) {
        if (!lhspack.isValue) {
          auto ival = lhspack.i_val;
          assert(ival.isNegative());
          assert(ival.isSingleWord());
          // Via Alive2: https://alive2.llvm.org/ce/z/5Fk78i
          //
          // if lhs >= 0, (as checked from above)
          // then this is correct with signed vs unsigned so long as the rhs !=
          // just the sign bit.
          if (ival.isMinSignedValue()) {
            LLVM_DEBUG(llvm::dbgs() << "illegal const greater lhs icmp: "
                                    << cmpi << " - " << ival << "\n");
            return false;
          }
        } else {
          LLVM_DEBUG(llvm::dbgs() << "illegal greater lhs icmp: " << cmpi
                                  << " - " << lhspack.v_val << "\n");
          return false;
        }
      }
    for (auto &rhspack : rhs)
      if (!valueCmp(Cmp::GE, rhspack, 0)) {
        if (!rhspack.isValue) {
          auto ival = rhspack.i_val;
          assert(ival.isNegative());
          assert(ival.isSingleWord());
          // Via Alive2: https://alive2.llvm.org/ce/z/5Fk78i
          //
          // if lhs >= 0, (as checked from above)
          // then this is correct with signed vs unsigned so long as the rhs !=
          // just the sign bit.
          if (ival.isMinSignedValue()) {
            LLVM_DEBUG(llvm::dbgs() << "illegal const greater rhs icmp: "
                                    << cmpi << " - " << ival << "\n");
            return false;
          }
        } else {
          LLVM_DEBUG(llvm::dbgs() << "illegal greater rhs icmp: " << cmpi
                                  << " - " << rhspack.v_val << "\n");
          return false;
        }
      }
    LLVM_FALLTHROUGH;

  case CmpIPredicate::sge:
  case CmpIPredicate::sgt: {
    // if lhs >=? rhs
    // if lhs is a min(a, b) both must be true and this is fine
    // if lhs is a max(a, b) either may be true, and sets require and
    // similarly if rhs is a max(), both must be true;
    if (lhs_max || rhs_min)
      return false;
    for (auto lhspack : lhs)
      for (auto rhspack : rhs) {
        eqflags.push_back(false);
        applies.push_back(lhspack);
        applies.push_back(rhspack);
        AffineExpr dims[2] = {b.getAffineSymbolExpr(2 * exprs.size() + 0),
                              b.getAffineSymbolExpr(2 * exprs.size() + 1)};
        auto expr = dims[0] - dims[1];
        if (pred == CmpIPredicate::sgt || pred == CmpIPredicate::ugt)
          expr = expr - 1;
        exprs.push_back(expr);
      }
  } break;

  case CmpIPredicate::ult:
  case CmpIPredicate::ule:
    for (auto lhspack : lhs) {
      if (!valueCmp(Cmp::GE, lhspack, 0)) {
        // Assuming the rhs is strictly positive, even if the lhs is non
        // positive, we can add this as an additional check, that lhs >= 0.
        // Therefore lhs unsigned< rhs -> lhs signed< rhs && lhs >= 0
        eqflags.push_back(false);
        applies.push_back(lhspack);
        applies.push_back(lhspack);
        AffineExpr expr = b.getAffineSymbolExpr(2 * exprs.size() + 0);
        exprs.push_back(expr);
      }
    }
    for (auto rhspack : rhs)
      if (!valueCmp(Cmp::GE, rhspack, 0)) {
        if (rhspack.isValue)
          LLVM_DEBUG(llvm::dbgs() << "illegal less rhs icmp: " << cmpi << " - "
                                  << rhspack.v_val << "\n");
        else
          LLVM_DEBUG(llvm::dbgs() << "illegal less rhs icmp: " << cmpi << " - "
                                  << rhspack.i_val << "\n");
        return false;
      }

    LLVM_FALLTHROUGH;
  case CmpIPredicate::slt:
  case CmpIPredicate::sle: {
    if (lhs_min || rhs_max)
      return false;
    for (auto lhspack : lhs)
      for (auto rhspack : rhs) {
        eqflags.push_back(false);
        applies.push_back(lhspack);
        applies.push_back(rhspack);
        AffineExpr dims[2] = {b.getAffineSymbolExpr(2 * exprs.size() + 0),
                              b.getAffineSymbolExpr(2 * exprs.size() + 1)};
        auto expr = dims[1] - dims[0];
        if (pred == CmpIPredicate::slt || pred == CmpIPredicate::ult)
          expr = expr - 1;
        exprs.push_back(expr);
      }
  } break;

  case CmpIPredicate::ne: {
    if (rhs.size() == 1 && !rhs[0].isValue && rhs[0] == 0) {
      bool legal = true;
      for (auto lhspack : lhs) {
        bool atLeastZero = false;
        if (valueCmp(Cmp::GE, lhspack, 0))
          atLeastZero = true;
        else if (lhspack.isValue) {
          AffineExpr exprTmp[] = {b.getAffineSymbolExpr(0)};
          auto mapTmp = AffineMap::get(/*dimCount=*/0, /*symbolCount=*/1,
                                       exprTmp, b.getContext());
          SmallVector<Value> tmp = {lhspack.v_val};

          fully2ComposeAffineMapAndOperands(nullptr, &mapTmp, &tmp, nullptr);
          mapTmp = recreateExpr(mapTmp);
          if (valueCmp(Cmp::GE, mapTmp.getResult(0), mapTmp.getNumDims(), tmp,
                       0)) {
            atLeastZero = true;
          } else {
            LLVM_DEBUG(llvm::dbgs()
                       << "illegal icmp ne lhs is not at least zero: "
                       << lhspack.v_val << "\n");
            LLVM_DEBUG(llvm::dbgs() << "simplified map: " << mapTmp << "\n");
          }
        } else {
          LLVM_DEBUG(llvm::dbgs()
                     << "illegal icmp ne lhs is not at least zero: "
                     << lhspack.i_val << "\n");
        }
        if (!atLeastZero) {
          legal = false;
          break;
        }
        eqflags.push_back(false);
        applies.push_back(lhspack);
        applies.push_back(lhspack);
        AffineExpr expr = b.getAffineSymbolExpr(2 * exprs.size() + 0);
        exprs.push_back(expr - 1);
      }
      if (legal)
        return true;
    }
    LLVM_DEBUG(llvm::dbgs() << "illegal icmp ne: " << cmpi << "\n");
    return false;
  }
  }
  return true;
}
/*
static void replaceStore(memref::StoreOp store,
                         const SmallVector<Value, 2> &newIndexes) {
  auto memrefType = store.getMemRef().getType().cast<MemRefType>();
  size_t rank = memrefType.getRank();
  if (rank != newIndexes.size()) {
    llvm::errs() << store << "\n";
  }
  assert(rank == newIndexes.size() && "Expect rank to match new indexes");

  PatternRewriter builder(store);
  Location loc = store.getLoc();
  builder.create<affine::AffineStoreOp>(loc, store.getValueToStore(),
store.getMemRef(), newIndexes); store.erase();
}

static void replaceLoad(memref::LoadOp load,
                        const SmallVector<Value, 2> &newIndexes) {
  PatternRewriter builder(load);
  Location loc = load.getLoc();

  auto memrefType = load.getMemRef().getType().cast<MemRefType>();
  size_t rank = memrefType.getRank();
  if (rank != newIndexes.size()) {
    llvm::errs() << load << "\n";
  }
  assert(rank == newIndexes.size() && "rank must equal new indexes size");

  affine::AffineLoadOp affineLoad =
      builder.create<affine::AffineLoadOp>(loc, load.getMemRef(), newIndexes);
  load.getResult().replaceAllUsesWith(affineLoad.getResult());
  load.erase();
}
*/
struct MoveLoadToAffine : public OpRewritePattern<memref::LoadOp> {
  using OpRewritePattern<memref::LoadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(memref::LoadOp load,
                                PatternRewriter &rewriter) const override {
    for (auto idx : load.getIndices()) {
      if (!isValidIndex(idx)) {
        return failure();
      }
    }

    auto memrefType = cast<MemRefType>(load.getMemRef().getType());
    int64_t rank = memrefType.getRank();

    // Create identity map for memrefs with at least one dimension or () -> ()
    // for zero-dimensional memrefs.
    SmallVector<AffineExpr, 4> dimExprs;
    dimExprs.reserve(rank);
    for (unsigned i = 0; i < rank; ++i)
      dimExprs.push_back(rewriter.getAffineSymbolExpr(i));
    auto map = AffineMap::get(/*dimCount=*/0, /*symbolCount=*/rank, dimExprs,
                              rewriter.getContext());

    SmallVector<Value, 4> operands = load.getIndices();

    if (map.getNumInputs() != operands.size()) {
      // load->getParentOfType<FuncOp>().dump();
      llvm::errs() << " load: " << load << "\n";
    }
    auto *scope = getLocalAffineScope(load)->getParentOp();
    DominanceInfo DI(scope);
    assert(map.getNumInputs() == operands.size());
    fully2ComposeAffineMapAndOperands(rewriter, &map, &operands, DI);
    assert(map.getNumInputs() == operands.size());
    affine::canonicalizeMapAndOperands(&map, &operands);
    map = recreateExpr(map);
    assert(map.getNumInputs() == operands.size());

    affine::AffineLoadOp affineLoad = rewriter.create<affine::AffineLoadOp>(
        load.getLoc(), load.getMemRef(), map, operands);
    load.getResult().replaceAllUsesWith(affineLoad.getResult());
    rewriter.eraseOp(load);
    return success();
  }
};

struct MoveStoreToAffine : public OpRewritePattern<memref::StoreOp> {
  using OpRewritePattern<memref::StoreOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(memref::StoreOp store,
                                PatternRewriter &rewriter) const override {
    if (!llvm::all_of(store.getIndices(), isValidIndex))
      return failure();

    auto memrefType = cast<MemRefType>(store.getMemRef().getType());
    int64_t rank = memrefType.getRank();

    // Create identity map for memrefs with at least one dimension or () -> ()
    // for zero-dimensional memrefs.
    SmallVector<AffineExpr, 4> dimExprs;
    dimExprs.reserve(rank);
    for (unsigned i = 0; i < rank; ++i)
      dimExprs.push_back(rewriter.getAffineSymbolExpr(i));
    auto map = AffineMap::get(/*dimCount=*/0, /*symbolCount=*/rank, dimExprs,
                              rewriter.getContext());
    SmallVector<Value, 4> operands = store.getIndices();

    auto *scope = getLocalAffineScope(store)->getParentOp();
    DominanceInfo DI(scope);

    fully2ComposeAffineMapAndOperands(rewriter, &map, &operands, DI);
    affine::canonicalizeMapAndOperands(&map, &operands);
    map = recreateExpr(map);

    rewriter.create<affine::AffineStoreOp>(store.getLoc(),
                                           store.getValueToStore(),
                                           store.getMemRef(), map, operands);
    rewriter.eraseOp(store);
    return success();
  }
};

static bool areChanged(SmallVectorImpl<Value> &afterOperands,
                       SmallVectorImpl<Value> &beforeOperands) {
  if (afterOperands.size() != beforeOperands.size())
    return true;
  if (!std::equal(afterOperands.begin(), afterOperands.end(),
                  beforeOperands.begin()))
    return true;
  return false;
}

template <typename T> struct AffineFixup : public OpRewritePattern<T> {
  using OpRewritePattern<T>::OpRewritePattern;

  /// Replace the affine op with another instance of it with the supplied
  /// map and mapOperands.
  void replaceAffineOp(PatternRewriter &rewriter, T affineOp, AffineMap map,
                       ArrayRef<Value> mapOperands) const;

  LogicalResult matchAndRewrite(T op,
                                PatternRewriter &rewriter) const override {
    auto map = op.getAffineMap();
    SmallVector<Value, 4> operands = op.getMapOperands();

    auto prevMap = map;
    auto prevOperands = operands;

    auto *scope = getLocalAffineScope(op)->getParentOp();
    DominanceInfo DI(scope);

    assert(map.getNumInputs() == operands.size());
    fully2ComposeAffineMapAndOperands(rewriter, &map, &operands, DI);
    assert(map.getNumInputs() == operands.size());
    affine::canonicalizeMapAndOperands(&map, &operands);
    assert(map.getNumInputs() == operands.size());
    map = recreateExpr(map);

    if (map == prevMap && !areChanged(operands, prevOperands))
      return failure();

    replaceAffineOp(rewriter, op, map, operands);
    return success();
  }
};

// Specialize the template to account for the different build signatures for
// affine load, store, and apply ops.
template <>
void AffineFixup<affine::AffineLoadOp>::replaceAffineOp(
    PatternRewriter &rewriter, affine::AffineLoadOp load, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<affine::AffineLoadOp>(load, load.getMemRef(), map,
                                                    mapOperands);
}
template <>
void AffineFixup<affine::AffinePrefetchOp>::replaceAffineOp(
    PatternRewriter &rewriter, affine::AffinePrefetchOp prefetch, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<affine::AffinePrefetchOp>(
      prefetch, prefetch.getMemref(), map, mapOperands,
      prefetch.getLocalityHint(), prefetch.getIsWrite(),
      prefetch.getIsDataCache());
}
template <>
void AffineFixup<affine::AffineStoreOp>::replaceAffineOp(
    PatternRewriter &rewriter, affine::AffineStoreOp store, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<affine::AffineStoreOp>(
      store, store.getValueToStore(), store.getMemRef(), map, mapOperands);
}
template <>
void AffineFixup<affine::AffineVectorLoadOp>::replaceAffineOp(
    PatternRewriter &rewriter, affine::AffineVectorLoadOp vectorload,
    AffineMap map, ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<affine::AffineVectorLoadOp>(
      vectorload, vectorload.getVectorType(), vectorload.getMemRef(), map,
      mapOperands);
}
template <>
void AffineFixup<affine::AffineVectorStoreOp>::replaceAffineOp(
    PatternRewriter &rewriter, affine::AffineVectorStoreOp vectorstore,
    AffineMap map, ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<affine::AffineVectorStoreOp>(
      vectorstore, vectorstore.getValueToStore(), vectorstore.getMemRef(), map,
      mapOperands);
}

// Generic version for ops that don't have extra operands.
template <typename AffineOpTy>
void AffineFixup<AffineOpTy>::replaceAffineOp(
    PatternRewriter &rewriter, AffineOpTy op, AffineMap map,
    ArrayRef<Value> mapOperands) const {
  rewriter.replaceOpWithNewOp<AffineOpTy>(op, map, mapOperands);
}

struct CanonicalieForBounds : public OpRewritePattern<affine::AffineForOp> {
  using OpRewritePattern<affine::AffineForOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(affine::AffineForOp forOp,
                                PatternRewriter &rewriter) const override {
    SmallVector<Value, 4> lbOperands(forOp.getLowerBoundOperands());
    SmallVector<Value, 4> ubOperands(forOp.getUpperBoundOperands());
    SmallVector<Value, 4> origLbOperands(forOp.getLowerBoundOperands());
    SmallVector<Value, 4> origUbOperands(forOp.getUpperBoundOperands());

    auto lbMap = forOp.getLowerBoundMap();
    auto ubMap = forOp.getUpperBoundMap();
    auto prevLbMap = lbMap;
    auto prevUbMap = ubMap;

    // llvm::errs() << "*********\n";
    // ubMap.dump();

    auto *scope = getLocalAffineScope(forOp)->getParentOp();
    DominanceInfo DI(scope);

    fully2ComposeAffineMapAndOperands(rewriter, &lbMap, &lbOperands, DI);
    affine::canonicalizeMapAndOperands(&lbMap, &lbOperands);
    lbMap = removeDuplicateExprs(lbMap);
    lbMap = recreateExpr(lbMap);

    fully2ComposeAffineMapAndOperands(rewriter, &ubMap, &ubOperands, DI);
    affine::canonicalizeMapAndOperands(&ubMap, &ubOperands);
    ubMap = removeDuplicateExprs(ubMap);
    ubMap = recreateExpr(ubMap);

    // ubMap.dump();
    // forOp.dump();

    // Any canonicalization change in map or operands always leads to updated
    // map(s).
    if ((lbMap == prevLbMap && ubMap == prevUbMap) &&
        (!areChanged(lbOperands, origLbOperands)) &&
        (!areChanged(ubOperands, origUbOperands)))
      return failure();

    // llvm::errs() << "oldParent:" << *forOp.getParentOp() << "\n";
    // llvm::errs() << "oldfor:" << forOp << "\n";

    if ((lbMap != prevLbMap) || areChanged(lbOperands, origLbOperands))
      forOp.setLowerBound(lbOperands, lbMap);
    if ((ubMap != prevUbMap) || areChanged(ubOperands, origUbOperands))
      forOp.setUpperBound(ubOperands, ubMap);

    // llvm::errs() << "newfor:" << forOp << "\n";
    return success();
  }
};

struct CanonicalizIfBounds : public OpRewritePattern<affine::AffineIfOp> {
  using OpRewritePattern<affine::AffineIfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(affine::AffineIfOp op,
                                PatternRewriter &rewriter) const override {
    SmallVector<Value, 4> operands(op.getOperands());
    SmallVector<Value, 4> origOperands(operands);

    auto map = op.getIntegerSet();
    auto prevMap = map;

    // llvm::errs() << "*********\n";
    // ubMap.dump();

    auto *scope = getLocalAffineScope(op)->getParentOp();
    DominanceInfo DI(scope);

    fully2ComposeIntegerSetAndOperands(rewriter, &map, &operands, DI);
    affine::canonicalizeSetAndOperands(&map, &operands);
    map = recreateExpr(map);

    // map(s).
    if (map == prevMap && !areChanged(operands, origOperands))
      return failure();

    op.setConditional(map, operands);

    return success();
  }
};

struct MoveIfToAffine : public OpRewritePattern<scf::IfOp> {
  using OpRewritePattern<scf::IfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(scf::IfOp ifOp,
                                PatternRewriter &rewriter) const override {
    if (!ifOp->getParentOfType<affine::AffineForOp>() &&
        !ifOp->getParentOfType<affine::AffineParallelOp>())
      return failure();

    std::vector<mlir::Type> types;
    for (auto v : ifOp.getResults()) {
      types.push_back(v.getType());
    }

    for (auto tryNegate : {false, true}) {
      SmallVector<AffineExpr, 2> exprs;
      SmallVector<bool, 2> eqflags;
      SmallVector<ValueOrInt, 4> applies;

      // condition, Negated
      std::deque<std::pair<Value, bool>> todo = {
          std::make_pair(ifOp.getCondition(), tryNegate)};
      bool legal = true;
      while (todo.size()) {
        auto &&[cur, negated] = todo.front();
        todo.pop_front();
        if (auto cmpi = cur.getDefiningOp<CmpIOp>()) {
          if (!handle(rewriter, cmpi, exprs, eqflags, applies, negated)) {
            legal = false;
            break;
          }
          continue;
        }
        if (!negated) {
          if (auto andi = cur.getDefiningOp<AndIOp>()) {
            todo.emplace_back(andi.getOperand(0), negated);
            todo.emplace_back(andi.getOperand(1), negated);
            continue;
          }
        }
        if (negated) {
          if (auto andi = cur.getDefiningOp<OrIOp>()) {
            todo.emplace_back(andi.getOperand(0), negated);
            todo.emplace_back(andi.getOperand(1), negated);
            continue;
          }
        }

        if (auto noti = cur.getDefiningOp<XOrIOp>()) {
          if (matchPattern(noti.getOperand(1), m_One())) {
            todo.emplace_back(noti.getOperand(0), !negated);
            continue;
          }
        }
        if (auto ifOp = cur.getDefiningOp<affine::AffineIfOp>()) {
          auto idx = cast<OpResult>(cur).getResultNumber();
          if (!handle(rewriter, ifOp, idx, exprs, eqflags, applies, negated)) {
            legal = false;
            break;
          }
          continue;
        }
        LLVM_DEBUG(llvm::dbgs() << "illegal condition: " << cur
                                << " - negated: " << negated << "\n");
        legal = false;
        break;
      }
      if (!legal)
        continue;

      SmallVector<Value> operands;
      auto ity = IndexType::get(ifOp.getContext());
      for (auto vori : applies) {
        Value operand = vori.v_val;
        if (!vori.isValue) {
          operand = rewriter.create<arith::ConstantIndexOp>(
              ifOp.getLoc(), vori.i_val.getSExtValue());
        }
        if (!isa<IndexType>(operand.getType())) {
          operand =
              rewriter.create<arith::IndexCastOp>(ifOp.getLoc(), ity, operand);
        }
        operands.push_back(operand);
      }

      auto *scope = getLocalAffineScope(ifOp)->getParentOp();
      DominanceInfo DI(scope);

      auto iset = IntegerSet::get(/*dim*/ 0, /*symbol*/ 2 * exprs.size(), exprs,
                                  eqflags);
      fully2ComposeIntegerSetAndOperands(rewriter, &iset, &operands, DI);
      affine::canonicalizeSetAndOperands(&iset, &operands);
      affine::AffineIfOp affineIfOp = rewriter.create<affine::AffineIfOp>(
          ifOp.getLoc(), types, iset, operands,
          /*elseBlock=*/true);

      rewriter.setInsertionPoint(ifOp.thenYield());
      rewriter.replaceOpWithNewOp<affine::AffineYieldOp>(
          ifOp.thenYield(), ifOp.thenYield().getOperands());

      rewriter.eraseBlock(affineIfOp.getThenBlock());
      rewriter.eraseBlock(affineIfOp.getElseBlock());
      if (ifOp.getElseRegion().getBlocks().size()) {
        rewriter.setInsertionPoint(ifOp.elseYield());
        rewriter.replaceOpWithNewOp<affine::AffineYieldOp>(
            ifOp.elseYield(), ifOp.elseYield().getOperands());
      }

      if (!tryNegate) {
        rewriter.inlineRegionBefore(ifOp.getThenRegion(),
                                    affineIfOp.getThenRegion(),
                                    affineIfOp.getThenRegion().begin());
        rewriter.inlineRegionBefore(ifOp.getElseRegion(),
                                    affineIfOp.getElseRegion(),
                                    affineIfOp.getElseRegion().begin());
      } else {
        if (ifOp.getElseRegion().empty()) {
          rewriter.createBlock(&affineIfOp.getThenRegion());
          rewriter.create<affine::AffineYieldOp>(ifOp.getLoc());
        } else {
          rewriter.inlineRegionBefore(ifOp.getElseRegion(),
                                      affineIfOp.getThenRegion(),
                                      affineIfOp.getThenRegion().begin());
        }
        rewriter.inlineRegionBefore(ifOp.getThenRegion(),
                                    affineIfOp.getElseRegion(),
                                    affineIfOp.getElseRegion().begin());
      }
      rewriter.replaceOp(ifOp, affineIfOp.getResults());
      return success();
    }
    return failure();
  }
};

struct MoveExtToAffine : public OpRewritePattern<arith::ExtUIOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(arith::ExtUIOp ifOp,
                                PatternRewriter &rewriter) const override {
    if (!ifOp->getParentOfType<affine::AffineForOp>() &&
        !ifOp->getParentOfType<affine::AffineParallelOp>())
      return failure();

    if (!ifOp.getOperand().getType().isInteger(1))
      return failure();

    std::vector<mlir::Type> types = {ifOp.getType()};

    for (int i = 0; i < 2; i++) {
      SmallVector<AffineExpr, 2> exprs;
      SmallVector<bool, 2> eqflags;
      SmallVector<ValueOrInt, 4> applies;

      // condition, Negated
      std::deque<std::pair<Value, bool>> todo = {
          std::make_pair(ifOp.getOperand(), i == 1)};
      bool badcmp = false;
      while (todo.size()) {
        auto &&[cur, negated] = todo.front();
        todo.pop_front();
        if (auto cmpi = cur.getDefiningOp<CmpIOp>()) {
          if (!handle(rewriter, cmpi, exprs, eqflags, applies, negated)) {
            badcmp = true;
            break;
          }
          continue;
        }
        if (!negated) {
          if (auto andi = cur.getDefiningOp<AndIOp>()) {
            todo.emplace_back(andi.getOperand(0), negated);
            todo.emplace_back(andi.getOperand(1), negated);
            continue;
          }
        }
        if (negated) {
          if (auto andi = cur.getDefiningOp<OrIOp>()) {
            todo.emplace_back(andi.getOperand(0), negated);
            todo.emplace_back(andi.getOperand(1), negated);
            continue;
          }
        }

        if (auto noti = cur.getDefiningOp<XOrIOp>()) {
          if (matchPattern(noti.getOperand(1), m_One())) {
            todo.emplace_back(noti.getOperand(0), !negated);
            continue;
          }
        }
        if (auto ifOp = cur.getDefiningOp<affine::AffineIfOp>()) {
          auto idx = cast<OpResult>(cur).getResultNumber();
          if (!handle(rewriter, ifOp, idx, exprs, eqflags, applies, negated)) {
            badcmp = true;
            break;
          }
          continue;
        }
        LLVM_DEBUG(llvm::dbgs() << "illegal condition: " << cur
                                << " - negated: " << negated << "\n");
        badcmp = true;
        break;
      }
      if (badcmp)
        continue;

      SmallVector<Value> operands;
      auto ity = IndexType::get(ifOp.getContext());
      for (auto vori : applies) {
        Value operand = vori.v_val;
        if (!vori.isValue) {
          operand = rewriter.create<arith::ConstantIndexOp>(
              ifOp.getLoc(), vori.i_val.getSExtValue());
        }
        if (!isa<IndexType>(operand.getType())) {
          operand =
              rewriter.create<arith::IndexCastOp>(ifOp.getLoc(), ity, operand);
        }
        operands.push_back(operand);
      }

      auto *scope = getLocalAffineScope(ifOp)->getParentOp();
      DominanceInfo DI(scope);

      auto iset = IntegerSet::get(/*dim*/ 0, /*symbol*/ 2 * exprs.size(), exprs,
                                  eqflags);
      fully2ComposeIntegerSetAndOperands(rewriter, &iset, &operands, DI);
      affine::canonicalizeSetAndOperands(&iset, &operands);
      Value tval[1] = {rewriter.create<arith::ConstantIntOp>(
          ifOp.getLoc(), ifOp.getType(), 1)};
      Value fval[1] = {rewriter.create<arith::ConstantIntOp>(
          ifOp.getLoc(), ifOp.getType(), 0)};
      affine::AffineIfOp affineIfOp = rewriter.create<affine::AffineIfOp>(
          ifOp.getLoc(), types, iset, operands,
          /*elseBlock=*/true);

      rewriter.setInsertionPointToEnd(affineIfOp.getThenBlock());
      rewriter.create<affine::AffineYieldOp>(ifOp.getLoc(),
                                             i == 0 ? tval : fval);

      rewriter.setInsertionPointToEnd(affineIfOp.getElseBlock());
      rewriter.create<affine::AffineYieldOp>(ifOp.getLoc(),
                                             i == 0 ? fval : tval);

      rewriter.replaceOp(ifOp, affineIfOp);
      return success();
    }
    return failure();
  }
};

struct MoveSIToFPToAffine : public OpRewritePattern<arith::SIToFPOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(arith::SIToFPOp ifOp,
                                PatternRewriter &rewriter) const override {
    if (!ifOp->getParentOfType<affine::AffineForOp>() &&
        !ifOp->getParentOfType<affine::AffineParallelOp>())
      return failure();

    auto defop = ifOp.getOperand().getDefiningOp();
    if (!defop)
      return failure();
    if (isa<arith::IndexCastOp, arith::IndexCastUIOp>(defop))
      return failure();

    if (!isValidIndex(ifOp.getOperand()))
      return failure();

    SmallVector<AffineExpr, 1> dimExprs;
    dimExprs.push_back(rewriter.getAffineSymbolExpr(0));
    auto map = AffineMap::get(/*dimCount=*/0, /*symbolCount=*/1, dimExprs,
                              rewriter.getContext());
    SmallVector<Value, 1> operands = {ifOp.getOperand()};

    auto *scope = getLocalAffineScope(ifOp)->getParentOp();
    DominanceInfo DI(scope);

    fully2ComposeAffineMapAndOperands(rewriter, &map, &operands, DI);
    affine::canonicalizeMapAndOperands(&map, &operands);
    map = recreateExpr(map);

    auto app =
        rewriter.create<affine::AffineApplyOp>(ifOp.getLoc(), map, operands);

    auto cast = rewriter.create<arith::IndexCastOp>(
        ifOp.getLoc(), ifOp.getOperand().getType(), app);

    rewriter.modifyOpInPlace(ifOp, [&]() { ifOp.getInMutable().assign(cast); });
    return success();
  }
};

struct CmpExt : public OpRewritePattern<arith::CmpIOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(arith::CmpIOp cmpOp,
                                PatternRewriter &rewriter) const override {
    auto ext = cmpOp.getLhs().getDefiningOp<arith::ExtUIOp>();
    if (!ext)
      return failure();
    if (!ext.getOperand().getType().isInteger(1))
      return failure();
    if (!matchPattern(cmpOp.getRhs(), m_Zero()))
      return failure();

    // ext (i1 -> i64) == 0, !%c
    if (cmpOp.getPredicate() == arith::CmpIPredicate::eq) {
      auto tval = rewriter.create<arith::ConstantIntOp>(
          cmpOp.getLoc(), ext.getOperand().getType(), 1);
      rewriter.replaceOpWithNewOp<arith::XOrIOp>(cmpOp, ext.getOperand(), tval);
      return success();
    }
    return failure();
  }
};

struct MoveSelectToAffine : public OpRewritePattern<arith::SelectOp> {
  using OpRewritePattern<arith::SelectOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(arith::SelectOp ifOp,
                                PatternRewriter &rewriter) const override {
    if (!ifOp->getParentOfType<affine::AffineForOp>() &&
        !ifOp->getParentOfType<affine::AffineParallelOp>())
      return failure();

    std::vector<mlir::Type> types = {ifOp.getType()};

    for (int i = 0; i < 2; i++) {
      SmallVector<AffineExpr, 2> exprs;
      SmallVector<bool, 2> eqflags;
      SmallVector<ValueOrInt, 4> applies;

      // condition, Negated
      std::deque<std::pair<Value, bool>> todo = {
          std::make_pair(ifOp.getCondition(), i == 1)};
      bool badcmp = false;
      while (todo.size()) {
        auto &&[cur, negated] = todo.front();
        todo.pop_front();
        if (auto cmpi = cur.getDefiningOp<CmpIOp>()) {
          if (!handle(rewriter, cmpi, exprs, eqflags, applies, negated)) {
            badcmp = true;
            break;
          }
          continue;
        }
        if (!negated) {
          if (auto andi = cur.getDefiningOp<AndIOp>()) {
            todo.emplace_back(andi.getOperand(0), negated);
            todo.emplace_back(andi.getOperand(1), negated);
            continue;
          }
        }
        if (negated) {
          if (auto andi = cur.getDefiningOp<OrIOp>()) {
            todo.emplace_back(andi.getOperand(0), negated);
            todo.emplace_back(andi.getOperand(1), negated);
            continue;
          }
        }

        if (auto noti = cur.getDefiningOp<XOrIOp>()) {
          if (matchPattern(noti.getOperand(1), m_One())) {
            todo.emplace_back(noti.getOperand(0), !negated);
            continue;
          }
        }
        if (auto ifOp = cur.getDefiningOp<affine::AffineIfOp>()) {
          auto idx = cast<OpResult>(cur).getResultNumber();
          if (!handle(rewriter, ifOp, idx, exprs, eqflags, applies, negated)) {
            badcmp = true;
            break;
          }
          continue;
        }
        LLVM_DEBUG(llvm::dbgs() << "illegal condition: " << cur
                                << " - negated: " << negated << "\n");
        badcmp = true;
        break;
      }
      if (badcmp)
        continue;

      SmallVector<Value> operands;
      auto ity = IndexType::get(ifOp.getContext());
      for (auto vori : applies) {
        Value operand = vori.v_val;
        if (!vori.isValue) {
          operand = rewriter.create<arith::ConstantIndexOp>(
              ifOp.getLoc(), vori.i_val.getSExtValue());
        }
        if (!isa<IndexType>(operand.getType())) {
          operand =
              rewriter.create<arith::IndexCastOp>(ifOp.getLoc(), ity, operand);
        }
        operands.push_back(operand);
      }

      auto *scope = getLocalAffineScope(ifOp)->getParentOp();
      DominanceInfo DI(scope);

      auto iset = IntegerSet::get(/*dim*/ 0, /*symbol*/ 2 * exprs.size(), exprs,
                                  eqflags);
      fully2ComposeIntegerSetAndOperands(rewriter, &iset, &operands, DI);
      affine::canonicalizeSetAndOperands(&iset, &operands);
      affine::AffineIfOp affineIfOp = rewriter.create<affine::AffineIfOp>(
          ifOp.getLoc(), types, iset, operands,
          /*elseBlock=*/true);

      rewriter.setInsertionPointToEnd(affineIfOp.getThenBlock());
      rewriter.create<affine::AffineYieldOp>(
          ifOp.getLoc(), i == 0 ? ifOp.getTrueValue() : ifOp.getFalseValue());

      rewriter.setInsertionPointToEnd(affineIfOp.getElseBlock());
      rewriter.create<affine::AffineYieldOp>(
          ifOp.getLoc(), i == 0 ? ifOp.getFalseValue() : ifOp.getTrueValue());

      rewriter.replaceOp(ifOp, affineIfOp.getResults());
      return success();
    }

    bool changed = false;
    auto condOp = ifOp.getCondition().getDefiningOp();
    if (isa<AndIOp, OrIOp>(condOp)) {
      // condition, Negated

      for (auto &opv : condOp->getOpOperands()) {

        OpBuilder::InsertionGuard guard(rewriter);
        rewriter.setInsertionPoint(condOp);

        SmallVector<AffineExpr, 2> exprs;
        SmallVector<bool, 2> eqflags;
        SmallVector<ValueOrInt, 4> applies;
        if (auto midIf = opv.get().getDefiningOp<AffineIfOp>()) {
          auto idx = cast<OpResult>(opv.get()).getResultNumber();
          auto tval = cast<AffineYieldOp>(midIf.getThenBlock()->getTerminator())
                          .getOperand(idx);
          auto fval = cast<AffineYieldOp>(midIf.getThenBlock()->getTerminator())
                          .getOperand(idx);
          if (matchPattern(tval, m_One()) && matchPattern(fval, m_Zero()))
            continue;
          if (matchPattern(tval, m_Zero()) && matchPattern(fval, m_One()))
            continue;
        }
        for (int i = 0; i < 2; i++) {
          bool badcmp = false;
          std::deque<std::pair<Value, bool>> todo = {
              std::make_pair(opv.get(), i == 1)};
          while (todo.size()) {
            auto &&[cur, negated] = todo.front();
            todo.pop_front();
            if (auto cmpi = cur.getDefiningOp<CmpIOp>()) {
              if (!handle(rewriter, cmpi, exprs, eqflags, applies, negated)) {
                badcmp = true;
                break;
              }
              continue;
            }
            if (!negated) {
              if (auto andi = cur.getDefiningOp<AndIOp>()) {
                todo.emplace_back(andi.getOperand(0), negated);
                todo.emplace_back(andi.getOperand(1), negated);
                continue;
              }
            }
            if (negated) {
              if (auto andi = cur.getDefiningOp<OrIOp>()) {
                todo.emplace_back(andi.getOperand(0), negated);
                todo.emplace_back(andi.getOperand(1), negated);
                continue;
              }
            }

            if (auto noti = cur.getDefiningOp<XOrIOp>()) {
              if (matchPattern(noti.getOperand(1), m_One())) {
                todo.emplace_back(noti.getOperand(0), !negated);
                continue;
              }
            }
            if (auto ifOp = cur.getDefiningOp<affine::AffineIfOp>()) {
              auto idx = cast<OpResult>(cur).getResultNumber();
              if (!handle(rewriter, ifOp, idx, exprs, eqflags, applies,
                          negated)) {
                badcmp = true;
                break;
              }
              continue;
            }
            LLVM_DEBUG(llvm::dbgs() << "illegal condition: " << cur
                                    << " - negated: " << negated << "\n");
            badcmp = true;
            break;
          }

          if (badcmp)
            continue;

          SmallVector<Value> operands;
          auto ity = IndexType::get(ifOp.getContext());
          for (auto vori : applies) {
            Value operand = vori.v_val;
            if (!vori.isValue) {
              operand = rewriter.create<arith::ConstantIndexOp>(
                  ifOp.getLoc(), vori.i_val.getSExtValue());
            }
            if (!isa<IndexType>(operand.getType())) {
              operand = rewriter.create<arith::IndexCastOp>(ifOp.getLoc(), ity,
                                                            operand);
            }
            operands.push_back(operand);
          }

          auto *scope = getLocalAffineScope(ifOp)->getParentOp();
          DominanceInfo DI(scope);

          std::vector<mlir::Type> types = {ifOp.getCondition().getType()};

          auto iset = IntegerSet::get(/*dim*/ 0, /*symbol*/ 2 * exprs.size(),
                                      exprs, eqflags);
          fully2ComposeIntegerSetAndOperands(rewriter, &iset, &operands, DI);
          affine::canonicalizeSetAndOperands(&iset, &operands);

          Value tval[1] = {rewriter.create<arith::ConstantIntOp>(ifOp.getLoc(),
                                                                 types[0], 1)};
          Value fval[1] = {rewriter.create<arith::ConstantIntOp>(ifOp.getLoc(),
                                                                 types[0], 0)};

          affine::AffineIfOp affineIfOp = rewriter.create<affine::AffineIfOp>(
              ifOp.getLoc(), types, iset, operands,
              /*elseBlock=*/true);

          rewriter.setInsertionPointToEnd(affineIfOp.getThenBlock());
          rewriter.create<affine::AffineYieldOp>(ifOp.getLoc(),
                                                 i == 0 ? tval : fval);

          rewriter.setInsertionPointToEnd(affineIfOp.getElseBlock());
          rewriter.create<affine::AffineYieldOp>(ifOp.getLoc(),
                                                 i == 0 ? fval : tval);

          rewriter.modifyOpInPlace(
              condOp, [&] { opv.assign(affineIfOp.getResult(0)); });
          changed = true;
        }
      }
    }

    return success(changed);
  }
};

struct ForOpRaising : public OpRewritePattern<scf::ForOp> {
  using OpRewritePattern<scf::ForOp>::OpRewritePattern;

  // TODO: remove me or rename me.
  bool isAffine(scf::ForOp loop) const {
    // return true;
    // enforce step to be a ConstantIndexOp (maybe too restrictive).
    APInt apint;
    return affine::isValidSymbol(loop.getStep()) ||
           matchPattern(loop.getStep(), m_ConstantInt(&apint));
  }

  int64_t getStep(mlir::Value value) const {
    APInt apint;
    if (matchPattern(value, m_ConstantInt(&apint)))
      return apint.getZExtValue();
    else
      return 1;
  }

  AffineMap getMultiSymbolIdentity(Builder &B, unsigned rank) const {
    SmallVector<AffineExpr, 4> dimExprs;
    dimExprs.reserve(rank);
    for (unsigned i = 0; i < rank; ++i)
      dimExprs.push_back(B.getAffineSymbolExpr(i));
    return AffineMap::get(/*dimCount=*/0, /*symbolCount=*/rank, dimExprs,
                          B.getContext());
  }
  LogicalResult matchAndRewrite(scf::ForOp loop,
                                PatternRewriter &rewriter) const final {
    if (isAffine(loop)) {
      OpBuilder builder(loop);

      SmallVector<Value> lbs;
      {
        SmallVector<Value> todo = {loop.getLowerBound()};
        while (todo.size()) {
          auto cur = todo.back();
          todo.pop_back();
          if (isValidIndex(cur)) {
            lbs.push_back(cur);
            continue;
          } else if (auto selOp = cur.getDefiningOp<SelectOp>()) {
            // LB only has max of operands
            if (auto cmp = selOp.getCondition().getDefiningOp<CmpIOp>()) {
              if (cmp.getLhs() == selOp.getTrueValue() &&
                  cmp.getRhs() == selOp.getFalseValue() &&
                  cmp.getPredicate() == CmpIPredicate::sge) {
                todo.push_back(cmp.getLhs());
                todo.push_back(cmp.getRhs());
                continue;
              }
            }
          }
          return failure();
        }
      }

      SmallVector<Value> ubs;
      {
        SmallVector<Value> todo = {loop.getUpperBound()};
        while (todo.size()) {
          auto cur = todo.back();
          todo.pop_back();
          if (isValidIndex(cur)) {
            ubs.push_back(cur);
            continue;
          } else if (auto selOp = cur.getDefiningOp<SelectOp>()) {
            // UB only has min of operands
            if (auto cmp = selOp.getCondition().getDefiningOp<CmpIOp>()) {
              if (cmp.getLhs() == selOp.getTrueValue() &&
                  cmp.getRhs() == selOp.getFalseValue() &&
                  cmp.getPredicate() == CmpIPredicate::sle) {
                todo.push_back(cmp.getLhs());
                todo.push_back(cmp.getRhs());
                continue;
              }
            }
          }
          return failure();
        }
      }

      bool rewrittenStep = false;
      if (!loop.getStep().getDefiningOp<ConstantIndexOp>()) {
        if (ubs.size() != 1 || lbs.size() != 1)
          return failure();
        ubs[0] = rewriter.create<DivUIOp>(
            loop.getLoc(),
            rewriter.create<AddIOp>(
                loop.getLoc(),
                rewriter
                    .create<SubIOp>(
                        loop.getLoc(), loop.getStep(),
                        isa<IndexType>(loop.getStep().getType())
                            ? rewriter.create<ConstantIndexOp>(loop.getLoc(), 1)
                                  .getResult()
                            : rewriter.create<ConstantIntOp>(
                                  loop.getLoc(), loop.getStep().getType(), 1))
                    .getResult(),
                rewriter.create<SubIOp>(loop.getLoc(), loop.getUpperBound(),
                                        loop.getLowerBound())),
            loop.getStep());
        lbs[0] = rewriter.create<ConstantIndexOp>(loop.getLoc(), 0);
        rewrittenStep = true;
      }

      auto *scope = getLocalAffineScope(loop)->getParentOp();
      DominanceInfo DI(scope);

      AffineMap lbMap = getMultiSymbolIdentity(builder, lbs.size());
      {
        fully2ComposeAffineMapAndOperands(rewriter, &lbMap, &lbs, DI);
        affine::canonicalizeMapAndOperands(&lbMap, &lbs);
        lbMap = removeDuplicateExprs(lbMap);
        lbMap = recreateExpr(lbMap);
      }
      AffineMap ubMap = getMultiSymbolIdentity(builder, ubs.size());
      {
        fully2ComposeAffineMapAndOperands(rewriter, &ubMap, &ubs, DI);
        affine::canonicalizeMapAndOperands(&ubMap, &ubs);
        ubMap = removeDuplicateExprs(ubMap);
        ubMap = recreateExpr(ubMap);
      }

      affine::AffineForOp affineLoop = rewriter.create<affine::AffineForOp>(
          loop.getLoc(), lbs, lbMap, ubs, ubMap, getStep(loop.getStep()),
          loop.getInits());

      auto mergedYieldOp =
          cast<scf::YieldOp>(loop.getRegion().front().getTerminator());

      Block &newBlock = affineLoop.getRegion().front();

      // The terminator is added if the iterator args are not provided.
      // see the ::build method.
      if (affineLoop.getNumIterOperands() == 0) {
        auto *affineYieldOp = newBlock.getTerminator();
        rewriter.eraseOp(affineYieldOp);
      }

      SmallVector<Value> vals;
      rewriter.setInsertionPointToStart(&affineLoop.getRegion().front());
      for (Value arg : affineLoop.getRegion().front().getArguments()) {
        bool isInduction = arg == affineLoop.getInductionVar();
        if (isInduction && arg.getType() != loop.getInductionVar().getType()) {
          arg = rewriter.create<arith::IndexCastOp>(
              loop.getLoc(), loop.getInductionVar().getType(), arg);
        }
        if (rewrittenStep && isInduction) {
          arg = rewriter.create<AddIOp>(
              loop.getLoc(), loop.getLowerBound(),
              rewriter.create<MulIOp>(loop.getLoc(), arg, loop.getStep()));
        }
        vals.push_back(arg);
      }
      assert(vals.size() == loop.getRegion().front().getNumArguments());
      rewriter.mergeBlocks(&loop.getRegion().front(),
                           &affineLoop.getRegion().front(), vals);

      rewriter.setInsertionPoint(mergedYieldOp);
      rewriter.create<affine::AffineYieldOp>(mergedYieldOp.getLoc(),
                                             mergedYieldOp.getOperands());
      rewriter.eraseOp(mergedYieldOp);

      rewriter.replaceOp(loop, affineLoop.getResults());

      return success();
    }
    return failure();
  }
};

struct ParallelOpRaising : public OpRewritePattern<scf::ParallelOp> {
  using OpRewritePattern<scf::ParallelOp>::OpRewritePattern;

  void canonicalizeLoopBounds(PatternRewriter &rewriter,
                              affine::AffineParallelOp forOp) const {
    SmallVector<Value, 4> lbOperands(forOp.getLowerBoundsOperands());
    SmallVector<Value, 4> ubOperands(forOp.getUpperBoundsOperands());

    auto lbMap = forOp.getLowerBoundsMap();
    auto ubMap = forOp.getUpperBoundsMap();

    auto *scope = getLocalAffineScope(forOp)->getParentOp();
    DominanceInfo DI(scope);

    fully2ComposeAffineMapAndOperands(rewriter, &lbMap, &lbOperands, DI);
    affine::canonicalizeMapAndOperands(&lbMap, &lbOperands);
    lbMap = recreateExpr(lbMap);

    fully2ComposeAffineMapAndOperands(rewriter, &ubMap, &ubOperands, DI);
    affine::canonicalizeMapAndOperands(&ubMap, &ubOperands);
    ubMap = recreateExpr(ubMap);

    forOp.setLowerBounds(lbOperands, lbMap);
    forOp.setUpperBounds(ubOperands, ubMap);
  }

  LogicalResult matchAndRewrite(scf::ParallelOp loop,
                                PatternRewriter &rewriter) const final {
    OpBuilder builder(loop);

    if (loop.getResults().size())
      return rewriter.notifyMatchFailure(
          loop, "not dependent on a conditional result");

    if (!llvm::all_of(loop.getLowerBound(), isValidIndex)) {
      return failure();
    }

    if (!llvm::all_of(loop.getUpperBound(), isValidIndex)) {
      return failure();
    }

    SmallVector<int64_t> steps;
    for (auto step : loop.getStep())
      if (auto cst = step.getDefiningOp<ConstantIndexOp>())
        steps.push_back(cst.value());
      else
        return failure();

    ArrayRef<AtomicRMWKind> reductions;
    SmallVector<AffineMap> bounds;
    for (size_t i = 0; i < loop.getLowerBound().size(); i++)
      bounds.push_back(AffineMap::get(
          /*dimCount=*/0, /*symbolCount=*/loop.getLowerBound().size(),
          builder.getAffineSymbolExpr(i)));
    affine::AffineParallelOp affineLoop =
        rewriter.create<affine::AffineParallelOp>(
            loop.getLoc(), loop.getResultTypes(), reductions, bounds,
            loop.getLowerBound(), bounds, loop.getUpperBound(),
            steps); //, loop.getInitVals());

    canonicalizeLoopBounds(rewriter, affineLoop);

    auto mergedYieldOp =
        cast<scf::ReduceOp>(loop.getRegion().front().getTerminator());

    Block &newBlock = affineLoop.getRegion().front();

    // The terminator is added if the iterator args are not provided.
    // see the ::build method.
    if (affineLoop.getResults().size() == 0) {
      auto *affineYieldOp = newBlock.getTerminator();
      rewriter.eraseOp(affineYieldOp);
    }

    SmallVector<Value> vals;
    for (Value arg : affineLoop.getRegion().front().getArguments()) {
      vals.push_back(arg);
    }
    rewriter.mergeBlocks(&loop.getRegion().front(),
                         &affineLoop.getRegion().front(), vals);

    rewriter.setInsertionPoint(mergedYieldOp);
    rewriter.create<affine::AffineYieldOp>(mergedYieldOp.getLoc(),
                                           mergedYieldOp.getOperands());
    rewriter.eraseOp(mergedYieldOp);

    rewriter.replaceOp(loop, affineLoop.getResults());

    return success();
  }
};

static void replaceOpWithRegion(PatternRewriter &rewriter, Operation *op,
                                Region &region, ValueRange blockArgs = {}) {
  assert(llvm::hasSingleElement(region) && "expected single-region block");
  Block *block = &region.front();
  Operation *terminator = block->getTerminator();
  ValueRange results = terminator->getOperands();
  rewriter.inlineBlockBefore(block, op, blockArgs);
  rewriter.replaceOp(op, results);
  rewriter.eraseOp(terminator);
}

struct AffineIfSimplificationIsl : public OpRewritePattern<affine::AffineIfOp> {
  using OpRewritePattern<affine::AffineIfOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(affine::AffineIfOp ifOp,
                                PatternRewriter &rewriter) const override {
    IslAnalysis ia;
    isl_set *inThen = ia.getDomain(&ifOp.getThenBlock()->front());
    isl_set *outsideIf = ia.getDomain(ifOp);
    isl_set *inElse =
        isl_set_subtract(isl_set_copy(outsideIf), isl_set_copy(inThen));

    bool succeeded = false;
    if (isl_set_is_empty(inThen) == isl_bool_true) {
      if (ifOp.hasElse()) {
        Operation *term = ifOp.getElseBlock()->getTerminator();
        rewriter.inlineBlockBefore(ifOp.getElseBlock(), ifOp);
        rewriter.replaceOp(ifOp, term->getOperands());
        rewriter.eraseOp(term);
      } else {
        rewriter.eraseOp(ifOp);
      }
      succeeded = true;
    } else if (isl_set_is_empty(inElse) == isl_bool_true) {
      Operation *term = ifOp.getThenBlock()->getTerminator();
      rewriter.inlineBlockBefore(ifOp.getThenBlock(), ifOp);
      rewriter.replaceOp(ifOp, term->getOperands());
      rewriter.eraseOp(term);
      succeeded = true;
    }
    isl_set_free(inThen);
    isl_set_free(inElse);
    isl_set_free(outsideIf);

    return success(succeeded);
  }
};

struct AffineIfSimplification : public OpRewritePattern<affine::AffineIfOp> {
  using OpRewritePattern<affine::AffineIfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(affine::AffineIfOp op,
                                PatternRewriter &rewriter) const override {
    SmallVector<AffineExpr> todo;
    SmallVector<bool> eqFlags;
    bool knownFalse = false;
    bool removed = false;
    for (auto cst : llvm::enumerate(op.getIntegerSet().getConstraints())) {
      auto opd = dyn_cast<AffineConstantExpr>(cst.value());
      if (!opd) {
        if (op.getIntegerSet().isEq(cst.index())) {
          if (auto bop = dyn_cast<AffineBinaryOpExpr>(cst.value())) {
            if (bop.getKind() == AffineExprKind::Mul &&
                bop.getRHS().getKind() == AffineExprKind::Constant) {
              removed = true;
              if (cast<AffineConstantExpr>(bop.getRHS()).getValue() != 0) {
                todo.push_back(bop.getLHS());
                eqFlags.push_back(op.getIntegerSet().isEq(cst.index()));
              }
              continue;
            }
            if (bop.getKind() == AffineExprKind::Add &&
                valueCmp(Cmp::GE, bop, op.getIntegerSet().getNumDims(),
                         op.getOperands(), 0)) {
              todo.push_back(bop.getLHS());
              eqFlags.push_back(op.getIntegerSet().isEq(cst.index()));
              todo.push_back(bop.getRHS());
              eqFlags.push_back(op.getIntegerSet().isEq(cst.index()));
              removed = true;
              continue;
            }
          }
        }

        bool canRemove = false;
        for (auto paren = op->getParentOfType<affine::AffineIfOp>(); paren;
             paren = paren->getParentOfType<affine::AffineIfOp>()) {
          for (auto cst2 : paren.getIntegerSet().getConstraints()) {
            if (paren.getElseRegion().isAncestor(op->getParentRegion()))
              continue;
            if (cst2 == cst.value() &&
                paren.getIntegerSet().getNumDims() ==
                    op.getIntegerSet().getNumDims() &&
                paren.getIntegerSet().getNumSymbols() ==
                    op.getIntegerSet().getNumSymbols() &&
                llvm::all_of(llvm::zip(paren.getOperands(), op.getOperands()),
                             [](std::tuple<Value, Value> p) {
                               return std::get<0>(p) == std::get<1>(p);
                             })) {
              canRemove = true;
              break;
            }
          }
          if (canRemove)
            break;
        }
        //// expr -1 >= 0    => expr > 0
        if (!op.getIntegerSet().isEq(cst.index())) {
          auto expr = cst.value() + 1;
          for (auto paren = op->getParentOfType<affine::AffineParallelOp>();
               paren;
               paren = paren->getParentOfType<affine::AffineParallelOp>()) {
            if (canRemove)
              break;
            for (auto tup : llvm::enumerate(paren.getSteps())) {
              bool found = false;
              for (auto ub : paren.getUpperBoundMap(tup.index()).getResults()) {
                if (auto exprS = dyn_cast<AffineSymbolExpr>(expr)) {
                  if (auto ubS = dyn_cast<AffineSymbolExpr>(ub)) {
                    if (op.getOperands()[exprS.getPosition() +
                                         op.getIntegerSet().getNumDims()] ==
                        paren.getUpperBoundsOperands()[ubS.getPosition() +
                                                       paren.getUpperBoundsMap()
                                                           .getNumDims()]) {

                      found = true;
                      break;
                    }
                  }
                }
              }
              if (!found)
                continue;

              if (!valueCmp(Cmp::GE, paren.getIVs()[tup.index()], 0))
                continue;

              canRemove = true;
              break;
            }
          }
          if (auto bop = dyn_cast<AffineBinaryOpExpr>(cst.value())) {
            if (bop.getKind() == AffineExprKind::Add) {
            }
          }
        }
        if (canRemove) {
          removed = true;
          continue;
        }

        todo.push_back(cst.value());
        eqFlags.push_back(op.getIntegerSet().isEq(cst.index()));
        continue;
      }
      removed = true;

      if (op.getIntegerSet().isEq(cst.index())) {
        if (opd.getValue() != 0) {
          knownFalse = true;
          break;
        }
      }
      if (!(opd.getValue() >= 0)) {
        knownFalse = true;
        break;
      }
    }

    if (knownFalse) {
      todo.clear();
    }

    if (todo.size() == 0) {

      if (!knownFalse)
        replaceOpWithRegion(rewriter, op, op.getThenRegion());
      else if (!op.getElseRegion().empty())
        replaceOpWithRegion(rewriter, op, op.getElseRegion());
      else
        rewriter.eraseOp(op);

      return success();
    }

    if (!removed)
      return failure();

    auto iset =
        IntegerSet::get(op.getIntegerSet().getNumDims(),
                        op.getIntegerSet().getNumSymbols(), todo, eqFlags);

    auto newIf = rewriter.create<affine::AffineIfOp>(
        op.getLoc(), op.getResultTypes(), iset, op.getOperands(),
        /*hasElse*/ true);
    rewriter.eraseBlock(newIf.getThenBlock());
    rewriter.eraseBlock(newIf.getElseBlock());
    rewriter.inlineRegionBefore(op.getThenRegion(), newIf.getThenRegion(),
                                newIf.getThenRegion().begin());
    rewriter.inlineRegionBefore(op.getElseRegion(), newIf.getElseRegion(),
                                newIf.getElseRegion().begin());
    rewriter.replaceOp(op, newIf.getResults());
    return success();
  }
};

struct CombineAffineIfs : public OpRewritePattern<affine::AffineIfOp> {
  using OpRewritePattern<affine::AffineIfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(affine::AffineIfOp nextIf,
                                PatternRewriter &rewriter) const override {
    Block *parent = nextIf->getBlock();
    if (nextIf == &parent->front())
      return failure();

    auto prevIf = dyn_cast<affine::AffineIfOp>(nextIf->getPrevNode());
    if (!prevIf)
      return failure();

    // Determine the logical then/else blocks when prevIf's
    // condition is used. Null means the block does not exist
    // in that case (e.g. empty else). If neither of these
    // are set, the two conditions cannot be compared.
    Block *nextThen = nullptr;
    Block *nextElse = nullptr;

    if (nextIf.getIntegerSet() == prevIf.getIntegerSet() &&
        llvm::all_of(llvm::zip(nextIf.getOperands(), prevIf.getOperands()),
                     [](std::tuple<Value, Value> p) {
                       return std::get<0>(p) == std::get<1>(p);
                     })) {
      nextThen = nextIf.getThenBlock();
      if (!nextIf.getElseRegion().empty())
        nextElse = nextIf.getElseBlock();
    }

    if (!nextThen && !nextElse)
      return failure();

    SmallVector<Value> prevElseYielded;
    if (!prevIf.getElseRegion().empty())
      prevElseYielded =
          cast<affine::AffineYieldOp>(prevIf.getElseBlock()->getTerminator())
              .getOperands();
    // Replace all uses of return values of op within nextIf with the
    // corresponding yields
    for (auto it : llvm::zip(
             prevIf.getResults(),
             cast<affine::AffineYieldOp>(prevIf.getThenBlock()->getTerminator())
                 .getOperands(),
             prevElseYielded))
      for (OpOperand &use :
           llvm::make_early_inc_range(std::get<0>(it).getUses())) {
        if (nextThen && nextThen->getParent()->isAncestor(
                            use.getOwner()->getParentRegion())) {
          rewriter.startOpModification(use.getOwner());
          use.set(std::get<1>(it));
          rewriter.finalizeOpModification(use.getOwner());
        } else if (nextElse && nextElse->getParent()->isAncestor(
                                   use.getOwner()->getParentRegion())) {
          rewriter.startOpModification(use.getOwner());
          use.set(std::get<2>(it));
          rewriter.finalizeOpModification(use.getOwner());
        }
      }

    SmallVector<Type> mergedTypes(prevIf.getResultTypes());
    llvm::append_range(mergedTypes, nextIf.getResultTypes());

    affine::AffineIfOp combinedIf = rewriter.create<affine::AffineIfOp>(
        nextIf.getLoc(), mergedTypes, prevIf.getIntegerSet(),
        prevIf.getOperands(), /*hasElse=*/true);
    rewriter.eraseBlock(&combinedIf.getThenRegion().back());
    rewriter.eraseBlock(&combinedIf.getElseRegion().back());

    rewriter.inlineRegionBefore(prevIf.getThenRegion(),
                                combinedIf.getThenRegion(),
                                combinedIf.getThenRegion().begin());

    if (nextThen) {
      affine::AffineYieldOp thenYield = cast<affine::AffineYieldOp>(
          combinedIf.getThenBlock()->getTerminator());
      affine::AffineYieldOp thenYield2 =
          cast<affine::AffineYieldOp>(nextThen->getTerminator());
      rewriter.mergeBlocks(nextThen, combinedIf.getThenBlock());
      rewriter.setInsertionPointToEnd(combinedIf.getThenBlock());

      SmallVector<Value> mergedYields(thenYield.getOperands());
      llvm::append_range(mergedYields, thenYield2.getOperands());
      rewriter.create<affine::AffineYieldOp>(thenYield2.getLoc(), mergedYields);
      rewriter.eraseOp(thenYield);
      rewriter.eraseOp(thenYield2);
    }

    rewriter.inlineRegionBefore(prevIf.getElseRegion(),
                                combinedIf.getElseRegion(),
                                combinedIf.getElseRegion().begin());

    if (nextElse) {
      if (combinedIf.getElseRegion().empty()) {
        rewriter.inlineRegionBefore(*nextElse->getParent(),
                                    combinedIf.getElseRegion(),
                                    combinedIf.getElseRegion().begin());
      } else {
        affine::AffineYieldOp elseYield = cast<affine::AffineYieldOp>(
            combinedIf.getElseBlock()->getTerminator());
        affine::AffineYieldOp elseYield2 =
            cast<affine::AffineYieldOp>(nextElse->getTerminator());
        rewriter.mergeBlocks(nextElse, combinedIf.getElseBlock());

        rewriter.setInsertionPointToEnd(combinedIf.getElseBlock());

        SmallVector<Value> mergedElseYields(elseYield.getOperands());
        llvm::append_range(mergedElseYields, elseYield2.getOperands());

        rewriter.create<affine::AffineYieldOp>(elseYield2.getLoc(),
                                               mergedElseYields);
        rewriter.eraseOp(elseYield);
        rewriter.eraseOp(elseYield2);
      }
    }

    SmallVector<Value> prevValues;
    SmallVector<Value> nextValues;
    for (const auto &pair : llvm::enumerate(combinedIf.getResults())) {
      if (pair.index() < prevIf.getNumResults())
        prevValues.push_back(pair.value());
      else
        nextValues.push_back(pair.value());
    }
    rewriter.replaceOp(prevIf, prevValues);
    rewriter.replaceOp(nextIf, nextValues);
    return success();
  }
};

struct MergeNestedAffineParallelLoops
    : public OpRewritePattern<affine::AffineParallelOp> {
  using OpRewritePattern<affine::AffineParallelOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(affine::AffineParallelOp op,
                                PatternRewriter &rewriter) const override {
    Block &outerBody = op.getRegion().getBlocks().front();
    if (!llvm::hasSingleElement(outerBody.without_terminator()))
      return failure();

    auto innerOp = dyn_cast<affine::AffineParallelOp>(outerBody.front());
    if (!innerOp)
      return failure();

    for (auto val : outerBody.getArguments())
      if (llvm::is_contained(innerOp.getLowerBoundsOperands(), val) ||
          llvm::is_contained(innerOp.getUpperBoundsOperands(), val))
        return failure();

    // Reductions are not supported yet.
    if (!op.getReductions().empty() || !innerOp.getReductions().empty())
      return failure();

    SmallVector<Type> newTypes(op.getResultTypes());
    for (auto T : innerOp.getResultTypes())
      newTypes.push_back(T);

    ArrayRef<Attribute> reductions;
    SmallVector<AffineExpr> lbounds;
    SmallVector<AffineExpr> ubounds;
    SmallVector<Value> lboundValues;
    SmallVector<Value> uboundValues;

    for (size_t i = 0; i < op.getLowerBoundsMap().getNumDims(); i++)
      lboundValues.push_back(op.getLowerBoundsOperands()[i]);

    for (size_t i = 0; i < op.getUpperBoundsMap().getNumDims(); i++)
      uboundValues.push_back(op.getUpperBoundsOperands()[i]);

    for (size_t i = 0; i < innerOp.getLowerBoundsMap().getNumDims(); i++)
      lboundValues.push_back(innerOp.getLowerBoundsOperands()[i]);

    for (size_t i = 0; i < innerOp.getUpperBoundsMap().getNumDims(); i++)
      uboundValues.push_back(innerOp.getUpperBoundsOperands()[i]);

    for (size_t i = 0; i < op.getLowerBoundsMap().getNumSymbols(); i++)
      lboundValues.push_back(
          op.getLowerBoundsOperands()[i + op.getLowerBoundsMap().getNumDims()]);

    for (size_t i = 0; i < op.getUpperBoundsMap().getNumSymbols(); i++)
      uboundValues.push_back(
          op.getUpperBoundsOperands()[i + op.getUpperBoundsMap().getNumDims()]);

    for (size_t i = 0; i < innerOp.getLowerBoundsMap().getNumSymbols(); i++)
      lboundValues.push_back(
          innerOp.getLowerBoundsOperands()[i + innerOp.getLowerBoundsMap()
                                                   .getNumDims()]);

    for (size_t i = 0; i < innerOp.getUpperBoundsMap().getNumSymbols(); i++)
      uboundValues.push_back(
          innerOp.getUpperBoundsOperands()[i + innerOp.getUpperBoundsMap()
                                                   .getNumDims()]);

    for (auto e : op.getLowerBoundsMap().getResults()) {
      lbounds.push_back(e);
    }

    for (auto e : op.getUpperBoundsMap().getResults()) {
      ubounds.push_back(e);
    }

    for (auto e : innerOp.getLowerBoundsMap()
                      .shiftDims(op.getLowerBoundsMap().getNumDims())
                      .shiftSymbols(op.getLowerBoundsMap().getNumSymbols())
                      .getResults()) {
      lbounds.push_back(e);
    }

    for (auto e : innerOp.getUpperBoundsMap()
                      .shiftDims(op.getUpperBoundsMap().getNumDims())
                      .shiftSymbols(op.getUpperBoundsMap().getNumSymbols())
                      .getResults()) {
      ubounds.push_back(e);
    }

    SmallVector<Value> operands = lboundValues;
    operands.append(uboundValues);

    SmallVector<int32_t> lboundGroup;
    SmallVector<int32_t> uboundGroup;
    for (auto U : op.getLowerBoundsGroups())
      lboundGroup.push_back(U.getZExtValue());
    for (auto U : innerOp.getLowerBoundsGroups())
      lboundGroup.push_back(U.getZExtValue());
    for (auto U : op.getUpperBoundsGroups())
      uboundGroup.push_back(U.getZExtValue());
    for (auto U : innerOp.getUpperBoundsGroups())
      uboundGroup.push_back(U.getZExtValue());

    SmallVector<int64_t> steps;
    for (auto U : op.getSteps())
      steps.push_back(U);
    for (auto U : innerOp.getSteps())
      steps.push_back(U);

    affine::AffineParallelOp affineLoop =
        rewriter.create<affine::AffineParallelOp>(
            op.getLoc(), newTypes, rewriter.getArrayAttr(reductions),
            AffineMapAttr::get(
                AffineMap::get(op.getLowerBoundsMap().getNumDims() +
                                   innerOp.getLowerBoundsMap().getNumDims(),
                               op.getLowerBoundsMap().getNumSymbols() +
                                   innerOp.getLowerBoundsMap().getNumSymbols(),
                               lbounds, op.getContext())),
            rewriter.getI32TensorAttr(lboundGroup),
            AffineMapAttr::get(
                AffineMap::get(op.getUpperBoundsMap().getNumDims() +
                                   innerOp.getUpperBoundsMap().getNumDims(),
                               op.getUpperBoundsMap().getNumSymbols() +
                                   innerOp.getUpperBoundsMap().getNumSymbols(),
                               ubounds, op.getContext())),
            rewriter.getI32TensorAttr(uboundGroup),
            rewriter.getI64ArrayAttr(steps), operands);

    rewriter.inlineRegionBefore(op.getRegion(), affineLoop.getRegion(),
                                affineLoop.getRegion().begin());
    auto yld = affineLoop.getBody()->getTerminator();
    rewriter.eraseOp(innerOp.getBody()->getTerminator());
    SmallVector<Value> post;
    for (auto v : innerOp.getIVs()) {
      post.push_back(
          affineLoop.getBody()->addArgument(v.getType(), v.getLoc()));
    }
    rewriter.inlineBlockBefore(innerOp.getBody(), yld, post);
    return success();
  }
};

struct PrepMergeNestedAffineParallelLoops
    : public OpRewritePattern<affine::AffineParallelOp> {
  using OpRewritePattern<affine::AffineParallelOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(affine::AffineParallelOp oop,
                                PatternRewriter &rewriter) const override {
    Block &outerBody = oop.getRegion().getBlocks().front();
    affine::AffineParallelOp innerOp = nullptr;
    SmallVector<Operation *> toMove;
    for (auto &op : outerBody) {
      if (auto innerOp2 = dyn_cast<affine::AffineParallelOp>(&op)) {
        if (innerOp)
          return failure();
        if (!isa<affine::AffineYieldOp>(innerOp2->getNextNode())) {
          return failure();
        }
        innerOp = innerOp2;
        continue;
      }
      if (isMemoryEffectFree(&op)) {
        if (!isa<affine::AffineYieldOp>(&op))
          toMove.push_back(&op);
        continue;
      }

      return failure();
    }

    if (!innerOp || !toMove.size()) {
      return failure();
    }

    IRMapping map;
    rewriter.setInsertionPointToStart(innerOp.getBody());
    for (auto o : toMove) {
      rewriter.replaceOp(o, rewriter.clone(*o)->getResults());
    }
    return success();
  }
};

/// Canonicalize the bounds of the given loop.
static LogicalResult canonicalizeLoopBounds(AffineParallelOp op) {
  AffineValueMap lb = op.getLowerBoundsValueMap();
  bool lbCanonicalized = succeeded(lb.canonicalize());

  AffineValueMap ub = op.getUpperBoundsValueMap();
  bool ubCanonicalized = succeeded(ub.canonicalize());

  // Any canonicalization change always leads to updated map(s).
  if (!lbCanonicalized && !ubCanonicalized)
    return failure();

  if (lbCanonicalized)
    op.setLowerBounds(lb.getOperands(), lb.getAffineMap());
  if (ubCanonicalized)
    op.setUpperBounds(ub.getOperands(), ub.getAffineMap());

  return success();
}

struct MergeNestedAffineParallelIf
    : public OpRewritePattern<affine::AffineParallelOp> {
  using OpRewritePattern<affine::AffineParallelOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(affine::AffineParallelOp op,
                                PatternRewriter &rewriter) const override {
    Block &outerBody = op.getRegion().getBlocks().front();

    affine::AffineIfOp innerOp = nullptr;
    for (auto &op : outerBody) {
      if (auto innerOp2 = dyn_cast<affine::AffineIfOp>(&op)) {
        if (innerOp)
          return failure();
        if (!isa<affine::AffineYieldOp>(innerOp2->getNextNode())) {
          return failure();
        }
        innerOp = innerOp2;
        continue;
      }
      if (!isReadOnly(&op))
        return failure();
    }

    if (!innerOp)
      return failure();

    // Reductions are not supported yet.
    if (!op.getReductions().empty())
      return failure();

    if (innerOp.hasElse())
      return failure();

    SmallVector<int32_t> lboundGroup;
    SmallVector<int32_t> uboundGroup;
    for (auto U : op.getLowerBoundsGroups())
      lboundGroup.push_back(U.getZExtValue());
    for (auto U : op.getUpperBoundsGroups())
      uboundGroup.push_back(U.getZExtValue());

    SmallVector<AffineExpr> lbounds;
    SmallVector<AffineExpr> ubounds;

    for (auto e : op.getLowerBoundsMap().getResults()) {
      lbounds.push_back(e);
    }

    for (auto e : op.getUpperBoundsMap().getResults()) {
      ubounds.push_back(e);
    }

    bool changed = false;
    SmallVector<AffineExpr> remaining;
    SmallVector<bool> isEq;
    for (auto cst : llvm::enumerate(innerOp.getIntegerSet().getConstraints())) {
      if (innerOp.getIntegerSet().isEq(cst.index())) {
        remaining.push_back(cst.value());
        isEq.push_back(innerOp.getIntegerSet().isEq(cst.index()));
        continue;
      }

      auto getIndUsage = [&op](AffineExpr cst, ValueRange operands,
                               std::map<size_t, AffineExpr> &indUsage,
                               bool &legal,
                               bool *failure = nullptr) -> AffineExpr {
        AffineExpr rhs = getAffineConstantExpr(0, cst.getContext());
        SmallVector<AffineExpr> todo = {cst};
        legal = true;
        while (todo.size()) {
          auto cur = todo.back();
          todo.pop_back();
          if (isa<AffineConstantExpr, AffineSymbolExpr>(cur)) {
            rhs = rhs + cur;
            continue;
          }
          if (auto dim = dyn_cast<AffineDimExpr>(cur)) {
            auto ival = dyn_cast<BlockArgument>(operands[dim.getPosition()]);
            if (!ival || ival.getOwner()->getParentOp() != op) {
              rhs = rhs + dim;
              if (failure)
                *failure = true;
              continue;
            }
            if (indUsage.find(ival.getArgNumber()) != indUsage.end()) {
              legal = false;
              continue;
            }
            indUsage[ival.getArgNumber()] =
                getAffineConstantExpr(1, op.getContext());
            continue;
          }
          if (auto bop = dyn_cast<AffineBinaryOpExpr>(cur)) {
            if (bop.getKind() == AffineExprKind::Add) {
              todo.push_back(bop.getLHS());
              todo.push_back(bop.getRHS());
              continue;
            }
            if (bop.getKind() == AffineExprKind::Mul) {
              if (!isa<AffineConstantExpr, AffineSymbolExpr>(bop.getRHS())) {
                legal = false;
                continue;
              }

              if (auto dim = dyn_cast<AffineDimExpr>(bop.getLHS())) {
                auto ival =
                    dyn_cast<BlockArgument>(operands[dim.getPosition()]);
                if (!ival || ival.getOwner()->getParentOp() != op) {
                  rhs = rhs + bop;
                  // While legal, this may run before parallel merging
                  // and prevent parallel fusion
                  legal = false;
                  if (failure)
                    *failure = true;
                  continue;
                }
                if (indUsage.find(ival.getArgNumber()) != indUsage.end()) {
                  legal = false;
                  continue;
                }
                indUsage[ival.getArgNumber()] = bop.getRHS();
                continue;
              }
            }
          }
          if (failure)
            *failure = true;
          legal = false;
          break;
        }
        return rhs;
      };

      bool legal;
      std::map<size_t, AffineExpr> indUsage;
      bool failureV = false;
      AffineExpr rhs = getIndUsage(cst.value(), innerOp.getOperands(), indUsage,
                                   legal, &failureV);
      if (failureV)
        return failure();

      if (!legal || indUsage.size() != 1) {
        remaining.push_back(cst.value());
        isEq.push_back(innerOp.getIntegerSet().isEq(cst.index()));
        continue;
      }
      auto pair = *indUsage.begin();
      auto affCst = dyn_cast<AffineConstantExpr>(pair.second);
      if (!affCst) {
        remaining.push_back(cst.value());
        isEq.push_back(innerOp.getIntegerSet().isEq(cst.index()));
        continue;
      }

      // currently aff * idx + rhs >= 0
      // currently aff * idx >= -rhs
      //    if aff is negative, then
      //       idx <= (-rhs).floorDiv(aff)
      //       idx <  (-rhs).floorDiv(aff) - 1
      //    else if idx is positive
      //       idx >= (-rhs).floorDiv(aff)
      assert(affCst.getValue() != 0);
      if (affCst.getValue() < 0) {
        changed = true;
        rhs = rhs.floorDiv(-affCst.getValue()) + 1;

        size_t off = 0;
        for (size_t i = 0; i < pair.first; i++)
          off += uboundGroup[i];

        if (auto newCst = dyn_cast<AffineConstantExpr>(rhs)) {
          bool seen = false;
          for (size_t i = 0; i < uboundGroup[pair.first]; i++) {
            if (auto oldCst = dyn_cast<AffineConstantExpr>(ubounds[off + i])) {
              seen = true;
              if (newCst.getValue() < oldCst.getValue())
                ubounds[off + i] = rhs;
            }
          }
          if (seen)
            continue;
        }
        ubounds.insert(
            ubounds.begin() + off,
            rhs.shiftDims(innerOp.getIntegerSet().getNumDims(),
                          op.getUpperBoundsMap().getNumDims())
                .shiftSymbols(innerOp.getIntegerSet().getNumSymbols(),
                              op.getUpperBoundsMap().getNumSymbols()));

        uboundGroup[pair.first]++;
      } else {
        auto min = rhs.floorDiv(-affCst.getValue());
        if (auto cst = dyn_cast<AffineConstantExpr>(min)) {

          size_t off = 0;
          for (size_t i = 0; i < pair.first; i++)
            off += lboundGroup[i];

          bool seen = false;
          for (size_t i = 0; i < lboundGroup[pair.first]; i++) {
            if (auto oldCst = dyn_cast<AffineConstantExpr>(lbounds[off + i])) {
              if (cst.getValue() <= oldCst.getValue()) {
                seen = true;
              } else if ((cst.getValue() - oldCst.getValue()) %
                             op.getSteps()[pair.first] ==
                         0) {
                lbounds[off + i] = min;
                seen = true;
              }
            }
          }
          if (seen) {
            changed = true;
            continue;
          }
        }

        remaining.push_back(cst.value());
        isEq.push_back(innerOp.getIntegerSet().isEq(cst.index()));
        continue;
      }
    }

    if (!changed)
      return failure();

    SmallVector<Value> lboundValues;
    SmallVector<Value> uboundValues;

    for (size_t i = 0; i < op.getLowerBoundsMap().getNumDims(); i++)
      lboundValues.push_back(op.getLowerBoundsOperands()[i]);

    for (size_t i = 0; i < op.getUpperBoundsMap().getNumDims(); i++)
      uboundValues.push_back(op.getUpperBoundsOperands()[i]);

    for (size_t i = 0; i < innerOp.getIntegerSet().getNumDims(); i++)
      uboundValues.push_back(innerOp.getOperands()[i]);

    for (size_t i = 0; i < op.getLowerBoundsMap().getNumSymbols(); i++)
      lboundValues.push_back(
          op.getLowerBoundsOperands()[i + op.getLowerBoundsMap().getNumDims()]);

    for (size_t i = 0; i < op.getUpperBoundsMap().getNumSymbols(); i++)
      uboundValues.push_back(
          op.getUpperBoundsOperands()[i + op.getUpperBoundsMap().getNumDims()]);

    for (size_t i = 0; i < innerOp.getIntegerSet().getNumSymbols(); i++)
      uboundValues.push_back(
          innerOp.getOperands()[i + innerOp.getIntegerSet().getNumDims()]);

    SmallVector<Value> operands = lboundValues;
    operands.append(uboundValues);

    ArrayRef<Attribute> reductions;

    affine::AffineParallelOp affineLoop =
        rewriter.create<affine::AffineParallelOp>(
            op.getLoc(), op.getResultTypes(), rewriter.getArrayAttr(reductions),
            AffineMapAttr::get(
                AffineMap::get(op.getLowerBoundsMap().getNumDims(),
                               op.getLowerBoundsMap().getNumSymbols(), lbounds,
                               op.getContext())),
            rewriter.getI32TensorAttr(lboundGroup),
            AffineMapAttr::get(
                AffineMap::get(op.getUpperBoundsMap().getNumDims() +
                                   innerOp.getIntegerSet().getNumDims(),
                               op.getUpperBoundsMap().getNumSymbols() +
                                   innerOp.getIntegerSet().getNumSymbols(),
                               ubounds, op.getContext())),
            rewriter.getI32TensorAttr(uboundGroup), op.getStepsAttr(),
            operands);
    rewriter.inlineRegionBefore(op.getRegion(), affineLoop.getRegion(),
                                affineLoop.getRegion().begin());

    rewriter.setInsertionPoint(innerOp);

    if (remaining.empty()) {
      auto yld =
          cast<affine::AffineYieldOp>(innerOp.getThenBlock()->getTerminator());
      SmallVector<Value> toRet(yld.getOperands());
      rewriter.eraseOp(yld);
      rewriter.inlineBlockBefore(innerOp.getThenBlock(), innerOp);
      rewriter.replaceOp(innerOp, toRet);
      rewriter.eraseOp(op);
    } else {
      affine::AffineIfOp newIf = rewriter.create<affine::AffineIfOp>(
          innerOp.getLoc(), innerOp.getResultTypes(),
          IntegerSet::get(innerOp.getIntegerSet().getNumDims(),
                          innerOp.getIntegerSet().getNumSymbols(), remaining,
                          isEq),
          innerOp.getOperands(), /*hasElse*/ false);

      rewriter.eraseBlock(newIf.getThenBlock());

      rewriter.inlineRegionBefore(innerOp.getThenRegion(),
                                  newIf.getThenRegion(),
                                  newIf.getThenRegion().begin());
      rewriter.inlineRegionBefore(innerOp.getElseRegion(),
                                  newIf.getElseRegion(),
                                  newIf.getElseRegion().begin());

      rewriter.replaceOp(innerOp, newIf->getResults());
      rewriter.replaceOp(op, affineLoop->getResults());
    }

    // We include the dims of the affine.if expressios (which include the IVs of
    // the parallel loop) in the new parallel which results in invalid IR. This
    // canonicalizes these dims away.
    return canonicalizeLoopBounds(affineLoop);
  }
};

struct AffineDimDescriptor {
  bool known = false;
  int64_t lb;
  int64_t ub;
  int64_t step;

  AffineDimDescriptor(int64_t lb_, int64_t ub_, int64_t step_)
      : known(true), lb(lb_), ub(ub_), step(step_) {}
  AffineDimDescriptor() : known(false) {}
};

static std::optional<AffineExpr>
optimizeExprFloorDiv(llvm::ArrayRef<AffineDimDescriptor> dims, AffineExpr lhs,
                     AffineExpr rhs) {
  if (!rhs.isSymbolicOrConstant())
    return std::nullopt;

  auto constRhs = dyn_cast<AffineConstantExpr>(rhs);
  if (!constRhs)
    return std::nullopt; // todo: symbolic

  if (auto lhsDim = dyn_cast<AffineDimExpr>(lhs)) {
    auto dim = dims[lhsDim.getPosition()];
    if (!dim.known)
      return std::nullopt;

    if (dim.step >= 0 && dim.ub > constRhs.getValue())
      return std::nullopt;

    return mlir::getAffineConstantExpr(0, lhs.getContext());
  }

  if (auto add = dyn_cast<AffineBinaryOpExpr>(lhs)) {
    if (add.getKind() == AffineExprKind::Add) {
      for (int i = 0; i < 2; i++) {
        auto lhs = i == 0 ? add.getLHS() : add.getRHS();
        auto rhs = i == 0 ? add.getRHS() : add.getLHS();
        auto lhse = dyn_cast<AffineDimExpr>(lhs);
        if (!lhse)
          continue;
        auto rhse = dyn_cast<AffineBinaryOpExpr>(rhs);
        if (!rhse)
          continue;
        if (rhse.getKind() != AffineExprKind::Mul)
          continue;
        auto mulconst = dyn_cast<AffineConstantExpr>(rhse.getRHS());
        if (!mulconst)
          continue;
        auto dim = dims[lhse.getPosition()];
        if (!dim.known)
          continue;

        if (dim.step < 0)
          continue;
        if (dim.lb != 0)
          continue;
        if (dim.ub != mulconst.getValue())
          continue;
        if (constRhs.getValue() % mulconst.getValue() == 0)
          return rhse.getLHS().floorDiv(constRhs.floorDiv(mulconst));
      }
    }
  }

  return std::nullopt;
}

static std::optional<AffineExpr>
optimizeExprMod(llvm::ArrayRef<AffineDimDescriptor> dims, AffineExpr lhs,
                AffineExpr rhs) {
  if (!rhs.isSymbolicOrConstant())
    return std::nullopt;

  if (auto lhsBin = dyn_cast<AffineBinaryOpExpr>(lhs)) {
    auto lhsKind = lhs.getKind();
    if (lhsKind == AffineExprKind::Mul) {
      // (a * x) % x => 0
      if (lhsBin.getRHS() == rhs)
        return mlir::getAffineConstantExpr(0, lhs.getContext());

      return std::nullopt;
    }
  }

  auto constRhs = dyn_cast<AffineConstantExpr>(rhs);
  if (!constRhs)
    return std::nullopt;

  if (auto lhsDim = dyn_cast<AffineDimExpr>(lhs)) {
    auto dim = dims[lhsDim.getPosition()];
    if (!dim.known || dim.step != 1 || dim.lb != 0 ||
        dim.ub != constRhs.getValue())
      return std::nullopt;

    return lhsDim;
  }

  return std::nullopt;
}

AffineExpr optimizeExprWithBounds(AffineExpr expr,
                                  llvm::ArrayRef<AffineDimDescriptor> dims) {
  auto binExpr = dyn_cast<AffineBinaryOpExpr>(expr);
  if (!binExpr)
    return expr;

  AffineExpr lhs = optimizeExprWithBounds(binExpr.getLHS(), dims);
  AffineExpr rhs = optimizeExprWithBounds(binExpr.getRHS(), dims);

  switch (expr.getKind()) {
  case AffineExprKind::Add:
    return lhs + rhs;
  case AffineExprKind::Mul:
    return lhs * rhs;
  case AffineExprKind::Mod:
    if (auto replacement = optimizeExprMod(dims, lhs, rhs))
      return *replacement;
    else
      return lhs % rhs;
  case AffineExprKind::FloorDiv:
    if (auto replacement = optimizeExprFloorDiv(dims, lhs, rhs))
      return *replacement;
    else
      return lhs.floorDiv(rhs);
  default:
    return expr;
  }

  return expr;
}

static AffineMap optimizeMap(AffineMap map,
                             llvm::ArrayRef<AffineDimDescriptor> dims) {
  llvm::DenseMap<AffineExpr, AffineExpr> replacements;
  SmallVector<AffineExpr> todo;
  for (auto expr : map.getResults())
    todo.push_back(optimizeExprWithBounds(expr, dims));
  return AffineMap::get(map.getNumDims(), map.getNumSymbols(), todo,
                        map.getContext());
}

struct OptimizeRem : public OpRewritePattern<arith::RemUIOp> {
  using OpRewritePattern<arith::RemUIOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(arith::RemUIOp op,
                                PatternRewriter &rewriter) const override {
    AddIOp sum = op.getLhs().getDefiningOp<arith::AddIOp>();
    if (!sum)
      return failure();
    for (int i = 0; i < 2; i++) {
      auto val = sum->getOperand(i).getDefiningOp<arith::MulIOp>();
      if (!val)
        continue;
      if (val.getRhs() != op.getRhs())
        continue;
      rewriter.replaceOpWithNewOp<arith::RemUIOp>(op, sum->getOperand(1 - i),
                                                  op.getRhs());
      return success();
    }
    return failure();
  }
};

// Reductions or min-max are not supported yet.
// When all uses of an IV are of the form (%i % cst) or (%i // cst), replace
// with two ivs: %i1 = (0) to (ub[i] // cst) %i0 = (0) to (cst)
struct SplitParallelInductions
    : public OpRewritePattern<affine::AffineParallelOp> {
  using OpRewritePattern<affine::AffineParallelOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(affine::AffineParallelOp op,
                                PatternRewriter &rewriter) const override {
    // Reductions or min-max are not supported yet.
    if (!op.getReductions().empty() || op.hasMinMaxBounds())
      return failure();

    for (auto it : llvm::enumerate(op.getIVs())) {
      auto idx = it.index();
      auto iv = it.value();
      ValueOrInt base(Value(nullptr));
      bool legal = true;

      for (auto lb : op.getLowerBoundMap(iv.getArgNumber()).getResults()) {
        if (auto cst = dyn_cast<AffineConstantExpr>(lb)) {
          if (cst.getValue() != 0) {
            legal = false;
            break;
          }
        } else {
          legal = false;
          break;
        }
      }

      bool seenub = false;
      for (auto ub : op.getUpperBoundMap(iv.getArgNumber()).getResults()) {
        if (seenub) {
          legal = false;
          break;
        }
        seenub = true;
        if (!isa<AffineConstantExpr>(ub)) {
          legal = false;
        }
      }

      auto step = op.getSteps()[idx];
      if (step != 1) {
        legal = false;
        continue;
      }

      SmallVector<std::pair<Operation *, Value>> users;
      for (auto U : iv.getUsers()) {
        users.emplace_back(U, iv);
      }
      bool hasRemainder = false;
      while (!users.empty()) {
        auto &&[U, pval] = users.pop_back_val();
        SmallVector<AffineExpr> exprs;
        ValueRange operands;

        if (auto AL = dyn_cast<affine::AffineLoadOp>(U)) {
          operands = AL.getMapOperands();
          for (auto E : AL.getAffineMap().getResults()) {
            bool functionOf = false;
            for (size_t i = 0; i < operands.size(); i++) {
              if (operands[i] != iv)
                continue;
              if (i < AL.getAffineMap().getNumDims()) {
                functionOf |= E.isFunctionOfDim(i);
              } else {
                functionOf |=
                    E.isFunctionOfSymbol(i - AL.getAffineMap().getNumSymbols());
              }
            }
            if (functionOf)
              exprs.push_back(E);
          }
        } else if (auto AS = dyn_cast<affine::AffineStoreOp>(U)) {
          if (AS.getValue() == iv)
            legal = false;
          operands = AS.getMapOperands();
          for (auto E : AS.getAffineMap().getResults()) {
            bool functionOf = false;
            for (size_t i = 0; i < operands.size(); i++) {
              if (operands[i] != iv)
                continue;
              if (i < AS.getAffineMap().getNumDims()) {
                functionOf |= E.isFunctionOfDim(i);
              } else {
                functionOf |=
                    E.isFunctionOfSymbol(i - AS.getAffineMap().getNumDims());
              }
            }
            if (functionOf)
              exprs.push_back(E);
          }
        } else if (auto AI = dyn_cast<affine::AffineIfOp>(U)) {
          operands = AI.getOperands();
          for (auto E : AI.getIntegerSet().getConstraints()) {
            bool functionOf = false;
            for (size_t i = 0; i < operands.size(); i++) {
              if (operands[i] != iv)
                continue;
              if (i < AI.getIntegerSet().getNumDims()) {
                functionOf |= E.isFunctionOfDim(i);
              } else {
                functionOf |=
                    E.isFunctionOfSymbol(i - AI.getIntegerSet().getNumDims());
              }
            }
            if (functionOf)
              exprs.push_back(E);
          }
        } else if (auto AA = dyn_cast<affine::AffineApplyOp>(U)) {
          operands = AA.getMapOperands();
          auto map = AA.getMap();
          exprs.append(map.getResults().begin(), map.getResults().end());
        } else if (auto cstOp = dyn_cast<arith::IndexCastUIOp>(U)) {
          for (auto UU : cstOp.getResult().getUsers()) {
            users.emplace_back(UU, cstOp->getResult(0));
          }
          continue;
        } else if (auto cstOp = dyn_cast<arith::IndexCastOp>(U)) {
          for (auto UU : cstOp.getResult().getUsers()) {
            users.emplace_back(UU, cstOp->getResult(0));
          }
          continue;
        } else if (isa<arith::FloorDivSIOp, arith::DivUIOp, arith::RemUIOp>(
                       U)) {
          if (isa<arith::RemUIOp>(U)) {
            hasRemainder |= isa<arith::RemUIOp>(U);
          }
          Value newBase = U->getOperand(1);

          if (base.isValue && !base.v_val)
            base = ValueOrInt(newBase);
          else if (base.isValue && base.v_val == newBase) {
            base = ValueOrInt(newBase);
          } else if (!base.isValue) {
            APInt iattr;
            if (!matchPattern(newBase, m_ConstantInt(&iattr))) {
              legal = false;
              break;
            }
            if (base.i_val.getBitWidth() != iattr.getBitWidth()) {
              base.i_val = base.i_val.sextOrTrunc(iattr.getBitWidth());
            }
            if (!(base.i_val == iattr ||
                  (isa<arith::FloorDivSIOp, arith::DivUIOp>(U) &&
                   (base.i_val.urem(iattr.getZExtValue()) == 0 ||
                    iattr.urem(base.i_val.getZExtValue()) == 0)))) {
              legal = false;
              break;
            }

            base.i_val = base.i_val.sgt(iattr) ? iattr : base.i_val;
          } else {
            legal = false;
            break;
          }
        } else {
          if (pval == iv)
            continue;
          legal = false;
          break;
        }
        auto findBasePattern = [](Value iv, AffineExpr root,
                                  ValueRange operands, ValueOrInt &base,
                                  bool &legal, bool &hasRemainder) {
          SmallVector<AffineExpr> todo = {root};
          while (!todo.empty()) {
            auto subExpr = todo.back();
            todo.pop_back();

            if (auto binExpr = dyn_cast<AffineBinaryOpExpr>(subExpr)) {
              auto dimExpr = dyn_cast<AffineDimExpr>(binExpr.getLHS());

              auto kind = subExpr.getKind();
              if (!dimExpr || operands[dimExpr.getPosition()] != iv ||
                  (kind != AffineExprKind::FloorDiv &&
                   kind != AffineExprKind::Mod)) {
                todo.push_back(binExpr.getLHS());
                todo.push_back(binExpr.getRHS());
                continue;
              }

              auto rhs = binExpr.getRHS();

              ValueOrInt newBase(nullptr);
              if (auto symExpr = dyn_cast<AffineSymbolExpr>(rhs)) {
                newBase = ValueOrInt(operands[symExpr.getPosition()]);
              } else if (auto constExpr = dyn_cast<AffineConstantExpr>(rhs)) {
                newBase = ValueOrInt(APInt(64, constExpr.getValue(), true));
              } else {
                legal = false;
                return;
              }

              if (kind == AffineExprKind::Mod) {
                hasRemainder = true;
              }

              if (base.isValue && base.v_val == nullptr) {
                base = newBase;
              } else if (base.isValue && newBase.isValue &&
                         base.v_val == newBase.v_val) {
                base = newBase;
              } else if (!base.isValue && !newBase.isValue &&
                         (base.i_val == newBase.i_val ||
                          (kind == AffineExprKind::FloorDiv &&
                           (base.i_val.urem(newBase.i_val) == 0 ||
                            newBase.i_val.urem(base.i_val) == 0)))) {
                base.i_val =
                    base.i_val.sgt(newBase.i_val) ? newBase.i_val : base.i_val;
              } else {
                legal = false;
                return;
              }
            } else if (auto dimExpr = dyn_cast<AffineDimExpr>(subExpr)) {
              // iv referenced without pattern
              // if (operands[dimExpr.getPosition()] == iv) {
              //   legal = false;
              //   return;
              // }
            }
          }
        };

        for (auto expr : exprs) {
          findBasePattern(iv, expr, operands, base, legal, hasRemainder);
          if (!legal)
            break;
        }

        if (!legal)
          break;
      }

      if (base.isValue && !base.v_val) {
        legal = false;
      }
      if (!hasRemainder)
        legal = false;

      // We can add an extra iv
      if (legal) {
        assert(!base.isValue && "todo");

        Block *body = op.getBody();

        SmallVector<int64_t> steps;

        for (auto s : op.getSteps())
          steps.push_back(s);

        steps.push_back(1);

        SmallVector<AffineExpr> lbounds(
            op.getLowerBoundsMap().getResults().begin(),
            op.getLowerBoundsMap().getResults().end());
        lbounds.push_back(mlir::getAffineConstantExpr(0, op.getContext()));

        SmallVector<AffineExpr> ubounds;

        for (size_t i = 0; i < idx; ++i)
          ubounds.push_back(op.getUpperBoundsMap().getResult(i));

        AffineExpr baseExpr =
            base.isValue ? mlir::getAffineSymbolExpr(0, op.getContext())
                         : mlir::getAffineConstantExpr(
                               base.i_val.getSExtValue(), op.getContext());

        AffineExpr ubound0 =
            op.getUpperBoundsMap().getResult(idx).floorDiv(baseExpr);

        if (ubound0 * baseExpr != op.getUpperBoundsMap().getResult(idx)) {
          continue;
        }

        if (ubound0 == mlir::getAffineConstantExpr(0, op.getContext())) {
          continue;
        }

        AffineExpr ubound1 =
            op.getUpperBoundsMap().getResult(idx).floorDiv(ubound0);

        ubounds.push_back(ubound0);

        for (size_t i = idx + 1, e = op.getUpperBoundsMap().getNumResults();
             i < e; ++i)
          ubounds.push_back(op.getUpperBoundsMap().getResult(i));

        ubounds.push_back(ubound1);

        SmallVector<int32_t> lowerBoundsGroup;
        SmallVector<int32_t> upperBoundsGroup;
        for (auto lb : op.getLowerBoundsGroups())
          lowerBoundsGroup.push_back(lb.getZExtValue());
        lowerBoundsGroup.push_back(1);

        for (auto ub : op.getUpperBoundsGroups())
          upperBoundsGroup.push_back(ub.getZExtValue());
        upperBoundsGroup.push_back(1);

        auto affineLoop = rewriter.create<affine::AffineParallelOp>(
            op.getLoc(), op.getResultTypes(), op.getReductionsAttr(),
            AffineMapAttr::get(
                AffineMap::get(op.getLowerBoundsMap().getNumDims(),
                               op.getLowerBoundsMap().getNumSymbols(), lbounds,
                               op.getContext())),
            rewriter.getI32TensorAttr(lowerBoundsGroup),
            AffineMapAttr::get(
                AffineMap::get(op.getUpperBoundsMap().getNumDims(),
                               op.getUpperBoundsMap().getNumSymbols(), ubounds,
                               op.getContext())),
            rewriter.getI32TensorAttr(upperBoundsGroup),
            rewriter.getI64ArrayAttr(steps), op.getMapOperands());

        rewriter.inlineRegionBefore(op.getRegion(), affineLoop.getRegion(),
                                    affineLoop.getRegion().begin());
        rewriter.eraseOp(op);

        Value newIv = body->addArgument(iv.getType(), iv.getLoc());

        SmallVector<Operation *> users(iv.getUsers().begin(),
                                       iv.getUsers().end());

        auto getDimExpr = [](Value iv, ValueRange operands) -> AffineDimExpr {
          unsigned ivPos = 0;
          for (unsigned i = 0; i < operands.size(); ++i) {
            if (operands[i] == iv) {
              ivPos = i;
              break;
            }
          }
          return cast<AffineDimExpr>(
              mlir::getAffineDimExpr(ivPos, iv.getContext()));
        };

        auto getNewMap = [getDimExpr, ubound0, base](Value iv, AffineMap oldMap,
                                                     ValueRange operands,
                                                     AffineExpr baseExpr) {
          SmallVector<AffineDimDescriptor> dimDescriptors(
              operands.size() + 1, AffineDimDescriptor());

          AffineExpr majorExpr = getDimExpr(iv, operands),
                     minorExpr = mlir::getAffineDimExpr(oldMap.getNumDims(),
                                                        iv.getContext());

          dimDescriptors[cast<AffineDimExpr>(majorExpr).getPosition()] =
              AffineDimDescriptor(
                  0, cast<AffineConstantExpr>(ubound0).getValue(), 1);
          dimDescriptors[cast<AffineDimExpr>(minorExpr).getPosition()] =
              AffineDimDescriptor(0, base.i_val.getSExtValue(), 1);

          return optimizeMap(
              oldMap.replace(majorExpr, majorExpr * baseExpr + minorExpr,
                             oldMap.getNumDims() + 1, oldMap.getNumSymbols()),
              dimDescriptors);
        };

        for (auto U : users) {
          if (auto AL = dyn_cast<affine::AffineLoadOp>(U)) {
            auto operands = AL.getMapOperands();
            auto map = AL.getAffineMap();
            auto newMap = getNewMap(iv, map, operands, baseExpr);

            rewriter.modifyOpInPlace(AL, [&]() {
              AL.setMap(newMap);
              AL->insertOperands(1 + map.getNumDims(), newIv);
            });
          } else if (auto AS = dyn_cast<affine::AffineStoreOp>(U)) {
            auto operands = AS.getMapOperands();
            auto map = AS.getAffineMap();
            auto newMap = getNewMap(iv, map, operands, baseExpr);

            rewriter.modifyOpInPlace(AS, [&]() {
              AS.setMap(newMap);
              AS->insertOperands(2 + map.getNumDims(), newIv);
            });
          } else if (auto AA = dyn_cast<affine::AffineApplyOp>(U)) {
            auto operands = AA.getMapOperands();
            auto map = AA.getMap();
            auto newMap = getNewMap(iv, map, operands, baseExpr);

            rewriter.modifyOpInPlace(AA, [&]() {
              AA.setMap(newMap);
              AA->insertOperands(map.getNumDims(), newIv);
            });
          } else if (auto AI = dyn_cast<affine::AffineIfOp>(U)) {
            auto operands = AI.getOperands();
            auto is = AI.getIntegerSet();

            AffineDimExpr majorExpr = getDimExpr(iv, operands);
            auto minorExpr =
                mlir::getAffineDimExpr(is.getNumDims(), iv.getContext());
            SmallVector<AffineDimDescriptor> dimDescriptors(
                is.getNumDims() + 1, AffineDimDescriptor());

            dimDescriptors[cast<AffineDimExpr>(majorExpr).getPosition()] =
                AffineDimDescriptor(
                    0, cast<AffineConstantExpr>(ubound0).getValue(), 1);
            dimDescriptors[cast<AffineDimExpr>(minorExpr).getPosition()] =
                AffineDimDescriptor(0, base.i_val.getSExtValue(), 1);

            SmallVector<AffineExpr> newConstraints;
            for (auto constraint : is.getConstraints()) {
              if (!constraint.isFunctionOfDim(majorExpr.getPosition())) {
                newConstraints.push_back(constraint);
                continue;
              }
              auto E = constraint.replace(majorExpr,
                                          majorExpr * baseExpr + minorExpr);
              E = optimizeExprWithBounds(E, dimDescriptors);

              newConstraints.push_back(E);
            }

            auto newIntegerSet =
                IntegerSet::get(is.getNumDims() + 1, is.getNumSymbols(),
                                newConstraints, is.getEqFlags());

            rewriter.modifyOpInPlace(AI, [&]() {
              AI.setIntegerSet(newIntegerSet);
              AI->insertOperands(is.getNumDims(), newIv);
            });
          } else if (isa<arith::IndexCastUIOp, arith::IndexCastOp>(U)) {
            OpBuilder::InsertionGuard guard(rewriter);
            rewriter.setInsertionPoint(U);

            for (auto UU :
                 llvm::make_early_inc_range(U->getResult(0).getUsers())) {

              if (isa<arith::FloorDivSIOp, arith::DivUIOp>(UU)) {
                rewriter.setInsertionPoint(UU);
                auto replacement = rewriter.create<arith::MulIOp>(
                    UU->getLoc(), U->getResult(0),
                    rewriter.create<arith::ConstantIntOp>(
                        UU->getLoc(), U->getResult(0).getType(),
                        base.i_val.getSExtValue()));
                replacement.setOverflowFlags(IntegerOverflowFlags::nuw);
                rewriter.replaceOpWithNewOp<arith::DivUIOp>(UU, replacement,
                                                            UU->getOperand(1));
              } else if (isa<arith::RemUIOp>(UU)) {
                rewriter.replaceAllUsesWith(
                    UU->getResult(0),
                    rewriter.create<arith::IndexCastUIOp>(
                        U->getLoc(), U->getResult(0).getType(), newIv));
              } else {
                llvm_unreachable("impossible use of cast");
              }
            }

          } else if (isa<arith::FloorDivSIOp, arith::DivUIOp>(U)) {
            rewriter.setInsertionPoint(U);
            auto replacement = rewriter.create<arith::MulIOp>(
                U->getLoc(), iv,
                rewriter.create<arith::ConstantIndexOp>(
                    U->getLoc(), base.i_val.getSExtValue()));
            replacement.setOverflowFlags(IntegerOverflowFlags::nuw);
            rewriter.replaceOpWithNewOp<arith::DivUIOp>(U, replacement,
                                                        U->getOperand(1));
          } else if (isa<arith::RemUIOp>(U)) {
            rewriter.replaceAllUsesWith(U->getResult(0), newIv);
          } else {
            rewriter.setInsertionPoint(U);
            auto replacement = rewriter.create<arith::MulIOp>(
                U->getLoc(), iv,
                rewriter.create<arith::ConstantIndexOp>(
                    U->getLoc(), base.i_val.getSExtValue()));
            replacement.setOverflowFlags(IntegerOverflowFlags::nuw);
            auto replacement2 =
                rewriter.create<arith::AddIOp>(U->getLoc(), replacement, newIv);
            rewriter.replaceUsesWithIf(
                iv, replacement2->getResult(0),
                [&](OpOperand &op) { return op.getOwner() == U; });
          }
        }

        return success();
      }
    }

    return failure();
  }
};

struct MergeParallelInductions
    : public OpRewritePattern<affine::AffineParallelOp> {
  using OpRewritePattern<affine::AffineParallelOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(affine::AffineParallelOp op,
                                PatternRewriter &rewriter) const override {
    // Reductions are not supported yet.
    if (!op.getReductions().empty())
      return failure();

    auto getIndUsage = [&op](AffineExpr cst, ValueRange operands,
                             std::map<size_t, AffineExpr> &indUsage,
                             bool &legal) -> AffineExpr {
      AffineExpr rhs = getAffineConstantExpr(0, cst.getContext());
      SmallVector<AffineExpr> todo = {cst};
      legal = true;
      while (todo.size()) {
        auto cur = todo.back();
        todo.pop_back();
        if (isa<AffineConstantExpr, AffineSymbolExpr>(cur)) {
          rhs = rhs + cur;
          continue;
        }
        if (auto dim = dyn_cast<AffineDimExpr>(cur)) {
          auto ival = dyn_cast<BlockArgument>(operands[dim.getPosition()]);
          if (!ival || ival.getOwner()->getParentOp() != op) {
            rhs = rhs + dim;
            continue;
          }
          if (indUsage.find(ival.getArgNumber()) != indUsage.end()) {
            LLVM_DEBUG(llvm::dbgs() << "Already used index " << ival << "\n");
            legal = false;
            continue;
          }
          indUsage[ival.getArgNumber()] =
              getAffineConstantExpr(1, op.getContext());
          continue;
        }
        if (auto bop = dyn_cast<AffineBinaryOpExpr>(cur)) {
          if (bop.getKind() == AffineExprKind::Add) {
            todo.push_back(bop.getLHS());
            todo.push_back(bop.getRHS());
            continue;
          }
          if (bop.getKind() == AffineExprKind::Mul) {
            if (!isa<AffineConstantExpr, AffineSymbolExpr>(bop.getRHS())) {
              legal = false;
              continue;
            }

            if (auto dim = dyn_cast<AffineDimExpr>(bop.getLHS())) {
              auto ival = dyn_cast<BlockArgument>(operands[dim.getPosition()]);
              if (!ival || ival.getOwner()->getParentOp() != op) {
                rhs = rhs + bop;
                continue;
              }
              if (indUsage.find(ival.getArgNumber()) != indUsage.end()) {
                legal = false;
                continue;
              }
              indUsage[ival.getArgNumber()] = bop.getRHS();
              continue;
            }
          }
        }
        LLVM_DEBUG(llvm::dbgs()
                   << "Unknown affine expression in parallel merge " << cur
                   << "\n");
        legal = false;
        break;
      }
      return rhs;
    };

    std::map<size_t, arith::AddIOp> addIndices;
    std::map<size_t, SmallVector<std::tuple<std::map<size_t, AffineExpr>,
                                            SmallVector<Value>, size_t>>>
        affineMapUsers;
    std::map<size_t, SmallVector<Operation *>> affineUsers;
    SmallVector<ValueOrInt> fixedUpperBounds;

    SetVector<size_t> CanonicalBounds;
    for (auto iv : op.getIVs()) {
      bool legal = true;

      for (auto lb : op.getLowerBoundMap(iv.getArgNumber()).getResults()) {
        if (auto cst = dyn_cast<AffineConstantExpr>(lb)) {
          if (cst.getValue() != 0) {
            LLVM_DEBUG(llvm::dbgs() << "Non-zero lower bound for iv "
                                    << iv.getArgNumber() << "\n");
            legal = false;
            break;
          }
        } else {
          LLVM_DEBUG(llvm::dbgs() << "Non-constant lower bound for iv "
                                  << iv.getArgNumber() << "\n");
          legal = false;
          break;
        }
      }
      auto ubMap = op.getUpperBoundMap(iv.getArgNumber());
      if (ubMap.getNumResults() == 1) {
        auto ub = ubMap.getResult(0);
        if (auto cst = dyn_cast<AffineConstantExpr>(ub)) {
          fixedUpperBounds.push_back(
              ValueOrInt(APInt(64, cst.getValue(), true)));
        } else if (auto dim = dyn_cast<AffineDimExpr>(ub)) {
          fixedUpperBounds.push_back(
              ValueOrInt(op.getUpperBoundsOperands()[dim.getPosition()]));
        } else if (auto sym = dyn_cast<AffineSymbolExpr>(ub)) {
          fixedUpperBounds.push_back(ValueOrInt(
              op.getUpperBoundsOperands()[op.getUpperBoundsMap().getNumDims() +
                                          sym.getPosition()]));
        } else {
          LLVM_DEBUG(llvm::dbgs() << "Non-constant upper bound for iv "
                                  << iv.getArgNumber() << "\n");
          legal = false;
          fixedUpperBounds.push_back(ValueOrInt(0));
        }
      } else {
        LLVM_DEBUG(llvm::dbgs() << "Non-single upper bound for iv "
                                << iv.getArgNumber() << "\n");
        fixedUpperBounds.push_back(ValueOrInt(0));
        legal = false;
      }

      if (legal)
        CanonicalBounds.insert(iv.getArgNumber());
    }

    std::map<size_t, SmallVector<Operation *>> illegalOps;
    SmallVector<Operation *> insertedOps;
    for (auto iv : op.getIVs()) {
      if (!CanonicalBounds.count(iv.getArgNumber()))
        continue;

      auto &illegal = illegalOps[iv.getArgNumber()];

      arith::AddIOp idxCst = nullptr;
      SmallVector<std::pair<Value, Operation *>> users;

      for (auto U : iv.getUsers()) {
        users.emplace_back(iv, U);
      }

      while (!users.empty()) {
        auto [val, U] = users.pop_back_val();
        SmallVector<AffineExpr> exprs;
        SmallVector<Value> operands;
        size_t numDims = 0;
        if (auto AL = dyn_cast<affine::AffineLoadOp>(U)) {
          operands = AL.getMapOperands();
          for (auto E : AL.getAffineMap().getResults()) {
            bool functionOf = false;
            for (size_t i = 0; i < operands.size(); i++) {
              if (operands[i] != iv)
                continue;
              if (i < AL.getAffineMap().getNumDims()) {
                functionOf |= E.isFunctionOfDim(i);
              } else {
                functionOf |=
                    E.isFunctionOfSymbol(i - AL.getAffineMap().getNumDims());
              }
            }
            if (functionOf)
              exprs.push_back(E);
          }
          numDims = AL.getAffineMap().getNumDims();
          affineUsers[iv.getArgNumber()].push_back(U);
        } else if (auto AS = dyn_cast<affine::AffineStoreOp>(U)) {
          if (AS.getValue() == iv) {
            illegal.push_back(nullptr); // legal = false;
            LLVM_DEBUG(llvm::dbgs()
                       << "Capturing user" << *U << " from " << val << "\n");
          }
          operands = AS.getMapOperands();
          for (auto E : AS.getAffineMap().getResults()) {
            bool functionOf = false;
            for (size_t i = 0; i < operands.size(); i++) {
              if (operands[i] != iv)
                continue;
              if (i < AS.getAffineMap().getNumDims()) {
                functionOf |= E.isFunctionOfDim(i);
              } else {
                functionOf |=
                    E.isFunctionOfSymbol(i - AS.getAffineMap().getNumDims());
              }
            }
            if (functionOf)
              exprs.push_back(E);
          }
          numDims = AS.getAffineMap().getNumDims();
          affineUsers[iv.getArgNumber()].push_back(U);
        } else if (auto AA = dyn_cast<affine::AffineApplyOp>(U)) {
          operands = AA.getMapOperands();
          for (auto E : AA.getMap().getResults()) {
            bool functionOf = false;
            for (size_t i = 0; i < operands.size(); i++) {
              if (operands[i] != iv)
                continue;
              if (i < AA.getMap().getNumDims()) {
                functionOf |= E.isFunctionOfDim(i);
              } else {
                functionOf |=
                    E.isFunctionOfSymbol(i - AA.getMap().getNumDims());
              }
            }
            if (functionOf)
              exprs.push_back(E);
          }
          numDims = AA.getMap().getNumDims();
          affineUsers[iv.getArgNumber()].push_back(U);
        } else if (auto AI = dyn_cast<affine::AffineIfOp>(U)) {
          operands = AI.getOperands();
          for (auto &&[E, isEqual] :
               llvm::zip_equal(AI.getIntegerSet().getConstraints(),
                               AI.getIntegerSet().getEqFlags())) {
            bool functionOf = false;
            for (size_t i = 0; i < operands.size(); i++) {
              if (operands[i] != iv)
                continue;
              if (i < AI.getIntegerSet().getNumDims()) {
                functionOf |= E.isFunctionOfDim(i);
              } else {
                functionOf |=
                    E.isFunctionOfSymbol(i - AI.getIntegerSet().getNumDims());
              }
            }
            if (functionOf) {
              // use of dim == 0 doesn't matter
              if (isEqual && isa<AffineDimExpr>(E)) {
                continue;
              }
              exprs.push_back(E);
            }
          }
          numDims = AI.getIntegerSet().getNumDims();
          affineUsers[iv.getArgNumber()].push_back(U);
        } else if (auto idx = dyn_cast<IndexCastOp>(U)) {
          for (auto U2 : idx->getUsers())
            users.emplace_back(idx, U2);
          continue;
        } else if (auto idx = dyn_cast<IndexCastUIOp>(U)) {
          for (auto U2 : idx->getUsers())
            users.emplace_back(idx, U2);
          continue;
        } else if (auto addOp = dyn_cast<arith::AddIOp>(U)) {
          if (idxCst) {
            illegal.push_back(nullptr);
            LLVM_DEBUG(llvm::dbgs()
                       << "Illegal add user " << *U << " from " << val << "\n");
            break;
          }

          idxCst = addOp;

          auto *scope = getLocalAffineScope(op)->getParentOp();
          DominanceInfo DI(scope);

          AffineExpr dimExprs[1] = {rewriter.getAffineSymbolExpr(0)};

          auto map = AffineMap::get(/*dimCount=*/0, /*symbolCount=*/1, dimExprs,
                                    rewriter.getContext());

          operands = {addOp->getResult(0)};
          fully2ComposeAffineMapAndOperands(rewriter, &map, &operands, DI,
                                            &insertedOps);

          exprs.push_back(map.getResult(0));
          numDims = map.getNumDims();
        } else {
          LLVM_DEBUG(llvm::dbgs() << "Illegal unknown user " << *U << " from "
                                  << val << "\n");
          illegal.push_back(U);
        }
        for (auto expr : exprs) {
          bool flegal = true;
          std::map<size_t, AffineExpr> indUsage;
          getIndUsage(expr, operands, indUsage, flegal);
          if (!flegal)
            LLVM_DEBUG(llvm::dbgs() << "Illegal indUsage expr: " << expr
                                    << " of " << *U << " from " << val << "\n");
          else if (indUsage.size() == 1)
            LLVM_DEBUG(llvm::dbgs() << "Single indUsage expr: " << expr
                                    << " of " << *U << " from " << val << "\n");
          if (!flegal || indUsage.size() == 1) {
            illegal.push_back(nullptr);
            break;
          }
          LLVM_DEBUG(llvm::dbgs() << "Legal indUsage expr: " << expr << " from "
                                  << val << "\n");
          affineMapUsers[iv.getArgNumber()].emplace_back(indUsage, operands,
                                                         numDims);
        }
      }
      if (idxCst)
        addIndices[iv.getArgNumber()] = idxCst;
    }
    for (auto &&[idx, illegal] : illegalOps) {
      if (illegal.size() == 0)
        continue;
      for (size_t i : CanonicalBounds) {
        if (!affineMapUsers.count(i))
          continue;
        bool hasInvalidUse = false;
        for (auto &&[indUsage, operands, numDims] : affineMapUsers[i])
          if (indUsage.count(idx))
            hasInvalidUse = true;
        if (hasInvalidUse) {
          bool onlyUsedInAdd = addIndices[i] != nullptr;
          if (onlyUsedInAdd)
            for (auto ilOp : illegal) {
              if (!ilOp || !isa<MulIOp, ShLIOp>(ilOp)) {
                onlyUsedInAdd = false;
                break;
              }
              if (ilOp->getResult(0) == addIndices[i].getLhs() ||
                  ilOp->getResult(0) == addIndices[i].getRhs()) {
                continue;
              }
              onlyUsedInAdd = false;
              break;
            }
          if (!onlyUsedInAdd) {
            LLVM_DEBUG(llvm::dbgs()
                       << "To merge operand has invalid use with: illegal idx="
                       << idx << " i=" << i << "\n");
            affineMapUsers.erase(i);
          }
        }
      }
    }

    for (auto &&pair : affineMapUsers) {
      if (illegalOps[pair.first].size())
        continue;
      if (pair.second.size() == 0)
        continue;
      auto &&[indUsage, operands, numDim] = pair.second[0];

      LLVM_DEBUG(llvm::dbgs()
                 << "Considering merge of affine pair: " << pair.first << "\n");

      // ivBeingAdded + ivBeingMuled * C
      // where ivBeingAdded = 0 ... C
      auto ivBeingAdded = pair.first;
      ssize_t ivBeingMuled = -1;
      size_t upperBound;
      if (fixedUpperBounds[ivBeingAdded].isValue)
        continue;
      upperBound = fixedUpperBounds[ivBeingAdded].i_val.getSExtValue();

      for (auto pair1 : indUsage) {
        // This expression is something of the form
        //    ivBeingAdded : A
        //    ivBeingMuled : A * B
        if (pair1.first == ivBeingAdded)
          continue;
        if (indUsage[ivBeingAdded] * upperBound == pair1.second) {
          ivBeingMuled = pair1.first;
          break;
        }
      }
      if (ivBeingMuled == -1)
        continue;

      // Don't merge with an upper with only one iteration, [this is required to
      // prevent infinte recursion].
      if (!fixedUpperBounds[ivBeingMuled].isValue &&
          fixedUpperBounds[ivBeingMuled].i_val == 1) {
        continue;
      }

      bool legalPair = true;
      for (auto &&[indUsage2, operands2, numDim2] : pair.second) {
        if (indUsage2[ivBeingAdded] * upperBound != indUsage2[ivBeingMuled]) {
          legalPair = false;
          break;
        }
      }
      if (!legalPair)
        continue;

      if (auto list = rewriter.getListener())
        for (auto op : insertedOps) {
          list->notifyOperationInserted(op, {});
        }
      SmallVector<int32_t> uboundGroup;
      for (auto U : op.getUpperBoundsGroups())
        uboundGroup.push_back(U.getZExtValue());

      SmallVector<AffineExpr> ubounds;

      for (auto e : op.getUpperBoundsMap().getResults()) {
        ubounds.push_back(e);
      }

      size_t off1 = 0;
      for (size_t i = 0; i < ivBeingAdded; i++)
        off1 += uboundGroup[i];
      size_t off2 = 0;
      for (size_t i = 0; i < ivBeingMuled; i++)
        off2 += uboundGroup[i];

      ubounds[off1] = ubounds[off1] * ubounds[off2];
      ubounds[off2] = getAffineConstantExpr(1, op.getContext());

      affine::AffineParallelOp affineLoop =
          rewriter.create<affine::AffineParallelOp>(
              op.getLoc(), op.getResultTypes(), op.getReductionsAttr(),
              op.getLowerBoundsMapAttr(), op.getLowerBoundsGroupsAttr(),
              AffineMapAttr::get(
                  AffineMap::get(op.getUpperBoundsMap().getNumDims(),
                                 op.getUpperBoundsMap().getNumSymbols(),
                                 ubounds, op.getContext())),
              op.getUpperBoundsGroupsAttr(), op.getStepsAttr(),
              op.getOperands());

      rewriter.inlineRegionBefore(op.getRegion(), affineLoop.getRegion(),
                                  affineLoop.getRegion().begin());
      rewriter.eraseOp(op);
      return success();
    }
    for (auto op : llvm::reverse(insertedOps)) {
      op->erase();
    }
    return failure();
  }
};

struct AddAddCstEnd : public OpRewritePattern<arith::AddIOp> {
  using OpRewritePattern<arith::AddIOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(arith::AddIOp op,
                                PatternRewriter &rewriter) const override {
    for (int i = 0; i < 2; i++) {
      auto val = op->getOperand(i).getDefiningOp<arith::AddIOp>();
      if (!val)
        continue;
      auto val2 = op->getOperand(1 - i);

      IntegerAttr iattr;
      if (matchPattern(val2, m_Constant(&iattr)))
        continue;

      if (!matchPattern(val.getRhs(), m_Constant(&iattr)))
        continue;

      auto tmp1 =
          rewriter.create<arith::AddIOp>(op.getLoc(), val2, val.getLhs());
      rewriter.replaceOpWithNewOp<arith::AddIOp>(op, tmp1, val.getRhs());
      return success();
    }
    return failure();
  }
};

// This function checks if given operands can be yielded instead of moved
// outside the if operation
// Checks:
// 1. If operand is a block argument
// 2. If operand is not in the same region as the if operation
// 3. If there is only 1 unique user of op
// 4. If the if and else operands are not of the same operation
// 5. If the operands are not readnone
// In any of these cases we can't propagate the operand outside the if operation
// and are yielded instead
bool isLegalToSinkYieldedValue(Value thenOperand, Value elseOperand,
                               affine::AffineIfOp ifOp) {
  for (auto operand : {thenOperand, elseOperand}) {
    auto defop = operand.getDefiningOp();
    if (!defop)
      return false;

    if (!ifOp->isAncestor(defop)) {
      if (!operand.hasOneUse() || ifOp->getBlock() != defop->getBlock()) {
        return false;
      }
    }

    if (!isReadNone(operand.getDefiningOp()))
      return false;

    if (operand.getDefiningOp()->getNumRegions())
      return false;
  }

  if (thenOperand.getDefiningOp()->getName() !=
      elseOperand.getDefiningOp()->getName())
    return false;

  if (thenOperand.getDefiningOp()->getAttrDictionary() !=
      elseOperand.getDefiningOp()->getAttrDictionary())
    return false;

  // Get defining operations
  auto thenOp = thenOperand.getDefiningOp();
  auto elseOp = elseOperand.getDefiningOp();

  // Check operand types match
  if (thenOp->getNumOperands() != elseOp->getNumOperands())
    return false;

  for (unsigned i = 0; i < thenOp->getNumOperands(); ++i) {
    if (thenOp->getOperand(i).getType() != elseOp->getOperand(i).getType())
      return false;
  }

  return true;
}

std::pair<Value, size_t> checkOperands(
    affine::AffineIfOp ifOp, Value operandIf, Value operandElse,
    llvm::MapVector<Operation *,
                    std::pair<Value, SmallVector<std::pair<Value, size_t>>>>
        &opsToMoveAfterIf,
    SmallVector<Value> &ifYieldOperands, SmallVector<Value> &elseYieldOperands,
    DenseMap<std::pair<Value, Value>, size_t> &thenOperationsToYieldIndex,
    PatternRewriter &rewriter) {

  if (operandIf == operandElse)
    return std::pair<Value, size_t>(operandIf, 0xdeadbeef);

  std::pair<Value, Value> key = {operandIf, operandElse};
  if (!isLegalToSinkYieldedValue(operandIf, operandElse, ifOp)) {
    if (!thenOperationsToYieldIndex.contains(key)) {
      thenOperationsToYieldIndex[key] = ifYieldOperands.size();
      ifYieldOperands.push_back(operandIf);
      elseYieldOperands.push_back(operandElse);
    }
    return std::pair<Value, size_t>(nullptr, thenOperationsToYieldIndex[key]);
  }

  Operation *opToMove = operandIf.getDefiningOp();

  auto foundAfterIf = opsToMoveAfterIf.find(opToMove);
  if (foundAfterIf != opsToMoveAfterIf.end()) {
    // We don't currently support the same if operand being moved after the if
    // when paired with a different instruction for the else
    if (foundAfterIf->second.first == operandElse)
      return std::pair<Value, size_t>(operandIf, 0xdeadbeef);
    else {
      if (!thenOperationsToYieldIndex.contains(key)) {
        thenOperationsToYieldIndex[key] = ifYieldOperands.size();
        ifYieldOperands.push_back(operandIf);
        elseYieldOperands.push_back(operandElse);
      }
      return std::pair<Value, size_t>(nullptr, thenOperationsToYieldIndex[key]);
    }
  }

  opsToMoveAfterIf.try_emplace(
      opToMove,
      std::make_pair(operandElse, SmallVector<std::pair<Value, size_t>>()));
  SmallVector<std::pair<Value, size_t>> newresults;

  for (auto [index, operands] : llvm::enumerate(
           llvm::zip_equal(operandIf.getDefiningOp()->getOperands(),
                           operandElse.getDefiningOp()->getOperands()))) {
    auto [thenOperand, elseOperand] = operands;
    newresults.push_back(checkOperands(
        ifOp, thenOperand, elseOperand, opsToMoveAfterIf, ifYieldOperands,
        elseYieldOperands, thenOperationsToYieldIndex, rewriter));
  }

  opsToMoveAfterIf[opToMove].second = std::move(newresults);

  return std::pair<Value, size_t>(operandIf, 0xdeadbeef);
}

// Forked from CanonicalizeFor
struct AffineIfYieldMovementPattern
    : public OpRewritePattern<affine::AffineIfOp> {
  using OpRewritePattern<affine::AffineIfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(affine::AffineIfOp ifOp,
                                PatternRewriter &rewriter) const override {
    // Ensure both regions exist and have single blocks
    if (ifOp.getThenRegion().empty() || ifOp.getElseRegion().empty())
      return failure();

    // Extract yield operations from both regions
    auto thenYield = cast<affine::AffineYieldOp>(
        ifOp.getThenRegion().front().getTerminator());
    auto elseYield = cast<affine::AffineYieldOp>(
        ifOp.getElseRegion().front().getTerminator());

    // List of replacement values for each of the original if's results
    // There are two kinds of replacements:
    //   1) A new value, which will be moved after the if statement
    //   2) if the value is null, the pair.second denotes the index of the new
    //   if
    //      statement that we should use here.
    SmallVector<std::pair<Value, size_t>> originalYields;

    // Use SetVector to ensure uniqueness while preserving order
    SmallVector<Value> ifYieldOperands, elseYieldOperands;
    llvm::MapVector<Operation *,
                    std::pair<Value, SmallVector<std::pair<Value, size_t>>>>
        opsToMoveAfterIf;

    // A list of operands defined within the if block, which have been promoted
    // to be yielded from the if statement. The size_t argument denotes the
    // index of the new if result which contains the value
    DenseMap<std::pair<Value, Value>, size_t> thenOperationsToYieldIndex;

    bool changed = false;

    for (auto [thenYieldOperand, elseYieldOperand] :
         llvm::zip(thenYield.getOperands(), elseYield.getOperands())) {

      auto yld =
          checkOperands(ifOp, thenYieldOperand, elseYieldOperand,
                        opsToMoveAfterIf, ifYieldOperands, elseYieldOperands,
                        thenOperationsToYieldIndex, rewriter);

      originalYields.emplace_back(yld);
      if (yld.first)
        changed = true;
    }

    // If no changes to yield operands, return failure
    if (!changed) {
      return failure();
    }

    // Create a new if operation with the same condition
    SmallVector<Type> resultTypes;

    // Cannot do unique, as unique might differ for if-else
    for (auto operand : ifYieldOperands) {
      resultTypes.push_back(operand.getType());
    }

    auto newIfOp = rewriter.create<affine::AffineIfOp>(
        ifOp.getLoc(), resultTypes, ifOp.getIntegerSet(), ifOp.getOperands(),
        /*hasElse=*/true);

    // Move operations from the original then block to the new then block

    rewriter.eraseBlock(&newIfOp.getThenRegion().front());
    if (ifOp.getElseRegion().getBlocks().size()) {
      rewriter.eraseBlock(&newIfOp.getElseRegion().front());
    }

    rewriter.inlineRegionBefore(ifOp.getThenRegion(), newIfOp.getThenRegion(),
                                newIfOp.getThenRegion().begin());
    rewriter.inlineRegionBefore(ifOp.getElseRegion(), newIfOp.getElseRegion(),
                                newIfOp.getElseRegion().begin());

    // Create new yield in then block
    {
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPointToEnd(newIfOp.getThenBlock());
      rewriter.create<affine::AffineYieldOp>(thenYield.getLoc(),
                                             ifYieldOperands);
      rewriter.eraseOp(thenYield);
    }

    // Create new yield in else block
    {
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPointToEnd(newIfOp.getElseBlock());
      rewriter.create<affine::AffineYieldOp>(elseYield.getLoc(),
                                             elseYieldOperands);
      rewriter.eraseOp(elseYield);
    }

    IRMapping mappingAfterIf;

    rewriter.setInsertionPointAfter(newIfOp);
    for (auto &op : ifOp->getBlock()->getOperations()) {
      if (&op == ifOp)
        break;
      if (opsToMoveAfterIf.find(&op) != opsToMoveAfterIf.end()) {
        SmallVector<Value> operands;
        for (auto &&[valoperand, idxop] : opsToMoveAfterIf[&op].second) {
          if (valoperand)
            operands.push_back(mappingAfterIf.lookupOrDefault(valoperand));
          else
            operands.push_back(newIfOp.getResult(idxop));
        }
        auto *newOp = rewriter.create(op.getLoc(), op.getName().getIdentifier(),
                                      operands, op.getResultTypes(),
                                      op.getAttrs(), op.getSuccessors());

        mappingAfterIf.map(&op, newOp);
        for (auto &&[prev, post] :
             llvm::zip_equal(op.getResults(), newOp->getResults()))
          mappingAfterIf.map(prev, post);
      }
    }
    for (auto &op : newIfOp.getThenBlock()->getOperations()) {
      if (opsToMoveAfterIf.find(&op) != opsToMoveAfterIf.end()) {
        SmallVector<Value> operands;
        for (auto &&[valoperand, idxop] : opsToMoveAfterIf[&op].second) {
          if (valoperand)
            operands.push_back(mappingAfterIf.lookupOrDefault(valoperand));
          else
            operands.push_back(newIfOp.getResult(idxop));
        }
        auto *newOp = rewriter.create(op.getLoc(), op.getName().getIdentifier(),
                                      operands, op.getResultTypes(),
                                      op.getAttrs(), op.getSuccessors());

        mappingAfterIf.map(&op, newOp);
        for (auto &&[prev, post] :
             llvm::zip_equal(op.getResults(), newOp->getResults()))
          mappingAfterIf.map(prev, post);
      }
    }

    // Replace uses of the original if operation with the new one
    SmallVector<Value> newResults;
    for (auto [idx, pair] : llvm::enumerate(originalYields)) {
      if (!pair.first) {
        newResults.push_back(newIfOp.getResult(pair.second));
      } else {
        newResults.push_back(mappingAfterIf.lookup(pair.first));
      }
    }

    // Erase yield operations of prev if operation
    rewriter.replaceOp(ifOp, newResults);
    return success();
  }
};

struct SinkStoreInIf : public OpRewritePattern<scf::IfOp> {
  using OpRewritePattern<scf::IfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(scf::IfOp ifOp,
                                PatternRewriter &rewriter) const override {
    // Ensure both regions exist and have single blocks
    if (ifOp.getThenRegion().empty() || ifOp.getElseRegion().empty())
      return failure();

    auto thenBlock = &ifOp.getThenRegion().front();
    auto elseBlock = &ifOp.getElseRegion().front();

    if (thenBlock->getOperations().size() < 2)
      return failure();
    if (elseBlock->getOperations().size() < 2)
      return failure();

    // Extract yield operations from both regions
    auto thenYield =
        cast<scf::YieldOp>(ifOp.getThenRegion().front().getTerminator());
    auto elseYield =
        cast<scf::YieldOp>(ifOp.getElseRegion().front().getTerminator());

    auto thenStore = dyn_cast<affine::AffineStoreOp>(thenYield->getPrevNode());
    if (!thenStore)
      return failure();

    auto elseStore = dyn_cast<affine::AffineStoreOp>(elseYield->getPrevNode());
    if (!elseStore)
      return failure();

    if (thenStore.getAffineMap() != elseStore.getAffineMap())
      return failure();
    if (thenStore.getMapOperands() != elseStore.getMapOperands())
      return failure();
    if (thenStore.getMemref() != elseStore.getMemref())
      return failure();

    // Use SetVector to ensure uniqueness while preserving order
    SmallVector<Value> ifYieldOperands, elseYieldOperands;

    for (auto [thenYieldOperand, elseYieldOperand] :
         llvm::zip(thenYield.getOperands(), elseYield.getOperands())) {
      ifYieldOperands.push_back(thenYieldOperand);
      elseYieldOperands.push_back(elseYieldOperand);
    }

    ifYieldOperands.push_back(thenStore.getValueToStore());
    elseYieldOperands.push_back(elseStore.getValueToStore());

    // Create a new if operation with the same condition
    SmallVector<Type> resultTypes;

    // Cannot do unique, as unique might differ for if-else
    for (auto operand : ifYieldOperands) {
      resultTypes.push_back(operand.getType());
    }

    auto newIfOp = rewriter.create<scf::IfOp>(ifOp.getLoc(), resultTypes,
                                              ifOp.getCondition(),
                                              /*hasElse=*/true);

    // Move operations from the original then block to the new then block

    rewriter.eraseBlock(&newIfOp.getThenRegion().front());
    if (ifOp.getElseRegion().getBlocks().size()) {
      rewriter.eraseBlock(&newIfOp.getElseRegion().front());
    }

    rewriter.inlineRegionBefore(ifOp.getThenRegion(), newIfOp.getThenRegion(),
                                newIfOp.getThenRegion().begin());
    rewriter.inlineRegionBefore(ifOp.getElseRegion(), newIfOp.getElseRegion(),
                                newIfOp.getElseRegion().begin());

    // Create new yield in then block
    {
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPointToEnd(newIfOp.thenBlock());
      rewriter.create<scf::YieldOp>(thenYield.getLoc(), ifYieldOperands);
      rewriter.eraseOp(thenYield);
    }

    // Create new yield in else block
    {
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPointToEnd(newIfOp.elseBlock());
      rewriter.create<scf::YieldOp>(elseYield.getLoc(), elseYieldOperands);
      rewriter.eraseOp(elseYield);
    }

    rewriter.create<affine::AffineStoreOp>(
        thenStore.getLoc(), newIfOp.getResult(ifOp.getNumResults()),
        thenStore.getMemref(), thenStore.getAffineMap(),
        thenStore.getMapOperands());

    rewriter.replaceOp(ifOp,
                       newIfOp.getResults().slice(0, ifOp.getNumResults()));
    rewriter.eraseOp(thenStore);
    rewriter.eraseOp(elseStore);
    return success();
  }
};

struct SinkStoreInAffineIf : public OpRewritePattern<affine::AffineIfOp> {
  using OpRewritePattern<affine::AffineIfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(affine::AffineIfOp ifOp,
                                PatternRewriter &rewriter) const override {
    // Ensure both regions exist and have single blocks
    if (ifOp.getThenRegion().empty() || ifOp.getElseRegion().empty())
      return failure();

    auto thenBlock = &ifOp.getThenRegion().front();
    auto elseBlock = &ifOp.getElseRegion().front();

    if (thenBlock->getOperations().size() < 2)
      return failure();
    if (elseBlock->getOperations().size() < 2)
      return failure();

    // Extract yield operations from both regions
    auto thenYield = cast<affine::AffineYieldOp>(
        ifOp.getThenRegion().front().getTerminator());
    auto elseYield = cast<affine::AffineYieldOp>(
        ifOp.getElseRegion().front().getTerminator());

    auto thenStore = dyn_cast<affine::AffineStoreOp>(thenYield->getPrevNode());
    if (!thenStore)
      return failure();

    auto elseStore = dyn_cast<affine::AffineStoreOp>(elseYield->getPrevNode());
    if (!elseStore)
      return failure();

    if (thenStore.getAffineMap() != elseStore.getAffineMap())
      return failure();
    if (thenStore.getMapOperands() != elseStore.getMapOperands())
      return failure();
    if (thenStore.getMemref() != elseStore.getMemref())
      return failure();

    // Use SetVector to ensure uniqueness while preserving order
    SmallVector<Value> ifYieldOperands, elseYieldOperands;

    for (auto [thenYieldOperand, elseYieldOperand] :
         llvm::zip(thenYield.getOperands(), elseYield.getOperands())) {
      ifYieldOperands.push_back(thenYieldOperand);
      elseYieldOperands.push_back(elseYieldOperand);
    }

    ifYieldOperands.push_back(thenStore.getValueToStore());
    elseYieldOperands.push_back(elseStore.getValueToStore());

    // Create a new if operation with the same condition
    SmallVector<Type> resultTypes;

    // Cannot do unique, as unique might differ for if-else
    for (auto operand : ifYieldOperands) {
      resultTypes.push_back(operand.getType());
    }

    auto newIfOp = rewriter.create<affine::AffineIfOp>(
        ifOp.getLoc(), resultTypes, ifOp.getIntegerSet(), ifOp.getOperands(),
        true);

    // Move operations from the original then block to the new then block

    rewriter.eraseBlock(&newIfOp.getThenRegion().front());
    if (ifOp.getElseRegion().getBlocks().size()) {
      rewriter.eraseBlock(&newIfOp.getElseRegion().front());
    }

    rewriter.inlineRegionBefore(ifOp.getThenRegion(), newIfOp.getThenRegion(),
                                newIfOp.getThenRegion().begin());
    rewriter.inlineRegionBefore(ifOp.getElseRegion(), newIfOp.getElseRegion(),
                                newIfOp.getElseRegion().begin());

    // Create new yield in then block
    {
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPointToEnd(newIfOp.getThenBlock());
      rewriter.create<affine::AffineYieldOp>(thenYield.getLoc(),
                                             ifYieldOperands);
      rewriter.eraseOp(thenYield);
    }

    // Create new yield in else block
    {
      OpBuilder::InsertionGuard guard(rewriter);
      rewriter.setInsertionPointToEnd(newIfOp.getElseBlock());
      rewriter.create<affine::AffineYieldOp>(elseYield.getLoc(),
                                             elseYieldOperands);
      rewriter.eraseOp(elseYield);
    }

    rewriter.create<affine::AffineStoreOp>(
        thenStore.getLoc(), newIfOp.getResult(ifOp.getNumResults()),
        thenStore.getMemref(), thenStore.getAffineMap(),
        thenStore.getMapOperands());

    rewriter.replaceOp(ifOp,
                       newIfOp.getResults().slice(0, ifOp.getNumResults()));
    rewriter.eraseOp(thenStore);
    rewriter.eraseOp(elseStore);
    return success();
  }
};

static bool definedOutside(Value v, Operation *op) {
  return !op->isAncestor(v.getParentBlock()->getParentOp());
}

/// Lift memref read depending on an scf.if into the body of that scf.if. This
/// proceeds by moving the operations in the backward slide of a `load` that
/// are dominated by the `if` into both the "then" and the "else" branch, as
/// long as all operations are pure.
class LiftMemrefRead : public OpRewritePattern<memref::LoadOp> {
public:
  using OpRewritePattern<memref::LoadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(memref::LoadOp loadOp,
                                PatternRewriter &rewriter) const override {
    SetVector<Operation *> backwardSlice;
    DominanceInfo dominance;
    BackwardSliceOptions options;
    if (getBackwardSlice(loadOp.getOperation(), &backwardSlice, options)
            .failed())
      return failure();

    Operation *conditional =
        llvm::find_singleton<Operation>(backwardSlice, [](Operation *op, bool) {
          return isa<affine::AffineIfOp, scf::IfOp>(op) ? op : nullptr;
        });
    if (!conditional || conditional->getRegion(1).empty() ||
        conditional->getNumResults() == 0)
      return rewriter.notifyMatchFailure(
          loadOp, "not dependent on a conditional result");

    auto toLift = llvm::filter_to_vector(backwardSlice, [&](Operation *op) {
      return dominance.properlyDominates(conditional, op);
    });

    SetVector<int> resultsNeeded;
    Operation *original_conditional = conditional;
    if (!llvm::all_of(toLift, isPure)) {
      auto trueYld = conditional->getRegion(0).front().getTerminator();
      auto falseYld = conditional->getRegion(1).front().getTerminator();
      Operation *postOp = nullptr;
      for (auto op : toLift) {
        if (op == conditional)
          continue;
        for (auto operand : op->getOperands()) {
          if (auto ores = dyn_cast<OpResult>(operand)) {
            if (ores.getOwner() == conditional) {
              if (postOp == nullptr)
                postOp = op;
              else if (dominance.dominates(op, postOp))
                postOp = op;
              auto rnum = ores.getResultNumber();
              resultsNeeded.insert(rnum);
              if (!definedOutside(trueYld->getOperand(rnum), conditional) ||
                  !definedOutside(falseYld->getOperand(rnum), conditional)) {
                return rewriter.notifyMatchFailure(
                    loadOp, "non-pure operation on the path");
              }
            }
          }
        }
      }
      assert(postOp);
      toLift = llvm::filter_to_vector(backwardSlice, [&](Operation *op) {
        return dominance.dominates(postOp, op);
      });
      if (!llvm::all_of(toLift, isPure)) {
        return rewriter.notifyMatchFailure(
            loadOp, "non-pure operation on the path (V2)");
      }
      for (auto op : toLift) {
        for (auto operand : op->getOperands()) {
          if (auto ba = dyn_cast<BlockArgument>(operand)) {
            if (!dominance.dominates(ba, postOp)) {
              return rewriter.notifyMatchFailure(
                  loadOp,
                  "block argument requirement not part dominating conditional");
            }
          }
        }
      }

      SmallVector<std::unique_ptr<Region>> regions;
      regions.emplace_back(new Region);
      regions.emplace_back(new Region);
      Block *tBlk = rewriter.createBlock(regions[0].get(), regions[0]->begin());
      Block *fBlk = rewriter.createBlock(regions[1].get(), regions[1]->begin());
      SmallVector<Value> trueResults, falseResults;
      SmallVector<Type> types;
      for (auto idx : resultsNeeded) {
        trueResults.push_back(trueYld->getOperand(idx));
        falseResults.push_back(falseYld->getOperand(idx));
        types.push_back(trueYld->getOperand(idx).getType());
      }

      if (isa<scf::IfOp>(conditional)) {
        rewriter.setInsertionPointToEnd(tBlk);
        rewriter.create<scf::YieldOp>(conditional->getLoc(), trueResults);
        rewriter.setInsertionPointToEnd(fBlk);
        rewriter.create<scf::YieldOp>(conditional->getLoc(), falseResults);
      } else {
        rewriter.setInsertionPointToEnd(tBlk);
        rewriter.create<affine::AffineYieldOp>(conditional->getLoc(),
                                               trueResults);
        rewriter.setInsertionPointToEnd(fBlk);
        rewriter.create<affine::AffineYieldOp>(conditional->getLoc(),
                                               falseResults);
      }
      rewriter.setInsertionPoint(postOp);
      auto conditional2 = rewriter.create(
          conditional->getLoc(), conditional->getName().getIdentifier(),
          conditional->getOperands(), types, conditional->getAttrs(),
          BlockRange(), regions);
      conditional = conditional2;
    } else {
      for (auto i = 0; i < conditional->getNumResults(); i++)
        resultsNeeded.insert(i);

      for (auto op : toLift) {
        for (auto operand : op->getOperands()) {
          if (auto ba = dyn_cast<BlockArgument>(operand)) {
            if (!dominance.dominates(ba, conditional)) {
              return rewriter.notifyMatchFailure(
                  loadOp,
                  "block argument requirement not part dominating conditional");
            }
          }
        }
      }
    }

    auto cloneIntoBlock = [&](unsigned blockNum) {
      IRMapping mapping;
      Block &targetBlock = conditional->getRegion(blockNum).front();
      for (auto iv : llvm::enumerate(resultsNeeded))
        mapping.map(original_conditional->getResults()[iv.value()],
                    targetBlock.getTerminator()->getOperands()[iv.index()]);
      rewriter.setInsertionPoint(targetBlock.getTerminator());
      for (Operation *op : toLift) {
        rewriter.clone(*op, mapping);
      }
      Operation *clonedLoad = rewriter.clone(*loadOp.getOperation(), mapping);
      return clonedLoad;
    };

    Operation *thenLoad = cloneIntoBlock(0);
    Operation *elseLoad = cloneIntoBlock(1);

    SmallVector<Type> types = llvm::to_vector(conditional->getResultTypes());
    llvm::append_range(types, thenLoad->getResultTypes());
    SmallVector<std::unique_ptr<Region>> regions;
    regions.emplace_back(new Region);
    regions.emplace_back(new Region);
    rewriter.setInsertionPoint(conditional);
    Operation *newConditional = rewriter.create(
        conditional->getLoc(), conditional->getName().getIdentifier(),
        conditional->getOperands(), types, conditional->getAttrs(),
        BlockRange(), regions);

    auto inlineBody = [&](unsigned regionNum, Operation *loadOp) {
      rewriter.inlineRegionBefore(conditional->getRegion(regionNum),
                                  newConditional->getRegion(regionNum),
                                  newConditional->getRegion(regionNum).begin());

      Operation *terminator =
          newConditional->getRegion(regionNum).front().getTerminator();
      SmallVector<Value> operands = llvm::to_vector(terminator->getOperands());
      llvm::append_range(operands, loadOp->getResults());
      rewriter.setInsertionPoint(terminator);
      Operation *newTerminator = rewriter.create(
          terminator->getLoc(), terminator->getName().getIdentifier(), operands,
          terminator->getResultTypes(), terminator->getAttrs(),
          terminator->getSuccessors());
      rewriter.replaceOp(terminator, newTerminator);
    };

    inlineBody(0, thenLoad);
    inlineBody(1, elseLoad);

    unsigned numLoadResults = loadOp->getNumResults();
    rewriter.replaceOp(loadOp,
                       newConditional->getResults().take_back(numLoadResults));
    rewriter.replaceOp(conditional,
                       newConditional->getResults().drop_back(numLoadResults));
    return success();
  }
};

template <typename Derived, typename BinOp>
struct FoldAffineApplyBase : public OpRewritePattern<BinOp> {
  using Super = FoldAffineApplyBase<Derived, BinOp>;
  using OpRewritePattern<BinOp>::OpRewritePattern;

  LogicalResult extractApply(BinOp binOp, affine::AffineApplyOp &apply,
                             Value &other, PatternRewriter &rewriter) const {
    apply = binOp.getLhs().template getDefiningOp<affine::AffineApplyOp>();
    other = binOp.getRhs();
    if (!apply) {
      apply = binOp.getRhs().template getDefiningOp<affine::AffineApplyOp>();
      other = binOp.getLhs();
    }
    if (!apply)
      return rewriter.notifyMatchFailure(binOp,
                                         "no affine.apply-defined operands");
    return success();
  }

  LogicalResult matchAndRewrite(BinOp binOp,
                                PatternRewriter &rewriter) const override {
    affine::AffineApplyOp apply;
    Value other;
    if (failed(static_cast<const Derived *>(this)->extractApply(
            binOp, apply, other, rewriter)))
      return failure();

    AffineExpr expr = apply.getMap().getResult(0);
    bool otherIsRHS = other == binOp.getRhs();
    if (affine::isValidSymbol(other)) {
      auto dimExpr = getAffineSymbolExpr(apply.getMap().getNumSymbols(),
                                         this->getContext());
      expr = static_cast<const Derived *>(this)->combineExprs(
          otherIsRHS ? expr : dimExpr, otherIsRHS ? dimExpr : expr);
      AffineMap updatedMap =
          AffineMap::get(apply.getMap().getNumDims(),
                         apply.getMap().getNumSymbols() + 1, expr);
      SmallVector<Value> operands = llvm::to_vector(apply->getOperands());
      operands.push_back(other);
      rewriter.replaceOpWithNewOp<affine::AffineApplyOp>(binOp, updatedMap,
                                                         operands);
      return success();
    }
    if (affine::isValidDim(other)) {
      auto dimExpr =
          getAffineDimExpr(apply.getMap().getNumDims(), this->getContext());
      expr = static_cast<const Derived *>(this)->combineExprs(
          otherIsRHS ? expr : dimExpr, otherIsRHS ? dimExpr : expr);
      AffineMap updatedMap =
          AffineMap::get(apply.getMap().getNumDims() + 1,
                         apply.getMap().getNumSymbols(), expr);
      SmallVector<Value> operands = llvm::to_vector(apply.getDimOperands());
      operands.push_back(other);
      llvm::append_range(operands, apply.getSymbolOperands());
      rewriter.replaceOpWithNewOp<affine::AffineApplyOp>(binOp, updatedMap,
                                                         operands);
      return success();
    }
    return failure();
  }
};

struct FoldAffineApplyAdd
    : FoldAffineApplyBase<FoldAffineApplyAdd, arith::AddIOp> {
  using Super::Super;

  AffineExpr combineExprs(AffineExpr lhs, AffineExpr rhs) const {
    return lhs + rhs;
  }
};

struct FoldAffineApplySub
    : FoldAffineApplyBase<FoldAffineApplySub, arith::SubIOp> {
  using Super::Super;

  AffineExpr combineExprs(AffineExpr lhs, AffineExpr rhs) const {
    return lhs - rhs;
  }
};

template <typename Derived, typename BinOp>
struct FoldAffineApplyConstRHS : public FoldAffineApplyBase<Derived, BinOp> {
  using FoldAffineApplyBase<Derived, BinOp>::FoldAffineApplyBase;
  using Super = FoldAffineApplyConstRHS<Derived, BinOp>;

  LogicalResult extractApply(BinOp binOp, affine::AffineApplyOp &apply,
                             Value &other, PatternRewriter &rewriter) const {
    apply = binOp.getLhs().template getDefiningOp<affine::AffineApplyOp>();
    other = binOp.getRhs();
    llvm::APInt ignore;
    return success(apply && matchPattern(other, m_ConstantInt(&ignore)));
  }
};

struct FoldAffineApplyDiv
    : FoldAffineApplyConstRHS<FoldAffineApplyDiv, arith::DivUIOp> {
  using Super::Super;

  AffineExpr combineExprs(AffineExpr lhs, AffineExpr rhs) const {
    return lhs.floorDiv(rhs);
  }
};

struct FoldAffineApplyRem
    : FoldAffineApplyConstRHS<FoldAffineApplyRem, arith::RemUIOp> {
  using Super::Super;

  AffineExpr combineExprs(AffineExpr lhs, AffineExpr rhs) const {
    return lhs % rhs;
  }
};

struct FoldAffineApplyMul
    : FoldAffineApplyBase<FoldAffineApplyMul, arith::MulIOp> {
  using Super::Super;

  LogicalResult extractApply(arith::MulIOp mulOp, affine::AffineApplyOp &apply,
                             Value &other, PatternRewriter &rewriter) const {
    if (failed(Super::extractApply(mulOp, apply, other, rewriter)))
      return failure();
    llvm::APInt ignore;
    return success(matchPattern(other, m_ConstantInt(&ignore)));
  }

  AffineExpr combineExprs(AffineExpr lhs, AffineExpr rhs) const {
    return lhs * rhs;
  }
};

struct FoldAppliesIntoLoad : public OpRewritePattern<memref::LoadOp> {
  using OpRewritePattern<memref::LoadOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(memref::LoadOp loadOp,
                                PatternRewriter &rewriter) const override {
    SmallVector<affine::AffineApplyOp> applies;
    for (Value index : loadOp.getIndices()) {
      applies.push_back(index.getDefiningOp<affine::AffineApplyOp>());
      if (!applies.back())
        return rewriter.notifyMatchFailure(loadOp,
                                           "operands is not an affine.apply");
    }

    SmallVector<Value> loadDimOperands, loadSymOperands;
    SmallVector<AffineExpr> exprs;
    for (affine::AffineApplyOp apply : applies) {
      AffineExpr expr = apply.getMap().getResult(0);
      expr = expr.shiftDims(apply.getMap().getNumDims(), loadDimOperands.size())
                 .shiftSymbols(apply.getMap().getNumSymbols(),
                               loadSymOperands.size());
      exprs.push_back(expr);
      llvm::append_range(loadDimOperands, apply.getDimOperands());
      llvm::append_range(loadSymOperands, apply.getSymbolOperands());
    }

    AffineMap combinedMap =
        AffineMap::inferFromExprList({exprs}, getContext())[0];
    llvm::append_range(loadDimOperands, loadSymOperands);
    rewriter.replaceOpWithNewOp<affine::AffineLoadOp>(
        loadOp, loadOp.getMemRef(), combinedMap, loadDimOperands);
    return success();
  }
};

struct CompareVs1 : public OpRewritePattern<arith::CmpIOp> {
  using OpRewritePattern<arith::CmpIOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(arith::CmpIOp cmpOp,
                                PatternRewriter &rewriter) const override {
    if (!matchPattern(cmpOp.getRhs(), m_One()))
      return failure();
    auto lhs = cmpOp.getLhs();
    if (auto cast = lhs.getDefiningOp<IndexCastOp>())
      lhs = cast.getOperand();
    if (auto cast = lhs.getDefiningOp<IndexCastUIOp>())
      lhs = cast.getOperand();

    auto barg = dyn_cast<BlockArgument>(lhs);
    if (!barg)
      return failure();

    auto oldpredicate = cmpOp.getPredicate();
    auto predicate = oldpredicate;
    switch (oldpredicate) {
    case arith::CmpIPredicate::ult:
    case arith::CmpIPredicate::slt:
      predicate = arith::CmpIPredicate::eq;
      break;
    case arith::CmpIPredicate::uge:
    case arith::CmpIPredicate::sge:
      predicate = arith::CmpIPredicate::ne;
      break;
    default:
      return failure();
    }

    auto par =
        dyn_cast<affine::AffineParallelOp>(barg.getOwner()->getParentOp());
    if (!par)
      return failure();

    for (auto iv : par.getIVs()) {
      if (iv != barg)
        continue;

      for (auto lb : par.getLowerBoundMap(iv.getArgNumber()).getResults()) {
        if (auto cst = dyn_cast<AffineConstantExpr>(lb)) {
          if (cst.getValue() != 0) {
            return failure();
          }
        } else {
          return failure();
        }
      }

      rewriter.replaceOpWithNewOp<arith::CmpIOp>(
          cmpOp, predicate, lhs,
          rewriter.create<arith::ConstantIndexOp>(cmpOp.getLoc(), 0));
      return success();
    }

    return failure();
  }
};

struct AffineForReductionIter : public OpRewritePattern<affine::AffineForOp> {
  using OpRewritePattern<affine::AffineForOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(affine::AffineForOp forOp,
                                PatternRewriter &rewriter) const override {
    auto limit = affine::getConstantTripCount(forOp);
    if (!limit)
      return failure();
    if ((*limit) == 0)
      return failure();
    Block *block = forOp.getBody();
    SmallVector<affine::AffineStoreOp> stores;
    block->walk([&](affine::AffineStoreOp store) {
      bool legal = store->getParentOp() == forOp;
      Value memref = store.getMemRef();
      if (!definedOutside(memref, forOp))
        legal = false;
      for (auto *user : memref.getUsers()) {
        if (user == store)
          continue;
        if (!forOp->isAncestor(user))
          continue;
        legal &= isReadOnly(user);
      }
      if (legal)
        stores.push_back(store);
    });
    SmallVector<std::pair<AffineStoreOp,
                          SmallVector<std::pair<AffineLoadOp, AffineMap>>>>
        todo;
    for (auto store : stores) {
      Value memref = store.getMemRef();
      SmallVector<std::pair<AffineLoadOp, AffineMap>> replacedLoads;
      for (auto *user : memref.getUsers()) {
        if (!forOp->isAncestor(user))
          continue;
        if (auto load = dyn_cast<affine::AffineLoadOp>(user)) {
          if (load.getMapOperands() != store.getMapOperands())
            continue;
          AffineMap loadMap = load.getAffineMap();
          bool legal = true;
          SmallVector<AffineExpr> dimReps;
          SmallVector<AffineExpr> dimReps2;
          SmallVector<AffineExpr> symReps;
          for (int i = 0; i < loadMap.getNumDims(); i++) {
            dimReps.push_back(rewriter.getAffineDimExpr(i));
            dimReps2.push_back(rewriter.getAffineDimExpr(i));
          }
          for (int i = 0; i < loadMap.getNumSymbols(); i++)
            symReps.push_back(rewriter.getAffineSymbolExpr(i));
          for (auto &&[i, val] : llvm::enumerate(load.getMapOperands())) {
            if (val == forOp.getInductionVar()) {
              if (i >= loadMap.getNumDims()) {
                legal = false;
                break;
              }
              dimReps[i] = dimReps[i] + rewriter.getAffineConstantExpr(1);
              dimReps2[i] = rewriter.getAffineConstantExpr(0);
            }
          }
          if (!legal)
            continue;
          auto loadMap2 = loadMap.replaceDimsAndSymbols(
              dimReps, symReps, loadMap.getNumDims(), loadMap.getNumSymbols());
          loadMap2 = simplifyAffineMap(loadMap2);
          if (store.getAffineMap() != loadMap2)
            continue;
          Operation *loadParen = load;
          while (loadParen->getParentOp() != forOp)
            loadParen = loadParen->getParentOp();
          if (!loadParen->isBeforeInBlock(store))
            continue;
          replacedLoads.emplace_back(
              load, loadMap.replaceDimsAndSymbols(dimReps2, symReps,
                                                  loadMap.getNumDims(),
                                                  loadMap.getNumSymbols()));
        }
      }
      if (replacedLoads.size() != 0)
        todo.emplace_back(store, replacedLoads);
    }

    if (!todo.size())
      return failure();

    SmallVector<Value, 4> newIterArgs;
    llvm::append_range(newIterArgs, forOp.getInits());
    rewriter.setInsertionPoint(forOp);
    IRMapping map;
    map.map(forOp.getInductionVar(),
            rewriter.create<arith::ConstantIndexOp>(forOp.getLoc(), 0));
    for (auto &&[store, loads] : todo) {
      auto movedLoad =
          cast<affine::AffineLoadOp>(rewriter.clone(*loads[0].first, map));
      movedLoad.setMap(loads[0].second);
      newIterArgs.push_back(movedLoad);
    }

    // create the for.
    affine::AffineForOp newForOp = rewriter.create<affine::AffineForOp>(
        forOp.getLoc(), forOp.getLowerBoundOperands(), forOp.getLowerBoundMap(),
        forOp.getUpperBoundOperands(), forOp.getUpperBoundMap(),
        forOp.getStep().getSExtValue(), newIterArgs);

    // remove load operation inside the for.
    size_t i = 0;
    size_t origNumRegionArgs = forOp.getNumRegionIterArgs();
    for (auto &&[store, loads] : todo) {
      auto arg = newForOp.getBody()->getArguments()[i + origNumRegionArgs + 1];
      for (auto &&[load, _] : loads) {
        rewriter.replaceOp(load, arg);
      }
      i++;
    }

    Block *newBlock = newForOp.getBody();
    Block *oldBlock = forOp.getBody();
    SmallVector<Value, 4> newBlockTransferArgs;
    newBlockTransferArgs.push_back(newForOp.getInductionVar());
    for (size_t i = 0; i < origNumRegionArgs; i++)
      newBlockTransferArgs.push_back(newForOp.getRegionIterArgs()[i]);
    assert(oldBlock->getNumArguments() == newBlockTransferArgs.size() &&
           "unexpected argument size mismatch");
    rewriter.mergeBlocks(oldBlock, newBlock, newBlockTransferArgs);

    auto cloneFilteredTerminator = [&](affine::AffineYieldOp mergedTerminator) {
      SmallVector<Value, 4> newOperands;
      llvm::append_range(newOperands, mergedTerminator.getOperands());
      for (auto &&[store, _] : todo) {
        newOperands.push_back(store.getValue());
      }
      mergedTerminator.getOperandsMutable().assign(newOperands);
    };

    auto mergedYieldOp = cast<affine::AffineYieldOp>(newBlock->getTerminator());
    cloneFilteredTerminator(mergedYieldOp);

    rewriter.replaceOp(forOp,
                       newForOp.getResults().slice(0, forOp.getNumResults()));
    return success();
  }
};

struct AffineForReductionSink : public OpRewritePattern<affine::AffineForOp> {
  using OpRewritePattern<affine::AffineForOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(affine::AffineForOp forOp,
                                PatternRewriter &rewriter) const override {
    auto limit = affine::getConstantTripCount(forOp);
    if (!limit)
      return failure();
    if ((*limit) == 0)
      return failure();
    if (forOp.getStep() != 1)
      return failure();
    auto ubMap = forOp.getUpperBoundMap();
    if (ubMap.getNumResults() != 1)
      return failure();
    auto ubExpr = dyn_cast<AffineConstantExpr>(ubMap.getResult(0));
    if (!ubExpr)
      return failure();
    auto ub = ubExpr.getValue();

    Block *block = forOp.getBody();
    SmallVector<affine::AffineStoreOp> stores;
    block->walk([&](affine::AffineStoreOp store) {
      bool legal = store->getParentOp() == forOp;
      Value memref = store.getMemRef();
      if (!definedOutside(memref, forOp))
        legal = false;
      for (auto *user : memref.getUsers()) {
        if (user == store)
          continue;
        if (!forOp->isAncestor(user))
          continue;
        legal = false;
      }
      if (legal)
        stores.push_back(store);
    });

    bool changed = false;
    for (auto store : stores) {
      Value val = store.getValue();
      affine::AffineYieldOp yld = nullptr;
      bool legal = true;
      size_t yldIdx = 0;
      for (auto &u : val.getUses()) {
        auto yldu = llvm::dyn_cast<AffineYieldOp>(u.getOwner());
        if (!yldu)
          continue;
        if (yld) {
          legal = false;
          break;
        }
        yld = yldu;
        yldIdx = u.getOperandNumber();
      }
      if (!yld)
        legal = false;
      if (!legal)
        continue;

      auto inp = forOp.getInits()[yldIdx].getDefiningOp<affine::AffineLoadOp>();
      if (!inp)
        continue;

      SmallVector<AffineExpr> dimReps;
      SmallVector<AffineExpr> dimReps2;
      SmallVector<AffineExpr> symReps;
      auto map = store.getAffineMap();
      for (int i = 0; i < map.getNumDims(); i++) {
        dimReps.push_back(rewriter.getAffineDimExpr(i));
        dimReps2.push_back(rewriter.getAffineDimExpr(i));
      }
      for (int i = 0; i < map.getNumSymbols(); i++)
        symReps.push_back(rewriter.getAffineSymbolExpr(i));

      for (auto &&[i, val] : llvm::enumerate(store.getMapOperands())) {
        if (val == forOp.getInductionVar()) {
          if (i >= map.getNumDims()) {
            legal = false;
            break;
          }
          dimReps[i] = rewriter.getAffineConstantExpr(0);
          dimReps2[i] = rewriter.getAffineConstantExpr(ub - 1);
        }
      }
      if (!legal)
        continue;

      auto loadMap = map.replaceDimsAndSymbols(
          dimReps, symReps, map.getNumDims(), map.getNumSymbols());
      loadMap = simplifyAffineMap(loadMap);
      if (store.getAffineMap() != loadMap)
        continue;

      auto storeMap2 = map.replaceDimsAndSymbols(
          dimReps2, symReps, map.getNumDims(), map.getNumSymbols());

      rewriter.modifyOpInPlace(store, [&]() {
        store.setMap(storeMap2);

        for (auto &&[i, val] : llvm::enumerate(store.getIndices())) {
          if (val == forOp.getInductionVar()) {
            store.getIndicesMutable()[i].assign(
                rewriter.create<arith::ConstantIndexOp>(store.getLoc(), 0));
          }
        }

        store->moveAfter(forOp);
        store.getValueMutable().set(forOp->getResult(yldIdx));
      });
      changed = true;
    }

    return success(changed);
  }
};

bool areOpposite(Value lhs, Value rhs) {
  if (auto xorOp = lhs.getDefiningOp<arith::XOrIOp>()) {
    if (xorOp.getLhs() == rhs && matchPattern(xorOp.getRhs(), m_One()))
      return true;
  }
  if (auto xorOp = rhs.getDefiningOp<arith::XOrIOp>()) {
    if (xorOp.getLhs() == lhs && matchPattern(xorOp.getRhs(), m_One()))
      return true;
  }
  return false;
}

struct SimplifyAndOr : public OpRewritePattern<arith::AndIOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(arith::AndIOp op,
                                PatternRewriter &rewriter) const override {

    for (int i = 0; i < 2; i++) {
      if (auto orOp = op->getOperand(i).getDefiningOp<arith::OrIOp>()) {
        for (int j = 0; j < 2; j++) {
          // and(a, or(a, b)) -> a
          if (orOp->getOperand(j) == op->getOperand(1 - i)) {
            rewriter.replaceOp(op, orOp->getOperand(j));
            return success();
          }
          // and(!a, or(a, b)) -> and(!a, b)
          if (areOpposite(orOp->getOperand(j), op->getOperand(1 - i))) {
            rewriter.modifyOpInPlace(
                op, [&]() { op->setOperand(i, orOp->getOperand(1 - j)); });
            return success();
          }
        }
      }
    }
    return failure();
  }
};

void mlir::enzyme::populateAffineCFGPatterns(RewritePatternSet &rpl) {
  MLIRContext *context = rpl.getContext();
  mlir::enzyme::addSingleIter(rpl, context);
  rpl.add</*SimplfyIntegerCastMath, */ CanonicalizeAffineApply, ForOpRaising,
          ParallelOpRaising, CanonicalizeIndexCast<IndexCastOp>,
          CanonicalizeIndexCast<IndexCastUIOp>, AffineIfYieldMovementPattern,
          /* IndexCastMovement,*/ AffineFixup<affine::AffineLoadOp>,
          AffineFixup<affine::AffineStoreOp>, CanonicalizIfBounds,
          MoveStoreToAffine, MoveIfToAffine, MoveLoadToAffine, MoveExtToAffine,
          MoveSIToFPToAffine, CmpExt, MoveSelectToAffine,
          AffineIfSimplification, AffineIfSimplificationIsl, CombineAffineIfs,
          MergeNestedAffineParallelLoops, PrepMergeNestedAffineParallelLoops,
          MergeNestedAffineParallelIf, MergeParallelInductions, OptimizeRem,
          CanonicalieForBounds, SinkStoreInIf, SinkStoreInAffineIf,
          AddAddCstEnd, LiftMemrefRead, CompareVs1, AffineForReductionIter,
          AffineForReductionSink>(context, 2);
  rpl.add<FoldAffineApplyAdd, FoldAffineApplySub, FoldAffineApplyRem,
          FoldAffineApplyDiv, FoldAffineApplyMul, FoldAppliesIntoLoad>(context,
                                                                       2);
  rpl.add<SimplifyAndOr>(context, 2);
  rpl.add<SplitParallelInductions>(context, 1);
}

void AffineCFGPass::runOnOperation() {
  mlir::RewritePatternSet rpl(getOperation()->getContext());
  populateAffineCFGPatterns(rpl);
  populateAffineParallelizationPattern(*getOperation()->getContext(), rpl);
  IslAnalysis islAnalysis;
  populateAffineExprSimplificationPatterns(islAnalysis, rpl);
  GreedyRewriteConfig config;
  if (failed(applyPatternsAndFoldGreedily(getOperation(), std::move(rpl),
                                          config))) {
    signalPassFailure();
  }
}

bool valueCmp(Cmp cmp, Value bval, ValueOrInt val) {
  if (auto icast = bval.getDefiningOp<IndexCastOp>()) {
    return valueCmp(cmp, icast.getIn(), val);
  }
  if (auto icast = bval.getDefiningOp<IndexCastUIOp>()) {
    return valueCmp(cmp, icast.getIn(), val);
  }

  IntegerAttr iattr;
  if (matchPattern(bval, m_Constant(&iattr))) {
    switch (cmp) {
    case Cmp::EQ:
      return val == iattr.getValue();
    case Cmp::LT:
      return val > iattr.getValue();
    case Cmp::LE:
      return val >= iattr.getValue();
    case Cmp::GT:
      return val < iattr.getValue();
    case Cmp::GE:
      return val <= iattr.getValue();
    }
  }

  if (cmp == Cmp::GE && !val.isValue && val.i_val == 0) {
    if (auto baval = bval.getDefiningOp<arith::AddIOp>()) {
      return valueCmp(cmp, baval.getLhs(), val) &&
             valueCmp(cmp, baval.getRhs(), val);
    }
    if (auto baval = bval.getDefiningOp<arith::MulIOp>()) {
      APInt ival;
      if (matchPattern(baval.getRhs(), m_ConstantInt(&ival))) {
        if (ival == 0)
          return true;
        if (ival.isStrictlyPositive())
          return valueCmp(cmp, baval.getLhs(), val);
        else
          return valueCmp(Cmp::LE, baval.getLhs(), val);
      }
    }
    if (auto baval = bval.getDefiningOp<arith::ShRUIOp>()) {
      return valueCmp(cmp, baval.getLhs(), val);
    }
    if (auto baval = bval.getDefiningOp<arith::ShLIOp>()) {
      return valueCmp(cmp, baval.getLhs(), val);
    }
    if (auto baval = bval.getDefiningOp<arith::DivUIOp>()) {
      return valueCmp(cmp, baval.getLhs(), val);
    }
  }

  if (auto baval = dyn_cast<BlockArgument>(bval)) {
    if (affine::AffineForOp afFor =
            dyn_cast<affine::AffineForOp>(baval.getOwner()->getParentOp())) {
      auto for_lb = afFor.getLowerBoundMap().getResults()[baval.getArgNumber()];
      auto for_ub = afFor.getUpperBoundMap().getResults()[baval.getArgNumber()];
      switch (cmp) {
      // \forall i \in [LB, UB) == k   => LB == k and UB == k+1
      case Cmp::EQ: {
        if (!valueCmp(Cmp::EQ, for_lb, afFor.getLowerBoundMap().getNumDims(),
                      afFor.getLowerBoundOperands(), val))
          return false;
        if (!val.isValue) {
          if (!valueCmp(Cmp::EQ, for_ub, afFor.getUpperBoundMap().getNumDims(),
                        afFor.getUpperBoundOperands(), val.i_val + 1))
            return false;
          return true;
        }
        return false;
      }
      // \forall i \in [LB, UB) < k   => UB <= k
      case Cmp::LT: {
        return valueCmp(Cmp::LE, for_ub, afFor.getUpperBoundMap().getNumDims(),
                        afFor.getUpperBoundOperands(), val);
      }
      // \forall i \in [LB, UB) <= k   => UB-1 <= k  => UB <= k+1
      case Cmp::LE: {
        if (!val.isValue) {
          return valueCmp(Cmp::LE, for_ub,
                          afFor.getUpperBoundMap().getNumDims(),
                          afFor.getUpperBoundOperands(), val.i_val + 1);
        }
        return valueCmp(Cmp::LE, for_ub, afFor.getUpperBoundMap().getNumDims(),
                        afFor.getUpperBoundOperands(), val);
      }
      // \forall i \in [LB, UB) > k   => LB > k
      case Cmp::GT: {
        return valueCmp(Cmp::GT, for_lb, afFor.getLowerBoundMap().getNumDims(),
                        afFor.getLowerBoundOperands(), val);
      }
      // \forall i \in [LB, UB) >= k   => LB >= k
      case Cmp::GE: {
        return valueCmp(Cmp::GE, for_lb, afFor.getLowerBoundMap().getNumDims(),
                        afFor.getLowerBoundOperands(), val);
      }
      }
    }
    if (affine::AffineParallelOp afFor = dyn_cast<affine::AffineParallelOp>(
            baval.getOwner()->getParentOp())) {
      switch (cmp) {
      // \forall i \in [max(LB...), min(UB...)) == k   => all(LB == k) and
      // all(UB == k+1)
      case Cmp::EQ: {
        for (auto for_lb :
             afFor.getLowerBoundMap(baval.getArgNumber()).getResults())
          if (!valueCmp(Cmp::EQ, for_lb, afFor.getLowerBoundsMap().getNumDims(),
                        afFor.getLowerBoundsOperands(), val))
            return false;
        if (!val.isValue) {
          for (auto for_ub :
               afFor.getUpperBoundMap(baval.getArgNumber()).getResults())
            if (!valueCmp(Cmp::EQ, for_ub,
                          afFor.getUpperBoundsMap().getNumDims(),
                          afFor.getUpperBoundsOperands(), val.i_val + 1))
              return false;
          return true;
        }
        return false;
      }
      // \forall i \in [max(LB...), min(UB...)) < k   => any(UB <= k)
      case Cmp::LT: {
        for (auto for_ub :
             afFor.getUpperBoundMap(baval.getArgNumber()).getResults())
          if (valueCmp(Cmp::LE, for_ub, afFor.getUpperBoundsMap().getNumDims(),
                       afFor.getUpperBoundsOperands(), val))
            return true;
        return false;
      }
      // \forall i \in [max(LB...), min(UB...)) <= k   => any(UB-1 <= k)  =>
      // any(UB <= k+1)
      case Cmp::LE: {
        if (!val.isValue) {
          for (auto for_ub :
               afFor.getUpperBoundMap(baval.getArgNumber()).getResults())
            if (valueCmp(Cmp::LE, for_ub,
                         afFor.getUpperBoundsMap().getNumDims(),
                         afFor.getUpperBoundsOperands(), val.i_val + 1))
              return true;
          return false;
        }

        for (auto for_ub :
             afFor.getUpperBoundMap(baval.getArgNumber()).getResults())
          if (valueCmp(Cmp::LE, for_ub, afFor.getUpperBoundsMap().getNumDims(),
                       afFor.getUpperBoundsOperands(), val))
            return true;
        return false;
      }
      // \forall i \in [max(LB...), min(UB...)) > k   => any(LB > k)
      case Cmp::GT: {
        for (auto for_lb :
             afFor.getLowerBoundMap(baval.getArgNumber()).getResults())
          if (valueCmp(Cmp::GT, for_lb, afFor.getLowerBoundsMap().getNumDims(),
                       afFor.getLowerBoundsOperands(), val))
            return true;
        return false;
      }
      // \forall i \in [max(LB...), min(UB...)) >= k   => any(LB >= k)
      case Cmp::GE: {
        for (auto for_lb :
             afFor.getLowerBoundMap(baval.getArgNumber()).getResults())
          if (valueCmp(Cmp::GE, for_lb, afFor.getLowerBoundsMap().getNumDims(),
                       afFor.getLowerBoundsOperands(), val))
            return true;
        return false;
      }
      }
    }

    if (scf::ForOp afFor =
            dyn_cast<scf::ForOp>(baval.getOwner()->getParentOp())) {
      if (baval.getArgNumber() == 0) {
        auto for_lb = afFor.getLowerBound();
        auto for_ub = afFor.getUpperBound();
        switch (cmp) {
        // \forall i \in [LB, UB) == k   => LB == k and UB == k+1
        case Cmp::EQ: {
          if (!valueCmp(Cmp::EQ, for_lb, val))
            return false;
          if (!val.isValue) {
            if (!valueCmp(Cmp::EQ, for_ub, val.i_val + 1))
              return false;
            return true;
          }
          return false;
        }
        // \forall i \in [LB, UB) < k   => UB <= k
        case Cmp::LT: {
          return valueCmp(Cmp::LE, for_ub, val);
        }
        // \forall i \in [LB, UB) <= k   => UB-1 <= k  => UB <= k+1
        case Cmp::LE: {
          if (!val.isValue) {
            return valueCmp(Cmp::LE, for_ub, val.i_val + 1);
          }
          return valueCmp(Cmp::LE, for_ub, val);
        }
        // \forall i \in [LB, UB) > k   => LB > k
        case Cmp::GT: {
          return valueCmp(Cmp::GT, for_lb, val);
        }
        // \forall i \in [LB, UB) >= k   => LB >= k
        case Cmp::GE: {
          return valueCmp(Cmp::GE, for_lb, val);
        }
        }
      }
    }

    if (scf::ParallelOp afFor =
            dyn_cast<scf::ParallelOp>(baval.getOwner()->getParentOp())) {
      auto for_lb = afFor.getLowerBound()[baval.getArgNumber()];
      auto for_ub = afFor.getUpperBound()[baval.getArgNumber()];
      switch (cmp) {
      // \forall i \in [LB, UB) == k   => LB == k and UB == k+1
      case Cmp::EQ: {
        if (!valueCmp(Cmp::EQ, for_lb, val))
          return false;
        if (!val.isValue) {
          if (!valueCmp(Cmp::EQ, for_ub, val.i_val + 1))
            return false;
          return true;
        }
        return false;
      }
      // \forall i \in [LB, UB) < k   => UB <= k
      case Cmp::LT: {
        return valueCmp(Cmp::LE, for_ub, val);
      }
      // \forall i \in [LB, UB) <= k   => UB-1 <= k  => UB <= k+1
      case Cmp::LE: {
        if (!val.isValue) {
          return valueCmp(Cmp::LE, for_ub, val.i_val + 1);
        }
        return valueCmp(Cmp::LE, for_ub, val);
      }
      // \forall i \in [LB, UB) > k   => LB > k
      case Cmp::GT: {
        return valueCmp(Cmp::GT, for_lb, val);
      }
      // \forall i \in [LB, UB) >= k   => LB >= k
      case Cmp::GE: {
        return valueCmp(Cmp::GE, for_lb, val);
      }
      }
    }
  }
  if (val.isValue && val.v_val == bval) {
    switch (cmp) {
    case Cmp::EQ:
      return true;
    case Cmp::LT:
      return false;
    case Cmp::LE:
      return true;
    case Cmp::GT:
      return false;
    case Cmp::GE:
      return true;
    }
  }
  return false;
}

bool valueCmp(Cmp cmp, ValueOrInt expr, ValueOrInt val) {
  if (expr.isValue)
    return valueCmp(cmp, expr.v_val, val);
  else {
    return valueCmp(cmp, expr.i_val, val);
  }
}

bool valueCmp(Cmp cmp, ValueOrInt expr, int64_t val) {
  return valueCmp(cmp, expr, APInt(64, val, true));
}

bool valueCmp(Cmp cmp, APInt expr, ValueOrInt val) {
  switch (cmp) {
  case Cmp::EQ:
    return val == expr;
  case Cmp::LT:
    return val > expr;
  case Cmp::LE:
    return val >= expr;
  case Cmp::GT:
    return val < expr;
  case Cmp::GE:
    return val <= expr;
  }
}

bool valueCmp(Cmp cmp, AffineExpr expr, size_t numDim, ValueRange operands,
              int64_t val) {
  return valueCmp(cmp, expr, numDim, operands, APInt(64, val, true));
}

bool valueCmp(Cmp cmp, AffineExpr expr, size_t numDim, ValueRange operands,
              ValueOrInt val) {

  if (auto opd = dyn_cast<AffineConstantExpr>(expr)) {
    switch (cmp) {
    case Cmp::EQ:
      return val == opd.getValue();
    case Cmp::LT:
      return val > opd.getValue();
    case Cmp::LE:
      return val >= opd.getValue();
    case Cmp::GT:
      return val < opd.getValue();
    case Cmp::GE:
      return val <= opd.getValue();
    }
  }
  if (auto opd = dyn_cast<AffineDimExpr>(expr)) {
    return valueCmp(cmp, operands[opd.getPosition()], val);
  }
  if (auto opd = dyn_cast<AffineSymbolExpr>(expr)) {
    return valueCmp(cmp, operands[opd.getPosition() + numDim], val);
  }

  if (auto bop = dyn_cast<AffineBinaryOpExpr>(expr)) {
    if (bop.getKind() == AffineExprKind::Add) {
      switch (cmp) {
      case Cmp::EQ:
        return (valueCmp(cmp, bop.getLHS(), numDim, operands, val) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, 0)) ||
               (valueCmp(cmp, bop.getLHS(), numDim, operands, 0) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, val));
      case Cmp::LT:
        return (valueCmp(cmp, bop.getLHS(), numDim, operands, val) &&
                valueCmp(Cmp::LE, bop.getRHS(), numDim, operands, 0)) ||
               (valueCmp(Cmp::LE, bop.getLHS(), numDim, operands, 0) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, val)) ||
               (valueCmp(Cmp::LE, bop.getLHS(), numDim, operands, val) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, 0)) ||
               (valueCmp(cmp, bop.getLHS(), numDim, operands, 0) &&
                valueCmp(Cmp::LE, bop.getRHS(), numDim, operands, val));
      case Cmp::LE:
        return (valueCmp(cmp, bop.getLHS(), numDim, operands, val) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, 0)) ||
               (valueCmp(cmp, bop.getLHS(), numDim, operands, 0) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, val));
      case Cmp::GT:
        return (valueCmp(cmp, bop.getLHS(), numDim, operands, val) &&
                valueCmp(Cmp::GE, bop.getRHS(), numDim, operands, 0)) ||
               (valueCmp(Cmp::GE, bop.getLHS(), numDim, operands, 0) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, val)) ||
               (valueCmp(Cmp::GE, bop.getLHS(), numDim, operands, val) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, 0)) ||
               (valueCmp(cmp, bop.getLHS(), numDim, operands, 0) &&
                valueCmp(Cmp::GE, bop.getRHS(), numDim, operands, val));
      case Cmp::GE:
        return (valueCmp(cmp, bop.getLHS(), numDim, operands, val) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, 0)) ||
               (valueCmp(cmp, bop.getLHS(), numDim, operands, 0) &&
                valueCmp(cmp, bop.getRHS(), numDim, operands, val));
      }
    }
    if (bop.getKind() == AffineExprKind::Mul && val == 0) {
      switch (cmp) {
      case Cmp::EQ:
        return (valueCmp(cmp, bop.getLHS(), numDim, operands, val) ||
                valueCmp(cmp, bop.getRHS(), numDim, operands, val));
      case Cmp::LT:
        return (valueCmp(Cmp::LT, bop.getLHS(), numDim, operands, val) &&
                valueCmp(Cmp::GT, bop.getRHS(), numDim, operands, 0)) ||
               (valueCmp(Cmp::GT, bop.getLHS(), numDim, operands, 0) &&
                valueCmp(Cmp::LT, bop.getRHS(), numDim, operands, val));
      case Cmp::LE:
        return valueCmp(Cmp::EQ, bop.getLHS(), numDim, operands, val) ||
               valueCmp(Cmp::EQ, bop.getRHS(), numDim, operands, val) ||
               ((valueCmp(Cmp::GE, bop.getLHS(), numDim, operands, 0) &&
                 valueCmp(Cmp::LE, bop.getRHS(), numDim, operands, val)) ||
                (valueCmp(Cmp::LE, bop.getLHS(), numDim, operands, 0) &&
                 valueCmp(Cmp::GE, bop.getRHS(), numDim, operands, val)));
      case Cmp::GT:
        return (valueCmp(Cmp::LT, bop.getLHS(), numDim, operands, val) &&
                valueCmp(Cmp::LT, bop.getRHS(), numDim, operands, 0)) ||
               (valueCmp(Cmp::GT, bop.getLHS(), numDim, operands, 0) &&
                valueCmp(Cmp::GT, bop.getRHS(), numDim, operands, val));
      case Cmp::GE:
        return valueCmp(Cmp::EQ, bop.getLHS(), numDim, operands, val) ||
               valueCmp(Cmp::EQ, bop.getRHS(), numDim, operands, val) ||
               ((valueCmp(Cmp::GE, bop.getLHS(), numDim, operands, 0) &&
                 valueCmp(Cmp::GE, bop.getRHS(), numDim, operands, val)) ||
                (valueCmp(Cmp::LE, bop.getLHS(), numDim, operands, 0) &&
                 valueCmp(Cmp::LE, bop.getRHS(), numDim, operands, val)));
      }
    }
    if ((bop.getKind() == AffineExprKind::Mod ||
         bop.getKind() == AffineExprKind::FloorDiv) &&
        val == 0 && isa<AffineConstantExpr>(bop.getRHS()) &&
        cast<AffineConstantExpr>(bop.getRHS()).getValue() > 0) {
      switch (cmp) {
      case Cmp::GE:
        return valueCmp(cmp, bop.getLHS(), numDim, operands, val);
      default:
        break;
      }
    }
  }
  return false;
}

// ------------------------- Vendored -------------------------

/// Returns true if `v` is allocated locally to `enclosingOp` -- i.e., it is
/// allocated by an operation nested within `enclosingOp`.
static bool isLocallyDefined(Value v, Operation *enclosingOp) {
  Operation *defOp = v.getDefiningOp();
  if (!defOp)
    return false;

  if (hasSingleEffect<MemoryEffects::Allocate>(defOp, v) &&
      enclosingOp->isProperAncestor(defOp))
    return true;

  // Aliasing ops.
  auto viewOp = dyn_cast<ViewLikeOpInterface>(defOp);
  return viewOp && isLocallyDefined(viewOp.getViewSource(), enclosingOp);
}

/// Returns the nesting depth of this statement, i.e., the number of loops
/// surrounding this statement.
static unsigned getNestingDepth(Operation *op) {
  Operation *currOp = op;
  unsigned depth = 0;
  while ((currOp = currOp->getParentOp())) {
    if (isa<AffineForOp>(currOp))
      depth++;
    if (auto parOp = dyn_cast<AffineParallelOp>(currOp))
      depth += parOp.getNumDims();
  }
  return depth;
}

static bool isLoopMemoryParallel(AffineForOp forOp) {
  // Any memref-typed iteration arguments are treated as serializing.
  if (llvm::any_of(forOp.getResultTypes(), llvm::IsaPred<BaseMemRefType>))
    return false;

  // Collect all load and store ops in loop nest rooted at 'forOp'.
  SmallVector<Operation *, 8> loadAndStoreOps;
  auto walkResult = forOp.walk([&](Operation *op) -> WalkResult {
    if (auto readOp = dyn_cast<AffineReadOpInterface>(op)) {
      // Memrefs that are allocated inside `forOp` need not be considered.
      if (!isLocallyDefined(readOp.getMemRef(), forOp))
        loadAndStoreOps.push_back(op);
    } else if (auto writeOp = dyn_cast<AffineWriteOpInterface>(op)) {
      // Filter out stores the same way as above.
      if (!isLocallyDefined(writeOp.getMemRef(), forOp))
        loadAndStoreOps.push_back(op);
    } else if (!isa<AffineForOp, AffineYieldOp, AffineIfOp>(op) &&
               !hasSingleEffect<MemoryEffects::Allocate>(op) &&
               !isMemoryEffectFree(op)) {
      // Alloc-like ops inside `forOp` are fine (they don't impact parallelism)
      // as long as they don't escape the loop (which has been checked above).
      return WalkResult::interrupt();
    }

    return WalkResult::advance();
  });

  // Stop early if the loop has unknown ops with side effects.
  if (walkResult.wasInterrupted())
    return false;

  // Dep check depth would be number of enclosing loops + 1.
  unsigned depth = ::getNestingDepth(forOp) + 1;

  // Check dependences between all pairs of ops in 'loadAndStoreOps'.
  for (auto *srcOp : loadAndStoreOps) {
    MemRefAccess srcAccess(srcOp);
    for (auto *dstOp : loadAndStoreOps) {
      MemRefAccess dstAccess(dstOp);
      DependenceResult result =
          checkMemrefAccessDependence(srcAccess, dstAccess, depth);
      if (result.value != DependenceResult::NoDependence)
        return false;
    }
  }
  return true;
}

/// Returns true if `forOp' is a parallel loop. If `parallelReductions` is
/// provided, populates it with descriptors of the parallelizable reductions and
/// treats them as not preventing parallelization.
static bool isLoopParallel(AffineForOp forOp,
                           SmallVectorImpl<LoopReduction> *parallelReductions) {
  unsigned numIterArgs = forOp.getNumIterOperands();

  // Loop is not parallel if it has SSA loop-carried dependences and reduction
  // detection is not requested.
  if (numIterArgs > 0 && !parallelReductions)
    return false;

  // Find supported reductions of requested.
  if (parallelReductions) {
    getSupportedReductions(forOp, *parallelReductions);
    // Return later to allow for identifying all parallel reductions even if the
    // loop is not parallel.
    if (parallelReductions->size() != numIterArgs)
      return false;
  }

  // Check memory dependences.
  return ::isLoopMemoryParallel(forOp);
}

/// Returns the closest surrounding block common to `opA` and `opB`. `opA` and
/// `opB` should be in the same affine scope. Returns nullptr if such a block
/// does not exist (when the two ops are in different blocks of an op starting
/// an `AffineScope`).
static Block *getCommonBlockInAffineScope(Operation *opA, Operation *opB) {
  // Get the chain of ancestor blocks for the given `MemRefAccess` instance. The
  // chain extends up to and includnig an op that starts an affine scope.
  auto getChainOfAncestorBlocks =
      [&](Operation *op, SmallVectorImpl<Block *> &ancestorBlocks) {
        Block *currBlock = op->getBlock();
        // Loop terminates when the currBlock is nullptr or its parent operation
        // holds an affine scope.
        while (currBlock &&
               !currBlock->getParentOp()->hasTrait<OpTrait::AffineScope>()) {
          ancestorBlocks.push_back(currBlock);
          currBlock = currBlock->getParentOp()->getBlock();
        }
        assert(currBlock &&
               "parent op starting an affine scope is always expected");
        ancestorBlocks.push_back(currBlock);
      };

  // Find the closest common block.
  SmallVector<Block *, 4> srcAncestorBlocks, dstAncestorBlocks;
  getChainOfAncestorBlocks(opA, srcAncestorBlocks);
  getChainOfAncestorBlocks(opB, dstAncestorBlocks);

  Block *commonBlock = nullptr;
  for (int i = srcAncestorBlocks.size() - 1, j = dstAncestorBlocks.size() - 1;
       i >= 0 && j >= 0 && srcAncestorBlocks[i] == dstAncestorBlocks[j];
       i--, j--)
    commonBlock = srcAncestorBlocks[i];

  return commonBlock;
}

/// Returns true if the ancestor operation of 'srcAccess' appears before the
/// ancestor operation of 'dstAccess' in their common ancestral block. The
/// operations for `srcAccess` and `dstAccess` are expected to be in the same
/// affine scope and have a common surrounding block within it.
static bool srcAppearsBeforeDstInAncestralBlock(const MemRefAccess &srcAccess,
                                                const MemRefAccess &dstAccess) {
  // Get Block common to 'srcAccess.opInst' and 'dstAccess.opInst'.
  Block *commonBlock =
      getCommonBlockInAffineScope(srcAccess.opInst, dstAccess.opInst);
  assert(commonBlock &&
         "ops expected to have a common surrounding block in affine scope");

  // Check the dominance relationship between the respective ancestors of the
  // src and dst in the Block of the innermost among the common loops.
  Operation *srcOp = commonBlock->findAncestorOpInBlock(*srcAccess.opInst);
  assert(srcOp && "src access op must lie in common block");
  Operation *dstOp = commonBlock->findAncestorOpInBlock(*dstAccess.opInst);
  assert(dstOp && "dest access op must lie in common block");

  // Determine whether dstOp comes after srcOp.
  return srcOp->isBeforeInBlock(dstOp);
}

// ------------------------- Vendored end -------------------------

enum class DepType { RAW, WAR, RAR, WAW };

static DepType getDepType(MemRefAccess src, MemRefAccess dst) {
  bool srcW = isa<AffineWriteOpInterface>(src.opInst);
  bool dstW = isa<AffineWriteOpInterface>(dst.opInst);
  if (srcW && dstW)
    return DepType::WAW;
  if (srcW && !dstW)
    return DepType::RAW;
  if (!srcW && dstW)
    return DepType::WAR;
  return DepType::RAR;
}

static bool isLoopMemoryLockStepExecutable(AffineForOp forOp) {
  // Any memref-typed iteration arguments are treated as serializing.
  if (llvm::any_of(forOp.getResultTypes(), llvm::IsaPred<BaseMemRefType>))
    return false;

  // Collect all load and store ops in loop nest rooted at 'forOp'.
  SmallVector<Operation *> loadAndStoreOps;
  auto walkResult = forOp.walk([&](Operation *op) -> WalkResult {
    if (auto readOp = dyn_cast<AffineReadOpInterface>(op)) {
      // Memrefs that are allocated inside `forOp` need not be considered.
      if (!isLocallyDefined(readOp.getMemRef(), forOp))
        loadAndStoreOps.push_back(op);
    } else if (auto writeOp = dyn_cast<AffineWriteOpInterface>(op)) {
      // Filter out stores the same way as above.
      if (!isLocallyDefined(writeOp.getMemRef(), forOp))
        loadAndStoreOps.push_back(op);
    } else if (!isa<AffineForOp, AffineYieldOp, AffineIfOp>(op) &&
               !isReadNone(op)) {
      return WalkResult::interrupt();
    }

    return WalkResult::advance();
  });

  // Stop early if the loop has unknown ops with side effects.
  if (walkResult.wasInterrupted())
    return false;

  // Dep check depth would be number of enclosing loops + 1.
  unsigned depth = ::getNestingDepth(forOp) + 1;

  // Check dependences between all pairs of ops in 'loadAndStoreOps'.
  for (auto *srcOp : loadAndStoreOps) {
    MemRefAccess srcAccess(srcOp);
    for (auto *dstOp : loadAndStoreOps) {
      LLVM_DEBUG(llvm::dbgs() << "Checking dep\n"
                              << "src: " << *srcOp << "\n"
                              << "dst: " << *dstOp << "\n");
      MemRefAccess dstAccess(dstOp);
      SmallVector<DependenceComponent, 2> dcs;
      DependenceResult result = checkMemrefAccessDependence(
          srcAccess, dstAccess, depth, nullptr, &dcs);

      if (result.value == DependenceResult::Failure) {
        LLVM_DEBUG(llvm::dbgs() << "Failed\n");
        return false;
      }

      // I haven't thought through the logic in this case, conservatively fail
      // for now.
      bool eitherNestedInNestedFor =
          dstOp->getParentOfType<affine::AffineForOp>() != forOp ||
          srcOp->getParentOfType<affine::AffineForOp>() != forOp;
      if (eitherNestedInNestedFor)
        return false;

      if (srcOp == dstOp) {
        // Since we will be executing different iterations of the same
        // instruction at the same time in lock step fashion, any dependence
        // here is illegal.
        if (result.value == DependenceResult::HasDependence) {
          LLVM_DEBUG(llvm::dbgs()
                     << "Would break dependence on same instruction\n");
          return false;
        }
      }

      // We will execute dst -> src in lock step
      if (!srcAppearsBeforeDstInAncestralBlock(srcAccess, dstAccess)) {
        // If there is any dependence src -> dst it means we will break it under
        // lock step execution.
        if (result.value == DependenceResult::HasDependence) {
          auto ty = getDepType(srcAccess, dstAccess);
          // Breaking a WAR dependency is fine because our lock step reads will
          // result in the correct value being read.
          if (ty == DepType::WAR) {
            LLVM_DEBUG(llvm::dbgs() << "WAR allowed\n");
          } else if (ty == DepType::RAR) {
            LLVM_DEBUG(llvm::dbgs() << "RAR allowed\n");
          } else if (ty == DepType::WAW) {
            LLVM_DEBUG(llvm::dbgs() << "WAW not allowed\n");
            return false;
          } else if (ty == DepType::RAW) {
            LLVM_DEBUG(llvm::dbgs() << "RAW not allowed\n");
            return false;
          }
        }
      }
    }
  }
  return true;
}

bool isLoopLockStepExecutable(
    AffineForOp forOp, SmallVectorImpl<LoopReduction> *parallelReductions) {
  unsigned numIterArgs = forOp.getNumIterOperands();

  if (numIterArgs > 0 && !parallelReductions)
    return false;

  return ::isLoopMemoryLockStepExecutable(forOp);
}

struct AffineParallelizePattern : public OpRewritePattern<affine::AffineForOp> {

  AffineParallelizePattern(bool parallelReductions, MLIRContext *context)
      : OpRewritePattern(context), parallelReductions(parallelReductions) {}

  LogicalResult matchAndRewrite(affine::AffineForOp forOp,
                                PatternRewriter &rewriter) const override {
    SmallVector<LoopReduction> reductions;
    if (!::isLoopParallel(forOp, parallelReductions ? &reductions : nullptr))
      return rewriter.notifyMatchFailure(forOp, "!isLoopParallel");

    // Fail early if there are iter arguments that are not reductions.
    unsigned numReductions = reductions.size();
    if (numReductions != forOp.getNumIterOperands())
      return rewriter.notifyMatchFailure(forOp, "reduction num mismatch");

    Location loc = forOp.getLoc();
    rewriter.setInsertionPoint(forOp);
    AffineMap lowerBoundMap = forOp.getLowerBoundMap();
    ValueRange lowerBoundOperands = forOp.getLowerBoundOperands();
    AffineMap upperBoundMap = forOp.getUpperBoundMap();
    ValueRange upperBoundOperands = forOp.getUpperBoundOperands();

    // Creating empty 1-D affine.parallel op.
    auto reducedValues = llvm::to_vector(llvm::map_range(
        reductions, [](const LoopReduction &red) { return red.value; }));
    auto reductionKinds = llvm::to_vector(llvm::map_range(
        reductions, [](const LoopReduction &red) { return red.kind; }));
    AffineParallelOp newPloop = rewriter.create<AffineParallelOp>(
        loc, ValueRange(reducedValues).getTypes(), reductionKinds,
        llvm::ArrayRef(lowerBoundMap), lowerBoundOperands,
        llvm::ArrayRef(upperBoundMap), upperBoundOperands,
        llvm::ArrayRef(forOp.getStepAsInt()));

    Operation *yieldOp = forOp.getBody()->getTerminator();

    // Handle the initial values of reductions because the parallel loop always
    // starts from the neutral value.
    SmallVector<Value> newResults;
    newResults.reserve(numReductions);
    for (unsigned i = 0; i < numReductions; ++i) {
      Value init = forOp.getInits()[i];
      // This works because we are only handling single-op reductions at the
      // moment. A switch on reduction kind or a mechanism to collect operations
      // participating in the reduction will be necessary for multi-op
      // reductions.

      Operation *reductionOp = yieldOp->getOperand(i).getDefiningOp();
      assert(reductionOp &&
             "yielded value is expected to be produced by an op");

      IRMapping irMapping;
      unsigned initPos =
          forOp.getRegionIterArgs()[i] == reductionOp->getOperand(0) ? 0 : 1;
      irMapping.map(reductionOp->getOperand(initPos), init);
      irMapping.map(reductionOp->getOperand(1 - initPos),
                    newPloop->getResult(i));
      Operation *clonedReductionOp = rewriter.clone(*reductionOp, irMapping);
      newResults.push_back(clonedReductionOp->getResult(0));
    }
    rewriter.inlineRegionBefore(forOp.getBodyRegion(), newPloop.getBodyRegion(),
                                newPloop.getBodyRegion().end());
    rewriter.replaceOp(forOp, newResults);

    // Update the loop terminator to yield reduced values bypassing the
    // reduction operation itself (now moved outside of the loop) and erase the
    // block arguments that correspond to reductions. Note that the loop always
    // has one "main" induction variable when coming from a non-parallel for.
    IRMapping irMapping;
    irMapping.map(yieldOp->getOperands(), reducedValues);
    rewriter.setInsertionPoint(yieldOp);
    Operation *clonedYield = rewriter.clone(*yieldOp, irMapping);

    SetVector<Operation *> opsToErase;
    for (unsigned i = 0; i < numReductions; ++i) {
      Operation *reductionOp = yieldOp->getOperand(i).getDefiningOp();
      opsToErase.insert(reductionOp);
    }
    rewriter.replaceOp(yieldOp, clonedYield);
    for (Operation *op : opsToErase)
      rewriter.eraseOp(op);

    SmallVector<Value> iterArgReplacements;
    llvm::append_range(
        iterArgReplacements,
        newPloop.getBodyRegion().getBlocks().front().getArguments());
    iterArgReplacements.append(newPloop->getNumResults(), nullptr);
    Block *origBlock = &newPloop.getBodyRegion().getBlocks().front();
    if (!origBlock->empty())
      rewriter.eraseOp(origBlock->getTerminator());

    rewriter.mergeBlocks(&newPloop.getBodyRegion().getBlocks().back(),
                         &newPloop.getBodyRegion().getBlocks().front(),
                         iterArgReplacements);

    return success();
  }

  bool parallelReductions = true;
};

void populateAffineParallelizationPattern(MLIRContext &context,
                                          RewritePatternSet &patterns) {
  patterns.insert<AffineParallelizePattern>(/*parallelReductions=*/true,
                                            &context);
}
