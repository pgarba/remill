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
 *   - a vector is its bits (an XMM register: unsigned __int128); lanes are
 *     shifted and masked out of it, double / float bit casts go through
 *     memcpy, so scalar SSE math reads as C doubles;
 *   - a call SICE kept (a "sice.call" function of the whole register state,
 *     returning all of them) is `name(c);`: the registers that changed are
 *     stored to c first, and the results used are read back right after;
 *   - anything not understood is printed as UNSUPPORTED("<IR>"), which is
 *     left undefined so the C does not compile -- never silently wrong.
 *
 * Any other function (not the flat ABI) is printed as a plain C function of
 * its integer arguments.
 *
 *   remill-ir2c-21 --ir in.ll [--function NAME] [--out out.c]
 */

#include <llvm/ADT/APFloat.h>
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
#include <cstdlib>
#include <optional>
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
    if (auto *v = llvm::dyn_cast<llvm::FixedVectorType>(t)) {  // a vector is its bits
      const unsigned b = static_cast<unsigned>(v->getPrimitiveSizeInBits().getFixedValue());
      return b == 8 || b == 16 || b == 32 || b == 64 || b == 128 ? b : 0;
    }
    return t->isIntegerTy() ? t->getIntegerBitWidth() : 0;
  }
  static bool IsFP(llvm::Type *t) { return t->isDoubleTy() || t->isFloatTy(); }
  // a vector's lane: its type and width
  static llvm::Type *Lane(llvm::Type *t) { return llvm::cast<llvm::FixedVectorType>(t)->getElementType(); }
  static unsigned LaneWidth(llvm::Type *t) {
    return static_cast<unsigned>(Lane(t)->getPrimitiveSizeInBits().getFixedValue());
  }
  static std::string CType(llvm::Type *t) {
    if (t->isDoubleTy()) {
      return "double";
    }
    if (t->isFloatTy()) {
      return "float";
    }
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
    const std::string seed = a.getValueAsString().str();
    if (v->getType()->isVectorTy()) {  // an XMM register: "low,high"
      const auto bits = VectorBits(llvm::dyn_cast<llvm::Constant>(v));
      if (!bits || bits->getBitWidth() != 128) {
        return false;
      }
      return std::to_string(bits->trunc(64).getZExtValue()) + "," +
                 std::to_string(bits->lshr(64).trunc(64).getZExtValue()) ==
             seed;
    }
    auto *c = llvm::dyn_cast<llvm::ConstantInt>(v);
    if (!c || c->getBitWidth() > 64) {
      return false;
    }
    return std::to_string(c->getZExtValue()) == seed;
  }
  // A vector constant's bits (undef / poison lanes as 0), or nothing.
  static std::optional<llvm::APInt> VectorBits(const llvm::Constant *c) {
    auto *vt = c ? llvm::dyn_cast<llvm::FixedVectorType>(c->getType()) : nullptr;
    if (!vt) {
      return std::nullopt;
    }
    const unsigned ew = LaneWidth(vt), n = vt->getNumElements();
    llvm::APInt bits(ew * n, 0);
    for (unsigned k = 0; k < n; ++k) {
      const llvm::Constant *e = c->getAggregateElement(k);
      llvm::APInt lane(ew, 0);
      if (auto *ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(e)) {
        lane = ci->getValue();
      } else if (auto *cf = llvm::dyn_cast_or_null<llvm::ConstantFP>(e)) {
        lane = cf->getValueAPF().bitcastToAPInt();
      } else if (!e || !llvm::isa<llvm::UndefValue>(e)) {
        return std::nullopt;
      }
      bits.insertBits(lane, k * ew);
    }
    return bits;
  }
  static const llvm::Function *Callee(const llvm::CallBase *c) {
    // match through the operand: getCalledFunction() is not reliable here
    return llvm::dyn_cast<llvm::Function>(c->getCalledOperand()->stripPointerCasts());
  }
  // a call SICE's !LIFT kept: an opaque function of all registers
  static const llvm::CallBase *KeptCall(const llvm::Value *v) {
    auto *c = llvm::dyn_cast<llvm::CallBase>(v);
    auto *f = c ? Callee(c) : nullptr;
    return f && f->hasFnAttribute("sice.call") && c->getType()->isStructTy() ? c : nullptr;
  }
  // its result for register r (what c->r holds after it)
  static bool IsKeptResult(const llvm::Value *v, int r) {
    auto *e = llvm::dyn_cast<llvm::ExtractValueInst>(v);
    return e && KeptCall(e->getAggregateOperand()) && e->getNumIndices() == 1 &&
           e->getIndices()[0] == static_cast<unsigned>(r);
  }
  // what c->r already holds: the input (or its seed), or a kept call's result
  bool InCpu(const llvm::Value *v, int r) const {
    const unsigned an = remill::kFlatFirstRegArgNum + r;
    const llvm::Value *stripped = v;
    if (auto *fr = llvm::dyn_cast<llvm::FreezeInst>(stripped)) {  // freeze(x) is x
      stripped = fr->getOperand(0);
    }
    if (auto *p = llvm::dyn_cast<llvm::PtrToIntOperator>(stripped)) {  // RSP/RBP
      stripped = p->getPointerOperand();
    }
    if (auto it = holds.find(r); it != holds.end() && (it->second == v || it->second == stripped)) {
      return true;
    }
    return stripped == F.getArg(an) || EqualsSeed(v, F.getArg(an)) || IsKeptResult(stripped, r);
  }
  // after a kept call, c holds what it got for each register it didn't
  // change (SICE passes those by it unchanged)
  std::map<int, const llvm::Value *> holds;
  // (the call's "sice.changed" attribute lists the registers it returns
  // changed; without it, every register is its result)
  static std::set<int> Changed(const llvm::CallBase *kc) {
    std::set<int> changed;
    const auto attr = kc->getFnAttr("sice.changed");
    if (!attr.isStringAttribute()) {
      for (int r = 0; r < static_cast<int>(remill::kFlatNumRegs); ++r) {
        changed.insert(r);
      }
    } else {
      std::istringstream list(attr.getValueAsString().str());
      for (std::string n; std::getline(list, n, ',');) {
        if (!n.empty()) {
          changed.insert(std::atoi(n.c_str()));
        }
      }
    }
    return changed;
  }
  void PassedBy(const llvm::CallBase *kc) {
    const std::set<int> changed = Changed(kc);
    for (int r = 0; r < static_cast<int>(remill::kFlatNumRegs); ++r) {
      const unsigned an = remill::kFlatFirstRegArgNum + r;
      if (an < kc->arg_size() && !changed.count(r)) {
        holds[r] = kc->getArgOperand(an);
      } else {
        holds.erase(r);
      }
    }
  }
  std::set<std::string> kept_calls;  // their prototypes

  // ---- a kept call SICE knew the callee's prototype of: a typed call
  // (call-site attributes: "sice.fn", "sice.ret", "sice.args" =
  // "type@where;..." with where a register or "rsp+0x8", the stack at the
  // call; "sice.header" or "sice.decl"; "sice.str" = "arg:addr:hex;..."
  // the text of const char * arguments as the call ran)
  static std::string Attr(const llvm::CallBase *kc, const char *name) {
    const auto a = kc->getFnAttr(name);
    return a.isStringAttribute() ? a.getValueAsString().str() : "";
  }
  static int RegByName(const std::string &n) {
    for (int r = 0; r < static_cast<int>(remill::kFlatNumRegs); ++r) {
      if (RegName(r) == n) {
        return r;
      }
    }
    return -1;
  }
  struct TypedArg {
    std::string type;
    int reg = -1;         // in this register, else
    uint64_t stack = 0;   // at rsp + stack
    std::optional<std::pair<uint64_t, std::string>> text;
  };
  // its arguments; nullopt when it isn't typed (or the attribute is off)
  static std::optional<std::vector<TypedArg>> TypedArgs(const llvm::CallBase *kc) {
    if (Attr(kc, "sice.fn").empty()) {
      return std::nullopt;
    }
    std::vector<TypedArg> out;
    std::istringstream list(Attr(kc, "sice.args"));
    for (std::string a; std::getline(list, a, ';');) {
      const size_t at = a.rfind('@');
      if (at == std::string::npos) {
        return std::nullopt;
      }
      TypedArg t;
      t.type = a.substr(0, at);
      const std::string where = a.substr(at + 1);
      if (where.rfind("rsp+", 0) == 0) {
        t.stack = std::strtoull(where.c_str() + 4, nullptr, 16);
      } else if ((t.reg = RegByName(where)) < 0) {
        return std::nullopt;
      }
      out.push_back(t);
    }
    std::istringstream strs(Attr(kc, "sice.str"));
    for (std::string e; std::getline(strs, e, ';');) {
      const size_t c1 = e.find(':'), c2 = e.find(':', c1 + 1);
      if (c1 == std::string::npos || c2 == std::string::npos) {
        continue;
      }
      const size_t k = std::strtoull(e.c_str(), nullptr, 10);
      std::string text;
      for (size_t i = c2 + 1; i + 1 < e.size(); i += 2) {
        text += static_cast<char>(std::strtoul(e.substr(i, 2).c_str(), nullptr, 16));
      }
      if (k < out.size()) {
        out[k].text = std::pair{std::strtoull(e.c_str() + c1 + 1, nullptr, 16), text};
      }
    }
    return out;
  }
  // A value known to be a constant: a literal, or a seeded input's seed.
  std::optional<uint64_t> ConstOf(const llvm::Value *v) const {
    if (auto *ce = llvm::dyn_cast<llvm::ConstantExpr>(v);
        ce && (ce->getOpcode() == llvm::Instruction::PtrToInt || ce->getOpcode() == llvm::Instruction::IntToPtr)) {
      v = ce->getOperand(0);
    }
    if (auto *c = llvm::dyn_cast<llvm::ConstantInt>(v); c && c->getBitWidth() <= 64) {
      return c->getZExtValue();
    }
    if (auto *a = llvm::dyn_cast<llvm::Argument>(v); a && a->getParent() == &F) {
      const auto seed = F.getAttributes().getParamAttr(a->getArgNo(), "sice.seed");
      if (seed.isStringAttribute() && !a->getType()->isVectorTy()) {
        return std::strtoull(seed.getValueAsString().str().c_str(), nullptr, 10);
      }
    }
    return std::nullopt;
  }
  static std::string CString(const std::string &s) {
    std::string out = "\"";
    for (const char ch : s) {
      const auto u = static_cast<unsigned char>(ch);
      switch (ch) {
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        default:
          if (u < 0x20 || u >= 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\%03o", u);
            out += buf;
          } else {
            out += ch;
          }
      }
    }
    return out + "\"";
  }
  // register r's value going into kc: c->r when c holds it (and it isn't
  // just a name or a literal), else the value
  Expr RegArg(const llvm::CallBase *kc, int r, const std::set<int> &stored, unsigned bits = 64) {
    const llvm::Value *v = kc->getArgOperand(remill::kFlatFirstRegArgNum + r);
    // for a narrower argument, the bits above it don't matter: x & 0xffffffff, zext x
    for (bool more = bits < 64; more;) {
      more = false;
      if (auto *a = llvm::dyn_cast<llvm::BinaryOperator>(v); a && a->getOpcode() == llvm::Instruction::And) {
        auto *m = llvm::dyn_cast<llvm::ConstantInt>(a->getOperand(1));
        if (m && m->getValue().countr_one() >= bits && (folded.count(a) || !live.count(a))) {
          v = a->getOperand(0);
          more = true;
        }
      } else if (auto *z = llvm::dyn_cast<llvm::ZExtInst>(v);
                 z && Width(z->getSrcTy()) >= bits && (folded.count(z) || !live.count(z))) {
        v = z->getOperand(0);
        more = true;
      }
    }
    if (llvm::isa<llvm::ConstantInt>(v)) {
      return Value(v);
    }
    auto *in = llvm::dyn_cast<llvm::Instruction>(v);
    if (stored.count(r) && in && !folded.count(in)) {
      return Value(v);  // its name
    }
    if (v == kc->getArgOperand(remill::kFlatFirstRegArgNum + r) && (stored.count(r) || InCpu(v, r))) {
      regs_written.insert(r);  // a field of struct cpu
      return {"c->" + RegName(r), kPrimary};
    }
    return Value(v);
  }
  // the index of the parenthesis closing the one at `open`
  static size_t Closes(const std::string &s, size_t open) {
    int depth = 0;
    for (size_t k = open; k < s.size(); ++k) {
      depth += s[k] == '(' ? 1 : s[k] == ')' ? -1 : 0;
      if (depth == 0) {
        return k;
      }
    }
    return std::string::npos;
  }
  // an integer C type's width (0: not one we know)
  static unsigned IntBits(const std::string &t) {
    static const std::map<std::string, unsigned> w = {
        {"char", 8}, {"signed char", 8}, {"unsigned char", 8}, {"_Bool", 8}, {"short", 16},
        {"unsigned short", 16}, {"int", 32}, {"unsigned int", 32}, {"long", 64}, {"unsigned long", 64},
        {"long long", 64}, {"unsigned long long", 64}, {"size_t", 64}, {"ssize_t", 64}, {"int8_t", 8},
        {"uint8_t", 8}, {"int16_t", 16}, {"uint16_t", 16}, {"int32_t", 32}, {"uint32_t", 32},
        {"int64_t", 64}, {"uint64_t", 64}, {"time_t", 64}, {"off_t", 64}, {"pid_t", 32}};
    const auto it = w.find(t);
    return it == w.end() ? 0 : it->second;
  }
  // the typed call: its value, the C expression of the call itself
  Expr TypedCall(const llvm::CallBase *kc, const std::vector<TypedArg> &args, const std::set<int> &stored) {
    std::string call = Attr(kc, "sice.fn") + "(";
    for (size_t k = 0; k < args.size(); ++k) {
      const TypedArg &a = args[k];
      const bool fp = a.type == "double" || a.type == "float";
      Expr e;
      if (a.reg < 0) {  // on the stack
        const Expr rsp = RegArg(kc, static_cast<int>(remill::kFlatRSPIndex), stored);
        e = {"*(" + a.type + (a.type.back() == '*' ? "" : " ") + "*)(" + Bin(rsp, "+", {Hex(a.stack), kPrimary}, kAdditive).s + ")", kUnary};
      } else if (fp) {
        const Expr bits = RegArg(kc, a.reg, stored);
        // a value just made from a double's bits: the double
        const std::string from = a.type == "double" ? "(unsigned __int128)bits_f64(" : "(unsigned __int128)bits_f32(";
        if (bits.s.rfind(from, 0) == 0 && Closes(bits.s, from.size() - 1) == bits.s.size() - 1) {
          e = {bits.s.substr(from.size(), bits.s.size() - from.size() - 1), kTernary};
        } else {
          e = a.type == "double" ? FromBits(llvm::Type::getDoubleTy(F.getContext()), Cast("uint64_t", bits))
                                 : FromBits(llvm::Type::getFloatTy(F.getContext()), Cast("uint32_t", bits));
        }
      } else {
        const llvm::Value *v = kc->getArgOperand(remill::kFlatFirstRegArgNum + a.reg);
        const auto c = ConstOf(v);
        const unsigned bits = IntBits(a.type);
        if (a.text && c && *c == a.text->first) {
          e = {CString(a.text->second), kPrimary};  // the string itself
        } else if (llvm::isa<llvm::ConstantInt>(v) && a.type.back() != '*' && bits) {
          // a literal: as the parameter's type takes it (-1 for an int's 0xffffffff)
          const uint64_t low = bits == 64 ? *c : *c & ((uint64_t(1) << bits) - 1);
          const bool neg = a.type.rfind("unsigned", 0) != 0 && a.type.rfind("uint", 0) != 0 && a.type != "size_t" &&
                           a.type != "_Bool" && low >> (bits - 1);
          e = neg ? Expr{"-" + Hex((bits == 64 ? 0 : uint64_t(1) << bits) - low), kUnary} : Expr{Hex(low), kPrimary};
          if (neg && bits == 64 && low == uint64_t(1) << 63) {
            e = Cast(a.type, Value(v));  // (no literal for INT64_MIN)
          }
        } else {
          e = Cast(a.type, RegArg(kc, a.reg, stored, bits ? bits : 64));
          if (a.text) {
            std::string t = CString(a.text->second.size() > 40 ? a.text->second.substr(0, 40) + "..." : a.text->second);
            for (size_t p = t.find("*/"); p != std::string::npos; p = t.find("*/", p)) {
              t.replace(p, 2, "*\\/");  // (it can't end the comment)
            }
            e.s += " /* " + t + " */";
          }
        }
      }
      call += (k ? ", " : "") + e.s;
    }
    return {call + ")", kPrimary};
  }
  std::set<std::string> c_headers, c_decls;  // what the typed calls need
  static bool IsFlatJump(const llvm::Instruction *i) {
    auto *c = llvm::dyn_cast<llvm::CallBase>(i);
    auto *f = c ? Callee(c) : nullptr;
    return f && f->getName() == "__remill_flat_jump";
  }

  // ---- values
  Expr Value(const llvm::Value *v);
  Expr Constant(const llvm::ConstantInt *c);
  Expr Constant(const llvm::APInt &v);
  Expr Float(const llvm::ConstantFP *c);
  Expr FCompare(const llvm::FCmpInst *c);
  Expr Vector(const llvm::Instruction *i);
  // a vector value's lane k, as its C type (FP lanes as double / float)
  Expr LaneOf(const llvm::Value *v, unsigned k);
  // bits as the lane type, and back: double <-> uint64_t through memcpy
  Expr FromBits(llvm::Type *t, const Expr &bits);
  Expr ToBits(llvm::Type *t, const Expr &v);
  void Need(const std::string &name, const std::string &def) {
    if (helpers_needed.insert(name).second) {
      helper_defs.push_back(def);
    }
  }
  Expr Compute(const llvm::Instruction *i);
  Expr Compare(const llvm::ICmpInst *c, llvm::CmpInst::Predicate pred);
  Expr Negate(const llvm::Value *cond);
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
  std::map<const llvm::AtomicCmpXchgInst *, std::pair<std::string, std::string>> cmpxchg_names;  // old, ok
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

Expr Printer::Constant(const llvm::ConstantInt *c) { return Constant(c->getValue()); }

Expr Printer::Constant(const llvm::APInt &v) {
  const unsigned w = v.getBitWidth();
  if (w == 1) {
    return {v.isZero() ? "0" : "1", kPrimary};
  }
  if (w <= 64) {
    return {Hex(v.getZExtValue()), kPrimary};
  }
  if (v.getActiveBits() <= 64) {
    return {"(unsigned __int128)" + Hex(v.getZExtValue()), kUnary};
  }
  const uint64_t hi = v.lshr(64).trunc(64).getZExtValue(), lo = v.trunc(64).getZExtValue();
  if (!lo) {
    return {"((unsigned __int128)" + Hex(hi) + " << 64)", kPrimary};
  }
  return {"((unsigned __int128)" + Hex(hi) + " << 64 | " + Hex(lo) + ")", kPrimary};
}

// A double / float literal: the shortest decimal that reads back exactly.
Expr Printer::Float(const llvm::ConstantFP *c) {
  const bool f32 = c->getType()->isFloatTy();
  if (!f32 && !c->getType()->isDoubleTy()) {
    return Unsupported(c);
  }
  const llvm::APFloat &a = c->getValueAPF();
  const std::string sfx = f32 ? "f" : "";
  if (a.isNaN()) {
    return {"__builtin_nan" + sfx + "(\"\")", kPrimary};
  }
  if (a.isInfinity()) {
    return a.isNegative() ? Expr{"-__builtin_inf" + sfx + "()", kUnary} : Expr{"__builtin_inf" + sfx + "()", kPrimary};
  }
  const double d = f32 ? static_cast<double>(a.convertToFloat()) : a.convertToDouble();
  char buf[64] = {};
  for (int prec = 1; prec <= 17; ++prec) {
    std::snprintf(buf, sizeof buf, "%.*g", prec, d);
    if (f32 ? std::strtof(buf, nullptr) == static_cast<float>(d) : std::strtod(buf, nullptr) == d) {
      break;
    }
  }
  std::string s = buf;
  if (s.find_first_of(".en") == std::string::npos) {
    s += ".0";
  }
  return {s + sfx, s[0] == '-' ? kUnary : kPrimary};
}

Expr Printer::FromBits(llvm::Type *t, const Expr &bits) {
  if (t->isDoubleTy()) {
    Need("f64_bits", "static inline double f64_bits(uint64_t x) { double d; __builtin_memcpy(&d, &x, 8); return d; }");
    return {"f64_bits(" + bits.s + ")", kPrimary};
  }
  if (t->isFloatTy()) {
    Need("f32_bits", "static inline float f32_bits(uint32_t x) { float f; __builtin_memcpy(&f, &x, 4); return f; }");
    return {"f32_bits(" + bits.s + ")", kPrimary};
  }
  return Cast(CType(t), bits);
}

Expr Printer::ToBits(llvm::Type *t, const Expr &v) {
  if (t->isDoubleTy()) {
    Need("bits_f64", "static inline uint64_t bits_f64(double d) { uint64_t x; __builtin_memcpy(&x, &d, 8); return x; }");
    return {"bits_f64(" + v.s + ")", kPrimary};
  }
  if (t->isFloatTy()) {
    Need("bits_f32", "static inline uint32_t bits_f32(float f) { uint32_t x; __builtin_memcpy(&x, &f, 4); return x; }");
    return {"bits_f32(" + v.s + ")", kPrimary};
  }
  return v;
}

Expr Printer::LaneOf(const llvm::Value *v, unsigned k) {
  llvm::Type *lt = Lane(v->getType());
  const unsigned ew = LaneWidth(v->getType());
  const Expr whole = Value(v);
  const Expr bits = k == 0 ? Cast(UType(ew), whole)
                           : Cast(UType(ew), Bin(whole, ">>", {std::to_string(k * ew), kPrimary}, kShift));
  return lt->isIntegerTy() ? bits : FromBits(lt, bits);
}

// Vector instructions, on the vector's bits: lanes in and out by shifts.
Expr Printer::Vector(const llvm::Instruction *i) {
  using llvm::Instruction;
  llvm::Type *t = i->getType();
  const unsigned W = Width(t);
  auto *vt = llvm::dyn_cast<llvm::FixedVectorType>(t);
  if (!W || !vt || !UType(LaneWidth(t)).size() || !CType(Lane(t)).size()) {
    return Unsupported(i);
  }
  const unsigned ew = LaneWidth(t), n = vt->getNumElements();
  llvm::Type *lt = Lane(t);
  // lane k's value (a C expression of the lane type) placed at its bits
  auto place = [&](const Expr &lane, unsigned k) {
    const Expr bits = Cast(UType(W), lt->isIntegerTy() ? lane : ToBits(lt, lane));  // unsigned: zero-extends
    return k == 0 ? bits : Bin(bits, "<<", {std::to_string(k * ew), kPrimary}, kShift);
  };
  auto pack = [&](const std::vector<std::optional<Expr>> &lanes) {
    std::optional<Expr> out;
    for (unsigned k = 0; k < lanes.size(); ++k) {
      if (lanes[k]) {
        const Expr p = place(*lanes[k], k);
        out = out ? Bin(*out, "|", p, kBitOr) : p;
      }
    }
    return out ? *out : Expr{"0", kPrimary};
  };
  if (auto *ie = llvm::dyn_cast<llvm::InsertElementInst>(i)) {
    auto *idx = llvm::dyn_cast<llvm::ConstantInt>(ie->getOperand(2));
    if (!idx || idx->getZExtValue() >= n) {
      return Unsupported(i);
    }
    const unsigned k = static_cast<unsigned>(idx->getZExtValue());
    const Expr lane = place(Value(ie->getOperand(1)), k);
    const llvm::Value *base = ie->getOperand(0);
    if (auto *bc = llvm::dyn_cast<llvm::Constant>(base)) {
      if (auto bits = VectorBits(bc)) {
        bits->insertBits(llvm::APInt(ew, 0), k * ew);  // the lane being replaced
        return bits->isZero() ? lane : Bin(Constant(*bits), "|", lane, kBitOr);
      }
    }
    llvm::APInt keep = ~llvm::APInt::getBitsSet(W, k * ew, k * ew + ew);
    return Bin(Bin(Value(base), "&", Constant(keep), kBitAnd), "|", lane, kBitOr);
  }
  if (auto *sv = llvm::dyn_cast<llvm::ShuffleVectorInst>(i)) {
    const unsigned in_n = llvm::cast<llvm::FixedVectorType>(sv->getOperand(0)->getType())->getNumElements();
    std::vector<std::optional<Expr>> lanes(n);
    for (unsigned k = 0; k < n; ++k) {
      const int m = sv->getMaskValue(k);
      if (m < 0) {
        continue;  // undef lane: 0
      }
      const llvm::Value *src = sv->getOperand(static_cast<unsigned>(m) < in_n ? 0 : 1);
      if (auto *sc = llvm::dyn_cast<llvm::Constant>(src); sc && VectorBits(sc) && VectorBits(sc)->isZero()) {
        continue;
      }
      lanes[k] = LaneOf(src, static_cast<unsigned>(m) % in_n);
    }
    return pack(lanes);
  }
  if (auto *b = llvm::dyn_cast<llvm::BinaryOperator>(i)) {
    switch (b->getOpcode()) {  // bitwise: on the whole vector
      case Instruction::And: return Bin(Value(b->getOperand(0)), "&", Value(b->getOperand(1)), kBitAnd);
      case Instruction::Or: return Bin(Value(b->getOperand(0)), "|", Value(b->getOperand(1)), kBitOr);
      case Instruction::Xor: return Bin(Value(b->getOperand(0)), "^", Value(b->getOperand(1)), kBitXor);
      default: break;
    }
    const char *op = nullptr;
    int prec = kAdditive;
    switch (b->getOpcode()) {  // lane by lane
      case Instruction::Add: case Instruction::FAdd: op = "+"; break;
      case Instruction::Sub: case Instruction::FSub: op = "-"; break;
      case Instruction::Mul: case Instruction::FMul: op = "*"; prec = kMultiplicative; break;
      case Instruction::FDiv: op = "/"; prec = kMultiplicative; break;
      default: return Unsupported(i);
    }
    std::vector<std::optional<Expr>> lanes(n);
    for (unsigned k = 0; k < n; ++k) {
      Expr l = LaneOf(b->getOperand(0), k);
      if (lt->isIntegerTy() && ew < 32) {
        l = Cast("uint32_t", l);
      }
      lanes[k] = Bin(l, op, LaneOf(b->getOperand(1), k), prec);
    }
    return pack(lanes);
  }
  if (auto *c = llvm::dyn_cast<llvm::CastInst>(i); c && c->getOpcode() == Instruction::BitCast) {
    llvm::Type *from = c->getOperand(0)->getType();
    if (Width(from) == W) {
      return Value(c->getOperand(0));  // vector <-> vector / integer: the same bits
    }
    if (IsFP(from) && from->getPrimitiveSizeInBits() == W) {
      return Cast(UType(W), ToBits(from, Value(c->getOperand(0))));
    }
  }
  return Unsupported(i);
}

Expr Printer::FCompare(const llvm::FCmpInst *c) {
  const Expr a = Value(c->getOperand(0)), b = Value(c->getOperand(1));
  auto call = [&](const char *fn) { return Expr{std::string(fn) + "(" + a.s + ", " + b.s + ")", kPrimary}; };
  auto not_ = [&](const Expr &e) { return Expr{"!" + Paren(e, kUnary), kUnary}; };
  switch (c->getPredicate()) {
    case llvm::CmpInst::FCMP_FALSE: return {"0", kPrimary};
    case llvm::CmpInst::FCMP_TRUE: return {"1", kPrimary};
    case llvm::CmpInst::FCMP_OEQ: return Bin(a, "==", b, kEquality);
    case llvm::CmpInst::FCMP_OGT: return Bin(a, ">", b, kRelational);
    case llvm::CmpInst::FCMP_OGE: return Bin(a, ">=", b, kRelational);
    case llvm::CmpInst::FCMP_OLT: return Bin(a, "<", b, kRelational);
    case llvm::CmpInst::FCMP_OLE: return Bin(a, "<=", b, kRelational);
    case llvm::CmpInst::FCMP_ONE: return call("__builtin_islessgreater");
    case llvm::CmpInst::FCMP_ORD: return not_(call("__builtin_isunordered"));
    case llvm::CmpInst::FCMP_UNO: return call("__builtin_isunordered");
    case llvm::CmpInst::FCMP_UEQ: return not_(call("__builtin_islessgreater"));
    case llvm::CmpInst::FCMP_UGT: return not_(Bin(a, "<=", b, kRelational));
    case llvm::CmpInst::FCMP_UGE: return not_(Bin(a, "<", b, kRelational));
    case llvm::CmpInst::FCMP_ULT: return not_(Bin(a, ">=", b, kRelational));
    case llvm::CmpInst::FCMP_ULE: return not_(Bin(a, ">", b, kRelational));
    case llvm::CmpInst::FCMP_UNE: return Bin(a, "!=", b, kEquality);
    default: return Unsupported(c);
  }
}

std::string Printer::NewName(const llvm::Value *v) {
  // a short, meaningful LLVM name is kept (v.i -> v_i); the rest are tN
  std::string base;
  if (v->hasName()) {
    for (char c : v->getName()) {
      base += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    }
    for (bool more = true; more;) {
      more = false;
      while (base.size() > 1 && std::isdigit(static_cast<unsigned char>(base.back()))) {
        base.pop_back();  // rsp_slot248 -> rsp_slot (LLVM's numbering)
      }
      if (base.size() > 2 && base.compare(base.size() - 2, 2, "_i") == 0) {
        base.resize(base.size() - 2);  // rsp_next.i -> rsp_next (the inliner's suffix)
        more = true;
      }
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
  if (auto *k = llvm::dyn_cast<llvm::Constant>(v); k && v->getType()->isVectorTy()) {
    if (const auto bits = VectorBits(k); bits && Width(v->getType())) {
      return Constant(*bits);
    }
    return Unsupported(v);
  }
  if (auto *c = llvm::dyn_cast<llvm::ConstantInt>(v)) {
    return Constant(c);
  }
  if (auto *f = llvm::dyn_cast<llvm::ConstantFP>(v)) {
    return Float(f);
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
  if (IsFP(c->getType())) {  // the libm-like intrinsics: a builtin each
    const std::string f32 = c->getType()->isFloatTy() ? "f" : "";
    const char *fn = nullptr;
    switch (id) {
      case llvm::Intrinsic::fabs: fn = "fabs"; break;
      case llvm::Intrinsic::sqrt: fn = "sqrt"; break;
      case llvm::Intrinsic::floor: fn = "floor"; break;
      case llvm::Intrinsic::ceil: fn = "ceil"; break;
      case llvm::Intrinsic::trunc: fn = "trunc"; break;
      case llvm::Intrinsic::round: fn = "round"; break;
      case llvm::Intrinsic::rint: fn = "rint"; break;
      case llvm::Intrinsic::nearbyint: fn = "nearbyint"; break;
      case llvm::Intrinsic::minnum: fn = "fmin"; break;
      case llvm::Intrinsic::maxnum: fn = "fmax"; break;
      case llvm::Intrinsic::copysign: fn = "copysign"; break;
      case llvm::Intrinsic::fma: fn = "fma"; break;
      default: break;
    }
    if (fn) {
      std::string s = "__builtin_" + std::string(fn) + f32 + "(";
      for (unsigned k = 0; k < c->arg_size(); ++k) {
        s += (k ? ", " : "") + Value(c->getArgOperand(k)).s;
      }
      return {s + ")", kPrimary};
    }
  }
  return Unsupported(c);
}

Expr Printer::Compare(const llvm::ICmpInst *c, llvm::CmpInst::Predicate pred) {
  const llvm::Value *l = c->getOperand(0), *r = c->getOperand(1);
  if (!CType(l->getType()).size()) {
    return Unsupported(c);
  }
  switch (pred) {
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
    default: return Unsupported(c);
  }
}

// The condition, negated: a folded comparison flips its operator.
Expr Printer::Negate(const llvm::Value *cond) {
  if (auto *c = llvm::dyn_cast<llvm::ICmpInst>(cond); c && folded.count(c)) {
    return Compare(c, c->getInversePredicate());
  }
  return {"!" + Paren(Value(cond), kUnary), kUnary};
}

Expr Printer::Compute(const llvm::Instruction *i) {
  using llvm::Instruction;
  const unsigned w = Width(i->getType());
  if (i->getType()->isVectorTy() && !llvm::isa<llvm::LoadInst>(i) && !llvm::isa<llvm::FreezeInst>(i) &&
      !llvm::isa<llvm::SelectInst>(i) && !llvm::isa<llvm::PHINode>(i)) {
    return Vector(i);
  }
  if (auto *s = llvm::dyn_cast<llvm::SelectInst>(i); s && s->getCondition()->getType()->isVectorTy()) {
    return Unsupported(i);  // a lane-wise select
  }
  if (auto *e = llvm::dyn_cast<llvm::ExtractElementInst>(i)) {
    auto *idx = llvm::dyn_cast<llvm::ConstantInt>(e->getIndexOperand());
    if (!idx || !Width(e->getVectorOperandType())) {
      return Unsupported(i);
    }
    return LaneOf(e->getVectorOperand(), static_cast<unsigned>(idx->getZExtValue()));
  }
  if (auto *c = llvm::dyn_cast<llvm::FCmpInst>(i)) {
    return FCompare(c);
  }
  if (auto *u = llvm::dyn_cast<llvm::UnaryOperator>(i); u && u->getOpcode() == Instruction::FNeg) {
    return {"-" + Paren(Value(u->getOperand(0)), kUnary), kUnary};
  }
  if (auto *b = llvm::dyn_cast<llvm::BinaryOperator>(i); b && IsFP(i->getType())) {
    const Expr l = Value(b->getOperand(0)), r = Value(b->getOperand(1));
    switch (b->getOpcode()) {
      case Instruction::FAdd: return Bin(l, "+", r, kAdditive);
      case Instruction::FSub: return Bin(l, "-", r, kAdditive);
      case Instruction::FMul: return Bin(l, "*", r, kMultiplicative);
      case Instruction::FDiv: return Bin(l, "/", r, kMultiplicative);
      case Instruction::FRem:
        return {std::string(i->getType()->isFloatTy() ? "__builtin_fmodf(" : "__builtin_fmod(") + l.s + ", " + r.s +
                    ")",
                kPrimary};
      default: return Unsupported(i);
    }
  }
  if (auto *b = llvm::dyn_cast<llvm::BinaryOperator>(i)) {
    const llvm::Value *l = b->getOperand(0), *r = b->getOperand(1);
    auto rc = llvm::dyn_cast<llvm::ConstantInt>(r);
    // x + -8 reads better as x - 8 (and x - -8 as x + 8)
    const bool neg = rc && w <= 64 && w > 1 && rc->getValue().isNegative() &&
                     !rc->getValue().isMinSignedValue();
    auto absc = [&] { return Expr{Hex(rc->getValue().abs().getZExtValue()), kPrimary}; };
    // a constant shift count reads best in decimal
    auto count = [&] {
      return rc && rc->getValue().getActiveBits() <= 64 ? Expr{std::to_string(rc->getZExtValue()), kPrimary}
                                                         : Value(r);
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
    return Compare(c, c->getPredicate());
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
        if (IsFP(i->getType()) && from == i->getType()->getPrimitiveSizeInBits()) {
          return FromBits(i->getType(), Value(o));  // the bits as a double / float
        }
        if (IsFP(o->getType()) && w == o->getType()->getPrimitiveSizeInBits()) {
          return ToBits(o->getType(), Value(o));
        }
        return Unsupported(i);
      case Instruction::SIToFP:
        return Cast(CType(i->getType()), from == 1 ? Cast("int", Value(o)) : Signed(o));
      case Instruction::UIToFP:
      case Instruction::FPExt:
      case Instruction::FPTrunc:
        return Cast(CType(i->getType()), Value(o));
      case Instruction::FPToSI:
        return Cast(CType(i->getType()), Cast(SType(w), Value(o)));
      case Instruction::FPToUI:
        return Cast(CType(i->getType()), Value(o));
      default:
        return Unsupported(i);
    }
  }
  if (auto *a = llvm::dyn_cast<llvm::AtomicRMWInst>(i)) {
    const char *fn = nullptr;
    switch (a->getOperation()) {
      case llvm::AtomicRMWInst::Xchg: fn = "__atomic_exchange_n"; break;
      case llvm::AtomicRMWInst::Add: fn = "__atomic_fetch_add"; break;
      case llvm::AtomicRMWInst::Sub: fn = "__atomic_fetch_sub"; break;
      case llvm::AtomicRMWInst::And: fn = "__atomic_fetch_and"; break;
      case llvm::AtomicRMWInst::Or: fn = "__atomic_fetch_or"; break;
      case llvm::AtomicRMWInst::Xor: fn = "__atomic_fetch_xor"; break;
      case llvm::AtomicRMWInst::Nand: fn = "__atomic_fetch_nand"; break;
      default: return Unsupported(i);
    }
    return {std::string(fn) + "((" + CType(i->getType()) + " *)" + Paren(Address(a->getPointerOperand()), kUnary) +
                ", " + Value(a->getValOperand()).s + ", __ATOMIC_SEQ_CST)",
            kPrimary};
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
    if (auto *cx = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(e->getAggregateOperand());
        cx && e->getNumIndices() == 1 && cmpxchg_names.count(cx)) {  // {old value, success}
      return {e->getIndices()[0] == 0 ? cmpxchg_names[cx].first : cmpxchg_names[cx].second, kPrimary};
    }
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
      if (auto *e = llvm::dyn_cast<llvm::ExtractValueInst>(&i); e && KeptCall(e->getAggregateOperand())) {
        continue;  // read from c right after the call, before anything overwrites it
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
      if (!noop) {
        continue;  // (and fence / unreachable have no operand 0)
      }
      auto *op = llvm::dyn_cast<llvm::Instruction>(i.getOperand(0));
      if (!i.hasOneUse() && !(op && folded.count(op))) {
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
  holds.clear();  // (in program order, as Print goes)
  for (auto &bb : F) {
    for (auto &i : bb) {
      if (auto *kc = KeptCall(&i)) {
        need(&i);
        // typed: what goes into c is what it passes by; the arguments are its own
        const auto typed = TypedArgs(kc);
        const std::set<int> changed = typed ? Changed(kc) : std::set<int>{};
        std::set<int> args;
        for (const auto &a : typed ? *typed : std::vector<TypedArg>{}) {
          args.insert(a.reg < 0 ? static_cast<int>(remill::kFlatRSPIndex) : a.reg);
        }
        for (int r = 0; r < static_cast<int>(remill::kFlatNumRegs); ++r) {
          const unsigned an = remill::kFlatFirstRegArgNum + r;
          if (r != static_cast<int>(remill::kFlatRIPIndex) && an < kc->arg_size() &&
              !InCpu(kc->getArgOperand(an), r) && (!changed.count(r) || args.count(r))) {
            need(kc->getArgOperand(an));
          }
        }
        PassedBy(kc);
        continue;
      }
      if (auto *s = llvm::dyn_cast<llvm::StoreInst>(&i)) {
        if (!IsPCPointer(s->getPointerOperand())) {
          need(&i);
        }
      } else if (IsFlatJump(&i)) {
        auto *c = llvm::cast<llvm::CallBase>(&i);
        for (int r = 0; r < static_cast<int>(remill::kFlatNumRegs); ++r) {
          const unsigned an = remill::kFlatFirstRegArgNum + r;
          if (r != static_cast<int>(remill::kFlatRIPIndex) && an < c->arg_size() &&
              !InCpu(c->getArgOperand(an), r)) {
            need(c->getArgOperand(an));  // (one already in c isn't printed)
          }
        }
      } else if (auto *rt = llvm::dyn_cast<llvm::ReturnInst>(&i)) {
        if (!flat && rt->getReturnValue()) {
          need(rt->getReturnValue());
        }
      } else if (auto *br = llvm::dyn_cast<llvm::BranchInst>(&i)) {
        if (br->isConditional()) {
          need(br->getCondition());
        }
      } else if (auto *sw = llvm::dyn_cast<llvm::SwitchInst>(&i)) {
        need(sw->getCondition());
      } else if (i.mayHaveSideEffects() && !llvm::isa<llvm::LoadInst>(i)) {
        need(&i);
      }
    }
  }
  while (!work.empty()) {
    auto *i = llvm::dyn_cast<llvm::Instruction>(work.back());
    work.pop_back();
    if (!i || !live.insert(i).second) {
      continue;
    }
    if (KeptCall(i)) {
      continue;  // its arguments: decided above, in order
    }
    if (auto *l = llvm::dyn_cast<llvm::LoadInst>(i); l && IsPCPointer(l->getPointerOperand())) {
      // the value last stored to that pointer before the load, if any
      const llvm::Value *stored = nullptr;
      for (auto &bb : F) {
        for (auto &j : bb) {
          if (&j == i) {
            goto found;
          }
          if (auto *s = llvm::dyn_cast<llvm::StoreInst>(&j); s && s->getPointerOperand() == l->getPointerOperand()) {
            stored = s->getValueOperand();
          }
        }
      }
    found:
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
  const bool multi = F.size() > 1;
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
  // every printed value's name, up front: an edge may assign a phi of a later
  // block. In a function with several blocks they are declared at the top
  // (C99 has no declaration right after a label).
  std::map<std::string, std::vector<std::string>> decls;  // type -> names
  for (auto &bb : F) {
    for (auto &i : bb) {
      if (folded.count(&i) || !live.count(&i) || i.getType()->isVoidTy()) {
        continue;
      }
      if (auto *cx = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&i)) {
        const std::string base = NewName(&i);
        cmpxchg_names[cx] = {base + "_old", base + "_ok"};
        decls[CType(cx->getCompareOperand()->getType())].push_back(base + "_old");
        decls["bool"].push_back(base + "_ok");
        continue;
      }
      if (i.getType()->isStructTy()) {
        continue;  // read through extractvalue (folded)
      }
      names[&i] = NewName(&i);
      const std::string t = CType(i.getType());
      decls[t.empty() ? "UNSUPPORTED_TYPE" : t].push_back(names[&i]);
    }
  }
  std::map<const llvm::BasicBlock *, std::string> label;
  {
    int n = 0;
    for (auto &bb : F) {
      if (&bb != &F.front()) {
        label[&bb] = "bb" + std::to_string(++n);
      }
    }
  }
  holds.clear();
  std::ostringstream body;
  std::set<std::string> jumped;  // labels a goto / case refers to
  auto stmt = [&](const std::string &s) { body << "    " << s << "\n"; };
  // define (one block) or assign (several) a printed value
  auto define = [&](const llvm::Instruction *i, const std::string &name, const std::string &expr) {
    const std::string t = CType(i->getType());
    if (multi) {
      stmt(name + " = " + expr + ";");
    } else {
      stmt((t.empty() ? "UNSUPPORTED_TYPE" : t) + " " + name + " = " + expr + ";");
    }
    if (t.empty()) {
      ++unsupported;
    }
  };
  // the phi assignments on the edge from -> to (through temporaries when one
  // incoming value is itself a phi of `to`: the copies are parallel)
  auto edge = [&](const llvm::BasicBlock *from, const llvm::BasicBlock *to) {
    std::vector<std::pair<const llvm::PHINode *, const llvm::Value *>> copies;
    bool clash = false;
    for (auto &phi : to->phis()) {
      if (!live.count(&phi)) {
        continue;
      }
      const llvm::Value *v = phi.getIncomingValueForBlock(from);
      if (v == &phi) {
        continue;  // p = p
      }
      if (auto *vp = llvm::dyn_cast<llvm::PHINode>(v); vp && vp->getParent() == to) {
        clash = true;
      }
      copies.push_back({&phi, v});
    }
    std::vector<std::string> out;
    if (!clash) {
      for (auto &[phi, v] : copies) {
        out.push_back(names[phi] + " = " + Value(v).s + ";");
      }
      return out;
    }
    std::string a = "{ ", b;
    int k = 0;
    for (auto &[phi, v] : copies) {
      const std::string tmp = "phi_" + std::to_string(k++);
      a += CType(phi->getType()) + " " + tmp + " = " + Value(v).s + "; ";
      b += names[phi] + " = " + tmp + "; ";
    }
    out.push_back(a + b + "}");
    return out;
  };
  auto jump = [&](const llvm::BasicBlock *from, const llvm::BasicBlock *to, const llvm::BasicBlock *next,
                  const std::string &indent) {
    for (auto &c : edge(from, to)) {
      stmt(indent + c);
    }
    if (to != next) {
      stmt(indent + "goto " + label[to] + ";");
      jumped.insert(label[to]);
    }
  };

  for (auto it = F.begin(); it != F.end(); ++it) {
    const llvm::BasicBlock &bb = *it;
    const llvm::BasicBlock *next = std::next(it) == F.end() ? nullptr : &*std::next(it);
    if (&bb != &F.front()) {
      body << label[&bb] << ":\n";
    }
    for (auto &i : bb) {
      if (folded.count(&i) || llvm::isa<llvm::PHINode>(i)) {
        continue;
      }
      if (auto *e = llvm::dyn_cast<llvm::ExtractValueInst>(&i); e && KeptCall(e->getAggregateOperand())) {
        continue;  // printed with its call
      }
      if (auto *kc = KeptCall(&i)) {  // registers into c, the call, the results used out of c
        // typed: only the registers it passes by go into c (the others it
        // changed: its arguments, and what it clobbers)
        const auto typed = TypedArgs(kc);
        const std::set<int> changed = typed ? Changed(kc) : std::set<int>{};
        std::set<int> stored;
        for (int r = 0; r < static_cast<int>(remill::kFlatNumRegs); ++r) {
          const unsigned an = remill::kFlatFirstRegArgNum + r;
          if (r == static_cast<int>(remill::kFlatRIPIndex) || an >= kc->arg_size() ||
              InCpu(kc->getArgOperand(an), r) || changed.count(r)) {
            continue;
          }
          regs_written.insert(r);
          stored.insert(r);
          stmt("c->" + RegName(r) + " = " + Value(kc->getArgOperand(an)).s + ";");
        }
        const llvm::Function *f = Callee(kc);
        std::string at = Attr(kc, "sice.target");
        if (at.empty()) {  // (older SICE: on the declaration)
          at = f->getFnAttribute("sice.call").getValueAsString().str();
        }
        if (at.empty()) {
          at = "0";
        }
        if (typed) {
          const std::string ret = Attr(kc, "sice.ret");
          const Expr call = TypedCall(kc, *typed, stored);
          const std::string note = at == "0" || at == "0x0" ? "" : "  // call " + at;
          if (const std::string h = Attr(kc, "sice.header"); !h.empty()) {
            c_headers.insert(h);
          } else if (const std::string d = Attr(kc, "sice.decl"); !d.empty()) {
            c_decls.insert(d + ";");
          }
          if (ret == "void" || ret.empty()) {
            stmt(call.s + ";" + note);
          } else if (ret == "double" || ret == "float") {
            const int x0 = RegByName("xmm0");
            regs_written.insert(x0);
            stmt("c->xmm0 = " + ToBits(ret == "double" ? llvm::Type::getDoubleTy(F.getContext())
                                                       : llvm::Type::getFloatTy(F.getContext()), call).s + ";" + note);
          } else {
            regs_written.insert(0);
            stmt("c->rax = " + (ret.back() == '*' ? Cast("uint64_t", call).s : call.s) + ";" + note);
          }
        } else {
          const std::string name = f->getName().str();
          kept_calls.insert(name);
          stmt(name + "(c, " + at + ");");
        }
        PassedBy(kc);
        std::map<unsigned, const llvm::ExtractValueInst *> results;
        for (auto *u : kc->users()) {
          if (auto *e = llvm::dyn_cast<llvm::ExtractValueInst>(u); e && live.count(e) && e->getNumIndices() == 1) {
            results[e->getIndices()[0]] = e;
          }
        }
        for (auto &[r, e] : results) {
          regs_written.insert(static_cast<int>(r));  // a field of struct cpu
          define(e, names[e], "c->" + RegName(static_cast<int>(r)));
        }
        continue;
      }
      if (auto *s = llvm::dyn_cast<llvm::StoreInst>(&i)) {
        if (IsPCPointer(s->getPointerOperand())) {  // remill's bookkeeping: not printed
          pc_store[s->getPointerOperand()] = s->getValueOperand();
          continue;
        }
        const Expr v = Value(s->getValueOperand());
        stmt(Deref(s->getValueOperand()->getType(), s->getPointerOperand()).s + " = " + v.s + ";");
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
          if (InCpu(v, r)) {
            continue;  // unchanged, or already there (a kept call's result)
          }
          regs_written.insert(r);
          stmt("c->" + RegName(r) + " = " + Value(v).s + ";");
        }
        if (multi) {
          stmt("return;");
        }
        continue;
      }
      if (auto *rt = llvm::dyn_cast<llvm::ReturnInst>(&i)) {
        if (!flat && rt->getReturnValue()) {
          stmt("return " + Value(rt->getReturnValue()).s + ";");
        } else if (multi && !flat) {
          stmt("return;");
        }
        continue;
      }
      if (auto *br = llvm::dyn_cast<llvm::BranchInst>(&i)) {
        if (br->isUnconditional()) {
          jump(&bb, br->getSuccessor(0), next, "");
          continue;
        }
        const llvm::BasicBlock *t = br->getSuccessor(0), *f = br->getSuccessor(1);
        std::string cond = Value(br->getCondition()).s;
        if (t == next && f != next) {  // fall into the true side: test the negation
          std::swap(t, f);
          cond = Negate(br->getCondition()).s;
        }
        const auto tc = edge(&bb, t);
        jumped.insert(label[t]);
        if (tc.empty()) {
          stmt("if (" + cond + ") goto " + label[t] + ";");
        } else {
          stmt("if (" + cond + ") {");
          for (auto &c : tc) {
            stmt("    " + c);
          }
          stmt("    goto " + label[t] + ";");
          stmt("}");
        }
        jump(&bb, f, next, "");
        continue;
      }
      if (auto *sw = llvm::dyn_cast<llvm::SwitchInst>(&i)) {
        stmt("switch (" + Value(sw->getCondition()).s + ") {");
        for (auto &cs : sw->cases()) {
          stmt("case " + Constant(cs.getCaseValue()).s + ":");
          jump(&bb, cs.getCaseSuccessor(), nullptr, "    ");
        }
        stmt("default:");
        jump(&bb, sw->getDefaultDest(), nullptr, "    ");
        stmt("}");
        continue;
      }
      if (llvm::isa<llvm::UnreachableInst>(i)) {
        stmt("__builtin_unreachable();");
        continue;
      }
      if (llvm::isa<llvm::FenceInst>(i)) {
        stmt("__atomic_thread_fence(__ATOMIC_SEQ_CST);");
        continue;
      }
      if (auto *cx = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&i)) {
        // *expected = old (C's builtin writes the current value back on failure)
        const auto &[old, ok] = cmpxchg_names[cx];
        const std::string t = CType(cx->getCompareOperand()->getType());
        const std::string call = "__atomic_compare_exchange_n((" + t + " *)" +
                                 Paren(Address(cx->getPointerOperand()), kUnary) + ", &" + old + ", " +
                                 Value(cx->getNewValOperand()).s + ", 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);";
        if (multi) {
          stmt(old + " = " + Value(cx->getCompareOperand()).s + ";");
          stmt(ok + " = " + call);
        } else {
          stmt(t + " " + old + " = " + Value(cx->getCompareOperand()).s + ";");
          stmt("bool " + ok + " = " + call);
        }
        continue;
      }
      if (i.getType()->isVoidTy()) {
        auto *c = llvm::dyn_cast<llvm::CallBase>(&i);
        auto *f = c ? Callee(c) : nullptr;
        if (f && (f->getIntrinsicID() == llvm::Intrinsic::trap || f->getIntrinsicID() == llvm::Intrinsic::ubsantrap)) {
          stmt("__builtin_trap();");  // e.g. #DE: remill's __remill_error, defined as a trap
          continue;
        }
        if (auto *ms = llvm::dyn_cast<llvm::MemSetInst>(&i)) {  // opt merges adjacent stores into these
          stmt("__builtin_memset((void *)" + Paren(Address(ms->getDest()), kUnary) + ", " +
               Value(ms->getValue()).s + ", " + Value(ms->getLength()).s + ");");
          continue;
        }
        if (auto *mt = llvm::dyn_cast<llvm::MemTransferInst>(&i)) {
          const char *fn = llvm::isa<llvm::MemMoveInst>(mt) ? "__builtin_memmove" : "__builtin_memcpy";
          stmt(std::string(fn) + "((void *)" + Paren(Address(mt->getDest()), kUnary) + ", (const void *)" +
               Paren(Address(mt->getSource()), kUnary) + ", " + Value(mt->getLength()).s + ");");
          continue;
        }
        if (f && f->isIntrinsic()) {
          switch (f->getIntrinsicID()) {
            case llvm::Intrinsic::assume:
            case llvm::Intrinsic::lifetime_start:
            case llvm::Intrinsic::lifetime_end:
            case llvm::Intrinsic::dbg_declare:
            case llvm::Intrinsic::dbg_value:
            case llvm::Intrinsic::dbg_assign:
            case llvm::Intrinsic::dbg_label:
            case llvm::Intrinsic::experimental_noalias_scope_decl:
            case llvm::Intrinsic::donothing:
            case llvm::Intrinsic::sideeffect:
            case llvm::Intrinsic::pseudoprobe: continue;  // no effect on the state
            default: break;  // anything else must not vanish
          }
        }
        stmt(Unsupported(&i).s + ";");
        continue;
      }
      if (!live.count(&i) || i.getType()->isStructTy()) {
        continue;  // dead, or feeding only remill's bookkeeping; aggregates via extractvalue
      }
      if (i.use_empty()) {  // kept for its effect (an atomic)
        stmt(Compute(&i).s + ";");
        continue;
      }
      define(&i, names[&i], Compute(&i).s);
    }
  }

  std::ostringstream out;
  out << "// remill-ir2c: @" << F.getName().str() << (flat ? " (remill flat ABI)" : "") << "\n";
  out << "#include <stdbool.h>\n#include <stdint.h>\n";
  for (const auto &h : c_headers) {  // the typed calls' functions
    out << "#include <" << h << ">\n";
  }
  out << "\n";
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
    for (const auto &k : kept_calls) {  // the calls kept: functions of the whole state, and where they went
      out << "void " << k << "(struct cpu *c, uint64_t target);\n";
    }
    for (const auto &d : c_decls) {  // typed calls of functions no header declares
      out << d << "\n";
    }
    if (!kept_calls.empty() || !c_decls.empty()) {
      out << "\n";
    }
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
  if (multi) {
    for (auto &[t, ns] : decls) {
      std::string line = "    " + t + " ";
      for (size_t k = 0; k < ns.size(); ++k) {
        line += (k ? ", " : "") + ns[k];
      }
      out << line << ";\n";
    }
  }
  // a label only reached by falling through is dropped (-Wunused-label)
  std::istringstream lines(body.str());
  for (std::string l; std::getline(lines, l);) {
    if (!l.empty() && l.back() == ':' && l.rfind("bb", 0) == 0 && !jumped.count(l.substr(0, l.size() - 1))) {
      continue;
    }
    out << l << "\n";
  }
  out << "}\n";
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
