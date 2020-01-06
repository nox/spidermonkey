/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JavaScript bytecode interpreter.
 */

#include "vm/Interpreter-inl.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Maybe.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Sprintf.h"

#include <string.h>

#include "jslibmath.h"
#include "jsnum.h"

#include "builtin/Array.h"
#include "builtin/Eval.h"
#include "builtin/ModuleObject.h"
#include "builtin/Promise.h"
#include "jit/AtomicOperations.h"
#include "jit/BaselineJIT.h"
#include "jit/Ion.h"
#include "jit/IonAnalysis.h"
#include "jit/Jit.h"
#include "js/CharacterEncoding.h"
#include "util/CheckedArithmetic.h"
#include "util/StringBuffer.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/BigIntType.h"
#include "vm/BytecodeUtil.h"        // JSDVG_SEARCH_STACK
#include "vm/EqualityOperations.h"  // js::StrictlyEqual
#include "vm/GeneratorObject.h"
#include "vm/Instrumentation.h"
#include "vm/Iteration.h"
#include "vm/JSAtom.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/Opcodes.h"
#include "vm/PIC.h"
#include "vm/Printer.h"
#include "vm/Scope.h"
#include "vm/Shape.h"
#include "vm/StringType.h"
#include "vm/TraceLogging.h"

#include "builtin/Boolean-inl.h"
#include "debugger/DebugAPI-inl.h"
#include "jit/JitFrames-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/GeckoProfiler-inl.h"
#include "vm/JSAtom-inl.h"
#include "vm/JSFunction-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/ObjectOperations-inl.h"
#include "vm/Probes-inl.h"
#include "vm/Stack-inl.h"

using namespace js;

using mozilla::DebugOnly;
using mozilla::NumberEqualsInt32;

using js::jit::JitScript;

template <bool Eq>
static MOZ_ALWAYS_INLINE bool LooseEqualityOp(JSContext* cx,
                                              InterpreterRegs& regs) {
  HandleValue rval = regs.stackHandleAt(-1);
  HandleValue lval = regs.stackHandleAt(-2);
  bool cond;
  if (!LooselyEqual(cx, lval, rval, &cond)) {
    return false;
  }
  cond = (cond == Eq);
  regs.sp--;
  regs.sp[-1].setBoolean(cond);
  return true;
}

bool js::BoxNonStrictThis(JSContext* cx, HandleValue thisv,
                          MutableHandleValue vp) {
  /*
   * Check for SynthesizeFrame poisoning and fast constructors which
   * didn't check their callee properly.
   */
  MOZ_ASSERT(!thisv.isMagic());

  if (thisv.isNullOrUndefined()) {
    vp.set(cx->global()->lexicalEnvironment().thisValue());
    return true;
  }

  if (thisv.isObject()) {
    vp.set(thisv);
    return true;
  }

  JSObject* obj = PrimitiveToObject(cx, thisv);
  if (!obj) {
    return false;
  }

  vp.setObject(*obj);
  return true;
}

bool js::GetFunctionThis(JSContext* cx, AbstractFramePtr frame,
                         MutableHandleValue res) {
  MOZ_ASSERT(frame.isFunctionFrame());
  MOZ_ASSERT(!frame.callee()->isArrow());

  if (frame.thisArgument().isObject() || frame.callee()->strict()) {
    res.set(frame.thisArgument());
    return true;
  }

  MOZ_ASSERT(!frame.callee()->isSelfHostedBuiltin(),
             "Self-hosted builtins must be strict");

  RootedValue thisv(cx, frame.thisArgument());

  // If there is a NSVO on environment chain, use it as basis for fallback
  // global |this|. This gives a consistent definition of global lexical
  // |this| between function and global contexts.
  //
  // NOTE: If only non-syntactic WithEnvironments are on the chain, we use the
  // global lexical |this| value. This is for compatibility with the Subscript
  // Loader.
  if (frame.script()->hasNonSyntacticScope() && thisv.isNullOrUndefined()) {
    RootedObject env(cx, frame.environmentChain());
    while (true) {
      if (IsNSVOLexicalEnvironment(env) || IsGlobalLexicalEnvironment(env)) {
        res.set(GetThisValueOfLexical(env));
        return true;
      }
      if (!env->enclosingEnvironment()) {
        // This can only happen in Debugger eval frames: in that case we
        // don't always have a global lexical env, see EvaluateInEnv.
        MOZ_ASSERT(env->is<GlobalObject>());
        res.set(GetThisValue(env));
        return true;
      }
      env = env->enclosingEnvironment();
    }
  }

  return BoxNonStrictThis(cx, thisv, res);
}

void js::GetNonSyntacticGlobalThis(JSContext* cx, HandleObject envChain,
                                   MutableHandleValue res) {
  RootedObject env(cx, envChain);
  while (true) {
    if (IsExtensibleLexicalEnvironment(env)) {
      res.set(GetThisValueOfLexical(env));
      return;
    }
    if (!env->enclosingEnvironment()) {
      // This can only happen in Debugger eval frames: in that case we
      // don't always have a global lexical env, see EvaluateInEnv.
      MOZ_ASSERT(env->is<GlobalObject>());
      res.set(GetThisValue(env));
      return;
    }
    env = env->enclosingEnvironment();
  }
}

bool js::Debug_CheckSelfHosted(JSContext* cx, HandleValue fun) {
#ifndef DEBUG
  MOZ_CRASH("self-hosted checks should only be done in Debug builds");
#endif

  RootedObject funObj(cx, UncheckedUnwrap(&fun.toObject()));
  MOZ_ASSERT(funObj->as<JSFunction>().isSelfHostedOrIntrinsic());

  // This is purely to police self-hosted code. There is no actual operation.
  return true;
}

static inline bool GetPropertyOperation(JSContext* cx, InterpreterFrame* fp,
                                        HandleScript script, jsbytecode* pc,
                                        MutableHandleValue lval,
                                        MutableHandleValue vp) {
  JSOp op = JSOp(*pc);

  if (op == JSOp::Length) {
    if (IsOptimizedArguments(fp, lval)) {
      vp.setInt32(fp->numActualArgs());
      return true;
    }

    if (GetLengthProperty(lval, vp)) {
      return true;
    }
  }

  RootedPropertyName name(cx, script->getName(pc));

  if (name == cx->names().callee && IsOptimizedArguments(fp, lval)) {
    vp.setObject(fp->callee());
    return true;
  }

  // Copy lval, because it might alias vp.
  RootedValue v(cx, lval);
  return GetProperty(cx, v, name, vp);
}

static inline bool GetNameOperation(JSContext* cx, InterpreterFrame* fp,
                                    jsbytecode* pc, MutableHandleValue vp) {
  RootedObject envChain(cx, fp->environmentChain());
  RootedPropertyName name(cx, fp->script()->getName(pc));

  /*
   * Skip along the env chain to the enclosing global object. This is
   * used for GNAME opcodes where the bytecode emitter has determined a
   * name access must be on the global. It also insulates us from bugs
   * in the emitter: type inference will assume that GNAME opcodes are
   * accessing the global object, and the inferred behavior should match
   * the actual behavior even if the id could be found on the env chain
   * before the global object.
   */
  if (IsGlobalOp(JSOp(*pc)) && !fp->script()->hasNonSyntacticScope()) {
    envChain = &cx->global()->lexicalEnvironment();
  }

  /* Kludge to allow (typeof foo == "undefined") tests. */
  JSOp op2 = JSOp(pc[JSOpLength_GetName]);
  if (op2 == JSOp::Typeof) {
    return GetEnvironmentName<GetNameMode::TypeOf>(cx, envChain, name, vp);
  }
  return GetEnvironmentName<GetNameMode::Normal>(cx, envChain, name, vp);
}

bool js::GetImportOperation(JSContext* cx, HandleObject envChain,
                            HandleScript script, jsbytecode* pc,
                            MutableHandleValue vp) {
  RootedObject env(cx), pobj(cx);
  RootedPropertyName name(cx, script->getName(pc));
  Rooted<PropertyResult> prop(cx);

  MOZ_ALWAYS_TRUE(LookupName(cx, name, envChain, &env, &pobj, &prop));
  MOZ_ASSERT(env && env->is<ModuleEnvironmentObject>());
  MOZ_ASSERT(env->as<ModuleEnvironmentObject>().hasImportBinding(name));
  return FetchName<GetNameMode::Normal>(cx, env, pobj, name, prop, vp);
}

static bool SetPropertyOperation(JSContext* cx, JSOp op, HandleValue lval,
                                 int lvalIndex, HandleId id, HandleValue rval) {
  MOZ_ASSERT(op == JSOp::SetProp || op == JSOp::StrictSetProp);

  RootedObject obj(cx,
                   ToObjectFromStackForPropertyAccess(cx, lval, lvalIndex, id));
  if (!obj) {
    return false;
  }

  ObjectOpResult result;
  return SetProperty(cx, obj, id, rval, lval, result) &&
         result.checkStrictErrorOrWarning(cx, obj, id,
                                          op == JSOp::StrictSetProp);
}

JSFunction* js::MakeDefaultConstructor(JSContext* cx, HandleScript script,
                                       jsbytecode* pc, HandleObject proto) {
  JSOp op = JSOp(*pc);
  bool derived = op == JSOp::DerivedConstructor;
  MOZ_ASSERT(derived == !!proto);

  uint32_t atomIndex = 0;
  uint32_t classStartOffset = 0, classEndOffset = 0;
  GetClassConstructorOperands(pc, &atomIndex, &classStartOffset,
                              &classEndOffset);

  JSAtom* atom = script->getAtom(atomIndex);
  PropertyName* lookup = derived ? cx->names().DefaultDerivedClassConstructor
                                 : cx->names().DefaultBaseClassConstructor;

  RootedPropertyName selfHostedName(cx, lookup);
  RootedAtom name(cx, atom == cx->names().empty ? nullptr : atom);

  RootedFunction ctor(cx);
  if (!cx->runtime()->createLazySelfHostedFunctionClone(
          cx, selfHostedName, name,
          /* nargs = */ !!derived, proto, TenuredObject, &ctor)) {
    return nullptr;
  }

  ctor->setIsConstructor();
  ctor->setIsClassConstructor();

  // Create the script now, so we can fix up its source span below.
  RootedScript ctorScript(cx, JSFunction::getOrCreateScript(cx, ctor));
  if (!ctorScript) {
    return nullptr;
  }

  // This function's frames are fine to expose to JS; it should not be treated
  // as an opaque self-hosted builtin. But the script cloning code naturally
  // expects to be applied to self-hosted functions, so do the clone first,
  // and clear this afterwards.
  ctor->clearIsSelfHosted();

  // Override the source span needs for toString. Calling toString on a class
  // constructor should return the class declaration, not the source for the
  // (self-hosted) constructor function.
  unsigned column;
  unsigned line = PCToLineNumber(script, pc, &column);
  ctorScript->setDefaultClassConstructorSpan(
      script->sourceObject(), classStartOffset, classEndOffset, line, column);

  DebugAPI::onNewScript(cx, ctorScript);

  return ctor;
}

static JSObject* SuperFunOperation(JSObject* callee) {
  MOZ_ASSERT(callee->as<JSFunction>().isClassConstructor());
  MOZ_ASSERT(
      callee->as<JSFunction>().baseScript()->isDerivedClassConstructor());

  return callee->as<JSFunction>().staticPrototype();
}

static JSObject* HomeObjectSuperBase(JSContext* cx, JSObject* homeObj) {
  MOZ_ASSERT(homeObj->is<PlainObject>() || homeObj->is<JSFunction>());

  if (JSObject* superBase = homeObj->staticPrototype()) {
    return superBase;
  }

  ThrowHomeObjectNotObject(cx);
  return nullptr;
}

bool js::ReportIsNotFunction(JSContext* cx, HandleValue v, int numToSkip,
                             MaybeConstruct construct) {
  unsigned error = construct ? JSMSG_NOT_CONSTRUCTOR : JSMSG_NOT_FUNCTION;
  int spIndex = numToSkip >= 0 ? -(numToSkip + 1) : JSDVG_SEARCH_STACK;

  ReportValueError(cx, error, spIndex, v, nullptr);
  return false;
}

JSObject* js::ValueToCallable(JSContext* cx, HandleValue v, int numToSkip,
                              MaybeConstruct construct) {
  if (v.isObject() && v.toObject().isCallable()) {
    return &v.toObject();
  }

  ReportIsNotFunction(cx, v, numToSkip, construct);
  return nullptr;
}

static bool MaybeCreateThisForConstructor(JSContext* cx, const CallArgs& args,
                                          bool createSingleton) {
  if (args.thisv().isObject()) {
    return true;
  }

  RootedFunction callee(cx, &args.callee().as<JSFunction>());
  RootedObject newTarget(cx, &args.newTarget().toObject());
  NewObjectKind newKind = createSingleton ? SingletonObject : GenericObject;

  return CreateThis(cx, callee, newTarget, newKind, args.mutableThisv());
}

static MOZ_NEVER_INLINE JS_HAZ_JSNATIVE_CALLER bool Interpret(JSContext* cx,
                                                              RunState& state);

InterpreterFrame* InvokeState::pushInterpreterFrame(JSContext* cx) {
  return cx->interpreterStack().pushInvokeFrame(cx, args_, construct_);
}

InterpreterFrame* ExecuteState::pushInterpreterFrame(JSContext* cx) {
  return cx->interpreterStack().pushExecuteFrame(cx, script_, newTargetValue_,
                                                 envChain_, evalInFrame_);
}

InterpreterFrame* RunState::pushInterpreterFrame(JSContext* cx) {
  if (isInvoke()) {
    return asInvoke()->pushInterpreterFrame(cx);
  }
  return asExecute()->pushInterpreterFrame(cx);
}

// MSVC with PGO inlines a lot of functions in RunScript, resulting in large
// stack frames and stack overflow issues, see bug 1167883. Turn off PGO to
// avoid this.
#ifdef _MSC_VER
#  pragma optimize("g", off)
#endif
bool js::RunScript(JSContext* cx, RunState& state) {
  if (!CheckRecursionLimit(cx)) {
    return false;
  }

  // Since any script can conceivably GC, make sure it's safe to do so.
  cx->verifyIsSafeToGC();

  MOZ_ASSERT(cx->realm() == state.script()->realm());

  MOZ_DIAGNOSTIC_ASSERT(cx->realm()->isSystem() ||
                        cx->runtime()->allowContentJS());

  MOZ_ASSERT(!cx->enableAccessValidation || cx->realm()->isAccessValid());

  if (!DebugAPI::checkNoExecute(cx, state.script())) {
    return false;
  }

  GeckoProfilerEntryMarker marker(cx, state.script());

  jit::EnterJitStatus status = jit::MaybeEnterJit(cx, state);
  switch (status) {
    case jit::EnterJitStatus::Error:
      return false;
    case jit::EnterJitStatus::Ok:
      return true;
    case jit::EnterJitStatus::NotEntered:
      break;
  }

  if (state.isInvoke()) {
    InvokeState& invoke = *state.asInvoke();
    TypeMonitorCall(cx, invoke.args(), invoke.constructing());
  }

  return Interpret(cx, state);
}
#ifdef _MSC_VER
#  pragma optimize("", on)
#endif

STATIC_PRECONDITION_ASSUME(ubound(args.argv_) >= argc)
MOZ_ALWAYS_INLINE bool CallJSNative(JSContext* cx, Native native,
                                    CallReason reason, const CallArgs& args) {
  TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx);
  AutoTraceLog traceLog(logger, TraceLogger_Call);

  if (!CheckRecursionLimit(cx)) {
    return false;
  }

  NativeResumeMode resumeMode = DebugAPI::onNativeCall(cx, args, reason);
  if (resumeMode != NativeResumeMode::Continue) {
    return resumeMode == NativeResumeMode::Override;
  }

#ifdef DEBUG
  bool alreadyThrowing = cx->isExceptionPending();
#endif
  cx->check(args);
  MOZ_ASSERT(!args.callee().is<ProxyObject>());

  AutoRealm ar(cx, &args.callee());
  bool ok = native(cx, args.length(), args.base());
  if (ok) {
    cx->check(args.rval());
    MOZ_ASSERT_IF(!alreadyThrowing, !cx->isExceptionPending());
  }
  return ok;
}

STATIC_PRECONDITION(ubound(args.argv_) >= argc)
MOZ_ALWAYS_INLINE bool CallJSNativeConstructor(JSContext* cx, Native native,
                                               const CallArgs& args) {
#ifdef DEBUG
  RootedObject callee(cx, &args.callee());
#endif

  MOZ_ASSERT(args.thisv().isMagic());
  if (!CallJSNative(cx, native, CallReason::Call, args)) {
    return false;
  }

  /*
   * Native constructors must return non-primitive values on success.
   * Although it is legal, if a constructor returns the callee, there is a
   * 99.9999% chance it is a bug. If any valid code actually wants the
   * constructor to return the callee, the assertion can be removed or
   * (another) conjunct can be added to the antecedent.
   *
   * Exception: (new Object(Object)) returns the callee.
   */
  MOZ_ASSERT_IF((!callee->is<JSFunction>() ||
                 callee->as<JSFunction>().native() != obj_construct),
                args.rval().isObject() && callee != &args.rval().toObject());

  return true;
}

/*
 * Find a function reference and its 'this' value implicit first parameter
 * under argc arguments on cx's stack, and call the function.  Push missing
 * required arguments, allocate declared local variables, and pop everything
 * when done.  Then push the return value.
 *
 * Note: This function DOES NOT call GetThisValue to munge |args.thisv()| if
 *       necessary.  The caller (usually the interpreter) must have performed
 *       this step already!
 */
bool js::InternalCallOrConstruct(JSContext* cx, const CallArgs& args,
                                 MaybeConstruct construct, CallReason reason) {
  MOZ_ASSERT(args.length() <= ARGS_LENGTH_MAX);
  MOZ_ASSERT(!cx->zone()->types.activeAnalysis);

  unsigned skipForCallee = args.length() + 1 + (construct == CONSTRUCT);
  if (args.calleev().isPrimitive()) {
    return ReportIsNotFunction(cx, args.calleev(), skipForCallee, construct);
  }

  /* Invoke non-functions. */
  if (MOZ_UNLIKELY(!args.callee().is<JSFunction>())) {
    MOZ_ASSERT_IF(construct, !args.callee().isConstructor());

    if (!args.callee().isCallable()) {
      return ReportIsNotFunction(cx, args.calleev(), skipForCallee, construct);
    }

    if (args.callee().is<ProxyObject>()) {
      RootedObject proxy(cx, &args.callee());
      return Proxy::call(cx, proxy, args);
    }

    JSNative call = args.callee().callHook();
    MOZ_ASSERT(call, "isCallable without a callHook?");

    return CallJSNative(cx, call, reason, args);
  }

  /* Invoke native functions. */
  RootedFunction fun(cx, &args.callee().as<JSFunction>());
  if (construct != CONSTRUCT && fun->isClassConstructor()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CANT_CALL_CLASS_CONSTRUCTOR);
    return false;
  }

  if (fun->isNative()) {
    MOZ_ASSERT_IF(construct, !fun->isConstructor());
    JSNative native = fun->native();
    if (!construct && args.ignoresReturnValue() && fun->hasJitInfo()) {
      const JSJitInfo* jitInfo = fun->jitInfo();
      if (jitInfo->type() == JSJitInfo::IgnoresReturnValueNative) {
        native = jitInfo->ignoresReturnValueMethod;
      }
    }
    return CallJSNative(cx, native, reason, args);
  }

  // Self-hosted builtins are considered native by the onNativeCall hook.
  if (fun->isSelfHostedBuiltin()) {
    NativeResumeMode resumeMode = DebugAPI::onNativeCall(cx, args, reason);
    if (resumeMode != NativeResumeMode::Continue) {
      return resumeMode == NativeResumeMode::Override;
    }
  }

  if (!JSFunction::getOrCreateScript(cx, fun)) {
    return false;
  }

  /* Run function until JSOp::RetRval, JSOp::Return or error. */
  InvokeState state(cx, args, construct);

  // Create |this| if we're constructing. Switch to the callee's realm to
  // ensure this object has the correct realm.
  AutoRealm ar(cx, state.script());
  if (construct) {
    bool createSingleton = false;
    jsbytecode* pc;
    if (JSScript* script = cx->currentScript(&pc)) {
      if (ObjectGroup::useSingletonForNewObject(cx, script, pc)) {
        createSingleton = true;
      }
    }

    if (!MaybeCreateThisForConstructor(cx, args, createSingleton)) {
      return false;
    }
  }

  bool ok = RunScript(cx, state);

  MOZ_ASSERT_IF(ok && construct, args.rval().isObject());
  return ok;
}

static bool InternalCall(JSContext* cx, const AnyInvokeArgs& args,
                         CallReason reason = CallReason::Call) {
  MOZ_ASSERT(args.array() + args.length() == args.end(),
             "must pass calling arguments to a calling attempt");

  if (args.thisv().isObject()) {
    // We must call the thisValue hook in case we are not called from the
    // interpreter, where a prior bytecode has computed an appropriate
    // |this| already.  But don't do that if fval is a DOM function.
    HandleValue fval = args.calleev();
    if (!fval.isObject() || !fval.toObject().is<JSFunction>() ||
        !fval.toObject().as<JSFunction>().isNative() ||
        !fval.toObject().as<JSFunction>().hasJitInfo() ||
        fval.toObject()
            .as<JSFunction>()
            .jitInfo()
            ->needsOuterizedThisObject()) {
      JSObject* thisObj = &args.thisv().toObject();
      args.mutableThisv().set(GetThisValue(thisObj));
    }
  }

  return InternalCallOrConstruct(cx, args, NO_CONSTRUCT, reason);
}

bool js::CallFromStack(JSContext* cx, const CallArgs& args) {
  return InternalCall(cx, static_cast<const AnyInvokeArgs&>(args));
}

// ES7 rev 0c1bd3004329336774cbc90de727cd0cf5f11e93
// 7.3.12 Call.
bool js::Call(JSContext* cx, HandleValue fval, HandleValue thisv,
              const AnyInvokeArgs& args, MutableHandleValue rval,
              CallReason reason) {
  // Explicitly qualify these methods to bypass AnyInvokeArgs's deliberate
  // shadowing.
  args.CallArgs::setCallee(fval);
  args.CallArgs::setThis(thisv);

  if (!InternalCall(cx, args, reason)) {
    return false;
  }

  rval.set(args.rval());
  return true;
}

static bool InternalConstruct(JSContext* cx, const AnyConstructArgs& args) {
  MOZ_ASSERT(args.array() + args.length() + 1 == args.end(),
             "must pass constructing arguments to a construction attempt");
  MOZ_ASSERT(!JSFunction::class_.getConstruct());

  // Callers are responsible for enforcing these preconditions.
  MOZ_ASSERT(IsConstructor(args.calleev()),
             "trying to construct a value that isn't a constructor");
  MOZ_ASSERT(IsConstructor(args.CallArgs::newTarget()),
             "provided new.target value must be a constructor");

  MOZ_ASSERT(args.thisv().isMagic(JS_IS_CONSTRUCTING) ||
             args.thisv().isObject());

  JSObject& callee = args.callee();
  if (callee.is<JSFunction>()) {
    RootedFunction fun(cx, &callee.as<JSFunction>());

    if (fun->isNative()) {
      return CallJSNativeConstructor(cx, fun->native(), args);
    }

    if (!InternalCallOrConstruct(cx, args, CONSTRUCT)) {
      return false;
    }

    MOZ_ASSERT(args.CallArgs::rval().isObject());
    return true;
  }

  if (callee.is<ProxyObject>()) {
    RootedObject proxy(cx, &callee);
    return Proxy::construct(cx, proxy, args);
  }

  JSNative construct = callee.constructHook();
  MOZ_ASSERT(construct != nullptr, "IsConstructor without a construct hook?");

  return CallJSNativeConstructor(cx, construct, args);
}

// Check that |callee|, the callee in a |new| expression, is a constructor.
static bool StackCheckIsConstructorCalleeNewTarget(JSContext* cx,
                                                   HandleValue callee,
                                                   HandleValue newTarget) {
  // Calls from the stack could have any old non-constructor callee.
  if (!IsConstructor(callee)) {
    ReportValueError(cx, JSMSG_NOT_CONSTRUCTOR, JSDVG_SEARCH_STACK, callee,
                     nullptr);
    return false;
  }

  // The new.target has already been vetted by previous calls, or is the callee.
  // We can just assert that it's a constructor.
  MOZ_ASSERT(IsConstructor(newTarget));

  return true;
}

bool js::ConstructFromStack(JSContext* cx, const CallArgs& args) {
  if (!StackCheckIsConstructorCalleeNewTarget(cx, args.calleev(),
                                              args.newTarget())) {
    return false;
  }

  return InternalConstruct(cx, static_cast<const AnyConstructArgs&>(args));
}

bool js::Construct(JSContext* cx, HandleValue fval,
                   const AnyConstructArgs& args, HandleValue newTarget,
                   MutableHandleObject objp) {
  MOZ_ASSERT(args.thisv().isMagic(JS_IS_CONSTRUCTING));

  // Explicitly qualify to bypass AnyConstructArgs's deliberate shadowing.
  args.CallArgs::setCallee(fval);
  args.CallArgs::newTarget().set(newTarget);

  if (!InternalConstruct(cx, args)) {
    return false;
  }

  MOZ_ASSERT(args.CallArgs::rval().isObject());
  objp.set(&args.CallArgs::rval().toObject());
  return true;
}

bool js::InternalConstructWithProvidedThis(JSContext* cx, HandleValue fval,
                                           HandleValue thisv,
                                           const AnyConstructArgs& args,
                                           HandleValue newTarget,
                                           MutableHandleValue rval) {
  args.CallArgs::setCallee(fval);

  MOZ_ASSERT(thisv.isObject());
  args.CallArgs::setThis(thisv);

  args.CallArgs::newTarget().set(newTarget);

  if (!InternalConstruct(cx, args)) {
    return false;
  }

  rval.set(args.CallArgs::rval());
  return true;
}

bool js::CallGetter(JSContext* cx, HandleValue thisv, HandleValue getter,
                    MutableHandleValue rval) {
  // Invoke could result in another try to get or set the same id again, see
  // bug 355497.
  if (!CheckRecursionLimit(cx)) {
    return false;
  }

  FixedInvokeArgs<0> args(cx);

  return Call(cx, getter, thisv, args, rval, CallReason::Getter);
}

bool js::CallSetter(JSContext* cx, HandleValue thisv, HandleValue setter,
                    HandleValue v) {
  if (!CheckRecursionLimit(cx)) {
    return false;
  }

  FixedInvokeArgs<1> args(cx);

  args[0].set(v);

  RootedValue ignored(cx);
  return Call(cx, setter, thisv, args, &ignored, CallReason::Setter);
}

bool js::ExecuteKernel(JSContext* cx, HandleScript script,
                       JSObject& envChainArg, const Value& newTargetValue,
                       AbstractFramePtr evalInFrame, Value* result) {
  MOZ_ASSERT_IF(script->isGlobalCode(),
                IsGlobalLexicalEnvironment(&envChainArg) ||
                    !IsSyntacticEnvironment(&envChainArg));
#ifdef DEBUG
  RootedObject terminatingEnv(cx, &envChainArg);
  while (IsSyntacticEnvironment(terminatingEnv)) {
    terminatingEnv = terminatingEnv->enclosingEnvironment();
  }
  MOZ_ASSERT(terminatingEnv->is<GlobalObject>() ||
             script->hasNonSyntacticScope());
#endif

  if (script->treatAsRunOnce()) {
    if (script->hasRunOnce()) {
      JS_ReportErrorASCII(cx,
                          "Trying to execute a run-once script multiple times");
      return false;
    }

    script->setHasRunOnce();
  }

  if (script->isEmpty()) {
    if (result) {
      result->setUndefined();
    }
    return true;
  }

  probes::StartExecution(script);
  ExecuteState state(cx, script, newTargetValue, envChainArg, evalInFrame,
                     result);
  bool ok = RunScript(cx, state);
  probes::StopExecution(script);

  return ok;
}

bool js::Execute(JSContext* cx, HandleScript script, JSObject& envChainArg,
                 Value* rval) {
  /* The env chain is something we control, so we know it can't
     have any outer objects on it. */
  RootedObject envChain(cx, &envChainArg);
  MOZ_ASSERT(!IsWindowProxy(envChain));

  if (script->isModule()) {
    MOZ_RELEASE_ASSERT(
        envChain == script->module()->environment(),
        "Module scripts can only be executed in the module's environment");
  } else {
    MOZ_RELEASE_ASSERT(
        IsGlobalLexicalEnvironment(envChain) || script->hasNonSyntacticScope(),
        "Only global scripts with non-syntactic envs can be executed with "
        "interesting envchains");
  }

  /* Ensure the env chain is all same-compartment and terminates in a global. */
#ifdef DEBUG
  JSObject* s = envChain;
  do {
    cx->check(s);
    MOZ_ASSERT_IF(!s->enclosingEnvironment(), s->is<GlobalObject>());
  } while ((s = s->enclosingEnvironment()));
#endif

  return ExecuteKernel(cx, script, *envChain, NullValue(),
                       NullFramePtr() /* evalInFrame */, rval);
}

/*
 * ES6 (4-25-16) 12.10.4 InstanceofOperator
 */
extern bool JS::InstanceofOperator(JSContext* cx, HandleObject obj,
                                   HandleValue v, bool* bp) {
  /* Step 1. is handled by caller. */

  /* Step 2. */
  RootedValue hasInstance(cx);
  RootedId id(cx, SYMBOL_TO_JSID(cx->wellKnownSymbols().hasInstance));
  if (!GetProperty(cx, obj, obj, id, &hasInstance)) {
    return false;
  }

  if (!hasInstance.isNullOrUndefined()) {
    if (!IsCallable(hasInstance)) {
      return ReportIsNotFunction(cx, hasInstance);
    }

    /* Step 3. */
    RootedValue rval(cx);
    if (!Call(cx, hasInstance, obj, v, &rval)) {
      return false;
    }
    *bp = ToBoolean(rval);
    return true;
  }

  /* Step 4. */
  if (!obj->isCallable()) {
    RootedValue val(cx, ObjectValue(*obj));
    return ReportIsNotFunction(cx, val);
  }

  /* Step 5. */
  return OrdinaryHasInstance(cx, obj, v, bp);
}

bool js::HasInstance(JSContext* cx, HandleObject obj, HandleValue v, bool* bp) {
  const JSClass* clasp = obj->getClass();
  RootedValue local(cx, v);
  if (JSHasInstanceOp hasInstance = clasp->getHasInstance()) {
    return hasInstance(cx, obj, &local, bp);
  }
  return JS::InstanceofOperator(cx, obj, local, bp);
}

JSType js::TypeOfObject(JSObject* obj) {
  if (EmulatesUndefined(obj)) {
    return JSTYPE_UNDEFINED;
  }
  if (obj->isCallable()) {
    return JSTYPE_FUNCTION;
  }
  return JSTYPE_OBJECT;
}

JSType js::TypeOfValue(const Value& v) {
  switch (v.type()) {
    case ValueType::Double:
    case ValueType::Int32:
      return JSTYPE_NUMBER;
    case ValueType::String:
      return JSTYPE_STRING;
    case ValueType::Null:
      return JSTYPE_OBJECT;
    case ValueType::Undefined:
      return JSTYPE_UNDEFINED;
    case ValueType::Object:
      return TypeOfObject(&v.toObject());
    case ValueType::Boolean:
      return JSTYPE_BOOLEAN;
    case ValueType::BigInt:
      return JSTYPE_BIGINT;
    case ValueType::Symbol:
      return JSTYPE_SYMBOL;
    case ValueType::Magic:
    case ValueType::PrivateGCThing:
      break;
  }

  MOZ_CRASH("unexpected type");
}

bool js::CheckClassHeritageOperation(JSContext* cx, HandleValue heritage) {
  if (IsConstructor(heritage)) {
    return true;
  }

  if (heritage.isNull()) {
    return true;
  }

  if (heritage.isObject()) {
    ReportIsNotFunction(cx, heritage, 0, CONSTRUCT);
    return false;
  }

  ReportValueError(cx, JSMSG_BAD_HERITAGE, -1, heritage, nullptr,
                   "not an object or null");
  return false;
}

JSObject* js::ObjectWithProtoOperation(JSContext* cx, HandleValue val) {
  if (!val.isObjectOrNull()) {
    ReportValueError(cx, JSMSG_NOT_OBJORNULL, -1, val, nullptr);
    return nullptr;
  }

  RootedObject proto(cx, val.toObjectOrNull());
  return NewObjectWithGivenProto<PlainObject>(cx, proto);
}

JSObject* js::FunWithProtoOperation(JSContext* cx, HandleFunction fun,
                                    HandleObject parent, HandleObject proto) {
  return CloneFunctionObjectIfNotSingleton(cx, fun, parent, proto);
}

/*
 * Enter the new with environment using an object at sp[-1] and associate the
 * depth of the with block with sp + stackIndex.
 */
bool js::EnterWithOperation(JSContext* cx, AbstractFramePtr frame,
                            HandleValue val, Handle<WithScope*> scope) {
  RootedObject obj(cx);
  if (val.isObject()) {
    obj = &val.toObject();
  } else {
    obj = ToObject(cx, val);
    if (!obj) {
      return false;
    }
  }

  RootedObject envChain(cx, frame.environmentChain());
  WithEnvironmentObject* withobj =
      WithEnvironmentObject::create(cx, obj, envChain, scope);
  if (!withobj) {
    return false;
  }

  frame.pushOnEnvironmentChain(*withobj);
  return true;
}

static void PopEnvironment(JSContext* cx, EnvironmentIter& ei) {
  switch (ei.scope().kind()) {
    case ScopeKind::Lexical:
    case ScopeKind::SimpleCatch:
    case ScopeKind::Catch:
    case ScopeKind::NamedLambda:
    case ScopeKind::StrictNamedLambda:
    case ScopeKind::FunctionLexical:
      if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
        DebugEnvironments::onPopLexical(cx, ei);
      }
      if (ei.scope().hasEnvironment()) {
        ei.initialFrame().popOffEnvironmentChain<LexicalEnvironmentObject>();
      }
      break;
    case ScopeKind::With:
      if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
        DebugEnvironments::onPopWith(ei.initialFrame());
      }
      ei.initialFrame().popOffEnvironmentChain<WithEnvironmentObject>();
      break;
    case ScopeKind::Function:
      if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
        DebugEnvironments::onPopCall(cx, ei.initialFrame());
      }
      if (ei.scope().hasEnvironment()) {
        ei.initialFrame().popOffEnvironmentChain<CallObject>();
      }
      break;
    case ScopeKind::FunctionBodyVar:
    case ScopeKind::StrictEval:
      if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
        DebugEnvironments::onPopVar(cx, ei);
      }
      if (ei.scope().hasEnvironment()) {
        ei.initialFrame().popOffEnvironmentChain<VarEnvironmentObject>();
      }
      break;
    case ScopeKind::Module:
      if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
        DebugEnvironments::onPopModule(cx, ei);
      }
      break;
    case ScopeKind::Eval:
    case ScopeKind::Global:
    case ScopeKind::NonSyntactic:
      break;
    case ScopeKind::WasmInstance:
    case ScopeKind::WasmFunction:
      MOZ_CRASH("wasm is not interpreted");
      break;
  }
}

// Unwind environment chain and iterator to match the env corresponding to
// the given bytecode position.
void js::UnwindEnvironment(JSContext* cx, EnvironmentIter& ei, jsbytecode* pc) {
  if (!ei.withinInitialFrame()) {
    return;
  }

  RootedScope scope(cx, ei.initialFrame().script()->innermostScope(pc));

#ifdef DEBUG
  // A frame's environment chain cannot be unwound to anything enclosing the
  // body scope of a script.  This includes the parameter defaults
  // environment and the decl env object. These environments, once pushed
  // onto the environment chain, are expected to be there for the duration
  // of the frame.
  //
  // Attempting to unwind to the parameter defaults code in a script is a
  // bug; that section of code has no try-catch blocks.
  JSScript* script = ei.initialFrame().script();
  for (uint32_t i = 0; i < script->bodyScopeIndex(); i++) {
    MOZ_ASSERT(scope != script->getScope(i));
  }
#endif

  for (; ei.maybeScope() != scope; ei++) {
    PopEnvironment(cx, ei);
  }
}

// Unwind all environments. This is needed because block scopes may cover the
// first bytecode at a script's main(). e.g.,
//
//     function f() { { let i = 0; } }
//
// will have no pc location distinguishing the first block scope from the
// outermost function scope.
void js::UnwindAllEnvironmentsInFrame(JSContext* cx, EnvironmentIter& ei) {
  for (; ei.withinInitialFrame(); ei++) {
    PopEnvironment(cx, ei);
  }
}

// Compute the pc needed to unwind the environment to the beginning of a try
// block. We cannot unwind to *after* the JSOp::Try, because that might be the
// first opcode of an inner scope, with the same problem as above. e.g.,
//
// try { { let x; } }
//
// will have no pc location distinguishing the try block scope from the inner
// let block scope.
jsbytecode* js::UnwindEnvironmentToTryPc(JSScript* script,
                                         const JSTryNote* tn) {
  jsbytecode* pc = script->offsetToPC(tn->start);
  if (tn->kind == JSTRY_CATCH || tn->kind == JSTRY_FINALLY) {
    pc -= JSOpLength_Try;
    MOZ_ASSERT(JSOp(*pc) == JSOp::Try);
  } else if (tn->kind == JSTRY_DESTRUCTURING) {
    pc -= JSOpLength_TryDestructuring;
    MOZ_ASSERT(JSOp(*pc) == JSOp::TryDestructuring);
  }
  return pc;
}

static void SettleOnTryNote(JSContext* cx, const JSTryNote* tn,
                            EnvironmentIter& ei, InterpreterRegs& regs) {
  // Unwind the environment to the beginning of the JSOp::Try.
  UnwindEnvironment(cx, ei, UnwindEnvironmentToTryPc(regs.fp()->script(), tn));

  // Set pc to the first bytecode after the the try note to point
  // to the beginning of catch or finally.
  regs.pc = regs.fp()->script()->offsetToPC(tn->start + tn->length);
  regs.sp = regs.spForStackDepth(tn->stackDepth);
}

class InterpreterTryNoteFilter {
  const InterpreterRegs& regs_;

 public:
  explicit InterpreterTryNoteFilter(const InterpreterRegs& regs)
      : regs_(regs) {}
  bool operator()(const JSTryNote* note) {
    return note->stackDepth <= regs_.stackDepth();
  }
};

class TryNoteIterInterpreter : public TryNoteIter<InterpreterTryNoteFilter> {
 public:
  TryNoteIterInterpreter(JSContext* cx, const InterpreterRegs& regs)
      : TryNoteIter(cx, regs.fp()->script(), regs.pc,
                    InterpreterTryNoteFilter(regs)) {}
};

static void UnwindIteratorsForUncatchableException(
    JSContext* cx, const InterpreterRegs& regs) {
  // c.f. the regular (catchable) TryNoteIterInterpreter loop in
  // ProcessTryNotes.
  for (TryNoteIterInterpreter tni(cx, regs); !tni.done(); ++tni) {
    const JSTryNote* tn = *tni;
    switch (tn->kind) {
      case JSTRY_FOR_IN: {
        Value* sp = regs.spForStackDepth(tn->stackDepth);
        UnwindIteratorForUncatchableException(&sp[-1].toObject());
        break;
      }
      default:
        break;
    }
  }
}

enum HandleErrorContinuation {
  SuccessfulReturnContinuation,
  ErrorReturnContinuation,
  CatchContinuation,
  FinallyContinuation
};

static HandleErrorContinuation ProcessTryNotes(JSContext* cx,
                                               EnvironmentIter& ei,
                                               InterpreterRegs& regs) {
  for (TryNoteIterInterpreter tni(cx, regs); !tni.done(); ++tni) {
    const JSTryNote* tn = *tni;

    switch (tn->kind) {
      case JSTRY_CATCH:
        /* Catch cannot intercept the closing of a generator. */
        if (cx->isClosingGenerator()) {
          break;
        }

        SettleOnTryNote(cx, tn, ei, regs);
        return CatchContinuation;

      case JSTRY_FINALLY:
        SettleOnTryNote(cx, tn, ei, regs);
        return FinallyContinuation;

      case JSTRY_FOR_IN: {
        /* This is similar to JSOp::EndIter in the interpreter loop. */
        MOZ_ASSERT(tn->stackDepth <= regs.stackDepth());
        Value* sp = regs.spForStackDepth(tn->stackDepth);
        JSObject* obj = &sp[-1].toObject();
        CloseIterator(obj);
        break;
      }

      case JSTRY_DESTRUCTURING: {
        // Whether the destructuring iterator is done is at the top of the
        // stack. The iterator object is second from the top.
        MOZ_ASSERT(tn->stackDepth > 1);
        Value* sp = regs.spForStackDepth(tn->stackDepth);
        RootedValue doneValue(cx, sp[-1]);
        MOZ_RELEASE_ASSERT(!doneValue.isMagic());
        bool done = ToBoolean(doneValue);
        if (!done) {
          RootedObject iterObject(cx, &sp[-2].toObject());
          if (!IteratorCloseForException(cx, iterObject)) {
            SettleOnTryNote(cx, tn, ei, regs);
            return ErrorReturnContinuation;
          }
        }
        break;
      }

      case JSTRY_FOR_OF:
      case JSTRY_LOOP:
        break;

      // JSTRY_FOR_OF_ITERCLOSE is handled internally by the try note iterator.
      default:
        MOZ_CRASH("Invalid try note");
    }
  }

  return SuccessfulReturnContinuation;
}

bool js::HandleClosingGeneratorReturn(JSContext* cx, AbstractFramePtr frame,
                                      bool ok) {
  /*
   * Propagate the exception or error to the caller unless the exception
   * is an asynchronous return from a generator.
   */
  if (cx->isClosingGenerator()) {
    cx->clearPendingException();
    ok = true;
    SetGeneratorClosed(cx, frame);
  }
  return ok;
}

static HandleErrorContinuation HandleError(JSContext* cx,
                                           InterpreterRegs& regs) {
  MOZ_ASSERT(regs.fp()->script()->containsPC(regs.pc));
  MOZ_ASSERT(cx->realm() == regs.fp()->script()->realm());

  if (regs.fp()->script()->hasScriptCounts()) {
    PCCounts* counts = regs.fp()->script()->getThrowCounts(regs.pc);
    // If we failed to allocate, then skip the increment and continue to
    // handle the exception.
    if (counts) {
      counts->numExec()++;
    }
  }

  EnvironmentIter ei(cx, regs.fp(), regs.pc);
  bool ok = false;

again:
  if (cx->isExceptionPending()) {
    /* Call debugger throw hooks. */
    if (!cx->isClosingGenerator()) {
      if (!DebugAPI::onExceptionUnwind(cx, regs.fp())) {
        if (!cx->isExceptionPending()) {
          goto again;
        }
      }
      // Ensure that the debugger hasn't returned 'true' while clearing the
      // exception state.
      MOZ_ASSERT(cx->isExceptionPending());
    }

    HandleErrorContinuation res = ProcessTryNotes(cx, ei, regs);
    switch (res) {
      case SuccessfulReturnContinuation:
        break;
      case ErrorReturnContinuation:
        goto again;
      case CatchContinuation:
      case FinallyContinuation:
        // No need to increment the PCCounts number of execution here, as
        // the interpreter increments any PCCounts if present.
        MOZ_ASSERT_IF(regs.fp()->script()->hasScriptCounts(),
                      regs.fp()->script()->maybeGetPCCounts(regs.pc));
        return res;
    }

    ok = HandleClosingGeneratorReturn(cx, regs.fp(), ok);
  } else {
    UnwindIteratorsForUncatchableException(cx, regs);

    // We may be propagating a forced return from a debugger hook function.
    if (MOZ_UNLIKELY(cx->isPropagatingForcedReturn())) {
      cx->clearPropagatingForcedReturn();
      ok = true;
    }
  }

  ok = DebugAPI::onLeaveFrame(cx, regs.fp(), regs.pc, ok);

  // After this point, we will pop the frame regardless. Settle the frame on
  // the end of the script.
  regs.setToEndOfScript();

  return ok ? SuccessfulReturnContinuation : ErrorReturnContinuation;
}

#define REGS (activation.regs())
#define PUSH_COPY(v)                 \
  do {                               \
    *REGS.sp++ = (v);                \
    cx->debugOnlyCheck(REGS.sp[-1]); \
  } while (0)
#define PUSH_COPY_SKIP_CHECK(v) *REGS.sp++ = (v)
#define PUSH_NULL() REGS.sp++->setNull()
#define PUSH_UNDEFINED() REGS.sp++->setUndefined()
#define PUSH_BOOLEAN(b) REGS.sp++->setBoolean(b)
#define PUSH_DOUBLE(d) REGS.sp++->setDouble(d)
#define PUSH_INT32(i) REGS.sp++->setInt32(i)
#define PUSH_SYMBOL(s) REGS.sp++->setSymbol(s)
#define PUSH_BIGINT(b) REGS.sp++->setBigInt(b)
#define PUSH_STRING(s)               \
  do {                               \
    REGS.sp++->setString(s);         \
    cx->debugOnlyCheck(REGS.sp[-1]); \
  } while (0)
#define PUSH_OBJECT(obj)             \
  do {                               \
    REGS.sp++->setObject(obj);       \
    cx->debugOnlyCheck(REGS.sp[-1]); \
  } while (0)
#define PUSH_OBJECT_OR_NULL(obj)     \
  do {                               \
    REGS.sp++->setObjectOrNull(obj); \
    cx->debugOnlyCheck(REGS.sp[-1]); \
  } while (0)
#define PUSH_MAGIC(magic) REGS.sp++->setMagic(magic)
#define POP_COPY_TO(v) (v) = *--REGS.sp
#define POP_RETURN_VALUE() REGS.fp()->setReturnValue(*--REGS.sp)

#define FETCH_OBJECT(cx, n, obj, key)                                \
  JS_BEGIN_MACRO                                                     \
    HandleValue val = REGS.stackHandleAt(n);                         \
    obj = ToObjectFromStackForPropertyAccess((cx), (val), n, (key)); \
    if (!(obj)) goto error;                                          \
  JS_END_MACRO

/*
 * Same for JSOp::SetName and JSOp::SetProp, which differ only slightly but
 * remain distinct for the decompiler.
 */
JS_STATIC_ASSERT(JSOpLength_SetName == JSOpLength_SetProp);

/* See TRY_BRANCH_AFTER_COND. */
JS_STATIC_ASSERT(JSOpLength_IfNe == JSOpLength_IfEq);
JS_STATIC_ASSERT(uint8_t(JSOp::IfNe) == uint8_t(JSOp::IfEq) + 1);

/*
 * Compute the implicit |this| value used by a call expression with an
 * unqualified name reference. The environment the binding was found on is
 * passed as argument, env.
 *
 * The implicit |this| is |undefined| for all environment types except
 * WithEnvironmentObject. This is the case for |with(...) {...}| expressions or
 * if the embedding uses a non-syntactic WithEnvironmentObject.
 *
 * NOTE: A non-syntactic WithEnvironmentObject may have a corresponding
 * extensible LexicalEnviornmentObject, but it will not be considered as an
 * implicit |this|. This is for compatibility with the Gecko subscript loader.
 */
static inline Value ComputeImplicitThis(JSObject* env) {
  // Fast-path for GlobalObject
  if (env->is<GlobalObject>()) {
    return UndefinedValue();
  }

  // WithEnvironmentObjects have an actual implicit |this|
  if (env->is<WithEnvironmentObject>()) {
    return GetThisValueOfWith(env);
  }

  // Debugger environments need special casing, as despite being
  // non-syntactic, they wrap syntactic environments and should not be
  // treated like other embedding-specific non-syntactic environments.
  if (env->is<DebugEnvironmentProxy>()) {
    return ComputeImplicitThis(&env->as<DebugEnvironmentProxy>().environment());
  }

  MOZ_ASSERT(env->is<EnvironmentObject>());
  return UndefinedValue();
}

static MOZ_ALWAYS_INLINE bool AddOperation(JSContext* cx,
                                           MutableHandleValue lhs,
                                           MutableHandleValue rhs,
                                           MutableHandleValue res) {
  if (lhs.isInt32() && rhs.isInt32()) {
    int32_t l = lhs.toInt32(), r = rhs.toInt32();
    int32_t t;
    if (MOZ_LIKELY(SafeAdd(l, r, &t))) {
      res.setInt32(t);
      return true;
    }
  }

  if (!ToPrimitive(cx, lhs)) {
    return false;
  }
  if (!ToPrimitive(cx, rhs)) {
    return false;
  }

  bool lIsString = lhs.isString();
  bool rIsString = rhs.isString();
  if (lIsString || rIsString) {
    JSString* lstr;
    if (lIsString) {
      lstr = lhs.toString();
    } else {
      lstr = ToString<CanGC>(cx, lhs);
      if (!lstr) {
        return false;
      }
    }

    JSString* rstr;
    if (rIsString) {
      rstr = rhs.toString();
    } else {
      // Save/restore lstr in case of GC activity under ToString.
      lhs.setString(lstr);
      rstr = ToString<CanGC>(cx, rhs);
      if (!rstr) {
        return false;
      }
      lstr = lhs.toString();
    }
    JSString* str = ConcatStrings<NoGC>(cx, lstr, rstr);
    if (!str) {
      RootedString nlstr(cx, lstr), nrstr(cx, rstr);
      str = ConcatStrings<CanGC>(cx, nlstr, nrstr);
      if (!str) {
        return false;
      }
    }
    res.setString(str);
    return true;
  }

  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::addValue(cx, lhs, rhs, res);
  }

  res.setNumber(lhs.toNumber() + rhs.toNumber());
  return true;
}

static MOZ_ALWAYS_INLINE bool SubOperation(JSContext* cx,
                                           MutableHandleValue lhs,
                                           MutableHandleValue rhs,
                                           MutableHandleValue res) {
  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::subValue(cx, lhs, rhs, res);
  }

  res.setNumber(lhs.toNumber() - rhs.toNumber());
  return true;
}

static MOZ_ALWAYS_INLINE bool MulOperation(JSContext* cx,
                                           MutableHandleValue lhs,
                                           MutableHandleValue rhs,
                                           MutableHandleValue res) {
  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::mulValue(cx, lhs, rhs, res);
  }

  res.setNumber(lhs.toNumber() * rhs.toNumber());
  return true;
}

static MOZ_ALWAYS_INLINE bool DivOperation(JSContext* cx,
                                           MutableHandleValue lhs,
                                           MutableHandleValue rhs,
                                           MutableHandleValue res) {
  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::divValue(cx, lhs, rhs, res);
  }

  res.setNumber(NumberDiv(lhs.toNumber(), rhs.toNumber()));
  return true;
}

static MOZ_ALWAYS_INLINE bool ModOperation(JSContext* cx,
                                           MutableHandleValue lhs,
                                           MutableHandleValue rhs,
                                           MutableHandleValue res) {
  int32_t l, r;
  if (lhs.isInt32() && rhs.isInt32() && (l = lhs.toInt32()) >= 0 &&
      (r = rhs.toInt32()) > 0) {
    int32_t mod = l % r;
    res.setInt32(mod);
    return true;
  }

  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::modValue(cx, lhs, rhs, res);
  }

  res.setNumber(NumberMod(lhs.toNumber(), rhs.toNumber()));
  return true;
}

static MOZ_ALWAYS_INLINE bool PowOperation(JSContext* cx,
                                           MutableHandleValue lhs,
                                           MutableHandleValue rhs,
                                           MutableHandleValue res) {
  if (!ToNumeric(cx, lhs) || !ToNumeric(cx, rhs)) {
    return false;
  }

  if (lhs.isBigInt() || rhs.isBigInt()) {
    return BigInt::powValue(cx, lhs, rhs, res);
  }

  res.setNumber(ecmaPow(lhs.toNumber(), rhs.toNumber()));
  return true;
}

static MOZ_ALWAYS_INLINE bool SetObjectElementOperation(
    JSContext* cx, HandleObject obj, HandleId id, HandleValue value,
    HandleValue receiver, bool strict, JSScript* script = nullptr,
    jsbytecode* pc = nullptr) {
  // receiver != obj happens only at super[expr], where we expect to find the
  // property. People probably aren't building hashtables with |super|
  // anyway.
  JitScript::MonitorAssign(cx, obj, id);

  if (obj->isNative() && JSID_IS_INT(id)) {
    uint32_t length = obj->as<NativeObject>().getDenseInitializedLength();
    int32_t i = JSID_TO_INT(id);
    if ((uint32_t)i >= length) {
      // Annotate script if provided with information (e.g. baseline)
      if (script && script->hasJitScript() && IsSetElemPC(pc)) {
        script->jitScript()->noteHasDenseAdd(script->pcToOffset(pc));
      }
    }
  }

  // Set the HadElementsAccess flag on the object if needed. This flag is
  // used to do more eager dictionary-mode conversion for objects that are
  // used as hashmaps. Set this flag only for objects with many properties,
  // to avoid unnecessary Shape changes.
  if (obj->isNative() && JSID_IS_ATOM(id) &&
      !obj->as<NativeObject>().inDictionaryMode() &&
      !obj->as<NativeObject>().hadElementsAccess() &&
      obj->as<NativeObject>().slotSpan() >
          PropertyTree::MAX_HEIGHT_WITH_ELEMENTS_ACCESS / 3) {
    if (!NativeObject::setHadElementsAccess(cx, obj.as<NativeObject>())) {
      return false;
    }
  }

  ObjectOpResult result;
  return SetProperty(cx, obj, id, value, receiver, result) &&
         result.checkStrictErrorOrWarning(cx, obj, id, strict);
}

/*
 * As an optimization, the interpreter creates a handful of reserved Rooted<T>
 * variables at the beginning, thus inserting them into the Rooted list once
 * upon entry. ReservedRooted "borrows" a reserved Rooted variable and uses it
 * within a local scope, resetting the value to nullptr (or the appropriate
 * equivalent for T) at scope end. This avoids inserting/removing the Rooted
 * from the rooter list, while preventing stale values from being kept alive
 * unnecessarily.
 */

template <typename T>
class ReservedRooted : public RootedBase<T, ReservedRooted<T>> {
  Rooted<T>* savedRoot;

 public:
  ReservedRooted(Rooted<T>* root, const T& ptr) : savedRoot(root) {
    *root = ptr;
  }

  explicit ReservedRooted(Rooted<T>* root) : savedRoot(root) {
    *root = JS::SafelyInitialized<T>();
  }

  ~ReservedRooted() { *savedRoot = JS::SafelyInitialized<T>(); }

  void set(const T& p) const { *savedRoot = p; }
  operator Handle<T>() { return *savedRoot; }
  operator Rooted<T>&() { return *savedRoot; }
  MutableHandle<T> operator&() { return &*savedRoot; }

  DECLARE_NONPOINTER_ACCESSOR_METHODS(savedRoot->get())
  DECLARE_NONPOINTER_MUTABLE_ACCESSOR_METHODS(savedRoot->get())
  DECLARE_POINTER_CONSTREF_OPS(T)
  DECLARE_POINTER_ASSIGN_OPS(ReservedRooted, T)
};

void js::ReportInNotObjectError(JSContext* cx, HandleValue lref, int lindex,
                                HandleValue rref, int rindex) {
  auto uniqueCharsFromString = [](JSContext* cx,
                                  HandleValue ref) -> UniqueChars {
    static const size_t MaxStringLength = 16;
    RootedString str(cx, ref.toString());
    if (str->length() > MaxStringLength) {
      JSStringBuilder buf(cx);
      if (!buf.appendSubstring(str, 0, MaxStringLength)) {
        return nullptr;
      }
      if (!buf.append("...")) {
        return nullptr;
      }
      str = buf.finishString();
      if (!str) {
        return nullptr;
      }
    }
    return QuoteString(cx, str, '"');
  };

  if (lref.isString() && rref.isString()) {
    UniqueChars lbytes = uniqueCharsFromString(cx, lref);
    if (!lbytes) {
      return;
    }
    UniqueChars rbytes = uniqueCharsFromString(cx, rref);
    if (!rbytes) {
      return;
    }
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_IN_STRING,
                             lbytes.get(), rbytes.get());
    return;
  }

  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_IN_NOT_OBJECT,
                            InformalValueTypeName(rref));
}

static MOZ_NEVER_INLINE JS_HAZ_JSNATIVE_CALLER bool Interpret(JSContext* cx,
                                                              RunState& state) {
/*
 * Define macros for an interpreter loop. Opcode dispatch is done by
 * indirect goto (aka a threaded interpreter), which is technically
 * non-standard but is supported by all of our supported compilers.
 */
#define INTERPRETER_LOOP()
#define CASE(OP) label_##OP:
#define DEFAULT() \
  label_default:
#define DISPATCH_TO(OP) goto* addresses[(OP)]

#define LABEL(X) (&&label_##X)

  // Use addresses instead of offsets to optimize for runtime speed over
  // load-time relocation overhead.
  static const void* const addresses[EnableInterruptsPseudoOpcode + 1] = {
#define OPCODE_LABEL(op, ...) LABEL(op),
      FOR_EACH_OPCODE(OPCODE_LABEL)
#undef OPCODE_LABEL
#define TRAILING_LABEL(v)                                                    \
  ((v) == EnableInterruptsPseudoOpcode ? LABEL(EnableInterruptsPseudoOpcode) \
                                       : LABEL(default)),
          FOR_EACH_TRAILING_UNUSED_OPCODE(TRAILING_LABEL)
#undef TRAILING_LABEL
  };

  /*
   * Increment REGS.pc by N, load the opcode at that position,
   * and jump to the code to execute it.
   *
   * When Debugger puts a script in single-step mode, all js::Interpret
   * invocations that might be presently running that script must have
   * interrupts enabled. It's not practical to simply check
   * script->stepModeEnabled() at each point some callee could have changed
   * it, because there are so many places js::Interpret could possibly cause
   * JavaScript to run: each place an object might be coerced to a primitive
   * or a number, for example. So instead, we expose a simple mechanism to
   * let Debugger tweak the affected js::Interpret frames when an onStep
   * handler is added: calling activation.enableInterruptsUnconditionally()
   * will enable interrupts, and activation.opMask() is or'd with the opcode
   * to implement a simple alternate dispatch.
   */
#define ADVANCE_AND_DISPATCH(N)                  \
  JS_BEGIN_MACRO                                 \
    REGS.pc += (N);                              \
    SANITY_CHECKS();                             \
    DISPATCH_TO(*REGS.pc | activation.opMask()); \
  JS_END_MACRO

  /*
   * Shorthand for the common sequence at the end of a fixed-size opcode.
   */
#define END_CASE(OP) ADVANCE_AND_DISPATCH(JSOpLength_##OP);

  /*
   * Prepare to call a user-supplied branch handler, and abort the script
   * if it returns false.
   */
#define CHECK_BRANCH()                      \
  JS_BEGIN_MACRO                            \
    if (!CheckForInterrupt(cx)) goto error; \
  JS_END_MACRO

  /*
   * This is a simple wrapper around ADVANCE_AND_DISPATCH which also does
   * a CHECK_BRANCH() if n is not positive, which possibly indicates that it
   * is the backedge of a loop.
   */
#define BRANCH(n)                  \
  JS_BEGIN_MACRO                   \
    int32_t nlen = (n);            \
    if (nlen <= 0) CHECK_BRANCH(); \
    ADVANCE_AND_DISPATCH(nlen);    \
  JS_END_MACRO

  /*
   * Initialize code coverage vectors.
   */
#define INIT_COVERAGE()                                \
  JS_BEGIN_MACRO                                       \
    if (!script->hasScriptCounts()) {                  \
      if (cx->realm()->collectCoverageForDebug()) {    \
        if (!script->initScriptCounts(cx)) goto error; \
      }                                                \
    }                                                  \
  JS_END_MACRO

  /*
   * Increment the code coverage counter associated with the given pc.
   */
#define COUNT_COVERAGE_PC(PC)                          \
  JS_BEGIN_MACRO                                       \
    if (script->hasScriptCounts()) {                   \
      PCCounts* counts = script->maybeGetPCCounts(PC); \
      MOZ_ASSERT(counts);                              \
      counts->numExec()++;                             \
    }                                                  \
  JS_END_MACRO

#define COUNT_COVERAGE_MAIN()                                        \
  JS_BEGIN_MACRO                                                     \
    jsbytecode* main = script->main();                               \
    if (!BytecodeIsJumpTarget(JSOp(*main))) COUNT_COVERAGE_PC(main); \
  JS_END_MACRO

#define COUNT_COVERAGE()                              \
  JS_BEGIN_MACRO                                      \
    MOZ_ASSERT(BytecodeIsJumpTarget(JSOp(*REGS.pc))); \
    COUNT_COVERAGE_PC(REGS.pc);                       \
  JS_END_MACRO

#define SET_SCRIPT(s)                                    \
  JS_BEGIN_MACRO                                         \
    script = (s);                                        \
    MOZ_ASSERT(cx->realm() == script->realm());          \
    if (DebugAPI::hasAnyBreakpointsOrStepMode(script) || \
        script->hasScriptCounts())                       \
      activation.enableInterruptsUnconditionally();      \
  JS_END_MACRO

#define SANITY_CHECKS()              \
  JS_BEGIN_MACRO                     \
    js::gc::MaybeVerifyBarriers(cx); \
  JS_END_MACRO

  gc::MaybeVerifyBarriers(cx, true);
  MOZ_ASSERT(!cx->zone()->types.activeAnalysis);

  InterpreterFrame* entryFrame = state.pushInterpreterFrame(cx);
  if (!entryFrame) {
    return false;
  }

  ActivationEntryMonitor entryMonitor(cx, entryFrame);
  InterpreterActivation activation(state, cx, entryFrame);

  /* The script is used frequently, so keep a local copy. */
  RootedScript script(cx);
  SET_SCRIPT(REGS.fp()->script());

  TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx);
  TraceLoggerEvent scriptEvent(TraceLogger_Scripts, script);
  TraceLogStartEvent(logger, scriptEvent);
  TraceLogStartEvent(logger, TraceLogger_Interpreter);

  /*
   * Pool of rooters for use in this interpreter frame. References to these
   * are used for local variables within interpreter cases. This avoids
   * creating new rooters each time an interpreter case is entered, and also
   * correctness pitfalls due to incorrect compilation of destructor calls
   * around computed gotos.
   */
  RootedValue rootValue0(cx), rootValue1(cx);
  RootedString rootString0(cx), rootString1(cx);
  RootedObject rootObject0(cx), rootObject1(cx), rootObject2(cx);
  RootedNativeObject rootNativeObject0(cx);
  RootedFunction rootFunction0(cx);
  RootedPropertyName rootName0(cx);
  RootedId rootId0(cx);
  RootedShape rootShape0(cx);
  RootedScript rootScript0(cx);
  Rooted<Scope*> rootScope0(cx);
  DebugOnly<uint32_t> blockDepth;

  /* State communicated between non-local jumps: */
  bool interpReturnOK;
  bool frameHalfInitialized;

  if (!activation.entryFrame()->prologue(cx)) {
    goto prologue_error;
  }

  if (!DebugAPI::onEnterFrame(cx, activation.entryFrame())) {
    goto error;
  }

  // Increment the coverage for the main entry point.
  INIT_COVERAGE();
  COUNT_COVERAGE_MAIN();

  // Enter the interpreter loop starting at the current pc.
  ADVANCE_AND_DISPATCH(0);

  INTERPRETER_LOOP() {
    CASE(EnableInterruptsPseudoOpcode) {
      bool moreInterrupts = false;
      jsbytecode op = *REGS.pc;

      if (!script->hasScriptCounts() &&
          cx->realm()->collectCoverageForDebug()) {
        if (!script->initScriptCounts(cx)) {
          goto error;
        }
      }

      if (script->isDebuggee()) {
        if (DebugAPI::stepModeEnabled(script)) {
          if (!DebugAPI::onSingleStep(cx)) {
            goto error;
          }
          moreInterrupts = true;
        }

        if (DebugAPI::hasAnyBreakpointsOrStepMode(script)) {
          moreInterrupts = true;
        }

        if (DebugAPI::hasBreakpointsAt(script, REGS.pc)) {
          if (!DebugAPI::onTrap(cx)) {
            goto error;
          }
        }
      }

      MOZ_ASSERT(activation.opMask() == EnableInterruptsPseudoOpcode);
      if (!moreInterrupts) {
        activation.clearInterruptsMask();
      }

      /* Commence executing the actual opcode. */
      SANITY_CHECKS();
      DISPATCH_TO(op);
    }

    /* Various 1-byte no-ops. */
    CASE(Nop)
    CASE(NopDestructuring)
    CASE(TryDestructuring) {
      MOZ_ASSERT(GetBytecodeLength(REGS.pc) == 1);
      ADVANCE_AND_DISPATCH(1);
    }

    CASE(Try)
    END_CASE(Try)

    CASE(JumpTarget)
    COUNT_COVERAGE();
    END_CASE(JumpTarget)

    CASE(LoopHead) {
      COUNT_COVERAGE();

      // Attempt on-stack replacement into the Baseline Interpreter.
      if (jit::IsBaselineInterpreterEnabled()) {
        script->incWarmUpCounter();

        jit::MethodStatus status =
            jit::CanEnterBaselineInterpreterAtBranch(cx, REGS.fp());
        if (status == jit::Method_Error) {
          goto error;
        }
        if (status == jit::Method_Compiled) {
          bool wasProfiler = REGS.fp()->hasPushedGeckoProfilerFrame();

          jit::JitExecStatus maybeOsr;
          {
            GeckoProfilerBaselineOSRMarker osr(cx, wasProfiler);
            maybeOsr =
                jit::EnterBaselineInterpreterAtBranch(cx, REGS.fp(), REGS.pc);
          }

          // We failed to call into baseline at all, so treat as an error.
          if (maybeOsr == jit::JitExec_Aborted) {
            goto error;
          }

          interpReturnOK = (maybeOsr == jit::JitExec_Ok);

          // Pop the profiler frame pushed by the interpreter.  (The compiled
          // version of the function popped a copy of the frame pushed by the
          // OSR trampoline.)
          if (wasProfiler) {
            cx->geckoProfiler().exit(cx, script);
          }

          if (activation.entryFrame() != REGS.fp()) {
            goto jit_return_pop_frame;
          }
          goto leave_on_safe_point;
        }
      }
      if (script->trackRecordReplayProgress()) {
        mozilla::recordreplay::AdvanceExecutionProgressCounter();
      }
    }
    END_CASE(LoopHead)

    CASE(Lineno)
    END_CASE(Lineno)

    CASE(ForceInterpreter) {
      // Ensure pattern matching still works.
      MOZ_ASSERT(script->hasForceInterpreterOp());
    }
    END_CASE(ForceInterpreter)

    CASE(Undefined) {
      // If this ever changes, change what JSOp::GImplicitThis does too.
      PUSH_UNDEFINED();
    }
    END_CASE(Undefined)

    CASE(Pop) { REGS.sp--; }
    END_CASE(Pop)

    CASE(PopN) {
      MOZ_ASSERT(GET_UINT16(REGS.pc) <= REGS.stackDepth());
      REGS.sp -= GET_UINT16(REGS.pc);
    }
    END_CASE(PopN)

    CASE(DupAt) {
      MOZ_ASSERT(GET_UINT24(REGS.pc) < REGS.stackDepth());
      unsigned i = GET_UINT24(REGS.pc);
      const Value& rref = REGS.sp[-int(i + 1)];
      PUSH_COPY(rref);
    }
    END_CASE(DupAt)

    CASE(SetRval) { POP_RETURN_VALUE(); }
    END_CASE(SetRval)

    CASE(GetRval) { PUSH_COPY(REGS.fp()->returnValue()); }
    END_CASE(GetRval)

    CASE(EnterWith) {
      ReservedRooted<Value> val(&rootValue0, REGS.sp[-1]);
      REGS.sp--;
      ReservedRooted<Scope*> scope(&rootScope0, script->getScope(REGS.pc));

      if (!EnterWithOperation(cx, REGS.fp(), val, scope.as<WithScope>())) {
        goto error;
      }
    }
    END_CASE(EnterWith)

    CASE(LeaveWith) {
      REGS.fp()->popOffEnvironmentChain<WithEnvironmentObject>();
    }
    END_CASE(LeaveWith)

    CASE(Return) {
      POP_RETURN_VALUE();
      /* FALL THROUGH */
    }
    CASE(RetRval) {
      /*
       * When the inlined frame exits with an exception or an error, ok will be
       * false after the inline_return label.
       */
      CHECK_BRANCH();

    successful_return_continuation:
      interpReturnOK = true;

    return_continuation:
      frameHalfInitialized = false;

    prologue_return_continuation:

      if (activation.entryFrame() != REGS.fp()) {
        // Stop the engine. (No details about which engine exactly, could be
        // interpreter, Baseline or IonMonkey.)
        TraceLogStopEvent(logger, TraceLogger_Engine);
        TraceLogStopEvent(logger, TraceLogger_Scripts);

        if (MOZ_LIKELY(!frameHalfInitialized)) {
          interpReturnOK =
              DebugAPI::onLeaveFrame(cx, REGS.fp(), REGS.pc, interpReturnOK);

          REGS.fp()->epilogue(cx, REGS.pc);
        }

      jit_return_pop_frame:

        activation.popInlineFrame(REGS.fp());
        {
          JSScript* callerScript = REGS.fp()->script();
          if (cx->realm() != callerScript->realm()) {
            cx->leaveRealm(callerScript->realm());
          }
          SET_SCRIPT(callerScript);
        }

      jit_return:

        MOZ_ASSERT(IsInvokePC(REGS.pc));
        MOZ_ASSERT(cx->realm() == script->realm());

        /* Resume execution in the calling frame. */
        if (MOZ_LIKELY(interpReturnOK)) {
          if (JSOp(*REGS.pc) == JSOp::Resume) {
            ADVANCE_AND_DISPATCH(JSOpLength_Resume);
          }

          JitScript::MonitorBytecodeType(cx, script, REGS.pc, REGS.sp[-1]);
          MOZ_ASSERT(GetBytecodeLength(REGS.pc) == JSOpLength_Call);
          ADVANCE_AND_DISPATCH(JSOpLength_Call);
        }

        goto error;
      } else {
        // Stack should be empty for the outer frame, unless we executed the
        // first |await| expression in an async function.
        MOZ_ASSERT(REGS.stackDepth() == 0 ||
                   (JSOp(*REGS.pc) == JSOp::Await &&
                    !REGS.fp()->isResumedGenerator()));
      }
      goto exit;
    }

    CASE(Default) {
      REGS.sp--;
      /* FALL THROUGH */
    }
    CASE(Goto) { BRANCH(GET_JUMP_OFFSET(REGS.pc)); }

    CASE(IfEq) {
      bool cond = ToBoolean(REGS.stackHandleAt(-1));
      REGS.sp--;
      if (!cond) {
        BRANCH(GET_JUMP_OFFSET(REGS.pc));
      }
    }
    END_CASE(IfEq)

    CASE(IfNe) {
      bool cond = ToBoolean(REGS.stackHandleAt(-1));
      REGS.sp--;
      if (cond) {
        BRANCH(GET_JUMP_OFFSET(REGS.pc));
      }
    }
    END_CASE(IfNe)

    CASE(Or) {
      bool cond = ToBoolean(REGS.stackHandleAt(-1));
      if (cond) {
        ADVANCE_AND_DISPATCH(GET_JUMP_OFFSET(REGS.pc));
      }
    }
    END_CASE(Or)

    CASE(Coalesce) {
      MutableHandleValue res = REGS.stackHandleAt(-1);
      bool cond = !res.isNullOrUndefined();
      if (cond) {
        ADVANCE_AND_DISPATCH(GET_JUMP_OFFSET(REGS.pc));
      }
    }
    END_CASE(Coalesce)

    CASE(And) {
      bool cond = ToBoolean(REGS.stackHandleAt(-1));
      if (!cond) {
        ADVANCE_AND_DISPATCH(GET_JUMP_OFFSET(REGS.pc));
      }
    }
    END_CASE(And)

#define FETCH_ELEMENT_ID(n, id)                                       \
  JS_BEGIN_MACRO                                                      \
    if (!ToPropertyKey(cx, REGS.stackHandleAt(n), &(id))) goto error; \
  JS_END_MACRO

#define TRY_BRANCH_AFTER_COND(cond, spdec)                                \
  JS_BEGIN_MACRO                                                          \
    MOZ_ASSERT(GetBytecodeLength(REGS.pc) == 1);                          \
    unsigned diff_ = (unsigned)GET_UINT8(REGS.pc) - (unsigned)JSOp::IfEq; \
    if (diff_ <= 1) {                                                     \
      REGS.sp -= (spdec);                                                 \
      if ((cond) == (diff_ != 0)) {                                       \
        ++REGS.pc;                                                        \
        BRANCH(GET_JUMP_OFFSET(REGS.pc));                                 \
      }                                                                   \
      ADVANCE_AND_DISPATCH(1 + JSOpLength_IfEq);                          \
    }                                                                     \
  JS_END_MACRO

    CASE(In) {
      HandleValue rref = REGS.stackHandleAt(-1);
      if (!rref.isObject()) {
        HandleValue lref = REGS.stackHandleAt(-2);
        ReportInNotObjectError(cx, lref, -2, rref, -1);
        goto error;
      }
      bool found;
      {
        ReservedRooted<JSObject*> obj(&rootObject0, &rref.toObject());
        ReservedRooted<jsid> id(&rootId0);
        FETCH_ELEMENT_ID(-2, id);
        if (!HasProperty(cx, obj, id, &found)) {
          goto error;
        }
      }
      TRY_BRANCH_AFTER_COND(found, 2);
      REGS.sp--;
      REGS.sp[-1].setBoolean(found);
    }
    END_CASE(In)

    CASE(HasOwn) {
      HandleValue val = REGS.stackHandleAt(-1);
      HandleValue idval = REGS.stackHandleAt(-2);

      bool found;
      if (!HasOwnProperty(cx, val, idval, &found)) {
        goto error;
      }

      REGS.sp--;
      REGS.sp[-1].setBoolean(found);
    }
    END_CASE(HasOwn)

    CASE(Iter) {
      MOZ_ASSERT(REGS.stackDepth() >= 1);
      HandleValue val = REGS.stackHandleAt(-1);
      JSObject* iter = ValueToIterator(cx, val);
      if (!iter) {
        goto error;
      }
      REGS.sp[-1].setObject(*iter);
    }
    END_CASE(Iter)

    CASE(MoreIter) {
      MOZ_ASSERT(REGS.stackDepth() >= 1);
      MOZ_ASSERT(REGS.sp[-1].isObject());
      Value v = IteratorMore(&REGS.sp[-1].toObject());
      PUSH_COPY(v);
    }
    END_CASE(MoreIter)

    CASE(IsNoIter) {
      bool b = REGS.sp[-1].isMagic(JS_NO_ITER_VALUE);
      PUSH_BOOLEAN(b);
    }
    END_CASE(IsNoIter)

    CASE(EndIter) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);
      CloseIterator(&REGS.sp[-2].toObject());
      REGS.sp -= 2;
    }
    END_CASE(EndIter)

    CASE(IsGenClosing) {
      bool b = REGS.sp[-1].isMagic(JS_GENERATOR_CLOSING);
      PUSH_BOOLEAN(b);
    }
    END_CASE(IsGenClosing)

    CASE(IterNext) {
      // Ion relies on this.
      MOZ_ASSERT(REGS.sp[-1].isString());
    }
    END_CASE(IterNext)

    CASE(Dup) {
      MOZ_ASSERT(REGS.stackDepth() >= 1);
      const Value& rref = REGS.sp[-1];
      PUSH_COPY(rref);
    }
    END_CASE(Dup)

    CASE(Dup2) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);
      const Value& lref = REGS.sp[-2];
      const Value& rref = REGS.sp[-1];
      PUSH_COPY(lref);
      PUSH_COPY(rref);
    }
    END_CASE(Dup2)

    CASE(Swap) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);
      Value& lref = REGS.sp[-2];
      Value& rref = REGS.sp[-1];
      lref.swap(rref);
    }
    END_CASE(Swap)

    CASE(Pick) {
      unsigned i = GET_UINT8(REGS.pc);
      MOZ_ASSERT(REGS.stackDepth() >= i + 1);
      Value lval = REGS.sp[-int(i + 1)];
      memmove(REGS.sp - (i + 1), REGS.sp - i, sizeof(Value) * i);
      REGS.sp[-1] = lval;
    }
    END_CASE(Pick)

    CASE(Unpick) {
      int i = GET_UINT8(REGS.pc);
      MOZ_ASSERT(REGS.stackDepth() >= unsigned(i) + 1);
      Value lval = REGS.sp[-1];
      memmove(REGS.sp - i, REGS.sp - (i + 1), sizeof(Value) * i);
      REGS.sp[-(i + 1)] = lval;
    }
    END_CASE(Unpick)

    CASE(BindGName)
    CASE(BindName) {
      JSOp op = JSOp(*REGS.pc);
      ReservedRooted<JSObject*> envChain(&rootObject0);
      if (op == JSOp::BindName || script->hasNonSyntacticScope()) {
        envChain.set(REGS.fp()->environmentChain());
      } else {
        envChain.set(&REGS.fp()->global().lexicalEnvironment());
      }
      ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));

      // Assigning to an undeclared name adds a property to the global object.
      ReservedRooted<JSObject*> env(&rootObject1);
      if (!LookupNameUnqualified(cx, name, envChain, &env)) {
        goto error;
      }

      PUSH_OBJECT(*env);

      static_assert(JSOpLength_BindName == JSOpLength_BindGName,
                    "We're sharing the END_CASE so the lengths better match");
    }
    END_CASE(BindName)

    CASE(BindVar) {
      JSObject* varObj = BindVarOperation(cx, REGS.fp()->environmentChain());
      PUSH_OBJECT(*varObj);
    }
    END_CASE(BindVar)

    CASE(BitOr) {
      MutableHandleValue lhs = REGS.stackHandleAt(-2);
      MutableHandleValue rhs = REGS.stackHandleAt(-1);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!BitOr(cx, lhs, rhs, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(BitOr)

    CASE(BitXor) {
      MutableHandleValue lhs = REGS.stackHandleAt(-2);
      MutableHandleValue rhs = REGS.stackHandleAt(-1);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!BitXor(cx, lhs, rhs, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(BitXor)

    CASE(BitAnd) {
      MutableHandleValue lhs = REGS.stackHandleAt(-2);
      MutableHandleValue rhs = REGS.stackHandleAt(-1);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!BitAnd(cx, lhs, rhs, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(BitAnd)

    CASE(Eq) {
      if (!LooseEqualityOp<true>(cx, REGS)) {
        goto error;
      }
    }
    END_CASE(Eq)

    CASE(Ne) {
      if (!LooseEqualityOp<false>(cx, REGS)) {
        goto error;
      }
    }
    END_CASE(Ne)

#define STRICT_EQUALITY_OP(OP, COND)                  \
  JS_BEGIN_MACRO                                      \
    HandleValue lval = REGS.stackHandleAt(-2);        \
    HandleValue rval = REGS.stackHandleAt(-1);        \
    bool equal;                                       \
    if (!js::StrictlyEqual(cx, lval, rval, &equal)) { \
      goto error;                                     \
    }                                                 \
    (COND) = equal OP true;                           \
    REGS.sp--;                                        \
  JS_END_MACRO

    CASE(StrictEq) {
      bool cond;
      STRICT_EQUALITY_OP(==, cond);
      REGS.sp[-1].setBoolean(cond);
    }
    END_CASE(StrictEq)

    CASE(StrictNe) {
      bool cond;
      STRICT_EQUALITY_OP(!=, cond);
      REGS.sp[-1].setBoolean(cond);
    }
    END_CASE(StrictNe)

#undef STRICT_EQUALITY_OP

    CASE(Case) {
      bool cond = REGS.sp[-1].toBoolean();
      REGS.sp--;
      if (cond) {
        REGS.sp--;
        BRANCH(GET_JUMP_OFFSET(REGS.pc));
      }
    }
    END_CASE(Case)

    CASE(Lt) {
      bool cond;
      MutableHandleValue lval = REGS.stackHandleAt(-2);
      MutableHandleValue rval = REGS.stackHandleAt(-1);
      if (!LessThanOperation(cx, lval, rval, &cond)) {
        goto error;
      }
      TRY_BRANCH_AFTER_COND(cond, 2);
      REGS.sp[-2].setBoolean(cond);
      REGS.sp--;
    }
    END_CASE(Lt)

    CASE(Le) {
      bool cond;
      MutableHandleValue lval = REGS.stackHandleAt(-2);
      MutableHandleValue rval = REGS.stackHandleAt(-1);
      if (!LessThanOrEqualOperation(cx, lval, rval, &cond)) {
        goto error;
      }
      TRY_BRANCH_AFTER_COND(cond, 2);
      REGS.sp[-2].setBoolean(cond);
      REGS.sp--;
    }
    END_CASE(Le)

    CASE(Gt) {
      bool cond;
      MutableHandleValue lval = REGS.stackHandleAt(-2);
      MutableHandleValue rval = REGS.stackHandleAt(-1);
      if (!GreaterThanOperation(cx, lval, rval, &cond)) {
        goto error;
      }
      TRY_BRANCH_AFTER_COND(cond, 2);
      REGS.sp[-2].setBoolean(cond);
      REGS.sp--;
    }
    END_CASE(Gt)

    CASE(Ge) {
      bool cond;
      MutableHandleValue lval = REGS.stackHandleAt(-2);
      MutableHandleValue rval = REGS.stackHandleAt(-1);
      if (!GreaterThanOrEqualOperation(cx, lval, rval, &cond)) {
        goto error;
      }
      TRY_BRANCH_AFTER_COND(cond, 2);
      REGS.sp[-2].setBoolean(cond);
      REGS.sp--;
    }
    END_CASE(Ge)

    CASE(Lsh) {
      MutableHandleValue lhs = REGS.stackHandleAt(-2);
      MutableHandleValue rhs = REGS.stackHandleAt(-1);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!BitLsh(cx, lhs, rhs, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(Lsh)

    CASE(Rsh) {
      MutableHandleValue lhs = REGS.stackHandleAt(-2);
      MutableHandleValue rhs = REGS.stackHandleAt(-1);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!BitRsh(cx, lhs, rhs, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(Rsh)

    CASE(Ursh) {
      MutableHandleValue lhs = REGS.stackHandleAt(-2);
      MutableHandleValue rhs = REGS.stackHandleAt(-1);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!UrshOperation(cx, lhs, rhs, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(Ursh)

    CASE(Add) {
      MutableHandleValue lval = REGS.stackHandleAt(-2);
      MutableHandleValue rval = REGS.stackHandleAt(-1);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!AddOperation(cx, lval, rval, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(Add)

    CASE(Sub) {
      ReservedRooted<Value> lval(&rootValue0, REGS.sp[-2]);
      ReservedRooted<Value> rval(&rootValue1, REGS.sp[-1]);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!SubOperation(cx, &lval, &rval, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(Sub)

    CASE(Mul) {
      ReservedRooted<Value> lval(&rootValue0, REGS.sp[-2]);
      ReservedRooted<Value> rval(&rootValue1, REGS.sp[-1]);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!MulOperation(cx, &lval, &rval, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(Mul)

    CASE(Div) {
      ReservedRooted<Value> lval(&rootValue0, REGS.sp[-2]);
      ReservedRooted<Value> rval(&rootValue1, REGS.sp[-1]);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!DivOperation(cx, &lval, &rval, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(Div)

    CASE(Mod) {
      ReservedRooted<Value> lval(&rootValue0, REGS.sp[-2]);
      ReservedRooted<Value> rval(&rootValue1, REGS.sp[-1]);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!ModOperation(cx, &lval, &rval, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(Mod)

    CASE(Pow) {
      ReservedRooted<Value> lval(&rootValue0, REGS.sp[-2]);
      ReservedRooted<Value> rval(&rootValue1, REGS.sp[-1]);
      MutableHandleValue res = REGS.stackHandleAt(-2);
      if (!PowOperation(cx, &lval, &rval, res)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(Pow)

    CASE(Not) {
      bool cond = ToBoolean(REGS.stackHandleAt(-1));
      REGS.sp--;
      PUSH_BOOLEAN(!cond);
    }
    END_CASE(Not)

    CASE(BitNot) {
      MutableHandleValue value = REGS.stackHandleAt(-1);
      MutableHandleValue res = REGS.stackHandleAt(-1);
      if (!BitNot(cx, value, res)) {
        goto error;
      }
    }
    END_CASE(BitNot)

    CASE(Neg) {
      ReservedRooted<Value> val(&rootValue0, REGS.sp[-1]);
      MutableHandleValue res = REGS.stackHandleAt(-1);
      if (!NegOperation(cx, &val, res)) {
        goto error;
      }
    }
    END_CASE(Neg)

    CASE(Pos) {
      if (!ToNumber(cx, REGS.stackHandleAt(-1))) {
        goto error;
      }
    }
    END_CASE(Pos)

    CASE(DelName) {
      ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));
      ReservedRooted<JSObject*> envObj(&rootObject0,
                                       REGS.fp()->environmentChain());

      PUSH_BOOLEAN(true);
      MutableHandleValue res = REGS.stackHandleAt(-1);
      if (!DeleteNameOperation(cx, name, envObj, res)) {
        goto error;
      }
    }
    END_CASE(DelName)

    CASE(DelProp)
    CASE(StrictDelProp) {
      static_assert(JSOpLength_DelProp == JSOpLength_StrictDelProp,
                    "delprop and strictdelprop must be the same size");
      ReservedRooted<jsid> id(&rootId0, NameToId(script->getName(REGS.pc)));
      ReservedRooted<JSObject*> obj(&rootObject0);
      FETCH_OBJECT(cx, -1, obj, id);

      ObjectOpResult result;
      if (!DeleteProperty(cx, obj, id, result)) {
        goto error;
      }
      if (!result && JSOp(*REGS.pc) == JSOp::StrictDelProp) {
        result.reportError(cx, obj, id);
        goto error;
      }
      MutableHandleValue res = REGS.stackHandleAt(-1);
      res.setBoolean(result.ok());
    }
    END_CASE(DelProp)

    CASE(DelElem)
    CASE(StrictDelElem) {
      static_assert(JSOpLength_DelElem == JSOpLength_StrictDelElem,
                    "delelem and strictdelelem must be the same size");
      /* Fetch the left part and resolve it to a non-null object. */
      ReservedRooted<JSObject*> obj(&rootObject0);

      ReservedRooted<Value> propval(&rootValue0, REGS.sp[-1]);
      FETCH_OBJECT(cx, -2, obj, propval);

      ObjectOpResult result;
      ReservedRooted<jsid> id(&rootId0);
      if (!ToPropertyKey(cx, propval, &id)) {
        goto error;
      }
      if (!DeleteProperty(cx, obj, id, result)) {
        goto error;
      }
      if (!result && JSOp(*REGS.pc) == JSOp::StrictDelElem) {
        result.reportError(cx, obj, id);
        goto error;
      }

      MutableHandleValue res = REGS.stackHandleAt(-2);
      res.setBoolean(result.ok());
      REGS.sp--;
    }
    END_CASE(DelElem)

    CASE(ToId) {
      /*
       * Increment or decrement requires use to lookup the same property twice,
       * but we need to avoid the observable stringification the second time.
       * There must be an object value below the id, which will not be popped.
       */
      ReservedRooted<Value> idval(&rootValue1, REGS.sp[-1]);
      MutableHandleValue res = REGS.stackHandleAt(-1);
      if (!ToIdOperation(cx, idval, res)) {
        goto error;
      }
    }
    END_CASE(ToId)

    CASE(TypeofExpr)
    CASE(Typeof) {
      REGS.sp[-1].setString(TypeOfOperation(REGS.sp[-1], cx->runtime()));
    }
    END_CASE(Typeof)

    CASE(Void) { REGS.sp[-1].setUndefined(); }
    END_CASE(Void)

    CASE(FunctionThis) {
      PUSH_NULL();
      if (!GetFunctionThis(cx, REGS.fp(), REGS.stackHandleAt(-1))) {
        goto error;
      }
    }
    END_CASE(FunctionThis)

    CASE(GlobalThis) {
      if (script->hasNonSyntacticScope()) {
        PUSH_NULL();
        GetNonSyntacticGlobalThis(cx, REGS.fp()->environmentChain(),
                                  REGS.stackHandleAt(-1));
      } else {
        PUSH_COPY(cx->global()->lexicalEnvironment().thisValue());
      }
    }
    END_CASE(GlobalThis)

    CASE(CheckIsObj) {
      if (!REGS.sp[-1].isObject()) {
        MOZ_ALWAYS_FALSE(
            ThrowCheckIsObject(cx, CheckIsObjectKind(GET_UINT8(REGS.pc))));
        goto error;
      }
    }
    END_CASE(CheckIsObj)

    CASE(CheckIsCallable) {
      if (!IsCallable(REGS.sp[-1])) {
        MOZ_ALWAYS_FALSE(
            ThrowCheckIsCallable(cx, CheckIsCallableKind(GET_UINT8(REGS.pc))));
        goto error;
      }
    }
    END_CASE(CheckIsCallable)

    CASE(CheckThis) {
      if (REGS.sp[-1].isMagic(JS_UNINITIALIZED_LEXICAL)) {
        MOZ_ALWAYS_FALSE(ThrowUninitializedThis(cx));
        goto error;
      }
    }
    END_CASE(CheckThis)

    CASE(CheckThisReinit) {
      if (!REGS.sp[-1].isMagic(JS_UNINITIALIZED_LEXICAL)) {
        MOZ_ALWAYS_FALSE(ThrowInitializedThis(cx));
        goto error;
      }
    }
    END_CASE(CheckThisReinit)

    CASE(CheckReturn) {
      if (!REGS.fp()->checkReturn(cx, REGS.stackHandleAt(-1))) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(CheckReturn)

    CASE(GetProp)
    CASE(Length)
    CASE(CallProp) {
      MutableHandleValue lval = REGS.stackHandleAt(-1);
      if (!GetPropertyOperation(cx, REGS.fp(), script, REGS.pc, lval, lval)) {
        goto error;
      }

      JitScript::MonitorBytecodeType(cx, script, REGS.pc, lval);
      cx->debugOnlyCheck(lval);
    }
    END_CASE(GetProp)

    CASE(GetPropSuper) {
      ReservedRooted<Value> receiver(&rootValue0, REGS.sp[-2]);
      ReservedRooted<JSObject*> obj(&rootObject1, &REGS.sp[-1].toObject());
      MutableHandleValue rref = REGS.stackHandleAt(-2);

      if (!GetProperty(cx, obj, receiver, script->getName(REGS.pc), rref)) {
        goto error;
      }

      JitScript::MonitorBytecodeType(cx, script, REGS.pc, rref);
      cx->debugOnlyCheck(rref);

      REGS.sp--;
    }
    END_CASE(GetPropSuper)

    CASE(GetBoundName) {
      ReservedRooted<JSObject*> env(&rootObject0, &REGS.sp[-1].toObject());
      ReservedRooted<jsid> id(&rootId0, NameToId(script->getName(REGS.pc)));
      MutableHandleValue rval = REGS.stackHandleAt(-1);
      if (!GetNameBoundInEnvironment(cx, env, id, rval)) {
        goto error;
      }

      JitScript::MonitorBytecodeType(cx, script, REGS.pc, rval);
      cx->debugOnlyCheck(rval);
    }
    END_CASE(GetBoundName)

    CASE(SetIntrinsic) {
      HandleValue value = REGS.stackHandleAt(-1);

      if (!SetIntrinsicOperation(cx, script, REGS.pc, value)) {
        goto error;
      }
    }
    END_CASE(SetIntrinsic)

    CASE(SetGName)
    CASE(StrictSetGName)
    CASE(SetName)
    CASE(StrictSetName) {
      static_assert(JSOpLength_SetName == JSOpLength_StrictSetName,
                    "setname and strictsetname must be the same size");
      static_assert(JSOpLength_SetGName == JSOpLength_StrictSetGName,
                    "setganem adn strictsetgname must be the same size");
      static_assert(JSOpLength_SetName == JSOpLength_SetGName,
                    "We're sharing the END_CASE so the lengths better match");

      ReservedRooted<JSObject*> env(&rootObject0, &REGS.sp[-2].toObject());
      HandleValue value = REGS.stackHandleAt(-1);

      if (!SetNameOperation(cx, script, REGS.pc, env, value)) {
        goto error;
      }

      REGS.sp[-2] = REGS.sp[-1];
      REGS.sp--;
    }
    END_CASE(SetName)

    CASE(SetProp)
    CASE(StrictSetProp) {
      static_assert(JSOpLength_SetProp == JSOpLength_StrictSetProp,
                    "setprop and strictsetprop must be the same size");
      int lvalIndex = -2;
      HandleValue lval = REGS.stackHandleAt(lvalIndex);
      HandleValue rval = REGS.stackHandleAt(-1);

      ReservedRooted<jsid> id(&rootId0, NameToId(script->getName(REGS.pc)));
      if (!SetPropertyOperation(cx, JSOp(*REGS.pc), lval, lvalIndex, id,
                                rval)) {
        goto error;
      }

      REGS.sp[-2] = REGS.sp[-1];
      REGS.sp--;
    }
    END_CASE(SetProp)

    CASE(SetPropSuper)
    CASE(StrictSetPropSuper) {
      static_assert(
          JSOpLength_SetPropSuper == JSOpLength_StrictSetPropSuper,
          "setprop-super and strictsetprop-super must be the same size");

      ReservedRooted<Value> receiver(&rootValue0, REGS.sp[-3]);
      ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-2].toObject());
      ReservedRooted<Value> rval(&rootValue1, REGS.sp[-1]);
      ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));

      bool strict = JSOp(*REGS.pc) == JSOp::StrictSetPropSuper;

      if (!SetPropertySuper(cx, obj, receiver, name, rval, strict)) {
        goto error;
      }

      REGS.sp[-3] = REGS.sp[-1];
      REGS.sp -= 2;
    }
    END_CASE(SetPropSuper)

    CASE(GetElem)
    CASE(CallElem) {
      int lvalIndex = -2;
      MutableHandleValue lval = REGS.stackHandleAt(lvalIndex);
      HandleValue rval = REGS.stackHandleAt(-1);
      MutableHandleValue res = REGS.stackHandleAt(-2);

      bool done = false;
      if (!GetElemOptimizedArguments(cx, REGS.fp(), lval, rval, res, &done)) {
        goto error;
      }

      if (!done) {
        if (!GetElementOperationWithStackIndex(cx, JSOp(*REGS.pc), lval,
                                               lvalIndex, rval, res)) {
          goto error;
        }
      }

      JitScript::MonitorBytecodeType(cx, script, REGS.pc, res);
      REGS.sp--;
    }
    END_CASE(GetElem)

    CASE(GetElemSuper) {
      ReservedRooted<Value> receiver(&rootValue1, REGS.sp[-3]);
      ReservedRooted<Value> rval(&rootValue0, REGS.sp[-2]);
      ReservedRooted<JSObject*> obj(&rootObject1, &REGS.sp[-1].toObject());

      MutableHandleValue res = REGS.stackHandleAt(-3);

      // Since we have asserted that obj has to be an object, it cannot be
      // either optimized arguments, or indeed any primitive. This simplifies
      // our task some.
      if (!GetObjectElementOperation(cx, JSOp(*REGS.pc), obj, receiver, rval,
                                     res)) {
        goto error;
      }

      JitScript::MonitorBytecodeType(cx, script, REGS.pc, res);
      REGS.sp -= 2;
    }
    END_CASE(GetElemSuper)

    CASE(SetElem)
    CASE(StrictSetElem) {
      static_assert(JSOpLength_SetElem == JSOpLength_StrictSetElem,
                    "setelem and strictsetelem must be the same size");
      int receiverIndex = -3;
      HandleValue receiver = REGS.stackHandleAt(receiverIndex);
      ReservedRooted<JSObject*> obj(&rootObject0);
      obj = ToObjectFromStackForPropertyAccess(cx, receiver, receiverIndex,
                                               REGS.stackHandleAt(-2));
      if (!obj) {
        goto error;
      }
      ReservedRooted<jsid> id(&rootId0);
      FETCH_ELEMENT_ID(-2, id);
      HandleValue value = REGS.stackHandleAt(-1);
      if (!SetObjectElementOperation(cx, obj, id, value, receiver,
                                     JSOp(*REGS.pc) == JSOp::StrictSetElem)) {
        goto error;
      }
      REGS.sp[-3] = value;
      REGS.sp -= 2;
    }
    END_CASE(SetElem)

    CASE(SetElemSuper)
    CASE(StrictSetElemSuper) {
      static_assert(
          JSOpLength_SetElemSuper == JSOpLength_StrictSetElemSuper,
          "setelem-super and strictsetelem-super must be the same size");

      ReservedRooted<Value> receiver(&rootValue0, REGS.sp[-4]);
      ReservedRooted<Value> index(&rootValue1, REGS.sp[-3]);
      ReservedRooted<JSObject*> obj(&rootObject1, &REGS.sp[-2].toObject());
      HandleValue value = REGS.stackHandleAt(-1);

      bool strict = JSOp(*REGS.pc) == JSOp::StrictSetElemSuper;
      if (!SetObjectElementWithReceiver(cx, obj, index, value, receiver,
                                        strict)) {
        goto error;
      }
      REGS.sp[-4] = value;
      REGS.sp -= 3;
    }
    END_CASE(SetElemSuper)

    CASE(Eval)
    CASE(StrictEval) {
      static_assert(JSOpLength_Eval == JSOpLength_StrictEval,
                    "eval and stricteval must be the same size");

      CallArgs args = CallArgsFromSp(GET_ARGC(REGS.pc), REGS.sp);
      if (cx->global()->valueIsEval(args.calleev())) {
        if (!DirectEval(cx, args.get(0), args.rval())) {
          goto error;
        }
      } else {
        if (!CallFromStack(cx, args)) {
          goto error;
        }
      }

      REGS.sp = args.spAfterCall();
      JitScript::MonitorBytecodeType(cx, script, REGS.pc, REGS.sp[-1]);
    }
    END_CASE(Eval)

    CASE(SpreadNew)
    CASE(SpreadCall)
    CASE(SpreadSuperCall) {
      if (REGS.fp()->hasPushedGeckoProfilerFrame()) {
        cx->geckoProfiler().updatePC(cx, script, REGS.pc);
      }
      /* FALL THROUGH */
    }

    CASE(SpreadEval)
    CASE(StrictSpreadEval) {
      static_assert(JSOpLength_SpreadEval == JSOpLength_StrictSpreadEval,
                    "spreadeval and strictspreadeval must be the same size");
      bool construct = JSOp(*REGS.pc) == JSOp::SpreadNew ||
                       JSOp(*REGS.pc) == JSOp::SpreadSuperCall;
      ;

      MOZ_ASSERT(REGS.stackDepth() >= 3u + construct);

      HandleValue callee = REGS.stackHandleAt(-3 - construct);
      HandleValue thisv = REGS.stackHandleAt(-2 - construct);
      HandleValue arr = REGS.stackHandleAt(-1 - construct);
      MutableHandleValue ret = REGS.stackHandleAt(-3 - construct);

      RootedValue& newTarget = rootValue0;
      if (construct) {
        newTarget = REGS.sp[-1];
      } else {
        newTarget = NullValue();
      }

      if (!SpreadCallOperation(cx, script, REGS.pc, thisv, callee, arr,
                               newTarget, ret)) {
        goto error;
      }

      REGS.sp -= 2 + construct;
    }
    END_CASE(SpreadCall)

    CASE(FunApply) {
      CallArgs args = CallArgsFromSp(GET_ARGC(REGS.pc), REGS.sp);
      if (!GuardFunApplyArgumentsOptimization(cx, REGS.fp(), args)) {
        goto error;
      }
      /* FALL THROUGH */
    }

    CASE(New)
    CASE(Call)
    CASE(CallIgnoresRv)
    CASE(CallIter)
    CASE(SuperCall)
    CASE(FunCall) {
      static_assert(JSOpLength_Call == JSOpLength_New,
                    "call and new must be the same size");
      static_assert(JSOpLength_Call == JSOpLength_CallIgnoresRv,
                    "call and call-ignores-rv must be the same size");
      static_assert(JSOpLength_Call == JSOpLength_CallIter,
                    "call and calliter must be the same size");
      static_assert(JSOpLength_Call == JSOpLength_SuperCall,
                    "call and supercall must be the same size");
      static_assert(JSOpLength_Call == JSOpLength_FunCall,
                    "call and funcall must be the same size");
      static_assert(JSOpLength_Call == JSOpLength_FunApply,
                    "call and funapply must be the same size");

      if (REGS.fp()->hasPushedGeckoProfilerFrame()) {
        cx->geckoProfiler().updatePC(cx, script, REGS.pc);
      }

      MaybeConstruct construct = MaybeConstruct(
          JSOp(*REGS.pc) == JSOp::New || JSOp(*REGS.pc) == JSOp::SuperCall);
      bool ignoresReturnValue = JSOp(*REGS.pc) == JSOp::CallIgnoresRv;
      unsigned argStackSlots = GET_ARGC(REGS.pc) + construct;

      MOZ_ASSERT(REGS.stackDepth() >= 2u + GET_ARGC(REGS.pc));
      CallArgs args =
          CallArgsFromSp(argStackSlots, REGS.sp, construct, ignoresReturnValue);

      JSFunction* maybeFun;
      bool isFunction = IsFunctionObject(args.calleev(), &maybeFun);

      // Use the slow path if the callee is not an interpreted function, if we
      // have to throw an exception, or if we might have to invoke the
      // OnNativeCall hook for a self-hosted builtin.
      if (!isFunction || !maybeFun->isInterpreted() ||
          (construct && !maybeFun->isConstructor()) ||
          (!construct && maybeFun->isClassConstructor()) ||
          cx->insideDebuggerEvaluationWithOnNativeCallHook) {
        if (construct) {
          if (!ConstructFromStack(cx, args)) {
            goto error;
          }
        } else {
          if (JSOp(*REGS.pc) == JSOp::CallIter &&
              args.calleev().isPrimitive()) {
            MOZ_ASSERT(args.length() == 0, "thisv must be on top of the stack");
            ReportValueError(cx, JSMSG_NOT_ITERABLE, -1, args.thisv(), nullptr);
            goto error;
          }
          if (!CallFromStack(cx, args)) {
            goto error;
          }
        }
        Value* newsp = args.spAfterCall();
        JitScript::MonitorBytecodeType(cx, script, REGS.pc, newsp[-1]);
        REGS.sp = newsp;
        ADVANCE_AND_DISPATCH(JSOpLength_Call);
      }

      {
        MOZ_ASSERT(maybeFun);
        ReservedRooted<JSFunction*> fun(&rootFunction0, maybeFun);
        ReservedRooted<JSScript*> funScript(
            &rootScript0, JSFunction::getOrCreateScript(cx, fun));
        if (!funScript) {
          goto error;
        }

        // Enter the callee's realm if this is a cross-realm call. Use
        // MakeScopeExit to leave this realm on all error/JIT-return paths
        // below.
        const bool isCrossRealm = cx->realm() != funScript->realm();
        if (isCrossRealm) {
          cx->enterRealmOf(funScript);
        }
        auto leaveRealmGuard =
            mozilla::MakeScopeExit([isCrossRealm, cx, &script] {
              if (isCrossRealm) {
                cx->leaveRealm(script->realm());
              }
            });

        if (construct) {
          bool createSingleton =
              ObjectGroup::useSingletonForNewObject(cx, script, REGS.pc);
          if (!MaybeCreateThisForConstructor(cx, args, createSingleton)) {
            goto error;
          }
        }

        TypeMonitorCall(cx, args, construct);

        {
          InvokeState state(cx, args, construct);

          jit::EnterJitStatus status = jit::MaybeEnterJit(cx, state);
          switch (status) {
            case jit::EnterJitStatus::Error:
              goto error;
            case jit::EnterJitStatus::Ok:
              interpReturnOK = true;
              CHECK_BRANCH();
              REGS.sp = args.spAfterCall();
              goto jit_return;
            case jit::EnterJitStatus::NotEntered:
              break;
          }
        }

        funScript = fun->nonLazyScript();

        if (!activation.pushInlineFrame(args, funScript, construct)) {
          goto error;
        }
        leaveRealmGuard.release();  // We leave the callee's realm when we call
                                    // popInlineFrame.
      }

      SET_SCRIPT(REGS.fp()->script());

      {
        TraceLoggerEvent event(TraceLogger_Scripts, script);
        TraceLogStartEvent(logger, event);
        TraceLogStartEvent(logger, TraceLogger_Interpreter);
      }

      if (!REGS.fp()->prologue(cx)) {
        goto prologue_error;
      }

      if (!DebugAPI::onEnterFrame(cx, REGS.fp())) {
        goto error;
      }

      // Increment the coverage for the main entry point.
      INIT_COVERAGE();
      COUNT_COVERAGE_MAIN();

      /* Load first op and dispatch it (safe since JSOp::RetRval). */
      ADVANCE_AND_DISPATCH(0);
    }

    CASE(OptimizeSpreadCall) {
      ReservedRooted<Value> val(&rootValue0, REGS.sp[-1]);

      bool optimized = false;
      if (!OptimizeSpreadCall(cx, val, &optimized)) {
        goto error;
      }

      PUSH_BOOLEAN(optimized);
    }
    END_CASE(OptimizeSpreadCall)

    CASE(ThrowMsg) {
      MOZ_ALWAYS_FALSE(ThrowMsgOperation(cx, GET_UINT16(REGS.pc)));
      goto error;
    }
    END_CASE(ThrowMsg)

    CASE(ImplicitThis)
    CASE(GImplicitThis) {
      JSOp op = JSOp(*REGS.pc);
      if (op == JSOp::ImplicitThis || script->hasNonSyntacticScope()) {
        ReservedRooted<PropertyName*> name(&rootName0,
                                           script->getName(REGS.pc));
        ReservedRooted<JSObject*> envObj(&rootObject0,
                                         REGS.fp()->environmentChain());
        ReservedRooted<JSObject*> env(&rootObject1);
        if (!LookupNameWithGlobalDefault(cx, name, envObj, &env)) {
          goto error;
        }

        Value v = ComputeImplicitThis(env);
        PUSH_COPY(v);
      } else {
        // Treat it like JSOp::Undefined.
        PUSH_UNDEFINED();
      }
      static_assert(JSOpLength_ImplicitThis == JSOpLength_GImplicitThis,
                    "We're sharing the END_CASE so the lengths better match");
    }
    END_CASE(ImplicitThis)

    CASE(GetGName)
    CASE(GetName) {
      ReservedRooted<Value> rval(&rootValue0);
      if (!GetNameOperation(cx, REGS.fp(), REGS.pc, &rval)) {
        goto error;
      }

      PUSH_COPY(rval);
      JitScript::MonitorBytecodeType(cx, script, REGS.pc, rval);
      static_assert(JSOpLength_GetName == JSOpLength_GetGName,
                    "We're sharing the END_CASE so the lengths better match");
    }
    END_CASE(GetName)

    CASE(GetImport) {
      PUSH_NULL();
      MutableHandleValue rval = REGS.stackHandleAt(-1);
      HandleObject envChain = REGS.fp()->environmentChain();
      if (!GetImportOperation(cx, envChain, script, REGS.pc, rval)) {
        goto error;
      }

      JitScript::MonitorBytecodeType(cx, script, REGS.pc, rval);
    }
    END_CASE(GetImport)

    CASE(GetIntrinsic) {
      ReservedRooted<Value> rval(&rootValue0);
      if (!GetIntrinsicOperation(cx, script, REGS.pc, &rval)) {
        goto error;
      }

      PUSH_COPY(rval);
      JitScript::MonitorBytecodeType(cx, script, REGS.pc, rval);
    }
    END_CASE(GetIntrinsic)

    CASE(Uint16) { PUSH_INT32((int32_t)GET_UINT16(REGS.pc)); }
    END_CASE(Uint16)

    CASE(Uint24)
    CASE(ResumeIndex) { PUSH_INT32((int32_t)GET_UINT24(REGS.pc)); }
    END_CASE(Uint24)

    CASE(Int8) { PUSH_INT32(GET_INT8(REGS.pc)); }
    END_CASE(Int8)

    CASE(Int32) { PUSH_INT32(GET_INT32(REGS.pc)); }
    END_CASE(Int32)

    CASE(Double) { PUSH_COPY(GET_INLINE_VALUE(REGS.pc)); }
    END_CASE(Double)

    CASE(String) { PUSH_STRING(script->getAtom(REGS.pc)); }
    END_CASE(String)

    CASE(ToString) {
      MutableHandleValue oper = REGS.stackHandleAt(-1);

      if (!oper.isString()) {
        JSString* operString = ToString<CanGC>(cx, oper);
        if (!operString) {
          goto error;
        }
        oper.setString(operString);
      }
    }
    END_CASE(ToString)

    CASE(Symbol) {
      PUSH_SYMBOL(cx->wellKnownSymbols().get(GET_UINT8(REGS.pc)));
    }
    END_CASE(Symbol)

    CASE(Object) {
      JSObject* obj = SingletonObjectLiteralOperation(cx, script, REGS.pc);
      if (!obj) {
        goto error;
      }
      PUSH_OBJECT(*obj);
    }
    END_CASE(Object)

    CASE(CallSiteObj) {
      JSObject* cso = ProcessCallSiteObjOperation(cx, script, REGS.pc);
      if (!cso) {
        goto error;
      }
      PUSH_OBJECT(*cso);
    }
    END_CASE(CallSiteObj)

    CASE(RegExp) {
      /*
       * Push a regexp object cloned from the regexp literal object mapped by
       * the bytecode at pc.
       */
      ReservedRooted<JSObject*> re(&rootObject0, script->getRegExp(REGS.pc));
      JSObject* obj = CloneRegExpObject(cx, re.as<RegExpObject>());
      if (!obj) {
        goto error;
      }
      PUSH_OBJECT(*obj);
    }
    END_CASE(RegExp)

    CASE(Zero) { PUSH_INT32(0); }
    END_CASE(Zero)

    CASE(One) { PUSH_INT32(1); }
    END_CASE(One)

    CASE(Null) { PUSH_NULL(); }
    END_CASE(Null)

    CASE(False) { PUSH_BOOLEAN(false); }
    END_CASE(False)

    CASE(True) { PUSH_BOOLEAN(true); }
    END_CASE(True)

    CASE(TableSwitch) {
      jsbytecode* pc2 = REGS.pc;
      int32_t len = GET_JUMP_OFFSET(pc2);

      /*
       * ECMAv2+ forbids conversion of discriminant, so we will skip to the
       * default case if the discriminant isn't already an int jsval.  (This
       * opcode is emitted only for dense int-domain switches.)
       */
      const Value& rref = *--REGS.sp;
      int32_t i;
      if (rref.isInt32()) {
        i = rref.toInt32();
      } else {
        /* Use mozilla::NumberEqualsInt32 to treat -0 (double) as 0. */
        if (!rref.isDouble() || !NumberEqualsInt32(rref.toDouble(), &i)) {
          ADVANCE_AND_DISPATCH(len);
        }
      }

      pc2 += JUMP_OFFSET_LEN;
      int32_t low = GET_JUMP_OFFSET(pc2);
      pc2 += JUMP_OFFSET_LEN;
      int32_t high = GET_JUMP_OFFSET(pc2);

      i = uint32_t(i) - uint32_t(low);
      if (uint32_t(i) < uint32_t(high - low + 1)) {
        len = script->tableSwitchCaseOffset(REGS.pc, uint32_t(i)) -
              script->pcToOffset(REGS.pc);
      }
      ADVANCE_AND_DISPATCH(len);
    }

    CASE(Arguments) {
      if (!script->ensureHasAnalyzedArgsUsage(cx)) {
        goto error;
      }
      if (script->needsArgsObj()) {
        ArgumentsObject* obj = ArgumentsObject::createExpected(cx, REGS.fp());
        if (!obj) {
          goto error;
        }
        PUSH_COPY(ObjectValue(*obj));
      } else {
        PUSH_COPY(MagicValue(JS_OPTIMIZED_ARGUMENTS));
      }
    }
    END_CASE(Arguments)

    CASE(Rest) {
      ReservedRooted<JSObject*> rest(&rootObject0,
                                     REGS.fp()->createRestParameter(cx));
      if (!rest) {
        goto error;
      }
      PUSH_COPY(ObjectValue(*rest));
    }
    END_CASE(Rest)

    CASE(GetAliasedVar) {
      EnvironmentCoordinate ec = EnvironmentCoordinate(REGS.pc);
      ReservedRooted<Value> val(
          &rootValue0, REGS.fp()->aliasedEnvironment(ec).aliasedBinding(ec));
#ifdef DEBUG
      // Only the .this slot can hold the TDZ MagicValue.
      if (IsUninitializedLexical(val)) {
        PropertyName* name = EnvironmentCoordinateNameSlow(script, REGS.pc);
        MOZ_ASSERT(name == cx->names().dotThis);
        JSOp next = JSOp(*GetNextPc(REGS.pc));
        MOZ_ASSERT(next == JSOp::CheckThis || next == JSOp::CheckReturn ||
                   next == JSOp::CheckThisReinit);
      }
#endif
      PUSH_COPY(val);
      JitScript::MonitorBytecodeType(cx, script, REGS.pc, REGS.sp[-1]);
    }
    END_CASE(GetAliasedVar)

    CASE(SetAliasedVar) {
      EnvironmentCoordinate ec = EnvironmentCoordinate(REGS.pc);
      EnvironmentObject& obj = REGS.fp()->aliasedEnvironment(ec);
      SetAliasedVarOperation(cx, script, REGS.pc, obj, ec, REGS.sp[-1],
                             CheckTDZ);
    }
    END_CASE(SetAliasedVar)

    CASE(ThrowSetConst)
    CASE(ThrowSetAliasedConst)
    CASE(ThrowSetCallee) {
      ReportRuntimeConstAssignment(cx, script, REGS.pc);
      goto error;
    }
    END_CASE(ThrowSetConst)

    CASE(CheckLexical) {
      uint32_t i = GET_LOCALNO(REGS.pc);
      ReservedRooted<Value> val(&rootValue0, REGS.fp()->unaliasedLocal(i));
      if (!CheckUninitializedLexical(cx, script, REGS.pc, val)) {
        goto error;
      }
    }
    END_CASE(CheckLexical)

    CASE(InitLexical) {
      uint32_t i = GET_LOCALNO(REGS.pc);
      REGS.fp()->unaliasedLocal(i) = REGS.sp[-1];
    }
    END_CASE(InitLexical)

    CASE(CheckAliasedLexical) {
      EnvironmentCoordinate ec = EnvironmentCoordinate(REGS.pc);
      ReservedRooted<Value> val(
          &rootValue0, REGS.fp()->aliasedEnvironment(ec).aliasedBinding(ec));
      if (!CheckUninitializedLexical(cx, script, REGS.pc, val)) {
        goto error;
      }
    }
    END_CASE(CheckAliasedLexical)

    CASE(InitAliasedLexical) {
      EnvironmentCoordinate ec = EnvironmentCoordinate(REGS.pc);
      EnvironmentObject& obj = REGS.fp()->aliasedEnvironment(ec);
      SetAliasedVarOperation(cx, script, REGS.pc, obj, ec, REGS.sp[-1],
                             DontCheckTDZ);
    }
    END_CASE(InitAliasedLexical)

    CASE(InitGLexical) {
      LexicalEnvironmentObject* lexicalEnv;
      if (script->hasNonSyntacticScope()) {
        lexicalEnv = &REGS.fp()->extensibleLexicalEnvironment();
      } else {
        lexicalEnv = &cx->global()->lexicalEnvironment();
      }
      HandleValue value = REGS.stackHandleAt(-1);
      InitGlobalLexicalOperation(cx, lexicalEnv, script, REGS.pc, value);
    }
    END_CASE(InitGLexical)

    CASE(Uninitialized) { PUSH_MAGIC(JS_UNINITIALIZED_LEXICAL); }
    END_CASE(Uninitialized)

    CASE(GetArg) {
      unsigned i = GET_ARGNO(REGS.pc);
      if (script->argsObjAliasesFormals()) {
        PUSH_COPY(REGS.fp()->argsObj().arg(i));
      } else {
        PUSH_COPY(REGS.fp()->unaliasedFormal(i));
      }
    }
    END_CASE(GetArg)

    CASE(SetArg) {
      unsigned i = GET_ARGNO(REGS.pc);
      if (script->argsObjAliasesFormals()) {
        REGS.fp()->argsObj().setArg(i, REGS.sp[-1]);
      } else {
        REGS.fp()->unaliasedFormal(i) = REGS.sp[-1];
      }
    }
    END_CASE(SetArg)

    CASE(GetLocal) {
      uint32_t i = GET_LOCALNO(REGS.pc);
      PUSH_COPY_SKIP_CHECK(REGS.fp()->unaliasedLocal(i));

#ifdef DEBUG
      // Derived class constructors store the TDZ Value in the .this slot
      // before a super() call.
      if (IsUninitializedLexical(REGS.sp[-1])) {
        MOZ_ASSERT(script->isDerivedClassConstructor());
        JSOp next = JSOp(*GetNextPc(REGS.pc));
        MOZ_ASSERT(next == JSOp::CheckThis || next == JSOp::CheckReturn ||
                   next == JSOp::CheckThisReinit);
      }
#endif

      /*
       * Skip the same-compartment assertion if the local will be immediately
       * popped. We do not guarantee sync for dead locals when coming in from
       * the method JIT, and a GetLocal followed by Pop is not considered to be
       * a use of the variable.
       */
      if (JSOp(REGS.pc[JSOpLength_GetLocal]) != JSOp::Pop) {
        cx->debugOnlyCheck(REGS.sp[-1]);
      }
    }
    END_CASE(GetLocal)

    CASE(SetLocal) {
      uint32_t i = GET_LOCALNO(REGS.pc);

      MOZ_ASSERT(!IsUninitializedLexical(REGS.fp()->unaliasedLocal(i)));

      REGS.fp()->unaliasedLocal(i) = REGS.sp[-1];
    }
    END_CASE(SetLocal)

    CASE(DefVar) {
      HandleObject env = REGS.fp()->environmentChain();
      if (!DefVarOperation(cx, env, script, REGS.pc)) {
        goto error;
      }
    }
    END_CASE(DefVar)

    CASE(DefConst)
    CASE(DefLet) {
      HandleObject env = REGS.fp()->environmentChain();
      if (!DefLexicalOperation(cx, env, script, REGS.pc)) {
        goto error;
      }
    }
    END_CASE(DefLet)

    CASE(DefFun) {
      /*
       * A top-level function defined in Global or Eval code (see ECMA-262
       * Ed. 3), or else a SpiderMonkey extension: a named function statement in
       * a compound statement (not at the top statement level of global code, or
       * at the top level of a function body).
       */
      ReservedRooted<JSFunction*> fun(&rootFunction0,
                                      &REGS.sp[-1].toObject().as<JSFunction>());
      if (!DefFunOperation(cx, script, REGS.fp()->environmentChain(), fun)) {
        goto error;
      }
      REGS.sp--;
    }
    END_CASE(DefFun)

    CASE(Lambda) {
      /* Load the specified function object literal. */
      ReservedRooted<JSFunction*> fun(
          &rootFunction0, script->getFunction(GET_UINT32_INDEX(REGS.pc)));
      JSObject* obj = Lambda(cx, fun, REGS.fp()->environmentChain());
      if (!obj) {
        goto error;
      }

      MOZ_ASSERT(obj->staticPrototype());
      PUSH_OBJECT(*obj);
    }
    END_CASE(Lambda)

    CASE(LambdaArrow) {
      /* Load the specified function object literal. */
      ReservedRooted<JSFunction*> fun(
          &rootFunction0, script->getFunction(GET_UINT32_INDEX(REGS.pc)));
      ReservedRooted<Value> newTarget(&rootValue1, REGS.sp[-1]);
      JSObject* obj =
          LambdaArrow(cx, fun, REGS.fp()->environmentChain(), newTarget);
      if (!obj) {
        goto error;
      }

      MOZ_ASSERT(obj->staticPrototype());
      REGS.sp[-1].setObject(*obj);
    }
    END_CASE(LambdaArrow)

    CASE(ToAsyncIter) {
      ReservedRooted<Value> nextMethod(&rootValue0, REGS.sp[-1]);
      ReservedRooted<JSObject*> iter(&rootObject1, &REGS.sp[-2].toObject());
      JSObject* asyncIter = CreateAsyncFromSyncIterator(cx, iter, nextMethod);
      if (!asyncIter) {
        goto error;
      }

      REGS.sp--;
      REGS.sp[-1].setObject(*asyncIter);
    }
    END_CASE(ToAsyncIter)

    CASE(TrySkipAwait) {
      ReservedRooted<Value> val(&rootValue0, REGS.sp[-1]);
      ReservedRooted<Value> resolved(&rootValue1);
      bool canSkip;

      if (!TrySkipAwait(cx, val, &canSkip, &resolved)) {
        goto error;
      }

      if (canSkip) {
        REGS.sp[-1] = resolved;
        PUSH_BOOLEAN(true);
      } else {
        PUSH_BOOLEAN(false);
      }
    }
    END_CASE(TrySkipAwait)

    CASE(AsyncAwait) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);
      ReservedRooted<JSObject*> gen(&rootObject1, &REGS.sp[-1].toObject());
      ReservedRooted<Value> value(&rootValue0, REGS.sp[-2]);
      JSObject* promise =
          AsyncFunctionAwait(cx, gen.as<AsyncFunctionGeneratorObject>(), value);
      if (!promise) {
        goto error;
      }

      REGS.sp--;
      REGS.sp[-1].setObject(*promise);
    }
    END_CASE(AsyncAwait)

    CASE(AsyncResolve) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);
      auto resolveKind = AsyncFunctionResolveKind(GET_UINT8(REGS.pc));
      ReservedRooted<JSObject*> gen(&rootObject1, &REGS.sp[-1].toObject());
      ReservedRooted<Value> valueOrReason(&rootValue0, REGS.sp[-2]);
      JSObject* promise =
          AsyncFunctionResolve(cx, gen.as<AsyncFunctionGeneratorObject>(),
                               valueOrReason, resolveKind);
      if (!promise) {
        goto error;
      }

      REGS.sp--;
      REGS.sp[-1].setObject(*promise);
    }
    END_CASE(AsyncResolve)

    CASE(SetFunName) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);
      FunctionPrefixKind prefixKind = FunctionPrefixKind(GET_UINT8(REGS.pc));
      ReservedRooted<Value> name(&rootValue0, REGS.sp[-1]);
      ReservedRooted<JSFunction*> fun(&rootFunction0,
                                      &REGS.sp[-2].toObject().as<JSFunction>());
      if (!SetFunctionName(cx, fun, name, prefixKind)) {
        goto error;
      }

      REGS.sp--;
    }
    END_CASE(SetFunName)

    CASE(Callee) {
      MOZ_ASSERT(REGS.fp()->isFunctionFrame());
      PUSH_COPY(REGS.fp()->calleev());
    }
    END_CASE(Callee)

    CASE(InitPropGetter)
    CASE(InitHiddenPropGetter)
    CASE(InitPropSetter)
    CASE(InitHiddenPropSetter) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);

      ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-2].toObject());
      ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));
      ReservedRooted<JSObject*> val(&rootObject1, &REGS.sp[-1].toObject());

      if (!InitPropGetterSetterOperation(cx, REGS.pc, obj, name, val)) {
        goto error;
      }

      REGS.sp--;
    }
    END_CASE(InitPropGetter)

    CASE(InitElemGetter)
    CASE(InitHiddenElemGetter)
    CASE(InitElemSetter)
    CASE(InitHiddenElemSetter) {
      MOZ_ASSERT(REGS.stackDepth() >= 3);

      ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-3].toObject());
      ReservedRooted<Value> idval(&rootValue0, REGS.sp[-2]);
      ReservedRooted<JSObject*> val(&rootObject1, &REGS.sp[-1].toObject());

      if (!InitElemGetterSetterOperation(cx, REGS.pc, obj, idval, val)) {
        goto error;
      }

      REGS.sp -= 2;
    }
    END_CASE(InitElemGetter)

    CASE(Hole) { PUSH_MAGIC(JS_ELEMENTS_HOLE); }
    END_CASE(Hole)

    CASE(NewInit) {
      JSObject* obj = NewObjectOperation(cx, script, REGS.pc);

      if (!obj) {
        goto error;
      }
      PUSH_OBJECT(*obj);
    }
    END_CASE(NewInit)

    CASE(NewArray) {
      uint32_t length = GET_UINT32(REGS.pc);
      JSObject* obj = NewArrayOperation(cx, script, REGS.pc, length);
      if (!obj) {
        goto error;
      }
      PUSH_OBJECT(*obj);
    }
    END_CASE(NewArray)

    CASE(NewArrayCopyOnWrite) {
      JSObject* obj = NewArrayCopyOnWriteOperation(cx, script, REGS.pc);
      if (!obj) {
        goto error;
      }

      PUSH_OBJECT(*obj);
    }
    END_CASE(NewArrayCopyOnWrite)

    CASE(NewObject)
    CASE(NewObjectWithGroup) {
      JSObject* obj = NewObjectOperation(cx, script, REGS.pc);
      if (!obj) {
        goto error;
      }
      PUSH_OBJECT(*obj);
    }
    END_CASE(NewObject)

    CASE(MutateProto) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);

      if (REGS.sp[-1].isObjectOrNull()) {
        ReservedRooted<JSObject*> newProto(&rootObject1,
                                           REGS.sp[-1].toObjectOrNull());
        ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-2].toObject());
        MOZ_ASSERT(obj->is<PlainObject>());

        if (!SetPrototype(cx, obj, newProto)) {
          goto error;
        }
      }

      REGS.sp--;
    }
    END_CASE(MutateProto)

    CASE(InitProp)
    CASE(InitLockedProp)
    CASE(InitHiddenProp) {
      static_assert(JSOpLength_InitProp == JSOpLength_InitLockedProp,
                    "initprop and initlockedprop must be the same size");
      static_assert(JSOpLength_InitProp == JSOpLength_InitHiddenProp,
                    "initprop and inithiddenprop must be the same size");
      /* Load the property's initial value into rval. */
      MOZ_ASSERT(REGS.stackDepth() >= 2);
      ReservedRooted<Value> rval(&rootValue0, REGS.sp[-1]);

      /* Load the object being initialized into lval/obj. */
      ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-2].toObject());

      ReservedRooted<PropertyName*> name(&rootName0, script->getName(REGS.pc));

      if (!InitPropertyOperation(cx, JSOp(*REGS.pc), obj, name, rval)) {
        goto error;
      }

      REGS.sp--;
    }
    END_CASE(InitProp)

    CASE(InitElem)
    CASE(InitHiddenElem) {
      MOZ_ASSERT(REGS.stackDepth() >= 3);
      HandleValue val = REGS.stackHandleAt(-1);
      HandleValue id = REGS.stackHandleAt(-2);

      ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-3].toObject());

      if (!InitElemOperation(cx, REGS.pc, obj, id, val)) {
        goto error;
      }

      REGS.sp -= 2;
    }
    END_CASE(InitElem)

    CASE(InitElemArray) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);
      HandleValue val = REGS.stackHandleAt(-1);

      ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-2].toObject());

      uint32_t index = GET_UINT32(REGS.pc);
      if (!InitArrayElemOperation(cx, REGS.pc, obj, index, val)) {
        goto error;
      }

      REGS.sp--;
    }
    END_CASE(InitElemArray)

    CASE(InitElemInc) {
      MOZ_ASSERT(REGS.stackDepth() >= 3);
      HandleValue val = REGS.stackHandleAt(-1);

      ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-3].toObject());

      uint32_t index = REGS.sp[-2].toInt32();
      if (!InitArrayElemOperation(cx, REGS.pc, obj, index, val)) {
        goto error;
      }

      REGS.sp[-2].setInt32(index + 1);
      REGS.sp--;
    }
    END_CASE(InitElemInc)

    CASE(Gosub) {
      int32_t len = GET_JUMP_OFFSET(REGS.pc);
      ADVANCE_AND_DISPATCH(len);
    }

    CASE(Retsub) {
      /* Pop [exception or hole, retsub pc-index]. */
      Value rval, lval;
      POP_COPY_TO(rval);
      POP_COPY_TO(lval);
      MOZ_ASSERT(lval.isBoolean());
      if (lval.toBoolean()) {
        /*
         * Exception was pending during finally, throw it *before* we adjust
         * pc, because pc indexes into script->trynotes.  This turns out not to
         * be necessary, but it seems clearer.  And it points out a FIXME:
         * 350509, due to Igor Bukanov.
         */
        ReservedRooted<Value> v(&rootValue0, rval);
        cx->setPendingExceptionAndCaptureStack(v);
        goto error;
      }

      MOZ_ASSERT(rval.toInt32() >= 0);

      uint32_t offset = script->resumeOffsets()[rval.toInt32()];
      REGS.pc = script->offsetToPC(offset);
      ADVANCE_AND_DISPATCH(0);
    }

    CASE(Exception) {
      PUSH_NULL();
      MutableHandleValue res = REGS.stackHandleAt(-1);
      if (!GetAndClearException(cx, res)) {
        goto error;
      }
    }
    END_CASE(Exception)

    CASE(Finally) { CHECK_BRANCH(); }
    END_CASE(Finally)

    CASE(Throw) {
      CHECK_BRANCH();
      ReservedRooted<Value> v(&rootValue0);
      POP_COPY_TO(v);
      MOZ_ALWAYS_FALSE(ThrowOperation(cx, v));
      /* let the code at error try to catch the exception. */
      goto error;
    }

    CASE(Instanceof) {
      ReservedRooted<Value> rref(&rootValue0, REGS.sp[-1]);
      if (HandleValue(rref).isPrimitive()) {
        ReportValueError(cx, JSMSG_BAD_INSTANCEOF_RHS, -1, rref, nullptr);
        goto error;
      }
      ReservedRooted<JSObject*> obj(&rootObject0, &rref.toObject());
      bool cond = false;
      if (!HasInstance(cx, obj, REGS.stackHandleAt(-2), &cond)) {
        goto error;
      }
      REGS.sp--;
      REGS.sp[-1].setBoolean(cond);
    }
    END_CASE(Instanceof)

    CASE(Debugger) {
      if (!DebugAPI::onDebuggerStatement(cx, REGS.fp())) {
        goto error;
      }
    }
    END_CASE(Debugger)

    CASE(PushLexicalEnv) {
      ReservedRooted<Scope*> scope(&rootScope0, script->getScope(REGS.pc));

      // Create block environment and push on scope chain.
      if (!REGS.fp()->pushLexicalEnvironment(cx, scope.as<LexicalScope>())) {
        goto error;
      }
    }
    END_CASE(PushLexicalEnv)

    CASE(PopLexicalEnv) {
#ifdef DEBUG
      // Pop block from scope chain.
      Scope* scope = script->lookupScope(REGS.pc);
      MOZ_ASSERT(scope);
      MOZ_ASSERT(scope->is<LexicalScope>());
      MOZ_ASSERT(scope->as<LexicalScope>().hasEnvironment());
#endif

      if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
        DebugEnvironments::onPopLexical(cx, REGS.fp(), REGS.pc);
      }

      // Pop block from scope chain.
      REGS.fp()->popOffEnvironmentChain<LexicalEnvironmentObject>();
    }
    END_CASE(PopLexicalEnv)

    CASE(DebugLeaveLexicalEnv) {
      MOZ_ASSERT(script->lookupScope(REGS.pc));
      MOZ_ASSERT(script->lookupScope(REGS.pc)->is<LexicalScope>());
      MOZ_ASSERT(
          !script->lookupScope(REGS.pc)->as<LexicalScope>().hasEnvironment());

      // FIXME: This opcode should not be necessary.  The debugger shouldn't
      // need help from bytecode to do its job.  See bug 927782.

      if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
        DebugEnvironments::onPopLexical(cx, REGS.fp(), REGS.pc);
      }
    }
    END_CASE(DebugLeaveLexicalEnv)

    CASE(FreshenLexicalEnv) {
      if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
        DebugEnvironments::onPopLexical(cx, REGS.fp(), REGS.pc);
      }

      if (!REGS.fp()->freshenLexicalEnvironment(cx)) {
        goto error;
      }
    }
    END_CASE(FreshenLexicalEnv)

    CASE(RecreateLexicalEnv) {
      if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
        DebugEnvironments::onPopLexical(cx, REGS.fp(), REGS.pc);
      }

      if (!REGS.fp()->recreateLexicalEnvironment(cx)) {
        goto error;
      }
    }
    END_CASE(RecreateLexicalEnv)

    CASE(PushVarEnv) {
      ReservedRooted<Scope*> scope(&rootScope0, script->getScope(REGS.pc));

      if (!REGS.fp()->pushVarEnvironment(cx, scope)) {
        goto error;
      }
    }
    END_CASE(PushVarEnv)

    CASE(PopVarEnv) {
#ifdef DEBUG
      Scope* scope = script->lookupScope(REGS.pc);
      MOZ_ASSERT(scope);
      MOZ_ASSERT(scope->is<VarScope>());
      MOZ_ASSERT(scope->as<VarScope>().hasEnvironment());
#endif

      if (MOZ_UNLIKELY(cx->realm()->isDebuggee())) {
        DebugEnvironments::onPopVar(cx, REGS.fp(), REGS.pc);
      }

      REGS.fp()->popOffEnvironmentChain<VarEnvironmentObject>();
    }
    END_CASE(PopVarEnv)

    CASE(Generator) {
      MOZ_ASSERT(!cx->isExceptionPending());
      MOZ_ASSERT(REGS.stackDepth() == 0);
      JSObject* obj = AbstractGeneratorObject::create(cx, REGS.fp());
      if (!obj) {
        goto error;
      }
      PUSH_OBJECT(*obj);
    }
    END_CASE(Generator)

    CASE(InitialYield) {
      MOZ_ASSERT(!cx->isExceptionPending());
      MOZ_ASSERT(REGS.fp()->isFunctionFrame());
      ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-1].toObject());
      POP_RETURN_VALUE();
      MOZ_ASSERT(REGS.stackDepth() == 0);
      if (!AbstractGeneratorObject::initialSuspend(cx, obj, REGS.fp(),
                                                   REGS.pc)) {
        goto error;
      }
      goto successful_return_continuation;
    }

    CASE(Yield)
    CASE(Await) {
      MOZ_ASSERT(!cx->isExceptionPending());
      MOZ_ASSERT(REGS.fp()->isFunctionFrame());
      ReservedRooted<JSObject*> obj(&rootObject0, &REGS.sp[-1].toObject());
      if (!AbstractGeneratorObject::normalSuspend(cx, obj, REGS.fp(), REGS.pc,
                                                  REGS.spForStackDepth(0),
                                                  REGS.stackDepth() - 2)) {
        goto error;
      }

      REGS.sp--;
      POP_RETURN_VALUE();

      goto successful_return_continuation;
    }

    CASE(ResumeKind) {
      GeneratorResumeKind resumeKind = ResumeKindFromPC(REGS.pc);
      PUSH_INT32(int32_t(resumeKind));
    }
    END_CASE(ResumeKind)

    CASE(CheckResumeKind) {
      int32_t kindInt = REGS.sp[-1].toInt32();
      GeneratorResumeKind resumeKind = IntToResumeKind(kindInt);
      if (MOZ_UNLIKELY(resumeKind != GeneratorResumeKind::Next)) {
        ReservedRooted<Value> val(&rootValue0, REGS.sp[-3]);
        Rooted<AbstractGeneratorObject*> gen(
            cx, &REGS.sp[-2].toObject().as<AbstractGeneratorObject>());
        MOZ_ALWAYS_FALSE(GeneratorThrowOrReturn(cx, activation.regs().fp(), gen,
                                                val, resumeKind));
        goto error;
      }
      REGS.sp -= 2;
    }
    END_CASE(CheckResumeKind)

    CASE(Resume) {
      {
        Rooted<AbstractGeneratorObject*> gen(
            cx, &REGS.sp[-3].toObject().as<AbstractGeneratorObject>());
        ReservedRooted<Value> val(&rootValue0, REGS.sp[-2]);
        ReservedRooted<Value> resumeKindVal(&rootValue1, REGS.sp[-1]);

        // popInlineFrame expects there to be an additional value on the stack
        // to pop off, so leave "gen" on the stack.
        REGS.sp -= 1;

        if (!AbstractGeneratorObject::resume(cx, activation, gen, val,
                                             resumeKindVal)) {
          goto error;
        }

        JSScript* generatorScript = REGS.fp()->script();
        if (cx->realm() != generatorScript->realm()) {
          cx->enterRealmOf(generatorScript);
        }
        SET_SCRIPT(generatorScript);

        TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx);
        TraceLoggerEvent scriptEvent(TraceLogger_Scripts, script);
        TraceLogStartEvent(logger, scriptEvent);
        TraceLogStartEvent(logger, TraceLogger_Interpreter);

        if (!DebugAPI::onResumeFrame(cx, REGS.fp())) {
          if (cx->isPropagatingForcedReturn()) {
            MOZ_ASSERT_IF(
                REGS.fp()
                    ->callee()
                    .isGenerator(),  // as opposed to an async function
                gen->isClosed());
          }
          goto error;
        }
      }
      ADVANCE_AND_DISPATCH(0);
    }

    CASE(AfterYield) {
      // AbstractGeneratorObject::resume takes care of setting the frame's
      // debuggee flag.
      MOZ_ASSERT_IF(REGS.fp()->script()->isDebuggee(), REGS.fp()->isDebuggee());
      COUNT_COVERAGE();
    }
    END_CASE(AfterYield)

    CASE(FinalYieldRval) {
      ReservedRooted<JSObject*> gen(&rootObject0, &REGS.sp[-1].toObject());
      REGS.sp--;
      AbstractGeneratorObject::finalSuspend(gen);
      goto successful_return_continuation;
    }

    CASE(CheckClassHeritage) {
      HandleValue heritage = REGS.stackHandleAt(-1);

      if (!CheckClassHeritageOperation(cx, heritage)) {
        goto error;
      }
    }
    END_CASE(CheckClassHeritage)

    CASE(BuiltinProto) {
      JSObject* builtin = BuiltinProtoOperation(cx, REGS.pc);
      if (!builtin) {
        goto error;
      }
      PUSH_OBJECT(*builtin);
    }
    END_CASE(BuiltinProto)

    CASE(FunWithProto) {
      ReservedRooted<JSObject*> proto(&rootObject1, &REGS.sp[-1].toObject());

      /* Load the specified function object literal. */
      ReservedRooted<JSFunction*> fun(
          &rootFunction0, script->getFunction(GET_UINT32_INDEX(REGS.pc)));

      JSObject* obj =
          FunWithProtoOperation(cx, fun, REGS.fp()->environmentChain(), proto);
      if (!obj) {
        goto error;
      }

      REGS.sp[-1].setObject(*obj);
    }
    END_CASE(FunWithProto)

    CASE(ObjWithProto) {
      JSObject* obj = ObjectWithProtoOperation(cx, REGS.stackHandleAt(-1));
      if (!obj) {
        goto error;
      }

      REGS.sp[-1].setObject(*obj);
    }
    END_CASE(ObjWithProto)

    CASE(InitHomeObject) {
      MOZ_ASSERT(REGS.stackDepth() >= 2);

      /* Load the function to be initialized */
      JSFunction* func = &REGS.sp[-2].toObject().as<JSFunction>();
      MOZ_ASSERT(func->allowSuperProperty());

      /* Load the home object */
      JSObject* obj = &REGS.sp[-1].toObject();
      MOZ_ASSERT(obj->is<PlainObject>() || obj->is<JSFunction>());

      func->setExtendedSlot(FunctionExtended::METHOD_HOMEOBJECT_SLOT,
                            ObjectValue(*obj));
      REGS.sp--;
    }
    END_CASE(InitHomeObject)

    CASE(SuperBase) {
      JSFunction& superEnvFunc = REGS.sp[-1].toObject().as<JSFunction>();
      MOZ_ASSERT(superEnvFunc.allowSuperProperty());
      MOZ_ASSERT(superEnvFunc.baseScript()->needsHomeObject());
      const Value& homeObjVal = superEnvFunc.getExtendedSlot(
          FunctionExtended::METHOD_HOMEOBJECT_SLOT);

      ReservedRooted<JSObject*> homeObj(&rootObject0, &homeObjVal.toObject());
      JSObject* superBase = HomeObjectSuperBase(cx, homeObj);
      if (!superBase) {
        goto error;
      }

      REGS.sp[-1].setObject(*superBase);
    }
    END_CASE(SuperBase)

    CASE(NewTarget) {
      PUSH_COPY(REGS.fp()->newTarget());
      MOZ_ASSERT(REGS.sp[-1].isObject() || REGS.sp[-1].isUndefined());
    }
    END_CASE(NewTarget)

    CASE(ImportMeta) {
      JSObject* metaObject = ImportMetaOperation(cx, script);
      if (!metaObject) {
        goto error;
      }

      PUSH_OBJECT(*metaObject);
    }
    END_CASE(ImportMeta)

    CASE(DynamicImport) {
      ReservedRooted<Value> specifier(&rootValue1);
      POP_COPY_TO(specifier);

      JSObject* promise = StartDynamicModuleImport(cx, script, specifier);
      if (!promise) goto error;

      PUSH_OBJECT(*promise);
    }
    END_CASE(DynamicImport)

    CASE(EnvCallee) {
      uint8_t numHops = GET_UINT8(REGS.pc);
      JSObject* env = &REGS.fp()->environmentChain()->as<EnvironmentObject>();
      for (unsigned i = 0; i < numHops; i++) {
        env = &env->as<EnvironmentObject>().enclosingEnvironment();
      }
      PUSH_OBJECT(env->as<CallObject>().callee());
    }
    END_CASE(EnvCallee)

    CASE(SuperFun) {
      JSObject* superEnvFunc = &REGS.sp[-1].toObject();
      JSObject* superFun = SuperFunOperation(superEnvFunc);
      REGS.sp[-1].setObjectOrNull(superFun);
    }
    END_CASE(SuperFun)

    CASE(DerivedConstructor) {
      MOZ_ASSERT(REGS.sp[-1].isObject());
      ReservedRooted<JSObject*> proto(&rootObject0, &REGS.sp[-1].toObject());

      JSFunction* constructor =
          MakeDefaultConstructor(cx, script, REGS.pc, proto);
      if (!constructor) {
        goto error;
      }

      REGS.sp[-1].setObject(*constructor);
    }
    END_CASE(DerivedConstructor)

    CASE(ClassConstructor) {
      JSFunction* constructor =
          MakeDefaultConstructor(cx, script, REGS.pc, nullptr);
      if (!constructor) {
        goto error;
      }
      PUSH_OBJECT(*constructor);
    }
    END_CASE(ClassConstructor)

    CASE(CheckObjCoercible) {
      ReservedRooted<Value> checkVal(&rootValue0, REGS.sp[-1]);
      if (checkVal.isNullOrUndefined() && !ToObjectFromStack(cx, checkVal)) {
        goto error;
      }
    }
    END_CASE(CheckObjCoercible)

    CASE(DebugCheckSelfHosted) {
#ifdef DEBUG
      ReservedRooted<Value> checkVal(&rootValue0, REGS.sp[-1]);
      if (!Debug_CheckSelfHosted(cx, checkVal)) {
        goto error;
      }
#endif
    }
    END_CASE(DebugCheckSelfHosted)

    CASE(IsConstructing) { PUSH_MAGIC(JS_IS_CONSTRUCTING); }
    END_CASE(IsConstructing)

    CASE(Inc) {
      ReservedRooted<Value> val(&rootValue0, REGS.sp[-1]);
      MutableHandleValue res = REGS.stackHandleAt(-1);
      if (!IncOperation(cx, &val, res)) {
        goto error;
      }
    }
    END_CASE(Inc)

    CASE(Dec) {
      ReservedRooted<Value> val(&rootValue0, REGS.sp[-1]);
      MutableHandleValue res = REGS.stackHandleAt(-1);
      if (!DecOperation(cx, &val, res)) {
        goto error;
      }
    }
    END_CASE(Dec)

    CASE(ToNumeric) {
      if (!ToNumeric(cx, REGS.stackHandleAt(-1))) {
        goto error;
      }
    }
    END_CASE(ToNumeric)

    CASE(BigInt) { PUSH_BIGINT(script->getBigInt(REGS.pc)); }
    END_CASE(BigInt)

    CASE(InstrumentationActive) {
      ReservedRooted<Value> rval(&rootValue0);
      if (!InstrumentationActiveOperation(cx, &rval)) {
        goto error;
      }
      PUSH_COPY(rval);
    }
    END_CASE(InstrumentationActive)

    CASE(InstrumentationCallback) {
      JSObject* obj = InstrumentationCallbackOperation(cx);
      MOZ_ASSERT(obj);
      PUSH_OBJECT(*obj);
    }
    END_CASE(InstrumentationCallback)

    CASE(InstrumentationScriptId) {
      ReservedRooted<Value> rval(&rootValue0);
      if (!InstrumentationScriptIdOperation(cx, script, &rval)) {
        goto error;
      }
      PUSH_COPY(rval);
    }
    END_CASE(InstrumentationScriptId)

    DEFAULT() {
      char numBuf[12];
      SprintfLiteral(numBuf, "%d", *REGS.pc);
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_BAD_BYTECODE, numBuf);
      goto error;
    }

  } /* interpreter loop */

  MOZ_CRASH("Interpreter loop exited via fallthrough");

error:
  switch (HandleError(cx, REGS)) {
    case SuccessfulReturnContinuation:
      goto successful_return_continuation;

    case ErrorReturnContinuation:
      interpReturnOK = false;
      goto return_continuation;

    case CatchContinuation:
      ADVANCE_AND_DISPATCH(0);

    case FinallyContinuation: {
      /*
       * Push (true, exception) pair for finally to indicate that [retsub]
       * should rethrow the exception.
       */
      ReservedRooted<Value> exception(&rootValue0);
      if (!cx->getPendingException(&exception)) {
        interpReturnOK = false;
        goto return_continuation;
      }
      PUSH_BOOLEAN(true);
      PUSH_COPY(exception);
      cx->clearPendingException();
    }
      ADVANCE_AND_DISPATCH(0);
  }

  MOZ_CRASH("Invalid HandleError continuation");

exit:
  if (MOZ_LIKELY(!frameHalfInitialized)) {
    interpReturnOK =
        DebugAPI::onLeaveFrame(cx, REGS.fp(), REGS.pc, interpReturnOK);

    REGS.fp()->epilogue(cx, REGS.pc);
  }

  gc::MaybeVerifyBarriers(cx, true);

  TraceLogStopEvent(logger, TraceLogger_Engine);
  TraceLogStopEvent(logger, scriptEvent);

  /*
   * This path is used when it's guaranteed the method can be finished
   * inside the JIT.
   */
leave_on_safe_point:

  if (interpReturnOK) {
    state.setReturnValue(activation.entryFrame()->returnValue());
  }

  return interpReturnOK;

prologue_error:
  interpReturnOK = false;
  frameHalfInitialized = true;
  goto prologue_return_continuation;
}

bool js::ThrowOperation(JSContext* cx, HandleValue v) {
  MOZ_ASSERT(!cx->isExceptionPending());
  cx->setPendingExceptionAndCaptureStack(v);
  return false;
}

bool js::GetProperty(JSContext* cx, HandleValue v, HandlePropertyName name,
                     MutableHandleValue vp) {
  if (name == cx->names().length) {
    // Fast path for strings, arrays and arguments.
    if (GetLengthProperty(v, vp)) {
      return true;
    }
  }

  // Optimize common cases like (2).toString() or "foo".valueOf() to not
  // create a wrapper object.
  if (v.isPrimitive() && !v.isNullOrUndefined()) {
    JSObject* proto;

    switch (v.type()) {
      case ValueType::Double:
      case ValueType::Int32:
        proto = GlobalObject::getOrCreateNumberPrototype(cx, cx->global());
        break;
      case ValueType::Boolean:
        proto = GlobalObject::getOrCreateBooleanPrototype(cx, cx->global());
        break;
      case ValueType::String:
        proto = GlobalObject::getOrCreateStringPrototype(cx, cx->global());
        break;
      case ValueType::Symbol:
        proto = GlobalObject::getOrCreateSymbolPrototype(cx, cx->global());
        break;
      case ValueType::BigInt:
        proto = GlobalObject::getOrCreateBigIntPrototype(cx, cx->global());
        break;
      case ValueType::Undefined:
      case ValueType::Null:
      case ValueType::Magic:
      case ValueType::PrivateGCThing:
      case ValueType::Object:
        MOZ_CRASH("unexpected type");
    }

    if (!proto) {
      return false;
    }

    if (GetPropertyPure(cx, proto, NameToId(name), vp.address())) {
      return true;
    }
  }

  RootedValue receiver(cx, v);
  RootedObject obj(
      cx, ToObjectFromStackForPropertyAccess(cx, v, JSDVG_SEARCH_STACK, name));
  if (!obj) {
    return false;
  }

  return GetProperty(cx, obj, receiver, name, vp);
}

bool js::GetValueProperty(JSContext* cx, HandleValue value,
                          HandlePropertyName name, MutableHandleValue vp) {
  return GetProperty(cx, value, name, vp);
}

JSObject* js::Lambda(JSContext* cx, HandleFunction fun, HandleObject parent) {
  MOZ_ASSERT(!fun->isArrow());

  JSFunction* clone;
  if (fun->isNative()) {
    MOZ_ASSERT(IsAsmJSModule(fun));
    clone = CloneAsmJSModuleFunction(cx, fun);
  } else {
    clone = CloneFunctionObjectIfNotSingleton(cx, fun, parent);
  }
  if (!clone) {
    return nullptr;
  }

  MOZ_ASSERT(fun->global() == clone->global());
  return clone;
}

JSObject* js::LambdaArrow(JSContext* cx, HandleFunction fun,
                          HandleObject parent, HandleValue newTargetv) {
  MOZ_ASSERT(fun->isArrow());

  JSFunction* clone = CloneFunctionObjectIfNotSingleton(cx, fun, parent);
  if (!clone) {
    return nullptr;
  }

  MOZ_ASSERT(clone->isArrow());
  clone->setExtendedSlot(0, newTargetv);

  MOZ_ASSERT(fun->global() == clone->global());
  return clone;
}

JSObject* js::BindVarOperation(JSContext* cx, JSObject* envChain) {
  // Note: BindVarOperation has an unused cx argument because the JIT callVM
  // machinery requires this.
  return &GetVariablesObject(envChain);
}

bool js::DefVarOperation(JSContext* cx, HandleObject envChain,
                         HandleScript script, jsbytecode* pc) {
  MOZ_ASSERT(JSOp(*pc) == JSOp::DefVar);

  RootedObject varobj(cx, &GetVariablesObject(envChain));
  MOZ_ASSERT(varobj->isQualifiedVarObj());

  RootedPropertyName name(cx, script->getName(pc));

  unsigned attrs = JSPROP_ENUMERATE;
  if (!script->isForEval()) {
    attrs |= JSPROP_PERMANENT;
  }

#ifdef DEBUG
  // Per spec, it is an error to redeclare a lexical binding. This should
  // have already been checked.
  if (JS_HasExtensibleLexicalEnvironment(varobj)) {
    Rooted<LexicalEnvironmentObject*> lexicalEnv(cx);
    lexicalEnv = &JS_ExtensibleLexicalEnvironment(varobj)
                      ->as<LexicalEnvironmentObject>();
    MOZ_ASSERT(CheckVarNameConflict(cx, lexicalEnv, name));
  }
#endif

  Rooted<PropertyResult> prop(cx);
  RootedObject obj2(cx);
  if (!LookupProperty(cx, varobj, name, &obj2, &prop)) {
    return false;
  }

  /* Steps 8c, 8d. */
  if (!prop || (obj2 != varobj && varobj->is<GlobalObject>())) {
    if (!DefineDataProperty(cx, varobj, name, UndefinedHandleValue, attrs)) {
      return false;
    }
  }

  if (varobj->is<GlobalObject>()) {
    if (!varobj->as<GlobalObject>().realm()->addToVarNames(cx, name)) {
      return false;
    }
  }

  return true;
}

bool js::DefLexicalOperation(JSContext* cx, HandleObject envChain,
                             HandleScript script, jsbytecode* pc) {
  MOZ_ASSERT(JSOp(*pc) == JSOp::DefLet || JSOp(*pc) == JSOp::DefConst);

  unsigned attrs = JSPROP_ENUMERATE | JSPROP_PERMANENT;
  if (JSOp(*pc) == JSOp::DefConst) {
    attrs |= JSPROP_READONLY;
  }

  Rooted<LexicalEnvironmentObject*> lexicalEnv(cx);
  if (script->hasNonSyntacticScope()) {
    lexicalEnv = &NearestEnclosingExtensibleLexicalEnvironment(envChain);
  } else {
    lexicalEnv = &cx->global()->lexicalEnvironment();
  }

#ifdef DEBUG
  RootedObject varObj(cx);
  if (script->hasNonSyntacticScope()) {
    varObj = &GetVariablesObject(envChain);
  } else {
    varObj = cx->global();
  }

  MOZ_ASSERT_IF(!script->hasNonSyntacticScope(),
                lexicalEnv == &cx->global()->lexicalEnvironment() &&
                    varObj == cx->global());

  // Redeclaration checks should have already been done.
  RootedPropertyName name(cx, script->getName(pc));
  MOZ_ASSERT(CheckLexicalNameConflict(cx, lexicalEnv, varObj, name));
#endif

  RootedId id(cx, NameToId(script->getName(pc)));
  RootedValue uninitialized(cx, MagicValue(JS_UNINITIALIZED_LEXICAL));
  return NativeDefineDataProperty(cx, lexicalEnv, id, uninitialized, attrs);
}

bool js::DefFunOperation(JSContext* cx, HandleScript script,
                         HandleObject envChain, HandleFunction fun) {
  /*
   * We define the function as a property of the variable object and not the
   * current scope chain even for the case of function expression statements
   * and functions defined by eval inside let or with blocks.
   */
  RootedObject parent(cx, envChain);
  while (!parent->isQualifiedVarObj()) {
    parent = parent->enclosingEnvironment();
  }

  /* ES5 10.5 (NB: with subsequent errata). */
  RootedPropertyName name(cx, fun->explicitName()->asPropertyName());

  Rooted<PropertyResult> prop(cx);
  RootedObject pobj(cx);
  if (!LookupProperty(cx, parent, name, &pobj, &prop)) {
    return false;
  }

  RootedValue rval(cx, ObjectValue(*fun));

  /*
   * ECMA requires functions defined when entering Eval code to be
   * impermanent.
   */
  unsigned attrs = script->isForEval() ? JSPROP_ENUMERATE
                                       : JSPROP_ENUMERATE | JSPROP_PERMANENT;

  /* Steps 5d, 5f. */
  if (!prop || pobj != parent) {
    if (!DefineDataProperty(cx, parent, name, rval, attrs)) {
      return false;
    }

    if (parent->is<GlobalObject>()) {
      return parent->as<GlobalObject>().realm()->addToVarNames(cx, name);
    }

    return true;
  }

  /*
   * Step 5e.
   *
   * A DebugEnvironmentProxy is okay here, and sometimes necessary. If
   * Debugger.Frame.prototype.eval defines a function with the same name as an
   * extant variable in the frame, the DebugEnvironmentProxy takes care of
   * storing the function in the stack frame (for non-aliased variables) or on
   * the scope object (for aliased).
   */
  MOZ_ASSERT(parent->isNative() || parent->is<DebugEnvironmentProxy>());
  if (parent->is<GlobalObject>()) {
    Shape* shape = prop.shape();
    if (shape->configurable()) {
      if (!DefineDataProperty(cx, parent, name, rval, attrs)) {
        return false;
      }
    } else {
      MOZ_ASSERT(shape->isDataDescriptor());
      MOZ_ASSERT(shape->writable());
      MOZ_ASSERT(shape->enumerable());
    }

    // Careful: the presence of a shape, even one appearing to derive from
    // a variable declaration, doesn't mean it's in [[VarNames]].
    if (!parent->as<GlobalObject>().realm()->addToVarNames(cx, name)) {
      return false;
    }
  }

  /*
   * Non-global properties, and global properties which we aren't simply
   * redefining, must be set.  First, this preserves their attributes.
   * Second, this will produce warnings and/or errors as necessary if the
   * specified Call object property is not writable (const).
   */

  /* Step 5f. */
  RootedId id(cx, NameToId(name));
  return PutProperty(cx, parent, id, rval, script->strict());
}

JSObject* js::SingletonObjectLiteralOperation(JSContext* cx,
                                              HandleScript script,
                                              jsbytecode* pc) {
  MOZ_ASSERT(JSOp(*pc) == JSOp::Object);

  RootedObject obj(cx, script->getObject(pc));
  if (cx->realm()->creationOptions().cloneSingletons()) {
    return DeepCloneObjectLiteral(cx, obj, TenuredObject);
  }

  cx->realm()->behaviors().setSingletonsAsValues();
  return obj;
}

JSObject* js::ImportMetaOperation(JSContext* cx, HandleScript script) {
  RootedObject module(cx, GetModuleObjectForScript(script));
  MOZ_ASSERT(module);
  return GetOrCreateModuleMetaObject(cx, module);
}

JSObject* js::BuiltinProtoOperation(JSContext* cx, jsbytecode* pc) {
  MOZ_ASSERT(JSOp(*pc) == JSOp::BuiltinProto);
  MOZ_ASSERT(GET_UINT8(pc) < JSProto_LIMIT);

  JSProtoKey key = static_cast<JSProtoKey>(GET_UINT8(pc));
  return GlobalObject::getOrCreatePrototype(cx, key);
}

bool js::ThrowMsgOperation(JSContext* cx, const unsigned errorNum) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, errorNum);
  return false;
}

bool js::GetAndClearExceptionAndStack(JSContext* cx, MutableHandleValue res,
                                      MutableHandleSavedFrame stack) {
  if (!cx->getPendingException(res)) {
    return false;
  }
  stack.set(cx->getPendingExceptionStack());
  cx->clearPendingException();

  // Allow interrupting deeply nested exception handling.
  return CheckForInterrupt(cx);
}

bool js::GetAndClearException(JSContext* cx, MutableHandleValue res) {
  RootedSavedFrame stack(cx);
  return GetAndClearExceptionAndStack(cx, res, &stack);
}

template <bool strict>
bool js::DeletePropertyJit(JSContext* cx, HandleValue v,
                           HandlePropertyName name, bool* bp) {
  RootedObject obj(
      cx, ToObjectFromStackForPropertyAccess(cx, v, JSDVG_SEARCH_STACK, name));
  if (!obj) {
    return false;
  }

  RootedId id(cx, NameToId(name));
  ObjectOpResult result;
  if (!DeleteProperty(cx, obj, id, result)) {
    return false;
  }

  if (strict) {
    if (!result) {
      return result.reportError(cx, obj, id);
    }
    *bp = true;
  } else {
    *bp = result.ok();
  }
  return true;
}

template bool js::DeletePropertyJit<true>(JSContext* cx, HandleValue val,
                                          HandlePropertyName name, bool* bp);
template bool js::DeletePropertyJit<false>(JSContext* cx, HandleValue val,
                                           HandlePropertyName name, bool* bp);

template <bool strict>
bool js::DeleteElementJit(JSContext* cx, HandleValue val, HandleValue index,
                          bool* bp) {
  RootedObject obj(cx, ToObjectFromStackForPropertyAccess(
                           cx, val, JSDVG_SEARCH_STACK, index));
  if (!obj) {
    return false;
  }

  RootedId id(cx);
  if (!ToPropertyKey(cx, index, &id)) {
    return false;
  }
  ObjectOpResult result;
  if (!DeleteProperty(cx, obj, id, result)) {
    return false;
  }

  if (strict) {
    if (!result) {
      return result.reportError(cx, obj, id);
    }
    *bp = true;
  } else {
    *bp = result.ok();
  }
  return true;
}

template bool js::DeleteElementJit<true>(JSContext*, HandleValue, HandleValue,
                                         bool* succeeded);
template bool js::DeleteElementJit<false>(JSContext*, HandleValue, HandleValue,
                                          bool* succeeded);

bool js::SetObjectElement(JSContext* cx, HandleObject obj, HandleValue index,
                          HandleValue value, bool strict) {
  RootedId id(cx);
  if (!ToPropertyKey(cx, index, &id)) {
    return false;
  }
  RootedValue receiver(cx, ObjectValue(*obj));
  return SetObjectElementOperation(cx, obj, id, value, receiver, strict);
}

bool js::SetObjectElementWithReceiver(JSContext* cx, HandleObject obj,
                                      HandleValue index, HandleValue value,
                                      HandleValue receiver, bool strict) {
  RootedId id(cx);
  if (!ToPropertyKey(cx, index, &id)) {
    return false;
  }
  return SetObjectElementOperation(cx, obj, id, value, receiver, strict);
}

bool js::SetObjectElement(JSContext* cx, HandleObject obj, HandleValue index,
                          HandleValue value, HandleValue receiver, bool strict,
                          HandleScript script, jsbytecode* pc) {
  MOZ_ASSERT(pc);
  RootedId id(cx);
  if (!ToPropertyKey(cx, index, &id)) {
    return false;
  }
  return SetObjectElementOperation(cx, obj, id, value, receiver, strict, script,
                                   pc);
}

bool js::InitElementArray(JSContext* cx, jsbytecode* pc, HandleObject obj,
                          uint32_t index, HandleValue value) {
  return InitArrayElemOperation(cx, pc, obj, index, value);
}

bool js::AddValues(JSContext* cx, MutableHandleValue lhs,
                   MutableHandleValue rhs, MutableHandleValue res) {
  return AddOperation(cx, lhs, rhs, res);
}

bool js::SubValues(JSContext* cx, MutableHandleValue lhs,
                   MutableHandleValue rhs, MutableHandleValue res) {
  return SubOperation(cx, lhs, rhs, res);
}

bool js::MulValues(JSContext* cx, MutableHandleValue lhs,
                   MutableHandleValue rhs, MutableHandleValue res) {
  return MulOperation(cx, lhs, rhs, res);
}

bool js::DivValues(JSContext* cx, MutableHandleValue lhs,
                   MutableHandleValue rhs, MutableHandleValue res) {
  return DivOperation(cx, lhs, rhs, res);
}

bool js::ModValues(JSContext* cx, MutableHandleValue lhs,
                   MutableHandleValue rhs, MutableHandleValue res) {
  return ModOperation(cx, lhs, rhs, res);
}

bool js::PowValues(JSContext* cx, MutableHandleValue lhs,
                   MutableHandleValue rhs, MutableHandleValue res) {
  return PowOperation(cx, lhs, rhs, res);
}

bool js::UrshValues(JSContext* cx, MutableHandleValue lhs,
                    MutableHandleValue rhs, MutableHandleValue res) {
  return UrshOperation(cx, lhs, rhs, res);
}

bool js::AtomicIsLockFree(JSContext* cx, HandleValue in, int* out) {
  int i;
  if (!ToInt32(cx, in, &i)) {
    return false;
  }
  *out = js::jit::AtomicOperations::isLockfreeJS(i);
  return true;
}

bool js::DeleteNameOperation(JSContext* cx, HandlePropertyName name,
                             HandleObject scopeObj, MutableHandleValue res) {
  RootedObject scope(cx), pobj(cx);
  Rooted<PropertyResult> prop(cx);
  if (!LookupName(cx, name, scopeObj, &scope, &pobj, &prop)) {
    return false;
  }

  if (!scope) {
    // Return true for non-existent names.
    res.setBoolean(true);
    return true;
  }

  ObjectOpResult result;
  RootedId id(cx, NameToId(name));
  if (!DeleteProperty(cx, scope, id, result)) {
    return false;
  }

  bool status = result.ok();
  res.setBoolean(status);

  if (status) {
    // Deleting a name from the global object removes it from [[VarNames]].
    if (pobj == scope && scope->is<GlobalObject>()) {
      scope->as<GlobalObject>().realm()->removeFromVarNames(name);
    }
  }

  return true;
}

bool js::ImplicitThisOperation(JSContext* cx, HandleObject scopeObj,
                               HandlePropertyName name,
                               MutableHandleValue res) {
  RootedObject obj(cx);
  if (!LookupNameWithGlobalDefault(cx, name, scopeObj, &obj)) {
    return false;
  }

  res.set(ComputeImplicitThis(obj));
  return true;
}

unsigned js::GetInitDataPropAttrs(JSOp op) {
  switch (op) {
    case JSOp::InitProp:
    case JSOp::InitElem:
      return JSPROP_ENUMERATE;
    case JSOp::InitLockedProp:
      return JSPROP_PERMANENT | JSPROP_READONLY;
    case JSOp::InitHiddenProp:
    case JSOp::InitHiddenElem:
      // Non-enumerable, but writable and configurable
      return 0;
    default:;
  }
  MOZ_CRASH("Unknown data initprop");
}

static bool InitGetterSetterOperation(JSContext* cx, jsbytecode* pc,
                                      HandleObject obj, HandleId id,
                                      HandleObject val) {
  MOZ_ASSERT(val->isCallable());

  JSOp op = JSOp(*pc);

  unsigned attrs = 0;
  if (!IsHiddenInitOp(op)) {
    attrs |= JSPROP_ENUMERATE;
  }

  if (op == JSOp::InitPropGetter || op == JSOp::InitElemGetter ||
      op == JSOp::InitHiddenPropGetter || op == JSOp::InitHiddenElemGetter) {
    attrs |= JSPROP_GETTER;
    return DefineAccessorProperty(cx, obj, id, val, nullptr, attrs);
  }

  MOZ_ASSERT(op == JSOp::InitPropSetter || op == JSOp::InitElemSetter ||
             op == JSOp::InitHiddenPropSetter ||
             op == JSOp::InitHiddenElemSetter);
  attrs |= JSPROP_SETTER;
  return DefineAccessorProperty(cx, obj, id, nullptr, val, attrs);
}

bool js::InitPropGetterSetterOperation(JSContext* cx, jsbytecode* pc,
                                       HandleObject obj,
                                       HandlePropertyName name,
                                       HandleObject val) {
  RootedId id(cx, NameToId(name));
  return InitGetterSetterOperation(cx, pc, obj, id, val);
}

bool js::InitElemGetterSetterOperation(JSContext* cx, jsbytecode* pc,
                                       HandleObject obj, HandleValue idval,
                                       HandleObject val) {
  RootedId id(cx);
  if (!ToPropertyKey(cx, idval, &id)) {
    return false;
  }

  return InitGetterSetterOperation(cx, pc, obj, id, val);
}

bool js::SpreadCallOperation(JSContext* cx, HandleScript script, jsbytecode* pc,
                             HandleValue thisv, HandleValue callee,
                             HandleValue arr, HandleValue newTarget,
                             MutableHandleValue res) {
  RootedArrayObject aobj(cx, &arr.toObject().as<ArrayObject>());
  uint32_t length = aobj->length();
  JSOp op = JSOp(*pc);
  bool constructing = op == JSOp::SpreadNew || op == JSOp::SpreadSuperCall;

  // {Construct,Invoke}Args::init does this too, but this gives us a better
  // error message.
  if (length > ARGS_LENGTH_MAX) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              constructing ? JSMSG_TOO_MANY_CON_SPREADARGS
                                           : JSMSG_TOO_MANY_FUN_SPREADARGS);
    return false;
  }

  // Do our own checks for the callee being a function, as Invoke uses the
  // expression decompiler to decompile the callee stack operand based on
  // the number of arguments. Spread operations have the callee at sp - 3
  // when not constructing, and sp - 4 when constructing.
  if (callee.isPrimitive()) {
    return ReportIsNotFunction(cx, callee, 2 + constructing,
                               constructing ? CONSTRUCT : NO_CONSTRUCT);
  }

  if (!callee.toObject().isCallable()) {
    return ReportIsNotFunction(cx, callee, 2 + constructing,
                               constructing ? CONSTRUCT : NO_CONSTRUCT);
  }

#ifdef DEBUG
  // The object must be an array with dense elements and no holes. Baseline's
  // optimized spread call stubs rely on this.
  MOZ_ASSERT(!aobj->isIndexed());
  MOZ_ASSERT(aobj->getDenseInitializedLength() == aobj->length());
  for (size_t i = 0; i < aobj->length(); i++) {
    MOZ_ASSERT(!aobj->getDenseElement(i).isMagic(JS_ELEMENTS_HOLE));
  }
#endif

  if (constructing) {
    if (!StackCheckIsConstructorCalleeNewTarget(cx, callee, newTarget)) {
      return false;
    }

    ConstructArgs cargs(cx);
    if (!cargs.init(cx, length)) {
      return false;
    }

    if (!GetElements(cx, aobj, length, cargs.array())) {
      return false;
    }

    RootedObject obj(cx);
    if (!Construct(cx, callee, cargs, newTarget, &obj)) {
      return false;
    }
    res.setObject(*obj);
  } else {
    InvokeArgs args(cx);
    if (!args.init(cx, length)) {
      return false;
    }

    if (!GetElements(cx, aobj, length, args.array())) {
      return false;
    }

    if ((op == JSOp::SpreadEval || op == JSOp::StrictSpreadEval) &&
        cx->global()->valueIsEval(callee)) {
      if (!DirectEval(cx, args.get(0), res)) {
        return false;
      }
    } else {
      MOZ_ASSERT(op == JSOp::SpreadCall || op == JSOp::SpreadEval ||
                     op == JSOp::StrictSpreadEval,
                 "bad spread opcode");

      if (!Call(cx, callee, thisv, args, res)) {
        return false;
      }
    }
  }

  JitScript::MonitorBytecodeType(cx, script, pc, res);
  return true;
}

bool js::OptimizeSpreadCall(JSContext* cx, HandleValue arg, bool* optimized) {
  // Optimize spread call by skipping spread operation when following
  // conditions are met:
  //   * the argument is an array
  //   * the array has no hole
  //   * array[@@iterator] is not modified
  //   * the array's prototype is Array.prototype
  //   * Array.prototype[@@iterator] is not modified
  //   * %ArrayIteratorPrototype%.next is not modified
  if (!arg.isObject()) {
    *optimized = false;
    return true;
  }

  RootedObject obj(cx, &arg.toObject());
  if (!IsPackedArray(obj)) {
    *optimized = false;
    return true;
  }

  ForOfPIC::Chain* stubChain = ForOfPIC::getOrCreate(cx);
  if (!stubChain) {
    return false;
  }

  return stubChain->tryOptimizeArray(cx, obj.as<ArrayObject>(), optimized);
}

JSObject* js::NewObjectOperation(JSContext* cx, HandleScript script,
                                 jsbytecode* pc,
                                 NewObjectKind newKind /* = GenericObject */) {
  MOZ_ASSERT(newKind != SingletonObject);
  bool withTemplate =
      (JSOp(*pc) == JSOp::NewObject || JSOp(*pc) == JSOp::NewObjectWithGroup);
  bool withTemplateGroup = (JSOp(*pc) == JSOp::NewObjectWithGroup);

  RootedObjectGroup group(cx);
  RootedPlainObject baseObject(cx);

  // Extract the template object, if one exists.
  if (withTemplate) {
    baseObject = &script->getObject(pc)->as<PlainObject>();
  }

  // Choose the group. Three cases:
  // - JSOp::NewObjectWithGroup explicitly indicates that we should use the
  //   same group as the template object's group.
  // - otherwise, if some heuristics indicate that we should use a singleton,
  //   we set the allocation-kind to ensure this.
  // - otherwise, we look up a group based on the allocation site, i.e., the
  //   (script, pc) tuple.
  if (withTemplateGroup) {
    group = baseObject->getGroup(cx, baseObject);
  } else if (ObjectGroup::useSingletonForAllocationSite(script, pc,
                                                        JSProto_Object)) {
    newKind = SingletonObject;
  } else {
    group = ObjectGroup::allocationSiteGroup(cx, script, pc, JSProto_Object);
    if (!group) {
      return nullptr;
    }

    {
      AutoSweepObjectGroup sweep(group);
      if (group->maybePreliminaryObjects(sweep)) {
        group->maybePreliminaryObjects(sweep)->maybeAnalyze(cx, group);
      }

      if (group->shouldPreTenure(sweep) ||
          group->maybePreliminaryObjects(sweep)) {
        newKind = TenuredObject;
      }
    }
  }

  RootedPlainObject obj(cx);

  // Actually allocate the object.
  if (withTemplate) {
    obj = CopyInitializerObject(cx, baseObject, newKind);
  } else {
    MOZ_ASSERT(JSOp(*pc) == JSOp::NewInit);
    obj = NewBuiltinClassInstance<PlainObject>(cx, newKind);
  }

  if (!obj) {
    return nullptr;
  }

  if (newKind == SingletonObject) {
    MOZ_ASSERT(obj->isSingleton());
  } else {
    obj->setGroup(group);

    if (!withTemplateGroup) {
      AutoSweepObjectGroup sweep(group);
      if (PreliminaryObjectArray* preliminaryObjects =
              group->maybePreliminaryObjects(sweep)) {
        preliminaryObjects->registerNewObject(obj);
      }
    }
  }

  return obj;
}

JSObject* js::NewObjectOperationWithTemplate(JSContext* cx,
                                             HandleObject templateObject) {
  // This is an optimized version of NewObjectOperation for use when the
  // object is not a singleton and has had its preliminary objects analyzed,
  // with the template object a copy of the object to create.
  MOZ_ASSERT(!templateObject->isSingleton());
  MOZ_ASSERT(cx->realm() == templateObject->nonCCWRealm());

  NewObjectKind newKind;
  {
    ObjectGroup* group = templateObject->group();
    AutoSweepObjectGroup sweep(group);
    newKind = group->shouldPreTenure(sweep) ? TenuredObject : GenericObject;
  }

  JSObject* obj =
      CopyInitializerObject(cx, templateObject.as<PlainObject>(), newKind);
  if (!obj) {
    return nullptr;
  }

  obj->setGroup(templateObject->group());
  return obj;
}

JSObject* js::CreateThisWithTemplate(JSContext* cx,
                                     HandleObject templateObject) {
  mozilla::Maybe<AutoRealm> ar;
  if (cx->realm() != templateObject->nonCCWRealm()) {
    MOZ_ASSERT(cx->compartment() == templateObject->compartment());
    ar.emplace(cx, templateObject);
  }

  return NewObjectOperationWithTemplate(cx, templateObject);
}

JSObject* js::NewArrayOperation(JSContext* cx, HandleScript script,
                                jsbytecode* pc, uint32_t length,
                                NewObjectKind newKind /* = GenericObject */) {
  MOZ_ASSERT(JSOp(*pc) == JSOp::NewArray);
  MOZ_ASSERT(newKind != SingletonObject);

  RootedObjectGroup group(cx);
  if (ObjectGroup::useSingletonForAllocationSite(script, pc, JSProto_Array)) {
    newKind = SingletonObject;
  } else {
    group = ObjectGroup::allocationSiteGroup(cx, script, pc, JSProto_Array);
    if (!group) {
      return nullptr;
    }

    AutoSweepObjectGroup sweep(group);
    if (group->shouldPreTenure(sweep)) {
      newKind = TenuredObject;
    }
  }

  ArrayObject* obj = NewDenseFullyAllocatedArray(cx, length, nullptr, newKind);
  if (!obj) {
    return nullptr;
  }

  if (newKind == SingletonObject) {
    MOZ_ASSERT(obj->isSingleton());
  } else {
    obj->setGroup(group);
  }

  return obj;
}

JSObject* js::NewArrayOperationWithTemplate(JSContext* cx,
                                            HandleObject templateObject) {
  MOZ_ASSERT(!templateObject->isSingleton());

  NewObjectKind newKind;
  {
    AutoSweepObjectGroup sweep(templateObject->group());
    newKind = templateObject->group()->shouldPreTenure(sweep) ? TenuredObject
                                                              : GenericObject;
  }

  ArrayObject* obj = NewDenseFullyAllocatedArray(
      cx, templateObject->as<ArrayObject>().length(), nullptr, newKind);
  if (!obj) {
    return nullptr;
  }

  MOZ_ASSERT(obj->lastProperty() ==
             templateObject->as<ArrayObject>().lastProperty());
  obj->setGroup(templateObject->group());
  return obj;
}

ArrayObject* js::NewArrayCopyOnWriteOperation(JSContext* cx,
                                              HandleScript script,
                                              jsbytecode* pc) {
  MOZ_ASSERT(JSOp(*pc) == JSOp::NewArrayCopyOnWrite);

  RootedArrayObject baseobj(
      cx, ObjectGroup::getOrFixupCopyOnWriteObject(cx, script, pc));
  if (!baseobj) {
    return nullptr;
  }

  return NewDenseCopyOnWriteArray(cx, baseobj);
}

void js::ReportRuntimeLexicalError(JSContext* cx, unsigned errorNumber,
                                   HandleId id) {
  MOZ_ASSERT(errorNumber == JSMSG_UNINITIALIZED_LEXICAL ||
             errorNumber == JSMSG_BAD_CONST_ASSIGN);
  if (UniqueChars printable =
          IdToPrintableUTF8(cx, id, IdToPrintableBehavior::IdIsIdentifier)) {
    JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, errorNumber,
                             printable.get());
  }
}

void js::ReportRuntimeLexicalError(JSContext* cx, unsigned errorNumber,
                                   HandlePropertyName name) {
  RootedId id(cx, NameToId(name));
  ReportRuntimeLexicalError(cx, errorNumber, id);
}

void js::ReportRuntimeLexicalError(JSContext* cx, unsigned errorNumber,
                                   HandleScript script, jsbytecode* pc) {
  JSOp op = JSOp(*pc);
  MOZ_ASSERT(op == JSOp::CheckLexical || op == JSOp::CheckAliasedLexical ||
             op == JSOp::ThrowSetConst || op == JSOp::ThrowSetAliasedConst ||
             op == JSOp::ThrowSetCallee || op == JSOp::GetImport);

  RootedPropertyName name(cx);

  if (op == JSOp::ThrowSetCallee) {
    name = script->function()->explicitName()->asPropertyName();
  } else if (IsLocalOp(op)) {
    name = FrameSlotName(script, pc)->asPropertyName();
  } else if (IsAtomOp(op)) {
    name = script->getName(pc);
  } else {
    MOZ_ASSERT(IsAliasedVarOp(op));
    name = EnvironmentCoordinateNameSlow(script, pc);
  }

  ReportRuntimeLexicalError(cx, errorNumber, name);
}

void js::ReportRuntimeRedeclaration(JSContext* cx, HandlePropertyName name,
                                    const char* redeclKind) {
  if (UniqueChars printable = AtomToPrintableString(cx, name)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_REDECLARED_VAR, redeclKind,
                              printable.get());
  }
}

bool js::ThrowCheckIsObject(JSContext* cx, CheckIsObjectKind kind) {
  switch (kind) {
    case CheckIsObjectKind::IteratorNext:
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_ITER_METHOD_RETURNED_PRIMITIVE, "next");
      break;
    case CheckIsObjectKind::IteratorReturn:
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_ITER_METHOD_RETURNED_PRIMITIVE, "return");
      break;
    case CheckIsObjectKind::IteratorThrow:
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_ITER_METHOD_RETURNED_PRIMITIVE, "throw");
      break;
    case CheckIsObjectKind::GetIterator:
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_GET_ITER_RETURNED_PRIMITIVE);
      break;
    case CheckIsObjectKind::GetAsyncIterator:
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_GET_ASYNC_ITER_RETURNED_PRIMITIVE);
      break;
    default:
      MOZ_CRASH("Unknown kind");
  }
  return false;
}

bool js::ThrowCheckIsCallable(JSContext* cx, CheckIsCallableKind kind) {
  switch (kind) {
    case CheckIsCallableKind::IteratorReturn:
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_RETURN_NOT_CALLABLE);
      break;
    default:
      MOZ_CRASH("Unknown kind");
  }
  return false;
}

bool js::ThrowUninitializedThis(JSContext* cx) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                            JSMSG_UNINITIALIZED_THIS);
  return false;
}

bool js::ThrowInitializedThis(JSContext* cx) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_REINIT_THIS);
  return false;
}

bool js::ThrowHomeObjectNotObject(JSContext* cx) {
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_CANT_CONVERT_TO,
                            "null", "object");
  return false;
}

bool js::SetPropertySuper(JSContext* cx, HandleObject obj, HandleValue receiver,
                          HandlePropertyName name, HandleValue rval,
                          bool strict) {
  RootedId id(cx, NameToId(name));
  ObjectOpResult result;
  if (!SetProperty(cx, obj, id, rval, receiver, result)) {
    return false;
  }

  return result.checkStrictErrorOrWarning(cx, obj, id, strict);
}
