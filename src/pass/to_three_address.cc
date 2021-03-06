/**
 * Copyright 2019 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <arithmetic/pattern_match.h>
#include <dmlc/common.h>
#include <tvm/ir.h>
#include <tvm/tensor.h>
#include <tvm/ir_functor_ext.h>
#include <tvm/ir_mutator.h>
#include <tvm/ir_visitor.h>
#include <tvm/api_registry.h>
#include <ir_pass.h>
#include <tvm.h>
#include <floating.h>
#include <limits>
#include <queue>
#include <algorithm>
#include "pass/utils.h"

namespace akg {
namespace ir {
using VarSet = std::unordered_set<Var, air::NodeHash, air::NodeEqual>;

// forward declaration
class ThreeAddressExprMutator;

struct ExpressionPattern {
  int min_level;                                                      // minimal level
  std::function<int(Expr)> score_func;                                // assign score to a subtree
  std::function<Expr(Expr, ThreeAddressExprMutator &)> replace_func;  // replace a subtree with this instruction
};

class ThreeAddressFilter : public IRVisitor {
 public:
  bool Find(const Stmt &s) {
    Visit(s);
    return need_;
  }

  void Visit_(const Call *op) override {
    if (op->name == "load3d_l1_ub") {
      need_ = false;
    }
    IRVisitor::Visit_(op);
  }

 private:
  bool need_{true};
};

class ScalarOperandFinder : public IRVisitor {
 public:
  bool Find(const Expr &e) {
    Visit(e);
    return find_;
  }

  // float32(input_2(i0) override) < float32(input_3(i0))
  void Visit_(const Cast *op) override {
    if (op->type.is_float()) in_float_cast_ = true;
    IRVisitor::Visit_(op);
    in_float_cast_ = false;
  }

  void Visit_(const Call *op) override {
    if (op->call_type == Call::CallType::Halide) {
      if (!in_index_ && (op->type.is_int() || op->type.is_uint()) && !in_float_cast_) {
        find_ = true;
      }
      in_index_++;
      IRVisitor::Visit_(op);
      in_index_--;
    }
  }

  void Visit_(const Variable *op) override {
    if (!in_index_) {
      find_ = true;
    }
  }

 private:
  int in_index_{0};
  bool find_{false};
  bool in_float_cast_{false};
};

// Assign a hash value for an expression. This is used for common expression elmination
class ExprHasher : public air::ir::ExprFunctor<size_t(const Expr &n)> {
 public:
  ExprHasher() : cross_simplify_(false) {}
  explicit ExprHasher(bool cross_simplify) : cross_simplify_(cross_simplify) {}
  ~ExprHasher() override = default;

 private:
  size_t VisitExpr_(const Add *op) final { return VisitExpr(op->a) + VisitExpr(op->b); }
  size_t VisitExpr_(const Sub *op) final { return VisitExpr(op->a) - VisitExpr(op->b); }
  size_t VisitExpr_(const Mul *op) final { return VisitExpr(op->a) * VisitExpr(op->b); }
  size_t VisitExpr_(const Div *op) final {
    size_t t = VisitExpr(op->b);
    if (t != 0) {
      return VisitExpr(op->a) / t;
    } else {
      return VisitExpr(op->a) + 1;
    }
  }
  size_t VisitExpr_(const Call *op) final {
    size_t ret = std::hash<const Node *>{}(op->func.get());
    if (cross_simplify_ && (op->func.get() == nullptr)) {
      ret = std::hash<std::string>{}(op->name);
    }
    for (size_t i = 0; i < op->args.size(); ++i) {
      ret = dmlc::HashCombine(ret, VisitExpr(op->args[i]));
    }
    return ret;
  }

  size_t VisitExpr_(const Variable *op) final {
    if (cross_simplify_) {
      return std::hash<std::string>()(op->name_hint);
    } else {
      return std::hash<const Node *>()(op);
    }
  }

  size_t VisitExpr_(const FloatImm *op) final { return std::hash<double>()(op->value); }

  size_t VisitExpr_(const IntImm *op) final { return std::hash<int64_t>()(op->value); }

  size_t VisitExprDefault_(const Node *op) final {
    if (cross_simplify_) {
      if (op->IsInstance<Cast>()) {  // Support for cases of float16(A), float32(A)
        auto cast_op = static_cast<const Cast *>(op);
        auto value_hash = VisitExpr(cast_op->value);
        std::ostringstream os;
        os << cast_op->type;
        auto type_hash = std::hash<std::string>()(os.str());
        return dmlc::HashCombine(type_hash, value_hash);
      }
    }
    return std::hash<const Node *>()(op);
  }

  bool cross_simplify_{false};
};

// poly does not support both AND and OR to exist in an expression.
class PolyUnsupportedExprChecker : public IRVisitor {
 public:
  bool isSupported(const Expr &expr) {
    and_found = false;
    or_found = false;
    Visit(expr);
    return !(and_found && or_found);
  }

 private:
  void Visit_(const And *expr) override {
    and_found = true;
    Visit(expr->a);
    Visit(expr->b);
  }

  void Visit_(const Or *expr) override {
    or_found = true;
    Visit(expr->a);
    Visit(expr->b);
  }

  bool or_found{false};
  bool and_found{false};
};

// Collect all Tensors used in an Expr
std::unordered_set<Tensor> GetExprTensors(const Expr expr) {
  std::unordered_set<Tensor> tensors;
  PostOrderVisit(expr, [&tensors](const NodeRef &node) {
    const Call *t_call = node.as<Call>();
    if (t_call != nullptr && t_call->func.defined()) {
      tensors.insert(Downcast<Operation>(t_call->func).output(t_call->value_index));
    }
  });
  return tensors;
}

// Replace all instances of a Tensor "from" in an Expr with a new one "to"
class ReplaceProvideTensors : public IRMutator {
 public:
  ReplaceProvideTensors(const Tensor &from, const Operation &to) : from_(from->op), to_(to) {}
  ~ReplaceProvideTensors() override = default;

  Stmt Mutate_(const Provide *op, const Stmt &s) final {
    Stmt stmt = IRMutator::Mutate_(op, s);
    op = stmt.as<Provide>();
    CHECK(op);
    if (op->func == from_) {
      stmt = Provide::make(to_, op->value_index, op->value, op->args);
    }
    return stmt;
  }

  Expr Mutate_(const Call *op, const Expr &e) override {
    Expr expr = IRMutator::Mutate_(op, e);
    const Call *n = expr.as<Call>();
    CHECK(n);
    if (n->func == from_) {
      expr = Call::make(n->type, to_->name, n->args, n->call_type, to_, n->value_index);
    }
    return expr;
  }

 private:
  const Operation from_;
  const Operation to_;
};

// Mutate expression according to selection choices
class ThreeAddressExprMutator : public IRMutator {
 public:
  ThreeAddressExprMutator(const Tensor output, const Array<Expr> &args, const Array<Expr> &shape,
                          const std::unordered_set<const Call *> &broadcast, bool IsReductionOp,
                          bool cross_stmt_simplify)
      : output_(output),
        args_(args),
        shape_(shape),
        broadcast_(broadcast),
        IsReductionOp_(IsReductionOp),
        cross_simplify_(cross_stmt_simplify),
        hasher_(cross_stmt_simplify) {
    CHECK_EQ(args_.size(), shape_.size());
    if (shape_.empty()) {  // scalar values should have at least one dimension and contains one element
      shape_.push_back(1);
      args_.push_back(0);
    }
    expand_floatimm_.push_back(true);  // expand float immediate by default
  }
  ~ThreeAddressExprMutator() override = default;

  std::unordered_map<size_t, std::pair<Expr, Expr>> GetCommonExpr() { return common_exprs_; }
  void SetCommonExpr(std::unordered_map<size_t, std::pair<Expr, Expr>> global_common_expr) {
    common_exprs_.insert(global_common_expr.begin(), global_common_expr.end());
  }

  Expr AllocateTmp(Expr value) {
    // detect common expression
    size_t hash_value = hasher_(value);
    auto x = common_exprs_[hash_value];
    if (Equal(x.first, value)) {
      return x.second;
    }
    if (cross_simplify_) {
      for (auto tmp_it : common_exprs_) {
        if (Equal(tmp_it.second.second, value)) {
          return value;
        }
      }
    }

    // allocate new immediate tensor
    Tensor imm;
    imm = PlaceholderOpNode::make(output_->op->name + "_" + std::to_string(ct_++), shape_, value.type()).output(0);
    imm_tensors.push_back(imm);
    imm_ops.insert(imm->op);

    // update common expr
    assign_stmt.push_back(Provide::make(imm->op, imm->value_index, value, args_));
    Expr ret = Call::make(value.type(), imm->op->name, args_, Call::CallType::Halide, imm->op, imm->value_index);
    common_exprs_[hash_value] = std::make_pair(value, ret);
    imm2hash_[imm->op] = hash_value;
    return ret;
  }

  Expr AssignTmp(const Expr tmp_tensor, Expr value) {
    Tensor imm = GetImmTensor(tmp_tensor);

    // delete old cached common expr
    size_t old_hash = imm2hash_[imm->op];
    common_exprs_.erase(old_hash);

    // update new common expr
    assign_stmt.push_back(Provide::make(imm->op, imm->value_index, value, args_));
    size_t hash_value = hasher_(value);
    Expr ret = Call::make(value.type(), imm->op->name, args_, Call::CallType::Halide, imm->op, imm->value_index);
    common_exprs_[hash_value] = std::make_pair(value, ret);
    imm2hash_[imm->op] = hash_value;
    return ret;
  }

  bool IsTmpTensor(const Expr expr) {
    const Call *node = expr.as<Call>();

    if (node == nullptr) {
      return false;
    }
    return imm_ops.count(node->func);
  }

  bool IsTmpTensor(const Tensor t) {
    if (!t.defined()) {
      return false;
    }
    return imm_ops.count(t->op);
  }

  Tensor GetImmTensor(const Expr expr) {
    const Call *node = expr.as<Call>();
    CHECK(node != nullptr);
    CHECK(imm_ops.count(node->func));
    return Downcast<Operation>(node->func).output(node->value_index);
  }

  // forward declaration
  Expr Mutate(Expr expr) override;

  // do naive three address translation without instruction selection
  Expr MutateWithoutSelection(const Expr expr) {
    disable_selection_ = true;
    Expr ret = Mutate(expr);
    disable_selection_ = false;
    return ret;
  }

  template <typename T>
  Expr MutateBinaryOp(const T *op, const Expr &e) {
    in_call_++;
    Expr l = Mutate(op->a);
    Expr r = Mutate(op->b);
    in_call_--;

    bool broadcast_l = !IsReductionOp_ && !is_constant(l) && CountVars(args_) > CountVars(l);
    bool broadcast_r = !IsReductionOp_ && !is_constant(r) && CountVars(args_) > CountVars(r);

    if (op->template IsInstance<Add>() || op->template IsInstance<Mul>()) {
      if (broadcast_l && broadcast_r) {
        l = AllocateTmp(l);
      } else if (is_constant(r) && broadcast_l) {
        l = AllocateTmp(l);
      } else if (is_constant(l) && broadcast_r) {
        r = AllocateTmp(r);
      }
    }

    return AllocateTmp(T::make(Mutate(l), Mutate(r)));
  }

  Expr Mutate_(const Add *op, const Expr &e) final { return MutateBinaryOp<Add>(op, e); }
  Expr Mutate_(const Sub *op, const Expr &e) final {
    in_call_++;
    Expr l = Mutate(op->a);
    Expr r = Mutate(op->b);
    in_call_--;
    if (is_constant(l)) {
      // fix the missing of vsubs (e.g.  b[i] = 1.0 - a[i] -> tmp[i] = a[i] * -1;  b[i] = tmp[i] + 1.0
      Expr tmp = AllocateTmp(Mul::make(r, make_const(r.type(), -1.0)));

      if (isZero(l)) return tmp;

      return AllocateTmp(Add::make(tmp, l));
    }

    const Call *_a = l.as<Call>();
    const Call *_b = r.as<Call>();
    if (_a && _b && IsReductionOp_ && CountVars(l) < CountVars(r)) {
      // for a[i] = a[i] - b[i, j] -> tmp[i, j] = b[i, j] * -1; a[i] = a[i] + tmp[i, j]
      Expr tmp = AllocateTmp(Mul::make(r, make_const(r.type(), -1.0)));
      return AllocateTmp(Add::make(l, tmp));
    }

    return AllocateTmp(Sub::make(l, r));
  }
  Expr Mutate_(const Mul *op, const Expr &e) final { return MutateBinaryOp<Mul>(op, e); }
  Expr Mutate_(const Div *op, const Expr &e) final { return MutateBinaryOp<Div>(op, e); }
  Expr Mutate_(const Mod *op, const Expr &e) final { return MutateBinaryOp<Mod>(op, e); }
  Expr Mutate_(const Max *op, const Expr &e) final {
    if (in_call_) {
      return AllocateTmp(IRMutator::Mutate_(op, e));
    }
    in_call_++;
    Expr ret = IRMutator::Mutate_(op, e);
    in_call_--;
    return ret;
  }
  Expr Mutate_(const Min *op, const Expr &e) final {
    if (in_call_) {
      return AllocateTmp(IRMutator::Mutate_(op, e));
    }
    in_call_++;
    Expr ret = IRMutator::Mutate_(op, e);
    in_call_--;
    return ret;
  }

  Expr Mutate_(const Call *op, const Expr &e) final {
    if (op->call_type == Call::CallType::Halide) {
      // broadcast for a[i, j] = cast(a[j]) -> t[i, j] = a[j]; a[i, j] = cast(t[i, j])
      if (expr_stack.size() >= 2 && expr_stack[expr_stack.size() - 2]->IsInstance<Cast>() &&
          CountVars(args_) > CountVars(e)) {
        return AllocateTmp(e);
      }

      // C[i] = A[i] op B[N-i]; ==> B'[i] = B[N-i]; C[i] = A[i] op B'[i]
      // only support the last axis reverse indexing
      if (op->args.size() > 0) {
        VarSet vars;
        GatherVars(op->args[op->args.size() - 1], &vars);
        if (vars.size() == 1) {
          auto coff = air::arith::DetectLinearEquation(op->args[op->args.size() - 1], {*vars.begin()});
          if (coff.size() > 0 && coff[0].as<IntImm>() && coff[0].as<IntImm>()->value < 0) {
            return AllocateTmp(e);
          }
        }
      }

      // need transpose A[i, j] = op(B[j, i]); ==> B'[i, j] = B[j, i]; A[i, j] = op(B'[i, j])
      if (args_.size() >= 1 && args_[args_.size() - 1]->IsInstance<Variable>() &&
          op->args[op->args.size() - 1]->IsInstance<Variable>()) {
        const Var innermost = Downcast<Var>(args_[args_.size() - 1]);
        if ((IsReductionOp_ && expr_stack.size() >= 3) ||
            (!IsReductionOp_ && expr_stack.size() >= 2 && op->args.size() > 1)) {
          Expr x = expr_stack[expr_stack.size() - 2];
          const Call *call = x.as<Call>();
          if (!(call && (call->name == "proposal_sort" || call->name == "topk_sort" || call->name == "iou" ||
                         call->name == "nms" || call->name == "four2five_nchw" || call->name == "vmadd" ||
                         call->name == "vmla"))) {
            VarSet vars;
            GatherVars(op->args[op->args.size() - 1], &vars);
            if (vars.count(innermost) == 0) {
              return AllocateTmp(e);
            }
          }
        }
      }

      bool broadcast = true;
      if (expr_stack.size() >= 2) {
        Expr x = expr_stack[expr_stack.size() - 2];
        if (x->IsInstance<Add>() || x->IsInstance<Mul>()) {
          broadcast = false;
        }
        const Call *call = x.as<Call>();
        if (call && (call->name == "proposal_sort" || call->name == "topk_sort" || call->name == "iou" ||
                     call->name == "nms" || call->name == "vmadd" || call->name == "vmla")) {
          broadcast = false;
        }
      }

      // broadcast when need
      if (broadcast_.count(op) && broadcast) {
        return AllocateTmp(e);
      }
      // this is going to generate a tensor of tensor expr, like A(B(i))
      return e;
    } else if (op->call_type == Call::CallType::PureIntrinsic && op->name == air::ir::intrinsic::tvm_if_then_else) {
      // do not split the condition of tvm_if_then_else
      Array<Expr> args;
      in_call_++;
      // do not expand FloatImm if found scalars operands in condition
      expand_floatimm_.push_back(!ScalarOperandFinder().Find(op->args[0]));
      args.push_back(op->args[0]);
      args.push_back(Mutate(op->args[1]));
      args.push_back(Mutate(op->args[2]));
      expand_floatimm_.pop_back();
      in_call_--;
      return AllocateTmp(Call::make(op->type, op->name, args, op->call_type, op->func, op->value_index));
    } else {
      Array<Expr> args;
      in_call_++;
      for (const auto &x : op->args) {
        args.push_back(Mutate(x));
      }
      in_call_--;
      if (op->name == "vmadd" || op->name == "vmla") {
        return FixMultivarInsn(op, args);
      }
      return AllocateTmp(Call::make(op->type, op->name, args, op->call_type, op->func, op->value_index));
    }
  }

  Expr Mutate_(const Select *op, const Expr &e) final {
    // do not split the condition of Select
    in_call_++;
    Expr cond = CanonicalSimplify(op->condition);
    if (!PolyUnsupportedExprChecker().isSupported(cond)) {
      cond = Simplify_cce(op->condition);
      if (!PolyUnsupportedExprChecker().isSupported(cond)) {
        cond = op->condition;
      }
    }
    if (!ScalarOperandFinder().Find(cond)) {
      cond = Mutate(cond);
    }
    Expr ret = AllocateTmp(Select::make(cond, Mutate(op->true_value), Mutate(op->false_value)));
    in_call_--;
    return ret;
  }

  Expr Mutate_(const Cast *op, const Expr &e) final {
    if (in_call_) {
      return AllocateTmp(IRMutator::Mutate_(op, e));
    }
    in_call_++;
    Expr ret = IRMutator::Mutate_(op, e);
    in_call_--;
    return ret;
  }

  template <typename T>
  Expr MutateConstOp(const T *op, const Expr &e) {
    std::set<std::string> excludeSet = {"nms"};
    bool excludeIntrin = expr_stack.size() >= 2 && expr_stack[expr_stack.size() - 2]->IsInstance<Call>() &&
                         excludeSet.count(expr_stack[expr_stack.size() - 2].as<Call>()->name) > 0;
    if (in_call_ && expand_floatimm_.back() &&
        ((expr_stack.size() >= 2 && (expr_stack[expr_stack.size() - 2]->IsInstance<Call>()    // log(0.1)
                                     || expr_stack[expr_stack.size() - 2]->IsInstance<Max>()  // Max(a, 0.1), Max(a,1)
                                     || expr_stack[expr_stack.size() - 2]->IsInstance<Min>()))) &&
        !excludeIntrin  // Don't handle nms intrin
    ) {
      return AllocateTmp(e);
    } else {
      return IRMutator::Mutate_(op, e);
    }
  }

  Expr Mutate_(const FloatImm *op, const Expr &e) final { return MutateConstOp(op, e); }
  Expr Mutate_(const IntImm *op, const Expr &e) final { return MutateConstOp(op, e); }

  std::vector<Stmt> assign_stmt;
  std::vector<Tensor> imm_tensors;
  std::unordered_set<FunctionRef, air::NodeHash, air::NodeEqual> imm_ops;

 private:
  Expr FixMultivarInsn(const Call *op, const Array<Expr> &args) {
    auto arg2 = IsTmpTensor(args[2]) ? args[2] : AllocateTmp(args[2]);
    Array<Expr> new_args({args[0], args[1], arg2});
    if (level_ > 1) {
      return AssignTmp(arg2, Call::make(op->type, op->name, new_args, op->call_type, op->func, op->value_index));
    } else {
      auto result = AssignTmp(arg2, Call::make(op->type, op->name, new_args, op->call_type, op->func, op->value_index));
      return AllocateTmp(result);
    }
  }

  Tensor output_;
  Array<Expr> args_;
  Array<Expr> shape_;

  std::unordered_map<size_t, std::pair<Expr, Expr>> common_exprs_;  // hash value -> <match expr, replace expr>
  std::unordered_map<FunctionRef, size_t, air::NodeHash, air::NodeEqual>
    imm2hash_;  // imm tensor -> hash value of the expr in the tensor

  int level_{0};
  int in_call_{0};
  std::vector<Expr> expr_stack;

  std::unordered_set<const Call *> broadcast_;

  static int ct_;
  bool disable_selection_{false};
  std::vector<bool> expand_floatimm_;
  bool IsReductionOp_{false};
  bool cross_simplify_;
  ExprHasher hasher_;
};

Expr CallPureIntrinsic(const std::string &name, const Array<Expr> &args, const Type type) {
  return Call::make(type, name, args, Call::CallType::PureIntrinsic);
}

// Match instructions by dynamic programming on the tree
class InstructionMatcher {
 public:
  void Match(const Expr value) {
    int max_score = -1;
    int max_i = -1;

    // try patterns
    for (size_t i = 0; i < ins_pattern.size(); ++i) {
      int score_ = ins_pattern[i].score_func(value);
      if (score_ > max_score) {
        max_score = score_;
        max_i = static_cast<int>(i);
      }
    }

    score = max_score;
    choice = max_i;
  }

  int score;
  int choice;
  const int NORMAL = 20;
  const int PRIOR = 50;
  const int UNMATCH = -1;
  air::arith::PVar<Expr> x, y, z, w;
  air::arith::PVar<Type> pt;
  air::arith::PVar<Floating> c1, c2;

  std::vector<ExpressionPattern> ins_pattern{
    // vmadd  [Xd] = [Xn] * [Xd] + [Xm]
    // vmla   [Xd] = [Xn] * [Xm] + [Xd]
    ExpressionPattern{
      2,
      [&, this](const Expr &expr) -> int {
        if (((x * y + z).Match(expr) || (z + x * y).Match(expr)) &&
            (!is_constant(x.Eval()) && !is_constant(y.Eval()) && !is_constant(z.Eval()))) {
          return PRIOR;
        }
        return UNMATCH;
      },
      [&, this](const Expr &expr, ThreeAddressExprMutator &mutator) -> Expr {
        CHECK(((x * y + z)).Match(expr) || (z + x * y).Match(expr));

        Expr x_eval = mutator.Mutate(x.Eval());
        Expr y_eval = mutator.Mutate(y.Eval());
        Expr z_eval = mutator.Mutate(z.Eval());
        // make sure elemwise inside
        if (CountVars(x_eval) != CountVars(y_eval) || CountVars(x_eval) != CountVars(z_eval)) {
          return mutator.MutateWithoutSelection(x_eval * y_eval + z_eval);
        }

        if (mutator.IsTmpTensor(x_eval)) {
          return mutator.AssignTmp(x_eval, CallPureIntrinsic("vmadd", {y_eval, z_eval, x_eval}, x_eval.type()));
        } else if (mutator.IsTmpTensor(y_eval)) {
          return mutator.AssignTmp(y_eval, CallPureIntrinsic("vmadd", {x_eval, z_eval, y_eval}, y_eval.type()));
        } else if (mutator.IsTmpTensor(z_eval)) {
          return mutator.AssignTmp(z_eval, CallPureIntrinsic("vmla", {x_eval, y_eval, z_eval}, z_eval.type()));
        } else {
          return mutator.MutateWithoutSelection(x_eval * y_eval + z_eval);
        }
      }},

    // vmaddrelu  [Xd] = max([Xn] * [Xd] + [Xm], 0)
    ExpressionPattern{
      2,
      [&, this](const Expr expr) -> int {
        if (((max(x * y + z, c1)).Match(expr) || (max(z + x * y, c1)).Match(expr) || (max(c1, x * y + z)).Match(expr) ||
             (max(c1, z + x * y)).Match(expr)) &&
            c1.Eval()->value == 0.0 && (!is_constant(x.Eval()) && !is_constant(y.Eval()) && !is_constant(z.Eval()))) {
          return PRIOR;
        }
        return UNMATCH;
      },
      [&, this](const Expr expr, ThreeAddressExprMutator &mutator) -> Expr {
        CHECK((max(x * y + z, c1)).Match(expr) || (max(z + x * y, c1)).Match(expr) ||
              (max(c1, x * y + z)).Match(expr) || (max(c1, z + x * y)).Match(expr));

        Expr x_eval = mutator.Mutate(x.Eval());
        Expr y_eval = mutator.Mutate(y.Eval());
        Expr z_eval = mutator.Mutate(z.Eval());
        // check elemwise
        if (CountVars(x_eval) != CountVars(y_eval) || CountVars(x_eval) != CountVars(z_eval)) {
          return mutator.MutateWithoutSelection(x_eval * y_eval + z_eval);
        }

        if (mutator.IsTmpTensor(x_eval) || x_eval.same_as(x.Eval())) {
          return mutator.AssignTmp(x_eval, CallPureIntrinsic("vmaddrelu", {y_eval, z_eval, x_eval}, x_eval.type()));
        } else if (mutator.IsTmpTensor(y_eval) || y_eval.same_as(y.Eval())) {
          return mutator.AssignTmp(y_eval, CallPureIntrinsic("vmaddrelu", {x_eval, z_eval, y_eval}, y_eval.type()));
        } else {
          return mutator.MutateWithoutSelection(max(x_eval * y_eval + z_eval, c1.Eval()));
        }
      }},

    // vaxpy [Xd] = Xm * [Xn] + [Xd]
    ExpressionPattern{
      2,
      [&, this](const Expr expr) -> int {
        if (((c1 * x + y).Match(expr) || (x * c1 + y).Match(expr) || (y + c1 * x).Match(expr) ||
             (y + c1 * x).Match(expr)) &&
            (!is_constant(x.Eval()) && !is_constant(y.Eval()))) {
          return PRIOR;
        }
        return UNMATCH;
      },
      [&, this](const Expr expr, ThreeAddressExprMutator &mutator) -> Expr {
        CHECK((c1 * x + y).Match(expr) || (x * c1 + y).Match(expr) || (y + c1 * x).Match(expr) ||
              (y + c1 * x).Match(expr));
        Expr x_eval = mutator.Mutate(x.Eval());
        Expr y_eval = mutator.Mutate(y.Eval());
        // check elemwise
        if (CountVars(x_eval) != CountVars(y_eval)) {
          return mutator.MutateWithoutSelection(c1.Eval() * x_eval + y_eval);
        }

        if (mutator.IsTmpTensor(y_eval) || y_eval.same_as(y.Eval())) {
          return mutator.AssignTmp(y_eval, CallPureIntrinsic("vaxpy", {x_eval, y_eval, c1.Eval()}, y_eval.type()));
        } else {
          return mutator.MutateWithoutSelection(c1.Eval() * x_eval + y_eval);
        }
      }},

    // vrelu [Xd] = max([Xn], 0)
    ExpressionPattern{1,
                      [&, this](const Expr expr) -> int {
                        if (((max(x, c1)).Match(expr) || (max(c1, x)).Match(expr)) && c1.Eval()->value == 0.0 &&
                            !is_constant(x.Eval()) && x.Eval().type() == Float(16, 1)) {
                          return NORMAL;
                        }
                        return UNMATCH;
                      },

                      [&, this](const Expr expr, ThreeAddressExprMutator &mutator) -> Expr {
                        CHECK(((max(x, c1)).Match(expr) || (max(c1, x)).Match(expr)));
                        Expr x_eval = mutator.Mutate(x.Eval());
                        return mutator.Mutate(CallPureIntrinsic("relu", {x_eval}, x_eval.type()));
                      }},

    // adds [Xd] = ([Xn] + [Yn]) + imm -> [Xn] + ([Yn] + imm)
    ExpressionPattern{1,
                      [&, this](const Expr expr) -> int {
                        if ((((x - y) + c1).Match(expr) || (c1 + (x - y)).Match(expr) || ((x + y) + c1).Match(expr) ||
                             (c1 + (x + y)).Match(expr)) &&
                            !is_constant(x.Eval()) && !is_constant(y.Eval())) {
                          return NORMAL;
                        }
                        return UNMATCH;
                      },

                      [&, this](Expr expr, ThreeAddressExprMutator &mutator) -> Expr {
                        if (((x - y) + c1).Match(expr) || (c1 + (x - y)).Match(expr)) {
                          Expr x_eval = mutator.Mutate(x.Eval());
                          Expr y_eval = mutator.Mutate(y.Eval());
                          return mutator.Mutate(x_eval + (c1.Eval() - y_eval));
                        }
                        if (((x + y) + c1).Match(expr) || (c1 + (x + y)).Match(expr)) {
                          Expr x_eval = mutator.Mutate(x.Eval());
                          Expr y_eval = mutator.Mutate(y.Eval());
                          return mutator.Mutate(x_eval + (y_eval + c1.Eval()));
                        }
                        return expr;
                      }},

    // int32 floor/ceil/round/trunc() --> floor/ceil/round/trunc()
    ExpressionPattern{
      1,
      [&, this](const Expr expr) -> int {
        if (((cast(pt, call_floor(x))).Match(expr) && pt.Eval().is_int()) ||
            ((cast(pt, call_ceil(x))).Match(expr) && pt.Eval().is_int()) ||
            ((cast(pt, call_round(x))).Match(expr) && pt.Eval().is_int()) ||
            ((cast(pt, call_trunc(x))).Match(expr) && pt.Eval().is_int())) {
          return NORMAL;
        }
        return UNMATCH;
      },

      [&, this](Expr expr, ThreeAddressExprMutator &mutator) -> Expr {
        if ((cast(pt, call_floor(x))).Match(expr) && pt.Eval().is_int()) {
          Expr x_eval = mutator.Mutate(x.Eval());
          return mutator.Mutate(Call::make(expr.type(), "floor", {x_eval}, Call::CallType::PureIntrinsic));
        }
        if ((cast(pt, call_ceil(x))).Match(expr) && pt.Eval().is_int()) {
          Expr x_eval = mutator.Mutate(x.Eval());
          return mutator.Mutate(Call::make(expr.type(), "ceil", {x_eval}, Call::CallType::PureIntrinsic));
        }
        if ((cast(pt, call_round(x))).Match(expr) && pt.Eval().is_int()) {
          Expr x_eval = mutator.Mutate(x.Eval());
          return mutator.Mutate(Call::make(expr.type(), "round", {x_eval}, Call::CallType::PureIntrinsic));
        }
        if ((cast(pt, call_trunc(x))).Match(expr) && pt.Eval().is_int()) {
          Expr x_eval = mutator.Mutate(x.Eval());
          return mutator.Mutate(Call::make(expr.type(), "trunc", {x_eval}, Call::CallType::PureIntrinsic));
        }
        return expr;
      }},

    // float(cc1) -> a[i] = cc1; cast(a[i])
    ExpressionPattern{1,
                      [&, this](const Expr expr) -> int {
                        if ((cast(pt, x)).Match(expr) && pt.Eval().is_float() && x.Eval().as<Variable>()) {
                          return NORMAL;
                        }
                        return UNMATCH;
                      },

                      [&, this](Expr expr, ThreeAddressExprMutator &mutator) -> Expr {
                        if ((cast(pt, x)).Match(expr) && pt.Eval().is_float() && x.Eval().as<Variable>()) {
                          Expr tmp = mutator.AllocateTmp(x.Eval());
                          return mutator.Mutate(Cast::make(expr.type(), tmp));
                        }
                        return expr;
                      }},

    // Imm / x ->  y = Imm; y/x
    ExpressionPattern{1,
                      [&, this](const Expr expr) -> int {
                        if (div(c1, y).Match(expr) && is_constant(c1.Eval()) && !is_constant(y.Eval())) {
                          return NORMAL;
                        }
                        return UNMATCH;
                      },

                      [&, this](const Expr expr, ThreeAddressExprMutator &mutator) -> Expr {
                        CHECK(div(c1, y).Match(expr) && is_constant(c1.Eval()) && !is_constant(y.Eval()));
                        Expr x_eval = mutator.AllocateTmp(c1.Eval());
                        return mutator.Mutate(Div::make(x_eval, y.Eval()));
                      }},

    ExpressionPattern{1,
                      [&, this](const Expr expr) -> int {
                        if ((c1 * (c2 + x)).Match(expr) || (c1 * (c2 - x)).Match(expr)) {
                          return NORMAL;
                        }
                        return UNMATCH;
                      },

                      [&, this](Expr expr, ThreeAddressExprMutator &mutator) -> Expr {
                        if ((c1 * (c2 + x)).Match(expr)) {
                          return mutator.Mutate(Simplify_cce(x.Eval() * c1.Eval() + c1.Eval() * c2.Eval()));
                        }
                        if ((c1 * (c2 - x)).Match(expr)) {
                          return mutator.Mutate(Simplify_cce(c1.Eval() * c2.Eval() - x.Eval() * c1.Eval()));
                        }
                        return expr;
                      }},
    ExpressionPattern{1,
                      [&, this](const Expr expr) -> int {
                        if ((select((z || w), x, y)).Match(expr) || (select((z && w), x, y)).Match(expr) ||
                            (select((!z), x, y)).Match(expr)) {
                          return NORMAL;
                        }
                        return UNMATCH;
                      },

                      [&, this](Expr expr, ThreeAddressExprMutator &mutator) -> Expr {
                        if ((select((z || w), x, y)).Match(expr)) {
                          Expr temp_eval = mutator.Mutate(Select::make(z.Eval(), x.Eval(), y.Eval()));
                          return mutator.Mutate(Select::make(w.Eval(), x.Eval(), temp_eval));
                        }
                        if ((select((z && w), x, y)).Match(expr)) {
                          Expr temp_eval = mutator.Mutate(Select::make(z.Eval(), x.Eval(), y.Eval()));
                          return mutator.Mutate(Select::make(w.Eval(), temp_eval, y.Eval()));
                        }
                        if ((select((!z), x, y)).Match(expr)) {
                          return mutator.Mutate(Select::make(z.Eval(), y.Eval(), x.Eval()));
                        }
                        return expr;
                      }}};
};

Expr ThreeAddressExprMutator::Mutate(Expr expr) {
  // select instructions
  InstructionMatcher matcher;
  matcher.Match(expr);
  int idx = matcher.choice;
  Expr ret;
  level_++;
  if (idx < 0 || disable_selection_ || level_ < matcher.ins_pattern[idx].min_level) {
    expr_stack.push_back(expr);
    ret = IRMutator::Mutate(expr);
    expr_stack.pop_back();
  } else {  // match an intrinsic
    ret = matcher.ins_pattern[idx].replace_func(expr, *this);
  }
  level_--;
  return ret;
}

int ThreeAddressExprMutator::ct_ = 0;

class InferUpperBound {
 private:
  class Bound {
   public:
    Expr min;
    Expr max;

    static Bound make(const Range range) {
      Bound bound;
      bound.min = range->min;
      bound.max = range->min + range->extent;
      return bound;
    }
    static Bound make(const Expr min, const Expr max) {
      Bound bound;
      bound.min = min;
      bound.max = max;
      return bound;
    }
  };

  Bound infer_range(const Expr &expr) {
    air::arith::Analyzer analyzer_;
    if (expr.as<IntImm>() || expr.as<UIntImm>() || expr.as<FloatImm>()) {
      return Bound::make(expr, expr);
    } else if (expr.as<Variable>()) {
      auto var = expr.as<Variable>()->name_hint;
      if (binds.count(var) > 0) {
        Bound var_min_range = infer_range(binds[var].min);
        Bound var_max_range = infer_range(binds[var].max);
        return Bound::make(var_min_range.min, var_max_range.max);
      } else {
        return Bound::make(expr, expr);
      }
    } else if (expr.as<Add>()) {
      auto add = expr.as<Add>();
      Bound bound_a = infer_range(add->a);
      Bound bound_b = infer_range(add->b);
      return Bound::make(Simplify_cce(bound_a.min + bound_b.min), Simplify_cce(bound_a.max + bound_b.max));
    } else if (expr.as<Sub>()) {
      auto sub = expr.as<Sub>();
      Bound bound_a = infer_range(sub->a);
      Bound bound_b = infer_range(sub->b);
      return Bound::make(Simplify_cce(bound_a.min - bound_b.max), Simplify_cce(bound_a.max - bound_b.min));
    } else if (expr.as<Mul>()) {
      auto mul = expr.as<Mul>();
      Bound bound_a = infer_range(mul->a);
      Bound bound_b = infer_range(mul->b);
      Bound bound;
      if (analyzer_.CanProve(bound_a.min >= 0) && analyzer_.CanProve(bound_b.min >= 0)) {
        bound.min = Simplify_cce(bound_a.min * bound_b.min);
      } else {
        bound.min = expr;
      }
      if (analyzer_.CanProve(bound_a.max >= 0) && analyzer_.CanProve(bound_b.max >= 0)) {
        bound.max = Simplify_cce(bound_a.max * bound_b.max);
      } else {
        bound.max = expr;
      }
      return bound;
    } else if (expr.as<Div>()) {
      auto div = expr.as<Div>();
      Bound bound_a = infer_range(div->a);
      Bound bound_b = infer_range(div->b);
      Bound bound;
      if (analyzer_.CanProve(bound_a.min >= 0) && analyzer_.CanProve(bound_b.max > 0)) {
        bound.min = Simplify_cce(bound_a.min / bound_b.max);
      } else {
        bound.min = expr;
      }
      if (analyzer_.CanProve(bound_a.max >= 0) && analyzer_.CanProve(bound_b.min > 0)) {
        bound.max = Simplify_cce(bound_a.max / bound_b.min);
      } else {
        bound.max = expr;
      }
      return bound;
    } else if (expr.as<Min>()) {
      auto min_expr = expr.as<Min>();
      Bound bound_a = infer_range(min_expr->a);
      Bound bound_b = infer_range(min_expr->b);
      return Bound::make(Simplify_cce(min(bound_a.min, bound_b.min)), Simplify_cce(min(bound_a.max, bound_b.max)));
    } else if (expr.as<Max>()) {
      auto max_expr = expr.as<Max>();
      Bound bound_a = infer_range(max_expr->a);
      Bound bound_b = infer_range(max_expr->b);
      return Bound::make(Simplify_cce(max(bound_a.min, bound_b.min)), Simplify_cce(max(bound_a.max, bound_b.max)));
    } else {
      return Bound::make(expr, expr);
    }
  }

 public:
  Expr run(const Expr &expr, const std::unordered_map<VarExpr, Range, air::NodeHash, air::NodeEqual> &dom_map) {
    for (auto bind : dom_map) {
      binds.emplace(bind.first->name_hint, Bound::make(bind.second));
    }
    Bound bound = infer_range(expr);
    return bound.max;
  }

 private:
  std::unordered_map<std::string, Bound> binds;
};

bool IsReductionOp(const Provide *op) {
  Tensor output = Downcast<Operation>(op->func).output(op->value_index);
  std::vector<bool> rhs_reduce;
  int call_num = 0;

  PostOrderVisit(op->value, [&rhs_reduce, output, &call_num, op](const NodeRef &node) {
    if (const Call *call = node.as<Call>()) {
      if (call->call_type == Call::CallType::Halide) {
        call_num++;
        if (Downcast<Operation>(call->func).output(call->value_index) == output) {
          bool match = true;
          for (size_t i = 0; i < call->args.size(); ++i) {
            if (!Equal(call->args[i], op->args[i])) {
              match = false;
            }
          }
          // A[j, j] = log(B[j, j])
          if (CountVars(call->args) == 1 && AllVars(call->args) > 1) {
            match = false;
          }
          rhs_reduce.push_back(match);
        }
      }
    }
  });

  if (rhs_reduce.size() != 1) {
    return false;
  }
  return rhs_reduce[0];
}

// Expand complicated expression to three address code
// Instruction selection is applied
class ThreeAddressStmtMutator : public IRMutator {
 public:
  ThreeAddressStmtMutator(bool reuse_variable, int minimum_split, bool cross_stmt_simplify)
      : reuse_variable_(reuse_variable), minimum_split_(minimum_split), cross_stmt_simplify_(cross_stmt_simplify) {}
  ~ThreeAddressStmtMutator() override = default;

  Stmt Mutate_(const Provide *op, const Stmt &s) final {
    // skip cube operators (conv2d, matmul)
    bool is_reduction = IsReductionOp(op);
    air::arith::Analyzer analyzer_;
    Expr value = analyzer_.rewrite_simplify(op->value);
    if (!PolyUnsupportedExprChecker().isSupported(value)) {
      value = Simplify_cce(op->value);
      if (!PolyUnsupportedExprChecker().isSupported(value)) {
        value = op->value;
      }
    }

    const Call *call = op->value.as<Call>();
    if (call && (call->name == "mad" || call->name == "load3d_l1_ub" || call->name == "divide_var")) {
      return IRMutator::Mutate_(op, s);
    }

    Tensor output = Downcast<Operation>(op->func).output(op->value_index);

    // special vectorization treatment for reduce operators
    Array<Expr> args = op->args;
    Array<Expr> shape = output->shape;

    if (is_reduction) {
      VarSet spatial_vars;
      std::vector<Var> all_vars;

      // collect reduction vars
      for (size_t i = 0; i < op->args.size(); ++i) {
        GatherVars(op->args[i], &spatial_vars);
      }
      all_vars = std::vector<Var>(spatial_vars.begin(), spatial_vars.end());
      GatherVars(value, &all_vars);

      VarSet reduce_vars;
      for (const auto &x : all_vars) {
        if (!spatial_vars.count(x)) {
          reduce_vars.insert(x);
        }
      }

      std::unordered_map<Var, VarSet, air::NodeHash, air::NodeEqual> edges;
      std::unordered_map<Var, size_t, air::NodeHash, air::NodeEqual> degree;
      VarSet new_args_vars;

      // sort reduction vars  (Here we use a simplified version, only deal with the relation
      // between spatial and reduce vars, while ignore the relation among reduce vars)
      // - 1. collect relations
      PostOrderVisit(value, [&reduce_vars, &spatial_vars, &new_args_vars, &edges](const NodeRef &node) {
        if (node->IsInstance<Call>() && node.as<Call>()->call_type == Call::Halide) {
          const Array<Expr> &call_args = node.as<Call>()->args;
          CHECK(call_args.defined());
          for (size_t i = 0; i < call_args.size(); ++i) {
            for (size_t j = i + 1; j < call_args.size(); ++j) {
              if (is_constant(call_args[i]) || !call_args[j]->IsInstance<Variable>()) {
                continue;
              }
              std::vector<Var> call_arg_vars;
              GatherVars(call_args[i], &call_arg_vars);
              if (call_arg_vars.size() == 1) {
                Var vi = call_arg_vars.front(), vj = Downcast<Var>(call_args[j]);
                if (!Equal(vi, vj)) {
                  new_args_vars.insert(vi);
                  new_args_vars.insert(vj);
                  edges[vi].insert(vj);
                }
              }
            }
          }
        }
      });

      // for non-variable terms, attach them to its previous variable term
      std::unordered_map<Var, std::vector<Expr>, air::NodeHash, air::NodeEqual> following_terms_arg;
      std::unordered_map<Var, std::vector<Expr>, air::NodeHash, air::NodeEqual> following_terms_shape;
      VarSet vars_add_to_args(reduce_vars.begin(), reduce_vars.end());

      size_t i = 0;
      while (i < args.size()) {
        size_t j = i + 1;
        if (!is_constant(args[i])) {
          std::vector<Var> arg_vars;
          GatherVars(args[i], &arg_vars);
          for (const auto &x : arg_vars) {
            Var vi = x;
            if (new_args_vars.size() == 0 && vars_add_to_args.size() == 0) {
              vars_add_to_args.insert(vi);
            } else if (new_args_vars.find(vi) != new_args_vars.end()) {
              vars_add_to_args.insert(vi);
              size_t k = j;
              while (k < args.size() && is_constant(args[k])) {
                following_terms_arg[vi].push_back(args[k]);
                following_terms_shape[vi].push_back(shape[k]);
                k++;
              }
            }
          }
        }
        i = j;
      }

      // topo-sort
      Array<Expr> new_args;
      Array<Expr> new_shape;

      size_t check_ct = 0;
      std::queue<Var> out_queue;

      for (const auto &iter : edges) {
        for (const auto &to : iter.second) {
          degree[to]++;
        }
      }

      for (const auto &x : all_vars) {
        if (degree[x] == 0) {
          out_queue.push(x);
        }
      }

      while (check_ct < all_vars.size()) {
        if (out_queue.empty()) {
          size_t min_degree = std::numeric_limits<int>::max();
          for (const auto &x : all_vars) {
            if (degree[x] > 0 && degree[x] < min_degree) {
              min_degree = degree[x];
            }
          }
          for (const auto &x : reduce_vars) {
            if (degree[x] == min_degree) {
              out_queue.push(x);
              degree[x] = 0;
              break;
            }
          }
          if (out_queue.empty()) {
            for (const auto &x : vars_add_to_args) {
              if (degree[x] == min_degree) {
                out_queue.push(x);
                degree[x] = 0;
                break;
              }
            }
          }
        }
        check_ct++;
        const Var x = out_queue.front();
        out_queue.pop();

        if (vars_add_to_args.count(x)) {
          new_args.push_back(x);
          CHECK_GT(dom_map.count(x), 0);
          new_shape.push_back(dom_map[x]->min + dom_map[x]->extent);

          CHECK_EQ(following_terms_arg[x].size(), following_terms_shape[x].size());
          for (size_t dim = 0; dim < following_terms_arg[x].size(); dim++) {
            const Expr &arg = following_terms_arg[x][dim];
            const Expr &shape_ = following_terms_shape[x][dim];
            bool index_is_const_zero = Equal(arg, Expr(0));
            bool dim_extent_is_one = Equal(shape_, Expr(1));
            if (!index_is_const_zero && !dim_extent_is_one) {
              new_args.push_back(arg);
              new_shape.push_back(shape_);
            }
          }
        }

        for (const auto &y : edges[x]) {
          if (--degree[y] == 0) {
            out_queue.push(y);
          }
        }
      }
      CHECK_EQ(check_ct, all_vars.size());
      args = !new_args.empty() ? new_args : args;
      shape = !new_shape.empty() ? new_shape : shape;
      CHECK_EQ(args.size(), shape.size());
    }

    // find broadcast call
    output_ = output;
    args_ = args;
    static_cast<void>(this->Mutate(op->value));
    // mutate according to the result of instruction selection
    ThreeAddressExprMutator mutator(output, args, shape, broadcast_, is_reduction, cross_stmt_simplify_);
    if (cross_stmt_simplify_) {
      // Bring over the common exprs from previous stage
      mutator.SetCommonExpr(global_common_expr_);
    }
    value = mutator.Mutate(value);
    if (cross_stmt_simplify_) {
      // Take back the common exprs for next stages
      global_common_expr_ = mutator.GetCommonExpr();
    }

    std::unordered_set<Tensor> replaced_tensors;

    if (reuse_variable_ && (static_cast<int>(mutator.assign_stmt.size()) > minimum_split_)) {
      std::unordered_map<FunctionRef, int, NodeHash, NodeEqual> tensors_last_id;
      for (size_t ii = 0; ii < mutator.assign_stmt.size(); ii++) {
        const auto tmp_op = mutator.assign_stmt[ii].as<Provide>();
        std::unordered_set<Tensor> tmpTensors = GetExprTensors(tmp_op->value);
        for (auto it : tmpTensors) {
          if (mutator.IsTmpTensor(it)) {
            tensors_last_id[it->op] = static_cast<int>(ii);
          }
        }
      }

      for (int ii = 0; ii < (static_cast<int>(mutator.assign_stmt.size()) - 1); ii++) {
        const auto tmp_op = mutator.assign_stmt[ii].as<Provide>();
        std::unordered_set<Tensor> tmpTensors = GetExprTensors(tmp_op->value);
      }
      LOG(INFO) << "Replaced " << replaced_tensors.size() << " from a total of " << mutator.assign_stmt.size()
                << " tensors.";
    }

    // remove the last useless copy
    if (value->IsInstance<Call>() && mutator.imm_ops.count(value.as<Call>()->func)) {
      const auto last_provide = mutator.assign_stmt.back().as<Provide>();
      CHECK(last_provide != nullptr);
      value = last_provide->value;

      mutator.assign_stmt.pop_back();
      mutator.imm_tensors.pop_back();
    }

    mutator.assign_stmt.push_back(Provide::make(op->func, op->value_index, value, op->args));

    // store info for adding Realize/Produce
    if (replaced_tensors.empty()) {
      if (split_to_.count(output)) {
        for (auto &i : mutator.imm_tensors) {
          split_to_[output].push_back(i);
        }
      } else {
        split_to_[output] = mutator.imm_tensors;
      }
    } else {
      for (auto &i : mutator.imm_tensors) {
        if (replaced_tensors.find(i) == replaced_tensors.end()) split_to_[output].push_back(i);
      }
    }
    op_indices_[output->op].insert(output->value_index);

    return Block::make(mutator.assign_stmt);
  }

  Stmt Mutate_(const Realize *op, const Stmt &s) final {
    realize_node_[Operation(GetObjPtr(op->func.get())).output(op->value_index)] = op;
    return IRMutator::Mutate_(op, s);
  }

  Expr Mutate_(const Call *op, const Expr &e) final {
    // a[i] = a[i] + b[i, j]
    if (op->call_type == Call::CallType::Halide && Downcast<Operation>(op->func).output(op->value_index) != output_ &&
        CountVars(args_) > CountVars(e)) {
      broadcast_.insert(op);
    }
    return IRMutator::Mutate_(op, e);
  }

  Stmt Mutate_(const AttrStmt *op, const Stmt &s) final {
    FunctionRef func = Downcast<FunctionRef>(op->node);
    attr_node_[func] = op;
    Stmt ret = IRMutator::Mutate_(op, s);
    if (op_indices_.count(func)) {
      Stmt inner = ret;
      for (int idx : op_indices_[func]) {
        Tensor output = Downcast<Operation>(func).output(idx);
        const Realize *ref_real = realize_node_[output];
        const AttrStmt *ref_attr = attr_node_[output->op];
        for (const auto &x : split_to_.at(output)) {
          Region bounds;
          for (size_t i = 0; i < x->shape.size(); ++i) {
            Expr upper_bound = InferUpperBound().run(x->shape[i], dom_map);
            bounds.push_back(Range::make_by_min_extent(0, upper_bound));
          }
          inner = Realize::make(x->op, x->value_index, x->dtype, bounds, ref_real->condition, inner);
          inner = AttrStmt::make(x->op, ref_attr->attr_key, ref_attr->value, inner);
        }
      }
      return inner;
    }
    return ret;
  }

  Stmt Mutate_(const For *op, const Stmt &s) final {
    dom_map[op->loop_var] = Range::make_by_min_extent(op->min, op->extent);
    return IRMutator::Mutate_(op, s);
  }

 private:
  std::unordered_map<Tensor, std::vector<Tensor>> split_to_;

  std::unordered_map<FunctionRef, std::set<int>, air::NodeHash, air::NodeEqual> op_indices_;
  std::unordered_map<Tensor, const Realize *> realize_node_;
  std::unordered_map<FunctionRef, const AttrStmt *, air::NodeHash, air::NodeEqual> attr_node_;

  std::unordered_map<VarExpr, Range, air::NodeHash, air::NodeEqual> dom_map;

  std::unordered_map<size_t, std::pair<Expr, Expr>> global_common_expr_;

  // mark broadcast
  Tensor output_;
  Array<Expr> args_;
  std::unordered_set<const Call *> broadcast_;
  bool reuse_variable_;
  int minimum_split_;
  bool cross_stmt_simplify_;
};

Stmt ToThreeAddress(Stmt stmt, bool reuse_variable, int minimum_split, bool cross_stmt_simplify) {
  stmt = ThreeAddressStmtMutator(reuse_variable, minimum_split, cross_stmt_simplify).Mutate(stmt);
  return Simplify_cce(stmt);
}
}  // namespace ir
}  // namespace akg
