/*
 * remill-ir2c: print a lifted LLVM function as readable C.
 *
 * Made for SICE's !LIFT (PLAN_LIFT_C.md in SICE): the input is one optimized,
 * flat-lifted function -- usually a single basic block of integer arithmetic,
 * loads and stores, ending in a tail call to __remill_flat_jump. The output
 * is compilable C99 that reads like the computation:
 *
 *   - the flat ABI's register arguments become `struct cpu` fields, copied
 *     into locals when read, and the __remill_flat_jump exit becomes
 *     assignments to the registers that changed;
 *   - remill's bookkeeping is dropped: the stores to %PC / %NEXT_PC and the
 *     memory token (a load from %PC is the entry rip);
 *   - memory reads as memory: *(uint32_t *)(rsp - 0x28);
 *   - a value used once is folded into its user (a load only if no write to
 *     memory lies between them, so memory order never changes);
 *   - LLVM's signless integers become unsigned C types, with casts where a
 *     signed operation needs them;
 *   - anything not understood is printed as UNSUPPORTED("<IR>"), which is
 *     left undefined so the C does not compile -- never silently wrong.
 *
 * Any other function (not the flat ABI) is printed as a plain C function of
 * its integer arguments.
 *
 *   remill-ir2c-21 --ir in.ll [--function NAME] [--out out.c]
 */

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <remill/BC/ABI.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

// C operator precedence, higher binds tighter.
enum Prec : int {
  kTernary = 3,
  kLogOr = 4,
  kLogAnd = 5,
  kBitOr = 6,
  kBitXor = 7,
  kBitAnd = 8,
  kEquality = 9,
  kRelational = 10,
  kShift = 11,
  kAdditive = 12,
  kMultiplicative = 13,
  kUnary = 15,
  kPrimary = 16,
};

struct Expr {
  std::string s;
  int prec = kPrimary;
};

std::string Paren(const Expr &e, int min_prec) {
  return e.prec >= min_prec ? e.s : "(" + e.s + ")";
}

Expr Bin(const Expr &a, const char *op, const Expr &b, int prec) {
  // inside a bitwise or shift operator, a different binary operator gets
  // parentheses even where C's precedence would do (x & 1 ^ 1 reads badly,
  // and is what -Wparentheses flags)
  const bool strict = prec == kBitAnd || prec == kBitXor || prec == kBitOr || prec == kShift;
  auto side = [&](const Expr &e, int min) {
    if (strict && e.prec < kUnary && e.prec != prec) {
      return "(" + e.s + ")";
    }
    return Paren(e, min);
  };
  return {side(a, prec) + " " + op + " " + side(b, prec + 1), prec};
}

std::string Hex(uint64_t v) {
  if (v < 10) {
    return std::to_string(v);
  }
  char buf[32];
  std::snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(v));
  return buf;
}

std::string IRText(const llvm::Value *v) {
  std::string s;
  llvm::raw_string_ostream os(s);
  v->print(os);
  // trim, and make it safe inside a C string literal
  std::string out;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c == '\n' ? ' ' : c;
  }
  const auto b = out.find_first_not_of(' ');
  return b == std::string::npos ? out : out.substr(b);
}

class Printer {
 public:
  Printer(llvm::Function &F) : F(F), DL(F.getParent()->getDataLayout()) {}

  std::string Print();
  int unsupported = 0;

 private:
  // ---- types
  static unsigned Width(llvm::Type *t) {
    if (t->isPointerTy()) {
      return 64;
    }
    return t->isIntegerTy() ? t->getIntegerBitWidth() : 0;
  }
  static std::string CType(llvm::Type *t) {
    switch (Width(t)) {
      case 1: return "bool";
      case 8: return "uint8_t";
      case 16: return "uint16_t";
      case 32: return "uint32_t";
      case 64: return "uint64_t";
      case 128: return "unsigned __int128";
      default: return "";
    }
  }
  static std::string SType(unsigned w) {
    switch (w) {
      case 8: return "int8_t";
      case 16: return "int16_t";
      case 32: return "int32_t";
      case 64: return "int64_t";
      case 128: return "__int128";
      default: return "";
    }
  }
  static std::string UType(unsigned w) {
    switch (w) {
      case 1: return "bool";
      case 8: return "uint8_t";
      case 16: return "uint16_t";
      case 32: return "uint32_t";
      case 64: return "uint64_t";
      case 128: return "unsigned __int128";
      default: return "";
    }
  }

  // ---- the flat ABI
  bool IsFlat() const {
    if (F.arg_size() != remill::kNumFlatBlockArgs) {
      return false;
    }
    return F.getArg(remill::kFlatPCArgNum)->getType()->isPointerTy() &&
           F.getArg(remill::kFlatNextPCArgNum)->getType()->isPointerTy();
  }
  // the register index of a flat argument, or -1
  int RegIndex(const llvm::Value *v) const {
    auto *a = llvm::dyn_cast<llvm::Argument>(v);
    if (!flat || !a || a->getParent() != &F) {
      return -1;
    }
    const int i = static_cast<int>(a->getArgNo()) - remill::kFlatFirstRegArgNum;
    return i >= 0 && i < static_cast<int>(remill::kFlatNumRegs) ? i : -1;
  }
  static std::string RegName(int i) {
    std::string s = remill::kFlatRegNames[i];
    for (auto &c : s) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
  }
  static std::string RegCType(int i) {
    if (remill::FlatRegIsFlag(i)) {
      return "uint8_t";
    }
    if (remill::FlatRegIsXMM(i) || remill::FlatRegIsX87(i)) {
      return "unsigned __int128";
    }
    return "uint64_t";
  }
  bool IsPCPointer(const llvm::Value *p) const {
    return flat && (p == F.getArg(remill::kFlatPCArgNum) ||
                    p == F.getArg(remill::kFlatNextPCArgNum));
  }
  // A seeded input (SICE !LIFT renames it X.in and records its live value as
  // the "sice.seed" parameter attribute): an output still equal to that
  // value is unchanged, not a write.
  bool EqualsSeed(const llvm::Value *v, const llvm::Argument *in) const {
    const auto a = F.getAttributes().getParamAttr(in->getArgNo(), "sice.seed");
    if (!a.isStringAttribute()) {
      return false;
    }
    if (auto *ce = llvm::dyn_cast<llvm::ConstantExpr>(v);
        ce && (ce->getOpcode() == llvm::Instruction::PtrToInt || ce->getOpcode() == llvm::Instruction::IntToPtr)) {
      v = ce->getOperand(0);
    }
    auto *c = llvm::dyn_cast<llvm::ConstantInt>(v);
    if (!c || c->getBitWidth() > 64) {
      return false;
    }
    return std::to_string(c->getZExtValue()) == a.getValueAsString().str();
  }
  static const llvm::Function *Callee(const llvm::CallBase *c) {
    // match through the operand: getCalledFunction() is not reliable here
    return llvm::dyn_cast<llvm::Function>(c->getCalledOperand()->stripPointerCasts());
  }
  static bool IsFlatJump(const llvm::Instruction *i) {
    auto *c = llvm::dyn_cast<llvm::CallBase>(i);
    auto *f = c ? Callee(c) : nullptr;
    return f && f->getName() == "__remill_flat_jump";
  }

  // ---- values
  Expr Value(const llvm::Value *v);
  Expr Constant(const llvm::ConstantInt *c);
  Expr Compute(const llvm::Instruction *i);
  const llvm::Instruction *computing = nullptr;  // the sext being printed (Signed)
  Expr Unsupported(const llvm::Value *v) {
    ++unsupported;
    return {"UNSUPPORTED(\"" + IRText(v) + "\")", kPrimary};
  }
  Expr Address(const llvm::Value *ptr) { return Value(ptr); }
  Expr Deref(llvm::Type *t, const llvm::Value *ptr) {
    const Expr a = Address(ptr);
    return {"*(" + CType(t) + " *)" + Paren(a, kUnary), kUnary};
  }
  Expr Cast(const std::string &type, const Expr &e) {
    return {"(" + type + ")" + Paren(e, kUnary), kUnary};
  }
  // a narrow (< 32 bit) arithmetic result, truncated back to its type
  Expr Narrow(llvm::Type *t, Expr e) {
    const unsigned w = Width(t);
    return w > 1 && w < 32 ? Cast(CType(t), e) : e;
  }
  Expr Signed(const llvm::Value *v) {
    if (auto *c = llvm::dyn_cast<llvm::ConstantInt>(v); c && c->getBitWidth() <= 64 && c->getBitWidth() > 1) {
      const int64_t s = c->getSExtValue();  // a literal needs no cast
      return s < 0 ? Expr{"-" + Hex(uint64_t(-(s + 1)) + 1), kUnary} : Expr{Hex(uint64_t(s)), kPrimary};
    }
    if (auto *x = llvm::dyn_cast<llvm::SExtInst>(v);
        x && (folded.count(x) || computing == x) && Width(x->getSrcTy()) > 1) {
      return Cast(SType(Width(v->getType())), Signed(x->getOperand(0)));  // (int64_t)(int32_t)d
    }
    return Cast(SType(Width(v->getType())), Value(v));
  }
  Expr Helper(const std::string &name, unsigned w, std::vector<Expr> args);
  Expr Intrinsic(const llvm::CallBase *c, const llvm::Function *f);

  // ---- naming and folding
  void DecideFolding();
  void DecideLiveness();
  std::set<const llvm::Instruction *> live;
  std::string NewName(const llvm::Value *v);

  llvm::Function &F;
  const llvm::DataLayout &DL;
  bool flat = false;
  std::map<const llvm::Value *, std::string> names;
  std::set<const llvm::Instruction *> folded;
  std::set<std::string> taken;
  std::set<std::string> helpers_needed;
  std::vector<std::string> helper_defs;
  std::set<int> regs_read, regs_written;
  bool reads_rip = false;
  // the last value remill's bookkeeping stored to %PC / %NEXT_PC
  std::map<const llvm::Value *, const llvm::Value *> pc_store;
  int temps = 0;
};

Expr Printer::Constant(const llvm::ConstantInt *c) {
  const unsigned w = c->getBitWidth();
  if (w == 1) {
    return {c->isZero() ? "0" : "1", kPrimary};
  }
  const llvm::APInt &v = c->getValue();
  if (w <= 64) {
    return {Hex(v.getZExtValue()), kPrimary};
  }
  if (v.getActiveBits() <= 64) {
    return {"(unsigned __int128)" + Hex(v.getZExtValue()), kUnary};
  }
  const uint64_t hi = v.lshr(64).trunc(64).getZExtValue(), lo = v.trunc(64).getZExtValue();
  return {"((unsigned __int128)" + Hex(hi) + " << 64 | " + Hex(lo) + ")", kPrimary};
}

std::string Printer::NewName(const llvm::Value *v) {
  // a short, meaningful LLVM name is kept (v.i -> v_i); the rest are tN
  std::string base;
  if (v->hasName()) {
    for (char c : v->getName()) {
      base += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    }
    while (base.size() > 1 && std::isdigit(static_cast<unsigned char>(base.back()))) {
      base.pop_back();  // rsp_slot248 -> rsp_slot (LLVM's numbering)
    }
    while (!base.empty() && base.back() == '_') {
      base.pop_back();
    }
    if (base.size() > 12 || base.find("sroa") != std::string::npos ||
        base.find("insert") != std::string::npos || base.find("extract") != std::string::npos ||
        std::isdigit(static_cast<unsigned char>(base[0]))) {
      base.clear();
    }
  }
  if (flat && base == "c") {
    base.clear();  // the struct cpu pointer
  }
  static const std::set<std::string> reserved = {
      "cpu", "bool", "int", "char", "long", "short", "unsigned", "signed", "if", "else", "for",
      "while", "do", "return", "goto", "switch", "case", "default", "break", "continue", "struct",
      "union", "enum", "void", "const", "static", "inline", "sizeof", "true", "false"};
  std::string name = base.empty() ? "t" + std::to_string(++temps) : base;
  for (int n = 2; taken.count(name) || reserved.count(name); ++n) {
    name = (base.empty() ? "t" + std::to_string(++temps) : base + "_" + std::to_string(n));
  }
  taken.insert(name);
  return name;
}

Expr Printer::Value(const llvm::Value *v) {
  if (auto *i = llvm::dyn_cast<llvm::Instruction>(v)) {
    if (folded.count(i)) {
      return Compute(i);
    }
    auto it = names.find(i);
    return it != names.end() ? Expr{it->second, kPrimary} : Unsupported(v);
  }
  if (auto *a = llvm::dyn_cast<llvm::Argument>(v)) {
    const int r = RegIndex(a);
    if (r >= 0) {
      regs_read.insert(r);
      return {RegName(r), kPrimary};
    }
    auto it = names.find(a);
    if (it != names.end()) {
      return {it->second, kPrimary};
    }
    if (flat && a == F.getArg(remill::kFlatMemoryPointerArgNum)) {
      return {"0 /* memory */", kPrimary};
    }
    return Unsupported(v);
  }
  if (auto *c = llvm::dyn_cast<llvm::ConstantInt>(v)) {
    return Constant(c);
  }
  if (llvm::isa<llvm::ConstantPointerNull>(v)) {
    return {"0", kPrimary};
  }
  if (llvm::isa<llvm::UndefValue>(v)) {  // undef and poison
    return {"0 /* undef */", kPrimary};
  }
  if (auto *ce = llvm::dyn_cast<llvm::ConstantExpr>(v)) {
    switch (ce->getOpcode()) {
      case llvm::Instruction::IntToPtr:
      case llvm::Instruction::PtrToInt:
      case llvm::Instruction::BitCast:
      case llvm::Instruction::AddrSpaceCast:
        return Value(ce->getOperand(0));
      case llvm::Instruction::GetElementPtr: {
        llvm::APInt off(64, 0);
        auto *gep = llvm::cast<llvm::GEPOperator>(ce);
        if (gep->accumulateConstantOffset(DL, off)) {
          const Expr base = Value(gep->getPointerOperand());
          if (off.isZero()) {
            return base;
          }
          const int64_t s = off.getSExtValue();
          return s < 0 ? Bin(base, "-", {Hex(uint64_t(-s)), kPrimary}, kAdditive)
                       : Bin(base, "+", {Hex(uint64_t(s)), kPrimary}, kAdditive);
        }
        break;
      }
      default:
        break;
    }
  }
  return Unsupported(v);
}

Expr Printer::Helper(const std::string &name, unsigned w, std::vector<Expr> args) {
  const std::string fn = name + std::to_string(w);
  if (!helpers_needed.count(fn)) {
    helpers_needed.insert(fn);
    const std::string u = UType(w), s = SType(w), W = std::to_string(w);
    std::string body;
    if (name == "rotl" || name == "fshl") {
      body = "static inline " + u + " " + fn + "(" + u + " a, " + u + " b, " + u + " n) {\n"
             "    n %= " + W + "; return n ? (" + u + ")(a << n | b >> (" + W + " - n)) : a;\n}";
    } else if (name == "fshr") {
      body = "static inline " + u + " " + fn + "(" + u + " a, " + u + " b, " + u + " n) {\n"
             "    n %= " + W + "; return n ? (" + u + ")(a << (" + W + " - n) | b >> n) : b;\n}";
    } else if (name == "umin" || name == "umax") {
      body = "static inline " + u + " " + fn + "(" + u + " a, " + u + " b) { return a " +
             (name == "umin" ? "<" : ">") + " b ? a : b; }";
    } else if (name == "smin" || name == "smax") {
      body = "static inline " + u + " " + fn + "(" + u + " a, " + u + " b) { return (" + s +
             ")a " + (name == "smin" ? "<" : ">") + " (" + s + ")b ? a : b; }";
    } else if (name == "abs") {
      body = "static inline " + u + " " + fn + "(" + u + " a) { return (" + s +
             ")a < 0 ? (" + u + ")-a : a; }";
    } else {  // the overflow bits: uadd_ov, sadd_ov, usub_ov, ssub_ov, umul_ov, smul_ov
      const bool sgn = name[0] == 's';
      const std::string op = name.substr(1, 3), t = sgn ? s : u;
      body = "static inline bool " + fn + "(" + u + " a, " + u + " b) { " + t + " r; return __builtin_" +
             op + "_overflow((" + t + ")a, (" + t + ")b, &r); }";
    }
    helper_defs.push_back(body);
  }
  std::string s = fn + "(";
  for (size_t i = 0; i < args.size(); ++i) {
    s += (i ? ", " : "") + args[i].s;
  }
  return {s + ")", kPrimary};
}

Expr Printer::Intrinsic(const llvm::CallBase *c, const llvm::Function *f) {
  const auto id = f->getIntrinsicID();
  const unsigned w = Width(c->getType());
  auto arg = [&](unsigned n) {
    auto *k = llvm::dyn_cast<llvm::ConstantInt>(c->getArgOperand(n));
    if (n == 2 && k && k->getBitWidth() <= 64) {  // a funnel shift / rotate count
      return Expr{std::to_string(k->getZExtValue()), kPrimary};
    }
    return Value(c->getArgOperand(n));
  };
  switch (id) {
    case llvm::Intrinsic::ctpop:
      return Cast(CType(c->getType()), {"__builtin_popcountll(" + arg(0).s + ")", kPrimary});
    case llvm::Intrinsic::bswap:
      if (w == 16 || w == 32 || w == 64) {
        return {"__builtin_bswap" + std::to_string(w) + "(" + arg(0).s + ")", kPrimary};
      }
      break;
    case llvm::Intrinsic::fshl:
      if (c->getArgOperand(0) == c->getArgOperand(1)) {
        return Helper("rotl", w, {arg(0), arg(1), arg(2)});
      }
      return Helper("fshl", w, {arg(0), arg(1), arg(2)});
    case llvm::Intrinsic::fshr:
      return Helper("fshr", w, {arg(0), arg(1), arg(2)});
    case llvm::Intrinsic::umin: return Helper("umin", w, {arg(0), arg(1)});
    case llvm::Intrinsic::umax: return Helper("umax", w, {arg(0), arg(1)});
    case llvm::Intrinsic::smin: return Helper("smin", w, {arg(0), arg(1)});
    case llvm::Intrinsic::smax: return Helper("smax", w, {arg(0), arg(1)});
    case llvm::Intrinsic::abs: return Helper("abs", w, {arg(0)});
    default:
      break;
  }
  return Unsupported(c);
}

Expr Printer::Compute(const llvm::Instruction *i) {
  using llvm::Instruction;
  const unsigned w = Width(i->getType());
  if (auto *b = llvm::dyn_cast<llvm::BinaryOperator>(i)) {
    const llvm::Value *l = b->getOperand(0), *r = b->getOperand(1);
    auto rc = llvm::dyn_cast<llvm::ConstantInt>(r);
    // x + -8 reads better as x - 8 (and x - -8 as x + 8)
    const bool neg = rc && w <= 64 && w > 1 && rc->getValue().isNegative() &&
                     !rc->getValue().isMinSignedValue();
    auto absc = [&] { return Expr{Hex(rc->getValue().abs().getZExtValue()), kPrimary}; };
    // a constant shift count reads best in decimal
    auto count = [&] {
      return rc && rc->getBitWidth() <= 64 ? Expr{std::to_string(rc->getZExtValue()), kPrimary} : Value(r);
    };
    // narrow multiply / shift: compute in 32 bits (int promotion could overflow)
    auto wide = [&](const llvm::Value *v) {
      return w > 1 && w < 32 ? Cast("uint32_t", Value(v)) : Value(v);
    };
    switch (b->getOpcode()) {
      case Instruction::Add:
        return Narrow(i->getType(), neg ? Bin(Value(l), "-", absc(), kAdditive)
                                        : Bin(Value(l), "+", Value(r), kAdditive));
      case Instruction::Sub:
        return Narrow(i->getType(), neg ? Bin(Value(l), "+", absc(), kAdditive)
                                        : Bin(Value(l), "-", Value(r), kAdditive));
      case Instruction::Mul: return Narrow(i->getType(), Bin(wide(l), "*", Value(r), kMultiplicative));
      case Instruction::UDiv: return Narrow(i->getType(), Bin(Value(l), "/", Value(r), kMultiplicative));
      case Instruction::URem: return Narrow(i->getType(), Bin(Value(l), "%", Value(r), kMultiplicative));
      case Instruction::SDiv:
        return Cast(CType(i->getType()), Bin(Signed(l), "/", Signed(r), kMultiplicative));
      case Instruction::SRem:
        return Cast(CType(i->getType()), Bin(Signed(l), "%", Signed(r), kMultiplicative));
      case Instruction::Shl: return Narrow(i->getType(), Bin(wide(l), "<<", count(), kShift));
      case Instruction::LShr: return Narrow(i->getType(), Bin(Value(l), ">>", count(), kShift));
      case Instruction::AShr:
        return Cast(CType(i->getType()), Bin(Signed(l), ">>", count(), kShift));
      case Instruction::And: return Bin(Value(l), "&", Value(r), kBitAnd);
      case Instruction::Or: return Bin(Value(l), "|", Value(r), kBitOr);
      case Instruction::Xor:
        if (rc && rc->isAllOnesValue() && w > 1) {  // ~x
          return Narrow(i->getType(), {"~" + Paren(Value(l), kUnary), kUnary});
        }
        if (rc && rc->isOne() && w == 1) {
          return {"!" + Paren(Value(l), kUnary), kUnary};
        }
        return Bin(Value(l), "^", Value(r), kBitXor);
      default:
        return Unsupported(i);
    }
  }
  if (auto *c = llvm::dyn_cast<llvm::ICmpInst>(i)) {
    const llvm::Value *l = c->getOperand(0), *r = c->getOperand(1);
    if (!CType(l->getType()).size()) {
      return Unsupported(i);
    }
    switch (c->getPredicate()) {
      case llvm::CmpInst::ICMP_EQ: return Bin(Value(l), "==", Value(r), kEquality);
      case llvm::CmpInst::ICMP_NE: return Bin(Value(l), "!=", Value(r), kEquality);
      case llvm::CmpInst::ICMP_ULT: return Bin(Value(l), "<", Value(r), kRelational);
      case llvm::CmpInst::ICMP_ULE: return Bin(Value(l), "<=", Value(r), kRelational);
      case llvm::CmpInst::ICMP_UGT: return Bin(Value(l), ">", Value(r), kRelational);
      case llvm::CmpInst::ICMP_UGE: return Bin(Value(l), ">=", Value(r), kRelational);
      case llvm::CmpInst::ICMP_SLT: return Bin(Signed(l), "<", Signed(r), kRelational);
      case llvm::CmpInst::ICMP_SLE: return Bin(Signed(l), "<=", Signed(r), kRelational);
      case llvm::CmpInst::ICMP_SGT: return Bin(Signed(l), ">", Signed(r), kRelational);
      case llvm::CmpInst::ICMP_SGE: return Bin(Signed(l), ">=", Signed(r), kRelational);
      default: return Unsupported(i);
    }
  }
  if (auto *s = llvm::dyn_cast<llvm::SelectInst>(i)) {
    if (!CType(i->getType()).size()) {
      return Unsupported(i);
    }
    return {Paren(Value(s->getCondition()), kLogOr) + " ? " + Paren(Value(s->getTrueValue()), kLogOr) +
                " : " + Paren(Value(s->getFalseValue()), kTernary),
            kTernary};
  }
  if (auto *c = llvm::dyn_cast<llvm::CastInst>(i)) {
    const llvm::Value *o = c->getOperand(0);
    const unsigned from = Width(o->getType());
    switch (c->getOpcode()) {
      case Instruction::ZExt: return Cast(CType(i->getType()), Value(o));
      case Instruction::SExt:
        computing = i;
        if (from == 1) {  // i1 -1 / 0
          return Cast(CType(i->getType()), {"-" + Paren(Cast(SType(w), Value(o)), kUnary), kUnary});
        }
        return Cast(CType(i->getType()), Signed(i));
      case Instruction::Trunc:
        if (w == 1) {
          return Cast("bool", Bin(Value(o), "&", {"1", kPrimary}, kBitAnd));
        }
        return Cast(CType(i->getType()), Value(o));
      case Instruction::PtrToInt:
      case Instruction::IntToPtr:
        return w == 64 ? Value(o) : Cast(CType(i->getType()), Value(o));
      case Instruction::BitCast:
        if (from && from == w) {
          return Value(o);
        }
        return Unsupported(i);
      default:
        return Unsupported(i);
    }
  }
  if (auto *f = llvm::dyn_cast<llvm::FreezeInst>(i)) {
    return Value(f->getOperand(0));
  }
  if (auto *l = llvm::dyn_cast<llvm::LoadInst>(i)) {
    if (IsPCPointer(l->getPointerOperand())) {  // remill's PC: the entry rip, or what was stored
      auto it = pc_store.find(l->getPointerOperand());
      if (it != pc_store.end()) {
        return Value(it->second);
      }
      reads_rip = true;
      return {"rip", kPrimary};
    }
    if (!CType(i->getType()).size()) {
      return Unsupported(i);
    }
    return Deref(i->getType(), l->getPointerOperand());
  }
  if (auto *g = llvm::dyn_cast<llvm::GetElementPtrInst>(i)) {
    llvm::APInt off(64, 0);
    const Expr base = Value(g->getPointerOperand());
    if (g->accumulateConstantOffset(DL, off)) {
      if (off.isZero()) {
        return base;
      }
      const int64_t s = off.getSExtValue();
      return s < 0 ? Bin(base, "-", {Hex(uint64_t(-s)), kPrimary}, kAdditive)
                   : Bin(base, "+", {Hex(uint64_t(s)), kPrimary}, kAdditive);
    }
    if (g->getNumIndices() == 1) {  // base + index * element size
      const uint64_t size = DL.getTypeAllocSize(g->getSourceElementType());
      Expr idx = Value(g->getOperand(1));
      if (size != 1) {
        idx = Bin(idx, "*", {Hex(size), kPrimary}, kMultiplicative);
      }
      return Bin(base, "+", idx, kAdditive);
    }
    return Unsupported(i);
  }
  if (auto *c = llvm::dyn_cast<llvm::CallBase>(i)) {
    if (auto *f = Callee(c); f && f->isIntrinsic()) {
      return Intrinsic(c, f);
    }
    if (auto *f = Callee(c); f && f->getName().starts_with("__remill_undefined_")) {
      return {"0 /* undefined */", kPrimary};
    }
    return Unsupported(i);
  }
  if (auto *e = llvm::dyn_cast<llvm::ExtractValueInst>(i)) {
    // {result, overflow} of an *.with.overflow intrinsic
    auto *c = llvm::dyn_cast<llvm::CallBase>(e->getAggregateOperand());
    auto *f = c ? Callee(c) : nullptr;
    if (f && e->getNumIndices() == 1) {
      const unsigned ow = Width(c->getArgOperand(0)->getType());
      const auto a = Value(c->getArgOperand(0)), b = Value(c->getArgOperand(1));
      struct Ov {
        llvm::Intrinsic::ID id;
        const char *helper, *op;
        int prec;
      };
      static const Ov ovs[] = {
          {llvm::Intrinsic::uadd_with_overflow, "uadd_ov", "+", kAdditive},
          {llvm::Intrinsic::sadd_with_overflow, "sadd_ov", "+", kAdditive},
          {llvm::Intrinsic::usub_with_overflow, "usub_ov", "-", kAdditive},
          {llvm::Intrinsic::ssub_with_overflow, "ssub_ov", "-", kAdditive},
          {llvm::Intrinsic::umul_with_overflow, "umul_ov", "*", kMultiplicative},
          {llvm::Intrinsic::smul_with_overflow, "smul_ov", "*", kMultiplicative}};
      for (const auto &o : ovs) {
        if (f->getIntrinsicID() != o.id) {
          continue;
        }
        if (e->getIndices()[0] == 0) {
          const Expr l = std::string(o.op) == "*" && ow < 32 ? Cast("uint32_t", a) : a;
          return Narrow(c->getArgOperand(0)->getType(), Bin(l, o.op, b, o.prec));
        }
        return Helper(o.helper, ow, {a, b});
      }
    }
    return Unsupported(i);
  }
  return Unsupported(i);
}

// Which values fold into their user: a single use in the same block, pure --
// or a load with no write to memory between it and where it is printed.
void Printer::DecideFolding() {
  std::map<const llvm::Instruction *, size_t> pos;
  size_t n = 0;
  for (auto &bb : F) {
    for (auto &i : bb) {
      pos[&i] = n++;
    }
  }
  auto pure = [&](const llvm::Instruction &i) {
    if (llvm::isa<llvm::LoadInst>(i)) {
      return true;  // checked below
    }
    if (auto *c = llvm::dyn_cast<llvm::CallBase>(&i)) {
      auto *f = Callee(c);
      return f && (f->isIntrinsic() || f->getName().starts_with("__remill_undefined_")) &&
             !i.mayHaveSideEffects();
    }
    return !i.mayHaveSideEffects() && !i.mayReadOrWriteMemory() && !llvm::isa<llvm::PHINode>(i) &&
           !i.isTerminator() && !llvm::isa<llvm::AllocaInst>(i);
  };
  for (auto &bb : F) {
    for (auto &i : bb) {
      if (i.getType()->isVoidTy() || !i.hasOneUse() || !pure(i)) {
        continue;
      }
      auto *user = llvm::dyn_cast<llvm::Instruction>(*i.user_begin());
      if (!user || user->getParent() != &bb || llvm::isa<llvm::PHINode>(user)) {
        continue;
      }
      // a load from remill's PC is folded wherever it is (it reads the entry
      // rip or a stored value); an aggregate (overflow intrinsics) always folds
      folded.insert(&i);
    }
  }
  // keep folded expressions readable: a tree of more than 8 operations
  // (casts are free) is cut -- its root gets a local instead
  {
    std::map<const llvm::Instruction *, int> size;
    for (auto &bb : F) {
      for (auto &i : bb) {
        int s = llvm::isa<llvm::CastInst>(i) || llvm::isa<llvm::FreezeInst>(i) ? 0 : 1;
        for (auto &op : i.operands()) {
          if (auto *oi = llvm::dyn_cast<llvm::Instruction>(op.get()); oi && folded.count(oi)) {
            s += size[oi];
          }
        }
        if (s > 8 && folded.count(&i)) {
          folded.erase(&i);
        }
        size[&i] = folded.count(&i) ? s : 0;
      }
    }
  }
  // a no-op cast prints as its operand: fold it at every use, unless its
  // operand is itself folded (that would duplicate a computation or a read)
  for (auto &bb : F) {
    for (auto &i : bb) {
      const bool noop = llvm::isa<llvm::FreezeInst>(i) ||
                        ((llvm::isa<llvm::PtrToIntInst>(i) || llvm::isa<llvm::IntToPtrInst>(i)) &&
                         Width(i.getType()) == 64) ||
                        (llvm::isa<llvm::BitCastInst>(i) && Width(i.getType()) &&
                         Width(i.getType()) == Width(i.getOperand(0)->getType()));
      auto *op = llvm::dyn_cast<llvm::Instruction>(i.getOperand(0));
      if (noop && !i.hasOneUse() && !(op && folded.count(op))) {
        folded.insert(&i);
      }
    }
  }
  // un-fold a load whose value would be printed after a write to memory
  auto writes = [&](size_t from, size_t to) {
    for (auto &bb : F) {
      for (auto &i : bb) {
        const size_t p = pos[&i];
        if (p > from && p < to && i.mayWriteToMemory() && !IsFlatJump(&i)) {
          // the bookkeeping stores to %PC / %NEXT_PC are not printed
          if (auto *s = llvm::dyn_cast<llvm::StoreInst>(&i); s && IsPCPointer(s->getPointerOperand())) {
            continue;
          }
          return true;
        }
      }
    }
    return false;
  };
  for (bool changed = true; changed;) {
    changed = false;
    for (auto &bb : F) {
      for (auto &i : bb) {
        if (!folded.count(&i) || !llvm::isa<llvm::LoadInst>(i)) {
          continue;
        }
        // where it is printed: the first unfolded user up the chain
        const llvm::Instruction *at = &i;
        while (folded.count(at)) {
          at = llvm::cast<llvm::Instruction>(*at->user_begin());
        }
        if (writes(pos[&i], pos[at])) {
          folded.erase(&i);
          changed = true;
        }
      }
    }
  }
}

// What is printed, and everything it needs: stores (but remill's bookkeeping
// to %PC / %NEXT_PC), the changed registers at the exit, the return value,
// and the effects that aren't plain values. A load from %PC needs the value
// last stored to it.
void Printer::DecideLiveness() {
  std::vector<const llvm::Value *> work;
  auto need = [&](const llvm::Value *v) { work.push_back(v); };
  for (auto &i : F.front()) {
    if (auto *s = llvm::dyn_cast<llvm::StoreInst>(&i)) {
      if (!IsPCPointer(s->getPointerOperand())) {
        need(&i);
      }
    } else if (IsFlatJump(&i)) {
      auto *c = llvm::cast<llvm::CallBase>(&i);
      for (int r = 0; r < static_cast<int>(remill::kFlatNumRegs); ++r) {
        const unsigned an = remill::kFlatFirstRegArgNum + r;
        if (r != static_cast<int>(remill::kFlatRIPIndex) && an < c->arg_size()) {
          need(c->getArgOperand(an));
        }
      }
    } else if (auto *rt = llvm::dyn_cast<llvm::ReturnInst>(&i)) {
      if (!flat && rt->getReturnValue()) {
        need(rt->getReturnValue());
      }
    } else if (i.mayHaveSideEffects() && !llvm::isa<llvm::LoadInst>(i)) {
      need(&i);
    }
  }
  while (!work.empty()) {
    auto *i = llvm::dyn_cast<llvm::Instruction>(work.back());
    work.pop_back();
    if (!i || !live.insert(i).second) {
      continue;
    }
    if (auto *l = llvm::dyn_cast<llvm::LoadInst>(i); l && IsPCPointer(l->getPointerOperand())) {
      // the value last stored to that pointer before the load, if any
      const llvm::Value *stored = nullptr;
      for (auto &j : F.front()) {
        if (&j == i) {
          break;
        }
        if (auto *s = llvm::dyn_cast<llvm::StoreInst>(&j); s && s->getPointerOperand() == l->getPointerOperand()) {
          stored = s->getValueOperand();
        }
      }
      if (stored) {
        need(stored);
      }
      continue;
    }
    for (auto &op : i->operands()) {
      need(op.get());
    }
  }
}

std::string Printer::Print() {
  flat = IsFlat();
  if (F.size() != 1) {
    ++unsupported;
    return "#error \"remill-ir2c: @" + F.getName().str() +
           " has several basic blocks; branches are not supported yet\"\n";
  }
  DecideFolding();
  DecideLiveness();
  for (int r = 0; r < static_cast<int>(remill::kFlatNumRegs); ++r) {
    taken.insert(RegName(r));
  }
  if (!flat) {  // a plain function's parameters, before anything refers to them
    int k = 0;
    for (auto &a : F.args()) {
      names[&a] = a.hasName() ? NewName(&a) : "a" + std::to_string(k);
      ++k;
    }
  }
  std::ostringstream body;
  std::vector<std::string> outputs;
  std::string ret;
  for (auto &i : F.front()) {
    if (folded.count(&i)) {
      continue;
    }
    if (auto *s = llvm::dyn_cast<llvm::StoreInst>(&i)) {
      if (IsPCPointer(s->getPointerOperand())) {  // remill's bookkeeping: not printed
        pc_store[s->getPointerOperand()] = s->getValueOperand();
        continue;
      }
      const Expr v = Value(s->getValueOperand());
      body << "    " << Deref(s->getValueOperand()->getType(), s->getPointerOperand()).s << " = " << v.s
           << ";\n";
      continue;
    }
    if (IsFlatJump(&i)) {  // the exit: the registers that changed
      auto *c = llvm::cast<llvm::CallBase>(&i);
      for (int r = 0; r < static_cast<int>(remill::kFlatNumRegs); ++r) {
        if (r == static_cast<int>(remill::kFlatRIPIndex)) {
          continue;  // remill's synthetic next pc, not the real one
        }
        const unsigned an = remill::kFlatFirstRegArgNum + r;
        if (an >= c->arg_size()) {
          break;
        }
        const llvm::Value *v = c->getArgOperand(an);
        const llvm::Value *in = F.getArg(an);
        const llvm::Value *stripped = v;
        if (auto *p = llvm::dyn_cast<llvm::PtrToIntOperator>(v)) {  // RSP/RBP
          stripped = p->getPointerOperand();
        }
        if (stripped == in || EqualsSeed(v, F.getArg(an))) {
          continue;  // unchanged
        }
        regs_written.insert(r);
        outputs.push_back("    c->" + RegName(r) + " = " + Value(v).s + ";\n");
      }
      continue;
    }
    if (auto *rt = llvm::dyn_cast<llvm::ReturnInst>(&i)) {
      if (!flat && rt->getReturnValue()) {
        ret = "    return " + Value(rt->getReturnValue()).s + ";\n";
      }
      continue;
    }
    if (llvm::isa<llvm::FenceInst>(i)) {
      body << "    __atomic_thread_fence(__ATOMIC_SEQ_CST);\n";
      continue;
    }
    if (i.getType()->isVoidTy()) {
      if (auto *c = llvm::dyn_cast<llvm::CallBase>(&i); c && Callee(c) && Callee(c)->isIntrinsic()) {
        continue;  // assume, lifetime, debug info
      }
      body << "    " << Unsupported(&i).s << ";\n";
      continue;
    }
    if (!live.count(&i)) {
      continue;  // dead, or feeding only remill's bookkeeping
    }
    if (i.getType()->isStructTy()) {  // only read through extractvalue (folded)
      continue;
    }
    const std::string type = CType(i.getType());
    const Expr e = Compute(&i);
    const std::string name = NewName(&i);
    names[&i] = name;
    body << "    " << (type.empty() ? "UNSUPPORTED_TYPE" : type) << " " << name << " = " << e.s << ";\n";
    if (type.empty()) {
      ++unsupported;
    }
  }

  std::ostringstream out;
  out << "// remill-ir2c: @" << F.getName().str() << (flat ? " (remill flat ABI)" : "") << "\n";
  out << "#include <stdbool.h>\n#include <stdint.h>\n\n";
  for (const auto &h : helper_defs) {
    out << h << "\n";
  }
  if (!helper_defs.empty()) {
    out << "\n";
  }
  if (flat) {
    if (reads_rip) {
      regs_read.insert(remill::kFlatRIPIndex);
    }
    std::set<int> fields = regs_read;
    fields.insert(regs_written.begin(), regs_written.end());
    out << "struct cpu {";
    if (fields.empty()) {
      out << " uint64_t unused;";  // C needs a member
    }
    std::string last;
    for (int r : fields) {
      const std::string t = RegCType(r);
      out << (t == last ? ", " : (last.empty() ? "\n    " : ";\n    ") + t + " ") << RegName(r);
      last = t;
    }
    out << (fields.empty() ? " };\n\n" : ";\n};\n\n");
    out << "void " << F.getName().str() << "(struct cpu *c) {\n";
    for (int r : regs_read) {  // inputs, read before any output is written
      out << "    " << RegCType(r) << " " << RegName(r) << " = c->" << RegName(r) << ";\n";
    }
  } else {
    const std::string rt = F.getReturnType()->isVoidTy() ? "void" : CType(F.getReturnType());
    out << (rt.empty() ? "UNSUPPORTED_TYPE" : rt) << " " << F.getName().str() << "(";
    int k = 0;
    for (auto &a : F.args()) {
      out << (k++ ? ", " : "") << CType(a.getType()) << " " << names[&a];
    }
    out << ") {\n";
  }
  out << body.str();
  for (const auto &o : outputs) {
    out << o;
  }
  out << ret << "}\n";
  return out.str();
}

std::string Arg(int argc, char **argv, const char *flag) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], flag) == 0) {
      return argv[i + 1];
    }
  }
  return "";
}

}  // namespace

int main(int argc, char **argv) {
  const std::string ir = Arg(argc, argv, "--ir"), fn = Arg(argc, argv, "--function"),
                    out = Arg(argc, argv, "--out");
  if (ir.empty()) {
    std::cerr << "usage: remill-ir2c --ir in.ll [--function NAME] [--out out.c]\n";
    return 1;
  }
  llvm::LLVMContext ctx;
  llvm::SMDiagnostic err;
  auto m = llvm::parseIRFile(ir, err, ctx);
  if (!m) {
    err.print(argv[0], llvm::errs());
    return 1;
  }
  llvm::Function *f = nullptr;
  for (auto &g : *m) {
    if (g.isDeclaration() || (!fn.empty() && g.getName() != fn)) {
      continue;
    }
    if (f && fn.empty()) {
      std::cerr << "remill-ir2c: several functions in " << ir << "; pick one with --function\n";
      return 1;
    }
    f = &g;
  }
  if (!f) {
    std::cerr << "remill-ir2c: no " << (fn.empty() ? "defined function" : "function @" + fn) << " in " << ir
              << "\n";
    return 1;
  }
  Printer p(*f);
  const std::string c = p.Print();
  if (out.empty()) {
    std::cout << c;
  } else {
    std::ofstream(out) << c;
  }
  if (p.unsupported) {
    std::cerr << "remill-ir2c: " << p.unsupported << " unsupported construct(s), marked UNSUPPORTED\n";
  }
  return 0;
}
