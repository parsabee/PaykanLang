// Copyright (c) 2026 Parsa Bagheri
// SPDX-License-Identifier: Apache-2.0
// The par-safe check (#368; docs/design/concurrency.md, "Calls, control and
// nesting").  A `parallel for` body calls only par-safe functions: no
// printing or I/O, no native outside an allow-list, no task started, no
// function value, and no virtual call the compiler cannot resolve.  The
// verdict on each of the module's functions is a fixed point over its call
// graph, taken once every body and instantiation is checked, and importers
// read it from the module's interface.
//
// The same scan answers for a `lock` block ("`locked` and `lock` blocks"):
// it calls nothing that may wait, a function that takes a lock, runs a
// `parallel for`, awaits a task or blocks on input, itself or through its
// calls, and nothing the compiler cannot resolve, which may do any of
// them.  That verdict is in the interface too (Waits).

#include "Names.h"
#include "Sema.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace paykan::sema {

bool isParSafeNative(const std::string &symbol) {
  // stdlib/math.pkn: the C library's own, which keep no state of ours.
  static const std::set<std::string, std::less<>> kMath = {
      "sqrt",   "cbrt",   "pow",      "hypot", "exp",      "exp2",  "expm1",
      "log",    "log2",   "log10",    "log1p", "sin",      "cos",   "tan",
      "asin",   "acos",   "atan",     "atan2", "sinh",     "cosh",  "tanh",
      "asinh",  "acosh",  "atanh",    "floor", "ceil",     "trunc", "round",
      "fmod",   "fabs",   "fmin",     "fmax",  "copysign", "erf",   "erfc",
      "tgamma", "lgamma", "remainder"};
  if (kMath.count(symbol))
    return true;
  // The Str, Buffer and cell runtime; the moves of `::memory`; the text of
  // an error a Buffer reports; a panic, which ends the program whichever
  // thread makes it.
  for (const char *prefix :
       {"PaykanString_", "PaykanBuffer_", "PaykanAtomic", "PaykanLocked_",
        "Paykan_mem_", "Paykan_panic_", "Paykan_unreachable_", "Paykan_error_"})
    if (symbol.starts_with(prefix))
      return true;
  return false;
}

bool isInputNative(const std::string &symbol) {
  // The reads of `::io`, of a file or of the standard input: they return
  // when there is input, which may be never.
  return symbol == "Paykan_io_readln" || symbol == "Paykan_io_readbytes" ||
         symbol == "Paykan_io_read";
}

struct Sema::ParSafety {
  /// A call that may stop its caller being par-safe: of a function of this
  /// module, whose verdict is the fixed point's (Fn), or one that cannot be
  /// made in a `parallel for` body (no Fn).  A call known to be safe is not
  /// recorded.
  struct Use {
    ast::SourceLocation Loc;
    /// Who is called; for a function value its name, when it has one.
    std::string Name;
    const ast::FuncDecl *Fn = nullptr;
    /// Without Fn: what is wrong with Name in a `parallel for` body ("is
    /// not par-safe"); empty when nothing is.
    std::string Why;
    /// A call of a function value: nothing says what it runs.
    bool Value = false;
    /// Without Fn: why a `lock` block cannot make the call.
    enum class Lock : uint8_t {
      None,    ///< it can
      Waits,   ///< another module's, which its interface says may wait
      Input,   ///< a native that blocks on input
      Value,   ///< a function value
      Virtual, ///< a method a class not known here may override (Class)
    };
    Lock InLock = Lock::None;
    std::string Class;
    /// A `__deinit__` that a destruction may run, not a call: it is a
    /// `parallel for` body's to answer for where the body makes the object,
    /// and no `lock` block's (what a block lets go of is not followed).
    bool Destroys = false;
    /// A call of an `async fn`: it starts a task.  A `parallel for` body
    /// starts none (Why).  A `lock` block starts one that waits for
    /// nothing: a task may run where it is started, as the C backend runs
    /// it (docs/c-backend.md), and its body is then the block's.
    bool Starts = false;
    /// With Starts: the body the task runs, when it is this module's.
    const ast::FuncDecl *Task = nullptr;

    Use(ast::SourceLocation loc, std::string name, const ast::FuncDecl *fn,
        std::string why)
        : Loc(loc), Name(std::move(name)), Fn(fn), Why(std::move(why)) {}

    /// Without Fn: whether a `parallel for` body cannot make the call.
    bool parBad() const { return Value || !Why.empty(); }
  };
  /// What a function does itself that waits: a `lock` block, a `parallel
  /// for`, an `await`.
  struct Wait {
    const char *What = nullptr; ///< "takes a lock"
    /// Or the call it waits through: of a function that waits (Fn), or one
    /// a `lock` block cannot make (InLock).
    const Use *Through = nullptr;
  };
  /// A `parallel for` body, or a `lock` block's.
  struct ParallelBody {
    std::vector<Use> Uses;
    /// The file of a template of another module; null for this module's.
    const SemaContext::SourceFile *Source = nullptr;
    /// False in another module's copy, whose module checked it.
    bool Report = true;
    /// The function, class or struct it is in.
    const ast::Decl *Decl = nullptr;
    /// The function it is in.
    const ast::FuncDecl *Fn = nullptr;
  };
  struct Function {
    const ast::FuncDecl *Fn;
    /// The class or struct it is a method of, or null.
    ast::Type *Owner;
    const SemaContext::SourceFile *Source;
    bool Report;
    const ast::Decl *Decl;
  };

  Sema &S;
  explicit ParSafety(Sema &s) : S(s) {}
  bool Closed = false;
  std::vector<Function> Functions;
  /// Free functions by name, methods by `Type<kNameSep>name`.
  std::unordered_map<std::string, const ast::FuncDecl *> Bodies;
  /// The classes and structs Bodies has the code of.
  std::unordered_set<const ast::Type *> Local;
  /// The module's own classes, whose verdicts its interface carries.
  std::vector<ast::ClassType *> OwnClasses;
  /// In a closed world, the classes this module derives from each class.
  std::unordered_map<const ast::ClassType *, std::vector<ast::ClassType *>>
      Subclasses;
  std::unordered_map<const ast::FuncDecl *, std::vector<Use>> Calls;
  std::deque<ParallelBody> ParallelBodies;
  /// The not par-safe functions, each with the call that makes it so.
  std::unordered_map<const ast::FuncDecl *, const Use *> Bad;
  /// The `lock` blocks, each with the calls it makes.
  std::deque<ParallelBody> LockBodies;
  /// The functions that may wait, or that call what cannot be resolved,
  /// each with why.
  std::unordered_map<const ast::FuncDecl *, Wait> Waiting;

  // What the scan is reading.
  std::vector<Use> *Uses = nullptr;
  std::vector<Use> *ParallelUses = nullptr;
  std::vector<Use> *LockUses = nullptr;
  const Function *Current = nullptr;
  /// In a lambda's body, which runs when its function value is called, not
  /// when the function it is written in does.
  bool InLambda = false;
  /// Recording what a call of an `async fn` runs (Use::Starts).
  bool Starting = false;

  void index(const ast::FuncDecl *fn, ast::Type *owner,
             const std::string &qualified, const ast::Decl *decl, bool report);
  void addClass(const ast::ClassDecl *cd, bool report);
  void addStruct(const ast::StructDecl *sd, bool report);
  bool run(ast::TranslationUnit *tu);
  void propagate();
  void recordVerdicts();
  bool report();
  /// Reports each call of @p bodies that @p refused says its body cannot
  /// make, once a place, with @p message's text: in the body's file and
  /// under the instantiation that asked for it.  False when any is.
  bool reportBodies(const std::deque<ParallelBody> &bodies,
                    const std::function<bool(const Use &)> &refused,
                    const std::function<std::string(const Use &)> &message);

  /// What a `lock` block answers for of the call @p u: the function it
  /// calls, or the body of the task it starts.
  static const ast::FuncDecl *runs(const Use &u) {
    return u.Fn ? u.Fn : u.Task;
  }
  void add(Use u) {
    if (Starting) {
      u.Starts = true;
      u.Task = std::exchange(u.Fn, nullptr);
      u.Why = "starts a task";
    }
    if (ParallelUses)
      ParallelUses->push_back(u);
    if (LockUses)
      LockUses->push_back(u);
    Uses->push_back(std::move(u));
  }
  bool isBad(const Use &u) const {
    return u.Fn ? Bad.count(u.Fn) != 0 : u.parBad();
  }
  std::string describe(const Use &u, unsigned depth = 0) const;
  /// The function being read does @p what itself, which waits.
  void waits(const char *what) {
    if (!InLambda && Current)
      Waiting.try_emplace(Current->Fn, Wait{what, nullptr});
  }
  /// Whether a `lock` block cannot make the call @p u.
  bool waitsIn(const Use &u) const {
    const ast::FuncDecl *fn = runs(u);
    return !u.Destroys &&
           (fn ? Waiting.count(fn) != 0 : u.InLock != Use::Lock::None);
  }
  /// The error of the call @p u made in a `lock` block.
  std::string describeLock(const Use &u) const;
  /// Whether a class this module has the code of derives from @p base.
  bool extendedHere(const ast::ClassType *base) const;

  ast::Type *canon(ast::Type *ty) const;
  ast::Type *typeOf(const ast::Expr *e) const {
    return canon(const_cast<ast::Expr *>(e)->getResolvedType());
  }
  /// The body of the method @p name of the class or struct named @p type.
  const ast::FuncDecl *bodyOf(const std::string &type,
                              const std::string &name) const;
  /// Whether @p name is a class or a struct Bodies has the code of: a call
  /// of it runs its `__init__`, if it has one.
  bool isLocalType(const std::string &name) const {
    ast::ClassType *ct = S.Ctx.lookupClassType(name);
    ast::StructType *st = S.Ctx.lookupStructType(name);
    return (ct && Local.count(ct)) || (st && Local.count(st));
  }
  bool isExact(const ast::ClassType *ct) const {
    return ct == S.Ctx.getStrTy() || ct->isFinal() ||
           ct->getName().find('<') != std::string::npos;
  }

  void stmt(ast::Stmt *s);
  void expr(ast::Expr *e);
  void exprs(const std::vector<ast::Expr *> &es) {
    for (ast::Expr *e : es)
      expr(e);
  }
  void call(ast::CallExpr *ce);
  void methodCall(ast::MethodCallExpr *mc);
  /// A call of a function value, named @p name when it has a name.
  void functionValue(ast::SourceLocation loc, const std::string &name = "") {
    Use u{loc, name, nullptr, ""};
    u.Value = true;
    u.InLock = Use::Lock::Value;
    add(std::move(u));
  }
  /// A call of @p shown, another module's, whose interface says what it
  /// does: recorded when a `parallel for` body or a `lock` block cannot
  /// make it.
  void imported(ast::SourceLocation loc, const std::string &shown, bool parSafe,
                bool waits) {
    if (parSafe && !waits && !Starting)
      return;
    Use u{loc, shown, nullptr, parSafe ? "" : "is not par-safe"};
    if (waits)
      u.InLock = Use::Lock::Waits;
    add(std::move(u));
  }
  /// Whether the code of @p name that an object of class @p ct has is
  /// par-safe, once the verdicts are in.
  bool runsSafely(ast::ClassType *ct, const std::string &name) const;
  /// Whether it may wait.
  bool runsWaiting(ast::ClassType *ct, const std::string &name) const;
  void runsMethodOf(ast::ClassType *ct, const std::string &name,
                    const std::string &shown, ast::SourceLocation loc);
  void classMethod(ast::ClassType *ct, const std::string &name,
                   ast::SourceLocation loc);
  /// What the builtin text, equality, hash or clone of a value of type
  /// @p ty runs: the method @p name of each object it holds.
  void valueMethod(ast::Type *ty, const std::string &name,
                   ast::SourceLocation loc, ast::Qualifier held = {});
  /// What the `clone` an empty `impl Cloneable for` block derives runs.
  void derivedClone(ast::ClassType *ct, ast::SourceLocation loc);
  /// Destroying an object runs its `__deinit__`s (#378).  An object dies in
  /// the code that made it or in what that code hands it to, so the ones a
  /// function may destroy are the ones it makes (makes), and what it can
  /// write that was made anywhere (drops): the contents of a `locked mut`
  /// cell it has `inout`, and what a `parallel for` body writes of the
  /// enclosing function.
  ///
  /// The `__deinit__` @p md of the class or struct named @p owner, where
  /// it declares one.
  void deinitOf(const ast::MethodDecl *md, const std::string &owner,
                ast::SourceLocation loc);
  /// An object of exactly @p ct is made at @p loc: its own `__deinit__`
  /// and its superclasses'.
  void makes(ast::ClassType *ct, ast::SourceLocation loc);
  /// A value of type @p ty may be destroyed at @p loc: the `__deinit__`s of
  /// each object it holds, of the class its type names and of the classes
  /// derived from it, then of what those objects hold.  Once per type for
  /// each function and parallel body.
  void drops(ast::Type *ty, ast::SourceLocation loc);
  std::set<std::tuple<const std::vector<Use> *, const std::vector<Use> *,
                      const ast::Type *>>
      Dropped;
};

// A class's or a struct's name can stand for another of its kind; what a
// value is held by (qualifier, optional) does not change what it runs.
ast::Type *Sema::ParSafety::canon(ast::Type *ty) const {
  for (;;) {
    if (auto *qt = ast::dyn_cast<ast::QualifiedType>(ty))
      ty = qt->getInnerType();
    else if (auto *ot = ast::dyn_cast<ast::OptionalType>(ty))
      ty = ot->getInnerType();
    else
      break;
  }
  if (auto *ct = ast::dyn_cast<ast::ClassType>(ty)) {
    if (auto *et = S.Ctx.lookupEnumType(ct->getName()))
      return et;
    if (auto *st = S.Ctx.lookupStructType(ct->getName()))
      return st;
    if (auto *c = S.Ctx.lookupClassType(ct->getName()))
      return c;
  } else if (auto *st = ast::dyn_cast<ast::StructType>(ty)) {
    if (auto *c = S.Ctx.lookupStructType(st->getName()))
      return c;
  }
  return ty;
}

const ast::FuncDecl *Sema::ParSafety::bodyOf(const std::string &type,
                                             const std::string &name) const {
  auto it = Bodies.find(type + names::kNameSep + name);
  return it == Bodies.end() ? nullptr : it->second;
}

void Sema::ParSafety::index(const ast::FuncDecl *fn, ast::Type *owner,
                            const std::string &qualified, const ast::Decl *decl,
                            bool report) {
  // This module's own copy comes first and wins over an import's.
  if (!Bodies.try_emplace(qualified, fn).second)
    return;
  auto source = S.ForeignInstantiationSources.find(decl);
  Functions.push_back({fn, owner,
                       source == S.ForeignInstantiationSources.end()
                           ? nullptr
                           : &source->second,
                       report, decl});
}

void Sema::ParSafety::addClass(const ast::ClassDecl *cd, bool report) {
  ast::ClassType *ct = S.Ctx.lookupClassType(cd->getName());
  if (!ct)
    return;
  Local.insert(ct);
  if (report)
    OwnClasses.push_back(ct);
  for (const ast::FuncDecl *fn : cd->getMethods())
    index(fn, ct, cd->getName() + names::kNameSep + fn->getName(), cd, report);
  if (Closed)
    for (ast::ClassType *c = ct->getSuperClass(); c; c = c->getSuperClass())
      Subclasses[c].push_back(ct);
}

void Sema::ParSafety::addStruct(const ast::StructDecl *sd, bool report) {
  ast::StructType *st = S.Ctx.lookupStructType(sd->getName());
  if (!st)
    return;
  Local.insert(st);
  for (const ast::FuncDecl *fn : sd->getMethods())
    index(fn, st, sd->getName() + names::kNameSep + fn->getName(), sd, report);
}

bool Sema::ParSafety::run(ast::TranslationUnit *tu) {
  const std::vector<ast::FuncDecl *> &free = tu->getFuncDecls();
  // Only a program's main module knows every class derived from its own.
  Closed = !S.OpenWorld &&
           std::any_of(free.begin(), free.end(), [](const ast::FuncDecl *fn) {
             return fn->getName() == names::kMain;
           });
  for (const ast::FuncDecl *fn : free)
    index(fn, nullptr, fn->getName(), fn, true);
  for (const ast::FuncDecl *fn : S.ForeignInstantiatedFuncDecls)
    index(fn, nullptr, fn->getName(), fn, true);
  for (const ast::StructDecl *sd : tu->getStructDecls())
    addStruct(sd, true);
  for (const ast::StructDecl *sd : S.ForeignInstantiatedStructDecls)
    addStruct(sd, true);
  for (const ast::ClassDecl *cd : tu->getClassDecls())
    addClass(cd, true);
  for (const ast::ClassDecl *cd : S.ForeignInstantiatedClassDecls)
    addClass(cd, true);
  // A generic method's instantiations on another module's type (#353) are
  // this module's code, though the type is not.
  for (const ast::StructDecl *sd : S.ForeignMethodStructDecls)
    if (ast::StructType *st = S.Ctx.lookupStructType(sd->getName()))
      for (const ast::FuncDecl *fn : sd->getMethods())
        index(fn, st, sd->getName() + names::kNameSep + fn->getName(), fn,
              true);
  for (const ast::ClassDecl *cd : S.ForeignMethodClassDecls)
    if (ast::ClassType *ct = S.Ctx.lookupClassType(cd->getName()))
      for (const ast::FuncDecl *fn : cd->getMethods())
        index(fn, ct, cd->getName() + names::kNameSep + fn->getName(), fn,
              true);
  for (const ast::StructDecl *sd : S.ImportedInstanceStructDecls)
    addStruct(sd, false);
  for (const ast::ClassDecl *cd : S.ImportedInstanceClassDecls)
    addClass(cd, false);

  for (const Function &f : Functions) {
    Current = &f;
    Uses = &Calls[f.Fn];
    if (f.Fn->getBody())
      stmt(f.Fn->getBody());
    else if (auto *ct = ast::dyn_cast<ast::ClassType>(f.Owner);
             ct && f.Fn->isDerivedClone())
      derivedClone(ct, f.Fn->getLocation());
  }
  propagate();
  recordVerdicts();
  return report();
}

void Sema::ParSafety::propagate() {
  std::unordered_map<const ast::FuncDecl *,
                     std::vector<std::pair<const ast::FuncDecl *, const Use *>>>
      callers;
  std::vector<const ast::FuncDecl *> work;
  // The functions that wait themselves are the scan's.
  std::vector<const ast::FuncDecl *> waiting;
  for (const Function &f : Functions)
    if (Waiting.count(f.Fn))
      waiting.push_back(f.Fn);
  // Who runs each function: by a call, or by starting it as a task.
  std::unordered_map<const ast::FuncDecl *,
                     std::vector<std::pair<const ast::FuncDecl *, const Use *>>>
      runners;
  for (const Function &f : Functions)
    for (const Use &u : Calls[f.Fn]) {
      if (u.Fn)
        callers[u.Fn].push_back({f.Fn, &u});
      else if (u.parBad() && Bad.try_emplace(f.Fn, &u).second)
        work.push_back(f.Fn);
      if (const ast::FuncDecl *fn = runs(u))
        runners[fn].push_back({f.Fn, &u});
      else if (u.InLock != Use::Lock::None &&
               Waiting.try_emplace(f.Fn, Wait{nullptr, &u}).second)
        waiting.push_back(f.Fn);
    }
  for (size_t i = 0; i < work.size(); ++i)
    for (const auto &[caller, use] : callers[work[i]])
      if (Bad.try_emplace(caller, use).second)
        work.push_back(caller);
  // (A function that lets go of an object runs its `__deinit__` by no
  // call: not followed.)
  for (size_t i = 0; i < waiting.size(); ++i)
    for (const auto &[caller, use] : runners[waiting[i]])
      if (!use->Destroys &&
          Waiting.try_emplace(caller, Wait{nullptr, use}).second)
        waiting.push_back(caller);
}

void Sema::ParSafety::recordVerdicts() {
  // What a call of a function or a constructor this module exports does.
  for (auto &[name, sig] : S.FunctionTable) {
    if (sig.Imported || sig.IsBuiltin)
      continue;
    // (An `async fn`'s is its task's body's: a `lock` block does not start
    // a task that waits.)
    if (sig.Async) {
      sig.ParSafe = false;
      auto body = Bodies.find(name);
      sig.Waits = body != Bodies.end() && Waiting.count(body->second) != 0;
    } else if (auto it = Bodies.find(name); it != Bodies.end()) {
      const ast::FuncDecl *fn = it->second;
      sig.ParSafe = fn->isNative() ? isParSafeNative(fn->getNativeSymbol())
                                   : !Bad.count(fn);
      sig.Waits = fn->isNative() ? isInputNative(fn->getNativeSymbol())
                                 : Waiting.count(fn) != 0;
    } else if (isLocalType(name)) {
      const ast::FuncDecl *init = bodyOf(name, names::kMethodInit);
      sig.ParSafe = !init || !Bad.count(init);
      sig.Waits = init && Waiting.count(init);
    }
  }
  // What a call of an imported struct's method does.
  for (const Function &f : Functions) {
    auto *st = ast::dyn_cast<ast::StructType>(f.Owner);
    if (!f.Report || !st)
      continue;
    if (ast::MethodDecl *md = st->findMethod(f.Fn->getName())) {
      md->setParSafe(!md->isAsync() && !Bad.count(f.Fn));
      md->setWaits(Waiting.count(f.Fn) != 0);
    }
  }
  // What an object of an exported class runs for each of its methods: an
  // importer has no bodies to look through, and the slots of its vtable a
  // class inherits are listed with it.
  for (ast::ClassType *ct : OwnClasses) {
    for (ast::MethodDecl *md : ct->getVTable()) {
      md->setParSafe(runsSafely(ct, md->getName()));
      md->setWaits(runsWaiting(ct, md->getName()));
    }
    for (ast::MethodDecl *md : ct->getImplMethods()) {
      md->setParSafe(runsSafely(ct, md->getName()));
      md->setWaits(runsWaiting(ct, md->getName()));
    }
    // A class without an `__init__` of its own has the compiler's.
    if (ast::MethodDecl *init = ct->findMethod(names::kMethodInit)) {
      init->setParSafe(runsSafely(ct, names::kMethodInit));
      init->setWaits(runsWaiting(ct, names::kMethodInit));
    }
    // Its own `__deinit__`: an importer's object of a class derived from
    // it runs it too.
    if (ast::MethodDecl *deinit = ct->getDeinitMethod()) {
      const ast::FuncDecl *fn = bodyOf(ct->getName(), names::kMethodDeinit);
      deinit->setParSafe(!fn || !Bad.count(fn));
    }
  }
}

bool Sema::ParSafety::runsWaiting(ast::ClassType *ct,
                                  const std::string &name) const {
  // (An async method's is its task's body's.)
  for (ast::ClassType *c = ct; c; c = c->getSuperClass()) {
    if (c->isBuiltin())
      return false; // the runtime's
    if (const ast::FuncDecl *fn = bodyOf(c->getName(), name))
      return Waiting.count(fn) != 0;
    if (!Local.count(c)) {
      const ast::MethodDecl *md = c->findMethod(name);
      return md && md->waits();
    }
  }
  return false; // a pure slot, which no object of the class fills
}

bool Sema::ParSafety::runsSafely(ast::ClassType *ct,
                                 const std::string &name) const {
  if (const ast::MethodDecl *md = ct->findMethod(name); md && md->isAsync())
    return false; // it starts a task
  for (ast::ClassType *c = ct; c; c = c->getSuperClass()) {
    if (c->isBuiltin())
      return true; // the runtime's
    if (const ast::FuncDecl *fn = bodyOf(c->getName(), name))
      return !Bad.count(fn);
    if (!Local.count(c)) {
      const ast::MethodDecl *md = c->findMethod(name);
      return !md || md->isParSafe();
    }
  }
  return true; // a pure slot, which no object of the class fills
}

std::string Sema::ParSafety::describe(const Use &u, unsigned depth) const {
  if (u.Value)
    return "a function value";
  std::string out = "'" + u.Name + "'";
  if (!u.Fn)
    return out + ", which " + u.Why;
  auto it = Bad.find(u.Fn);
  if (it == Bad.end())
    return out;
  // A long chain says where it starts; its first calls are the ones to fix.
  constexpr unsigned kLongest = 4;
  if (depth == kLongest)
    return out + ", which is not par-safe";
  return out + ", which calls " + describe(*it->second, depth + 1);
}

bool Sema::ParSafety::reportBodies(
    const std::deque<ParallelBody> &bodies,
    const std::function<bool(const Use &)> &refused,
    const std::function<std::string(const Use &)> &message) {
  bool ok = true;
  std::set<std::tuple<std::string, size_t, size_t>> reported;
  for (const ParallelBody &body : bodies) {
    if (!body.Report)
      continue;
    const std::string savedName = S.Diags.getSourceName();
    const std::vector<std::string> *savedLines = S.Diags.getSourceLines();
    if (body.Source)
      S.Diags.setSourceInfo(body.Source->Name, body.Source->Lines);
    // Its instantiations are over: say which one asked for it.
    // (A generic method's instance is found by the method, and is the one
    // to name: its owner's, when that is an instance too, is in its
    // frames.)
    auto frames = S.InstanceFrames.find(body.Fn);
    if (frames == S.InstanceFrames.end())
      frames = S.InstanceFrames.find(body.Decl);
    if (frames != S.InstanceFrames.end())
      S.InstantiationStack = frames->second;
    for (const Use &u : body.Uses) {
      if (!refused(u) ||
          !reported
               .insert({S.Diags.getSourceName(), u.Loc.getLineStart(),
                        u.Loc.getColumnStart()})
               .second)
        continue;
      S.error(u.Loc, message(u));
      ok = false;
    }
    S.InstantiationStack.clear();
    if (body.Source)
      S.Diags.setSourceInfo(savedName, savedLines);
  }
  return ok;
}

/// The class or struct whose `__deinit__` the use named @p name runs; ""
/// for a call.
static std::string destroyedBy(const std::string &name) {
  const std::string deinit =
      std::string(names::kNameSep) + names::kMethodDeinit;
  return name.ends_with(deinit) ? name.substr(0, name.size() - deinit.size())
                                : std::string();
}

bool Sema::ParSafety::report() {
  const bool parallel = reportBodies(
      ParallelBodies, [&](const Use &u) { return isBad(u); },
      [&](const Use &u) {
        // A destruction is no call the body wrote: it says what is
        // destroyed.
        const std::string destroyed = destroyedBy(u.Name);
        return destroyed.empty()
                   ? "a 'parallel for' body cannot call " + describe(u)
                   : "a 'parallel for' body cannot destroy a '" + destroyed +
                         "': it runs " + describe(u);
      });
  const bool locks = reportBodies(
      LockBodies, [&](const Use &u) { return waitsIn(u); },
      [&](const Use &u) { return describeLock(u); });
  return parallel && locks;
}

std::string Sema::ParSafety::describeLock(const Use &u) const {
  // From the call, through the functions it reaches, to what waits: a
  // statement of the last function (what), or a call it makes that a `lock`
  // block cannot (last).
  std::vector<std::string> through;
  const Use *last = &u;
  const char *what = nullptr;
  for (size_t steps = 0; runs(*last) && steps <= Functions.size(); ++steps) {
    const Wait &wait = Waiting.at(runs(*last));
    if (wait.What) {
      what = wait.What;
      break;
    }
    last = wait.Through;
    if (runs(*last))
      through.push_back(last->Name);
  }
  const bool direct = last == &u;
  using Lock = Use::Lock;
  // A call that cannot be resolved may run anything; any other is known to
  // wait.
  const bool unresolved =
      !what && (last->InLock == Lock::Value || last->InLock == Lock::Virtual);
  std::string why, advice;
  if (what) {
    why = std::string("it ") + what;
  } else if (last->InLock == Lock::Input) {
    why = "it blocks on input";
  } else if (last->InLock == Lock::Virtual) {
    why = direct
              ? "a class derived from '" + last->Class + "' may override it"
              : "it calls '" + last->Name + "', which a class derived from '" +
                    last->Class + "' may override";
    // A class of this module is its author's to close, unless the module
    // derives from it itself.
    ast::ClassType *base = S.Ctx.lookupClassType(last->Class);
    if (base && Local.count(base) && !extendedHere(base))
      advice = "; a 'final class " + last->Class + "' has none";
  } else if (!direct) {
    why = last->InLock == Lock::Value
              ? (last->Name.empty()
                     ? std::string("it calls a function value")
                     : "it calls '" + last->Name + "', a function value")
              : "it calls '" + last->Name + "', which may wait";
  }
  constexpr size_t kLongest = 3;
  for (size_t i = 0; i < through.size() && i < kLongest; ++i)
    why += (i == 0 ? ", through '"
            : i + 1 == std::min(through.size(), kLongest) &&
                    through.size() <= kLongest
                ? " and '"
                : ", '") +
           through[i] + "'";
  if (through.size() > kLongest)
    why += " and " + std::to_string(through.size() - kLongest) + " more";
  why += advice;
  if (!why.empty())
    why = " (" + why + ")";
  // What the block calls: a function, or a function value with or without
  // a name.
  const std::string verb =
      unresolved ? "may run code that takes a lock" : "may wait";
  const std::string who =
      u.Name.empty() ? std::string("a function value") : "'" + u.Name + "'";
  return who + " " + verb + why +
         (unresolved ? "; a 'lock' block calls only functions it can resolve"
          : u.Starts ? "; a 'lock' block cannot start it (a task may run "
                       "where it is started)"
                     : "; a 'lock' block cannot call it");
}

bool Sema::ParSafety::extendedHere(const ast::ClassType *base) const {
  for (const auto &[name, ct] : S.Ctx.getClassTypes())
    if (Local.count(ct))
      for (const ast::ClassType *c = ct->getSuperClass(); c;
           c = c->getSuperClass())
        if (c == base)
          return true;
  return false;
}

void Sema::ParSafety::stmt(ast::Stmt *s) {
  if (!s)
    return;
  if (auto *x = ast::dyn_cast<ast::CompoundStmt>(s)) {
    for (ast::Stmt *t : x->getStatements())
      stmt(t);
  } else if (auto *x = ast::dyn_cast<ast::ReturnStmt>(s)) {
    expr(x->getReturnValue());
  } else if (auto *x = ast::dyn_cast<ast::ExprStmt>(s)) {
    expr(x->getExpr());
  } else if (auto *x = ast::dyn_cast<ast::DeclStmt>(s)) {
    if (auto *vd = ast::dyn_cast<ast::VarDecl>(x->getDecl()))
      expr(vd->getInitExpr());
  } else if (auto *x = ast::dyn_cast<ast::AssignStmt>(s)) {
    expr(x->getValue());
  } else if (auto *x = ast::dyn_cast<ast::MemberAssignStmt>(s)) {
    expr(x->getReceiver());
    expr(x->getValue());
  } else if (auto *x = ast::dyn_cast<ast::SubscriptAssignStmt>(s)) {
    expr(x->getArray());
    expr(x->getIndex());
    expr(x->getValue());
  } else if (auto *x = ast::dyn_cast<ast::DestructureStmt>(s)) {
    expr(x->getValue());
  } else if (auto *x = ast::dyn_cast<ast::IfStmt>(s)) {
    expr(x->getCondition());
    stmt(x->getThenBranch());
    stmt(x->getElseBranch());
  } else if (auto *x = ast::dyn_cast<ast::WhileStmt>(s)) {
    expr(x->getCondition());
    stmt(x->getBody());
  } else if (auto *x = ast::dyn_cast<ast::LockStmt>(s)) {
    expr(x->getCell());
    // What a cell holds was made anywhere: a body that has it `inout` may
    // destroy it (an assignment over a part, `mem::swap`), whatever it is.
    if (x->getBinding() && x->getBinding()->getMode() == ast::ParamMode::Inout)
      drops(x->getBinding()->getType(), x->getLocation());
    waits("takes a lock");
    // The calls of its body are the block's.  (A nested one is an error of
    // its own.)
    if (!LockUses) {
      ParallelBody &body = LockBodies.emplace_back();
      body.Source = Current->Source;
      body.Report = Current->Report;
      body.Decl = Current->Decl;
      body.Fn = Current->Fn;
      LockUses = &body.Uses;
      stmt(x->getBody());
      LockUses = nullptr;
    } else {
      stmt(x->getBody());
    }
  } else if (auto *x = ast::dyn_cast<ast::MatchStmt>(s)) {
    expr(x->getSubject());
    for (ast::MatchArm *arm : x->getArms())
      stmt(arm->getBody());
  } else if (auto *x = ast::dyn_cast<ast::ForStmt>(s)) {
    expr(x->getStart());
    expr(x->getEnd());
    expr(x->getStep());
    // What a body writes of the enclosing function, an element at the
    // loop's variable, was made anywhere: an assignment over it, or what it
    // is handed to `inout`, may destroy it, whatever it is.
    auto written = [&] {
      if (auto it = S.ParallelOuterWrites.find(x);
          it != S.ParallelOuterWrites.end())
        for (const auto &[place, field] : it->second)
          drops(field ? field : typeOf(place), place->getLocation());
    };
    if (x->isParallel())
      waits("runs a 'parallel for'");
    // A nested one is part of the outermost's body, and reported with it.
    if (x->isParallel() && !ParallelUses) {
      ParallelBody &body = ParallelBodies.emplace_back();
      body.Source = Current->Source;
      body.Report = Current->Report;
      body.Decl = Current->Decl;
      body.Fn = Current->Fn;
      ParallelUses = &body.Uses;
      written();
      stmt(x->getBody());
      ParallelUses = nullptr;
    } else {
      if (x->isParallel())
        written();
      stmt(x->getBody());
    }
  }
}

void Sema::ParSafety::expr(ast::Expr *e) {
  if (!e)
    return;
  if (auto *x = ast::dyn_cast<ast::UnaryExpr>(e)) {
    expr(x->getOperand());
    if (x->getOpcode() == ast::UnaryOpcode::Await)
      waits("awaits a task");
  } else if (auto *x = ast::dyn_cast<ast::BinaryExpr>(e)) {
    expr(x->getLHS());
    expr(x->getRHS());
    // Values that reach objects are compared by the objects' `equals`.
    if ((x->getOpcode() == ast::BinaryOpcode::Eq ||
         x->getOpcode() == ast::BinaryOpcode::Ne) &&
        !ast::isa<ast::NoneLiteral>(x->getLHS()) &&
        !ast::isa<ast::NoneLiteral>(x->getRHS()))
      valueMethod(typeOf(x->getLHS()), names::kMethodEquals, x->getLocation());
  } else if (auto *x = ast::dyn_cast<ast::TernaryExpr>(e)) {
    expr(x->getCondition());
    expr(x->getTrueExpr());
    expr(x->getFalseExpr());
  } else if (auto *x = ast::dyn_cast<ast::MemberAccessExpr>(e)) {
    expr(x->getReceiver());
  } else if (auto *x = ast::dyn_cast<ast::SubscriptExpr>(e)) {
    expr(x->getArray());
    expr(x->getIndex());
  } else if (auto *x = ast::dyn_cast<ast::TupleIndexExpr>(e)) {
    expr(x->getTuple());
  } else if (auto *x = ast::dyn_cast<ast::ArrayLiteralExpr>(e)) {
    exprs(x->getElements());
  } else if (auto *x = ast::dyn_cast<ast::TupleLiteralExpr>(e)) {
    exprs(x->getElements());
  } else if (auto *x = ast::dyn_cast<ast::CallExpr>(e)) {
    call(x);
  } else if (auto *x = ast::dyn_cast<ast::MethodCallExpr>(e)) {
    methodCall(x);
  } else if (auto *x = ast::dyn_cast<ast::CallValueExpr>(e)) {
    expr(x->getCallee());
    exprs(x->getArguments());
    functionValue(x->getLocation());
  } else if (auto *x = ast::dyn_cast<ast::LambdaExpr>(e)) {
    // A lambda's body runs when it is called, through a function value,
    // which is judged there.  Its own `parallel for` loops are checked.
    // So are its `lock` blocks, and what it does that waits is not the
    // enclosing function's.
    std::vector<Use> ignored;
    std::vector<Use> *saved = std::exchange(Uses, &ignored);
    std::vector<Use> *savedParallel = std::exchange(ParallelUses, nullptr);
    std::vector<Use> *savedLock = std::exchange(LockUses, nullptr);
    const bool savedLambda = std::exchange(InLambda, true);
    stmt(x->getBody());
    Uses = saved;
    ParallelUses = savedParallel;
    LockUses = savedLock;
    InLambda = savedLambda;
  }
}

void Sema::ParSafety::call(ast::CallExpr *ce) {
  exprs(ce->getArguments());
  const std::string &name = ce->getCalleeName();
  const ast::SourceLocation loc = ce->getLocation();
  if (ce->getVariantEnum())
    return;
  if (ce->calleeIsValue())
    return functionValue(loc, name);
  if (name == names::kMethodSuper) {
    auto *ct = ast::dyn_cast<ast::ClassType>(Current->Owner);
    ast::ClassType *super = ct ? ct->getSuperClass() : nullptr;
    if (super && !super->isBuiltin())
      runsMethodOf(super, names::kMethodInit,
                   super->getName() + names::kNameSep + names::kMethodInit,
                   loc);
    return;
  }
  // `Str(x)` of a value with a text is `x.toString()`.
  if ((name == names::kString || name.starts_with("Str<")) &&
      ce->getNumArguments() == 1)
    return valueMethod(typeOf(ce->getArguments()[0]), names::kMethodToString,
                       loc);
  const FunctionSig *sig = S.lookupFunction(name);
  std::string shown = name;
  if (shown.starts_with(names::kHiddenQualPrefix))
    shown.erase(0, std::string_view(names::kHiddenQualPrefix).size());
  if (sig && sig->Async) {
    // What the task runs: this module's body, or what another module's
    // interface says of it.
    const bool saved = std::exchange(Starting, true);
    if (auto it = Bodies.find(name); it != Bodies.end())
      add({loc, shown, it->second, ""});
    else
      imported(loc, shown, /*parSafe=*/false, sig->Imported && sig->Waits);
    Starting = saved;
    return;
  }
  if (auto it = Bodies.find(name); it != Bodies.end()) {
    const ast::FuncDecl *fn = it->second;
    if (!fn->isNative())
      return add({loc, shown, fn, ""});
    const bool safe = isParSafeNative(fn->getNativeSymbol());
    const bool input = isInputNative(fn->getNativeSymbol());
    if (!safe || input) {
      Use u{loc, shown, nullptr,
            safe ? "" : "is a native function outside the par-safe list"};
      if (input)
        u.InLock = Use::Lock::Input;
      add(std::move(u));
    }
    return;
  }
  // An object made here is destroyed here too, or by what this code hands
  // it to: its `__deinit__`s are this call's (#378).
  if (ast::ClassType *made = S.Ctx.lookupClassType(name))
    makes(made, loc);
  // So is a value of a struct that has one.
  else if (ast::StructType *value = S.Ctx.lookupStructType(name))
    deinitOf(value->getDeinitMethod(), value->getName(), loc);
  if (isLocalType(name)) {
    if (const ast::FuncDecl *init = bodyOf(name, names::kMethodInit))
      add({loc, shown, init, ""});
    return;
  }
  // Another module's, as its interface says; none is a conversion.
  if (sig && sig->Imported)
    imported(loc, shown, sig->ParSafe, sig->Waits);
}

// The code of @p name that an object of class @p ct has: the closest class's
// from it up that defines it.
void Sema::ParSafety::runsMethodOf(ast::ClassType *ct, const std::string &name,
                                   const std::string &shown,
                                   ast::SourceLocation loc) {
  for (ast::ClassType *c = ct; c; c = c->getSuperClass()) {
    if (c->isBuiltin())
      return; // the runtime's
    if (const ast::FuncDecl *fn = bodyOf(c->getName(), name)) {
      add({loc, shown, fn, ""});
      return;
    }
    if (!Local.count(c)) {
      // Compiled by another module, whose interface says.
      if (const ast::MethodDecl *md = c->findMethod(name))
        imported(loc, shown, md->isParSafe(), md->waits());
      return;
    }
  }
}

void Sema::ParSafety::classMethod(ast::ClassType *ct, const std::string &name,
                                  ast::SourceLocation loc) {
  const std::string shown = ct->getName() + names::kNameSep + name;
  const ast::MethodDecl *md = ct->findMethod(name);
  // An async method starts a task of what a call of it would run.
  if (md && md->isAsync() && !Starting) {
    Starting = true;
    const size_t before = Uses->size();
    classMethod(ct, name, loc);
    if (Uses->size() == before)
      add({loc, shown, nullptr, ""});
    Starting = false;
    return;
  }
  if (md && !md->isVirtual())
    return runsMethodOf(ct, name, shown, loc);
  // An exact class has no subclass; a closed one has only those Subclasses
  // lists.  Any other's override is not known here.
  if (!isExact(ct) && !(Closed && Local.count(ct))) {
    Use u{loc, shown, nullptr,
          "a class derived from '" + ct->getName() + "' may override"};
    u.InLock = Use::Lock::Virtual;
    u.Class = ct->getName();
    return add(std::move(u));
  }
  runsMethodOf(ct, name, shown, loc);
  if (auto sub = Subclasses.find(ct); sub != Subclasses.end())
    for (ast::ClassType *c : sub->second)
      if (const ast::FuncDecl *fn = bodyOf(c->getName(), name))
        add({loc, c->getName() + names::kNameSep + name, fn, ""});
}

void Sema::ParSafety::methodCall(ast::MethodCallExpr *mc) {
  expr(mc->getReceiver());
  exprs(mc->getArguments());
  const ast::SourceLocation loc = mc->getLocation();
  if (mc->isFieldCall())
    return functionValue(loc, Sema::placeText(mc->getReceiver()) + "." +
                                  mc->getMethodName());
  const std::string &name = mc->getMethodName();
  if (name == names::kMethodClone && mc->cloneIsCopy())
    return;
  ast::Type *receiver = typeOf(mc->getReceiver());
  if (auto *ct = ast::dyn_cast<ast::ClassType>(receiver))
    return classMethod(ct, name, loc);
  if (auto *st = ast::dyn_cast<ast::StructType>(receiver)) {
    const ast::MethodDecl *md = st->findMethod(name);
    if (!md)
      return valueMethod(st, name, loc);
    const std::string shown = st->getName() + names::kNameSep + name;
    const bool saved = std::exchange(Starting, Starting || md->isAsync());
    if (const ast::FuncDecl *fn = bodyOf(st->getName(), name))
      add({loc, shown, fn, ""});
    else
      imported(loc, shown, md->isParSafe(), md->waits());
    Starting = saved;
    return;
  }
  // An array's `len()`, or the builtin text, equality, hash or clone of a
  // value, which are its parts'.
  if (name == names::kMethodToString || name == names::kMethodEquals ||
      name == names::kMethodHash || name == names::kMethodClone)
    valueMethod(receiver, name, loc);
}

void Sema::ParSafety::derivedClone(ast::ClassType *ct,
                                   ast::SourceLocation loc) {
  makes(ct, loc); // the clone is an object of the class
  for (ast::ClassType *c = ct; c && !c->isBuiltin(); c = c->getSuperClass())
    for (const auto &[field, ty] : c->getFields())
      if (!c->isWeakField(field))
        valueMethod(ty, names::kMethodClone, loc, c->findFieldQualifier(field));
}

void Sema::ParSafety::valueMethod(ast::Type *ty, const std::string &name,
                                  ast::SourceLocation loc,
                                  ast::Qualifier held) {
  std::unordered_set<const ast::Type *> seen;
  std::vector<std::pair<ast::Type *, ast::Qualifier>> work = {{ty, held}};
  while (!work.empty()) {
    auto [part, held] = work.back();
    work.pop_back();
    // A `shared` reference's clone is the copy, of the same object.
    if (name == names::kMethodClone && held.Form != ast::QualForm::Unique)
      continue;
    ast::Type *t = canon(part);
    if (!t || ast::isa<ast::BuiltinType>(t) || !seen.insert(t).second)
      continue;
    if (auto *ct = ast::dyn_cast<ast::ClassType>(t)) {
      classMethod(ct, name, loc);
    } else if (auto *et = ast::dyn_cast<ast::EnumType>(t)) {
      for (size_t i = 0; i < et->getNumVariants(); ++i)
        if (ast::StructType *payload = et->getPayload(i))
          work.push_back({payload, {}});
    } else if (auto *st = ast::dyn_cast<ast::StructType>(t)) {
      // `==` and the text of a struct are the runtime's, field by field,
      // whatever `equals` it declares; a part's `clone` and `hash` are the
      // ones it declares.
      const ast::MethodDecl *md =
          name == names::kMethodClone || name == names::kMethodHash
              ? st->findMethod(name)
              : nullptr;
      if (md) {
        const std::string shown = st->getName() + names::kNameSep + name;
        if (const ast::FuncDecl *fn = bodyOf(st->getName(), name))
          add({loc, shown, fn, ""});
        else
          imported(loc, shown, md->isParSafe(), md->waits());
        continue;
      }
      // A buffer's one field is the runtime's buffer object, which its
      // methods alone reach: its `==` and its text are the runtime's, and
      // read no element.
      if (S.bufferElementType(st))
        continue;
      for (size_t i = 0; i < st->getNumFields(); ++i)
        work.push_back({st->getElementType(i),
                        st->findFieldQualifier(st->getFieldName(i))});
    } else if (auto *tt = ast::dyn_cast<ast::TupleType>(t)) {
      for (size_t i = 0; i < tt->getArity(); ++i)
        work.push_back({tt->getElementType(i), tt->getElemQual(i)});
    } else if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(t)) {
      work.push_back({ft->getElementType(), ft->getElemQual()});
    } else if (auto *sl = ast::dyn_cast<ast::SliceType>(t)) {
      work.push_back({sl->getElementType(), {}});
    }
  }
}

void Sema::ParSafety::deinitOf(const ast::MethodDecl *md,
                               const std::string &owner,
                               ast::SourceLocation loc) {
  if (!md)
    return;
  const std::string shown = owner + names::kNameSep + names::kMethodDeinit;
  if (const ast::FuncDecl *fn = bodyOf(owner, names::kMethodDeinit)) {
    Use u{loc, shown, fn, ""};
    u.Destroys = true;
    add(std::move(u));
  } else if (!md->isParSafe()) {
    Use u{loc, shown, nullptr, "is not par-safe"};
    u.Destroys = true;
    add(std::move(u));
  }
}

void Sema::ParSafety::makes(ast::ClassType *ct, ast::SourceLocation loc) {
  for (ast::ClassType *c = ct; c && !c->isBuiltin(); c = c->getSuperClass())
    deinitOf(c->getDeinitMethod(), c->getName(), loc);
}

void Sema::ParSafety::drops(ast::Type *ty, ast::SourceLocation loc) {
  ast::Type *root = ty ? canon(ty) : nullptr;
  if (!root || ast::isa<ast::BuiltinType>(root) ||
      !Dropped.insert({Uses, ParallelUses, root}).second)
    return;
  std::unordered_set<const ast::Type *> seen;
  // With each type, whether an object of it is of exactly its class.
  std::vector<std::pair<ast::Type *, bool>> work = {{root, false}};
  while (!work.empty()) {
    auto [part, exact] = work.back();
    work.pop_back();
    ast::Type *t = part ? canon(part) : nullptr;
    if (!t || ast::isa<ast::BuiltinType>(t) || !seen.insert(t).second)
      continue;
    if (auto *ct = ast::dyn_cast<ast::ClassType>(t)) {
      if (ct == S.Ctx.getStrTy())
        continue;
      // An object of a class derived from the one named runs that class's
      // too: the ones this program derives, or any when it may have more.
      if (!exact && !isExact(ct)) {
        if (!(Closed && Local.count(ct))) {
          add({loc, ct->getName() + names::kNameSep + names::kMethodDeinit,
               nullptr,
               "a class derived from '" + ct->getName() + "' may declare"});
          continue;
        }
        if (auto sub = Subclasses.find(ct); sub != Subclasses.end())
          for (ast::ClassType *c : sub->second)
            work.push_back({c, true});
      }
      for (ast::ClassType *c = ct; c && !c->isBuiltin();
           c = c->getSuperClass()) {
        deinitOf(c->getDeinitMethod(), c->getName(), loc);
        // Then what it holds, a `weak` field aside.
        for (const auto &[field, fty] : c->getFields())
          if (!c->isWeakField(field))
            work.push_back({fty, false});
      }
    } else if (auto *et = ast::dyn_cast<ast::EnumType>(t)) {
      for (size_t i = 0; i < et->getNumVariants(); ++i)
        if (ast::StructType *payload = et->getPayload(i))
          work.push_back({payload, false});
    } else if (auto *st = ast::dyn_cast<ast::StructType>(t)) {
      // A buffer holds its elements, which its type names: its one field
      // is the runtime's buffer object.
      if (ast::Type *elem = S.bufferElementType(st)) {
        work.push_back({elem, false});
        continue;
      }
      deinitOf(st->getDeinitMethod(), st->getName(), loc); // its own
      for (size_t i = 0; i < st->getNumFields(); ++i)
        work.push_back({st->getElementType(i), false});
    } else if (auto *tt = ast::dyn_cast<ast::TupleType>(t)) {
      for (size_t i = 0; i < tt->getArity(); ++i)
        work.push_back({tt->getElementType(i), false});
    } else if (auto *ft = ast::dyn_cast<ast::FixedArrayType>(t)) {
      work.push_back({ft->getElementType(), false});
    } else if (auto *at = ast::dyn_cast<ast::ArrayType>(t)) {
      work.push_back({at->getElementType(), false});
    } else if (auto *sl = ast::dyn_cast<ast::SliceType>(t)) {
      work.push_back({sl->getElementType(), false});
    } else if (auto *tk = ast::dyn_cast<ast::TaskType>(t)) {
      work.push_back({tk->getInnerType(), false});
    } else if (auto *ac = ast::dyn_cast<ast::AtomicType>(t)) {
      work.push_back({ac->getInnerType(), false});
    }
  }
}

bool Sema::checkParSafety(ast::TranslationUnit *tu) {
  ParSafety pass(*this);
  return pass.run(tu);
}

} // namespace paykan::sema
