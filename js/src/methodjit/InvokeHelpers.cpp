/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla SpiderMonkey JavaScript 1.9 code, released
 * May 28, 2008.
 *
 * The Initial Developer of the Original Code is
 *   Brendan Eich <brendan@mozilla.org>
 *
 * Contributor(s):
 *   David Anderson <danderson@mozilla.com>
 *   David Mandelin <dmandelin@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "jscntxt.h"
#include "jsscope.h"
#include "jsobj.h"
#include "jslibmath.h"
#include "jsiter.h"
#include "jsnum.h"
#include "jsxml.h"
#include "jsstaticcheck.h"
#include "jsbool.h"
#include "assembler/assembler/MacroAssemblerCodeRef.h"
#include "assembler/assembler/CodeLocation.h"
#include "assembler/assembler/RepatchBuffer.h"
#include "jsiter.h"
#include "jstypes.h"
#include "methodjit/StubCalls.h"
#include "jstracer.h"
#include "jspropertycache.h"
#include "methodjit/MonoIC.h"

#include "jspropertycacheinlines.h"
#include "jsscopeinlines.h"
#include "jsscriptinlines.h"
#include "jsstrinlines.h"
#include "jsobjinlines.h"
#include "jscntxtinlines.h"
#include "jsatominlines.h"

#include "jsautooplen.h"

using namespace js;
using namespace js::mjit;
using namespace JSC;

#define THROW()  \
    do {         \
        void *ptr = JS_FUNC_TO_DATA_PTR(void *, JaegerThrowpoline); \
        *f.returnAddressLocation() = ptr; \
        return;  \
    } while (0)

#define THROWV(v)       \
    do {                \
        void *ptr = JS_FUNC_TO_DATA_PTR(void *, JaegerThrowpoline); \
        *f.returnAddressLocation() = ptr; \
        return v;       \
    } while (0)

static bool
InlineReturn(VMFrame &f, JSBool ok);

static jsbytecode *
FindExceptionHandler(JSContext *cx)
{
    JSStackFrame *fp = cx->fp();
    JSScript *script = fp->getScript();

top:
    if (cx->throwing && script->trynotesOffset) {
        // The PC is updated before every stub call, so we can use it here.
        unsigned offset = cx->regs->pc - script->main;

        JSTryNoteArray *tnarray = script->trynotes();
        for (unsigned i = 0; i < tnarray->length; ++i) {
            JSTryNote *tn = &tnarray->vector[i];
            JS_ASSERT(offset < script->length);
            // The following if condition actually tests two separate conditions:
            //   (1) offset - tn->start >= tn->length
            //       means the PC is not in the range of this try note, so we
            //       should continue searching, after considering:
            //   (2) offset - tn->start == tn->length
            //       means the PC is at the first op of the exception handler
            //       for this try note. This happens when an exception is thrown
            //       during recording: the interpreter sets the PC to the handler
            //       and then exits. In this case, we are in fact at the right
            //       exception handler. 
            //      
            //       Hypothetically, the op we are at might have thrown an
            //       exception, in which case this would not be the right handler.
            //       But the first ops of exception handlers generated by our
            //       bytecode compiler cannot throw, so this is not possible.
            if (offset - tn->start > tn->length)
                continue;
            if (tn->stackDepth > cx->regs->sp - fp->base())
                continue;

            jsbytecode *pc = script->main + tn->start + tn->length;
            JSBool ok = js_UnwindScope(cx, tn->stackDepth, JS_TRUE);
            JS_ASSERT(cx->regs->sp == fp->base() + tn->stackDepth);

            switch (tn->kind) {
                case JSTRY_CATCH:
                  JS_ASSERT(js_GetOpcode(cx, fp->getScript(), pc) == JSOP_ENTERBLOCK);

#if JS_HAS_GENERATORS
                  /* Catch cannot intercept the closing of a generator. */
                  if (JS_UNLIKELY(cx->exception.isMagic(JS_GENERATOR_CLOSING)))
                      break;
#endif

                  /*
                   * Don't clear cx->throwing to save cx->exception from GC
                   * until it is pushed to the stack via [exception] in the
                   * catch block.
                   */
                  return pc;

                case JSTRY_FINALLY:
                  /*
                   * Push (true, exception) pair for finally to indicate that
                   * [retsub] should rethrow the exception.
                   */
                  cx->regs->sp[0].setBoolean(true);
                  cx->regs->sp[1] = cx->exception;
                  cx->regs->sp += 2;
                  cx->throwing = JS_FALSE;
                  return pc;

                case JSTRY_ITER:
                {
                  /*
                   * This is similar to JSOP_ENDITER in the interpreter loop,
                   * except the code now uses the stack slot normally used by
                   * JSOP_NEXTITER, namely regs.sp[-1] before the regs.sp -= 2
                   * adjustment and regs.sp[1] after, to save and restore the
                   * pending exception.
                   */
                  AutoValueRooter tvr(cx, cx->exception);
                  JS_ASSERT(js_GetOpcode(cx, fp->getScript(), pc) == JSOP_ENDITER);
                  cx->throwing = JS_FALSE;
                  ok = !!js_CloseIterator(cx, &cx->regs->sp[-1].toObject());
                  cx->regs->sp -= 1;
                  if (!ok)
                      goto top;
                  cx->throwing = JS_TRUE;
                  cx->exception = tvr.value();
                }
            }
        }
    }

    return NULL;
}

static bool
InlineReturn(VMFrame &f, JSBool ok)
{
    JSContext *cx = f.cx;
    JSStackFrame *fp = f.regs.fp;

    JS_ASSERT(f.fp() != f.entryFp);

    JS_ASSERT(!fp->hasBlockChain());
    JS_ASSERT(!js_IsActiveWithOrBlock(cx, fp->getScopeChain(), 0));

    // Marker for debug support.
    if (JS_UNLIKELY(fp->hasHookData())) {
        JSInterpreterHook hook;
        JSBool status;

        hook = cx->debugHooks->callHook;
        if (hook) {
            /*
             * Do not pass &ok directly as exposing the address inhibits
             * optimizations and uninitialised warnings.
             */
            status = ok;
            hook(cx, fp, JS_FALSE, &status, fp->getHookData());
            ok = (status == JS_TRUE);
            // CHECK_INTERRUPT_HANDLER();
        }
    }

    fp->putActivationObjects(cx);

    /* :TODO: version stuff */

    if (fp->flags & JSFRAME_CONSTRUCTING && fp->getReturnValue().isPrimitive())
        fp->setReturnValue(fp->getThisValue());

    Value *newsp = fp->argv - 1;

    cx->stack().popInlineFrame(cx, fp, fp->down);

    cx->regs->sp = newsp;
    cx->regs->sp[-1] = fp->getReturnValue();

    JS_ASSERT(cx->regs->pc != JSStackFrame::sInvalidPC);

    return ok;
}

JSBool JS_FASTCALL
stubs::NewObject(VMFrame &f, uint32 argc)
{
    JSContext *cx = f.cx;
    Value *vp = f.regs.sp - (argc + 2);

    JSObject *funobj = &vp[0].toObject();
    JS_ASSERT(funobj->isFunction());

    jsid id = ATOM_TO_JSID(cx->runtime->atomState.classPrototypeAtom);
    if (!funobj->getProperty(cx, id, &vp[1]))
        THROWV(JS_FALSE);

    JSObject *proto = vp[1].isObject() ? &vp[1].toObject() : NULL;
    JSObject *obj = NewNonFunction<WithProto::Class>(cx, &js_ObjectClass, proto, funobj->getParent());
    if (!obj)
        THROWV(JS_FALSE);

    vp[1].setObject(*obj);

    return JS_TRUE;
}

void JS_FASTCALL
stubs::SlowCall(VMFrame &f, uint32 argc)
{
    Value *vp = f.regs.sp - (argc + 2);

    if (!Invoke(f.cx, InvokeArgsAlreadyOnTheStack(vp, argc), 0))
        THROW();
}

void JS_FASTCALL
stubs::SlowNew(VMFrame &f, uint32 argc)
{
    JSContext *cx = f.cx;
    Value *vp = f.regs.sp - (argc + 2);

    if (!InvokeConstructor(cx, InvokeArgsAlreadyOnTheStack(vp, argc)))
        THROW();
}

static inline void
RemovePartialFrame(VMFrame &f)
{
    /* Unwind the half-pushed frame. */
    f.regs.pc = f.fp()->down->savedPC;
    f.regs.sp = f.fp()->argv + f.fp()->argc;
#ifdef DEBUG
    f.fp()->down->savedPC = JSStackFrame::sInvalidPC;
#endif
    f.regs.fp = f.fp()->down;
}

void JS_FASTCALL
stubs::CheckStackQuota(VMFrame &f)
{
    if (JS_LIKELY(f.ensureSpace(0, f.fp()->getScript()->nslots)))
        return;

    RemovePartialFrame(f);

    js_ReportOverRecursed(f.cx);
    THROW();
}

void * JS_FASTCALL
stubs::CheckArity(VMFrame &f)
{
    JSContext *cx = f.cx;
    JSStackFrame *fp = f.fp();
    uint32 argc = fp->argc;
    JSFunction *fun = fp->getFunction();

    JS_ASSERT(argc < fun->nargs);

    /*
     * Grossssss! *move* the stack frame. If this ends up being perf-critical,
     * we can figure out how to spot-optimize it. As the frame shrinks it will
     * matter less.
     */
    uint32 flags = fp->flags;
    JSObject *scopeChain = fp->getScopeChain();
    Value *argv = fp->argv;
    JSStackFrame *down = fp->down;
    void *ncode = fp->ncode;

    /* Pop the inline frame. */
    RemovePartialFrame(f);

    uint32 missing = fun->nargs - argc;

    /* Include an extra stack frame for callees. */
    if (!f.ensureSpace(missing, fun->u.i.script->nslots + VALUES_PER_STACK_FRAME)) {
        js_ReportOverRecursed(cx);
        THROWV(NULL);
    }

#ifdef DEBUG
    down->savedPC = f.regs.pc;
#endif

    SetValueRangeToUndefined(f.regs.sp, missing);
    f.regs.sp += missing;

    JSStackFrame *newfp = (JSStackFrame *)f.regs.sp;
    newfp->argc = argc;
    newfp->setFunction(fun);
    newfp->flags = flags;
    newfp->argv = argv;
    newfp->setScopeChain(scopeChain);
    newfp->down = down;
    newfp->ncode = ncode;
    newfp->setThisValue(argv[-1]);

    return newfp;
}

void * JS_FASTCALL
stubs::CompileFunction(VMFrame &f)
{
    /*
     * We have a partially constructed frame. That's not really good enough to
     * compile though because we could throw, so get a full, adjusted frame.
     */
    JSContext *cx = f.cx;
    JSStackFrame *fp = f.fp();
    uint32 argc = fp->argc;

    JSObject *obj = &fp->argv[-2].toObject();
    JSFunction *fun = obj->getFunctionPrivate();
    JSScript *script = fun->u.i.script;

    bool callingNew = !!(fp->flags & JSFRAME_CONSTRUCTING);

    /* Empty script does nothing. */
    if (script->isEmpty()) {
        RemovePartialFrame(f);
        Value *vp = f.regs.sp - argc;
        if (callingNew)
            vp[-2] = vp[-1];
        else
            vp[-2].setUndefined();
        return NULL;
    }

    /* CheckArity expects fun to be set. */
    fp->setFunction(fun);

    if (argc < fun->nargs) {
        fp = (JSStackFrame *)CheckArity(f);
        if (!fp)
            return NULL;
    }

    fp->setCallObj(NULL);
    fp->setArgsObj(NULL);
    fp->setBlockChain(NULL);
    fp->setHookData(NULL);
    fp->setAnnotation(NULL);
    fp->setCallerVersion(fp->down->getCallerVersion());
    fp->setScript(script);
    fp->clearReturnValue();
#ifdef DEBUG
    fp->savedPC = JSStackFrame::sInvalidPC;
#endif

    f.regs.fp = fp;
    f.regs.sp = fp->base();
    f.regs.pc = script->code;

    SetValueRangeToUndefined(fp->slots(), script->nfixed);

    if (fun->isHeavyweight() && !js_GetCallObject(cx, fp))
        THROWV(NULL);

    CompileStatus status = CanMethodJIT(cx, script, fun, fp->getScopeChain());
    if (status == Compile_Okay)
        return script->jit->invoke;

    /* Function did not compile... interpret it. */
    JSBool ok = Interpret(cx, fp);
    InlineReturn(f, ok);

    if (!ok)
        THROWV(NULL);

    return NULL;
}

/* Preserved for when calls need to be slow (debug mode, no ICs) */
static bool
CreateFrame(VMFrame &f, uint32 flags, uint32 argc)
{
    JSContext *cx = f.cx;
    JSStackFrame *fp = f.fp();
    Value *vp = f.regs.sp - (argc + 2);
    JSObject *funobj = &vp->toObject();
    JSFunction *fun = GET_FUNCTION_PRIVATE(cx, funobj);

    JS_ASSERT(FUN_INTERPRETED(fun));

    JSScript *newscript = fun->u.i.script;

    /* Allocate the frame. */
    StackSpace &stack = cx->stack();
    uintN nslots = newscript->nslots;
    uintN funargs = fun->nargs;
    Value *argv = vp + 2;
    JSStackFrame *newfp;
    if (argc < funargs) {
        uintN missing = funargs - argc;
        if (!f.ensureSpace(missing, nslots))
            return false;
        newfp = stack.getInlineFrameUnchecked(cx, f.regs.sp, missing);
        if (!newfp)
            return false;
        for (Value *v = argv + argc, *end = v + missing; v != end; ++v)
            v->setUndefined();
    } else {
        if (!f.ensureSpace(0, nslots))
            return false;
        newfp = stack.getInlineFrameUnchecked(cx, f.regs.sp, 0);
        if (!newfp)
            return false;
    }

    /* Initialize the frame. */
    newfp->ncode = NULL;
    newfp->setCallObj(NULL);
    newfp->setArgsObj(NULL);
    newfp->setScript(newscript);
    newfp->setFunction(fun);
    newfp->argc = argc;
    newfp->argv = vp + 2;
    newfp->clearReturnValue();
    newfp->setAnnotation(NULL);
    newfp->setScopeChain(funobj->getParent());
    newfp->flags = flags;
    newfp->setBlockChain(NULL);
    newfp->setThisValue(vp[1]);
    JS_ASSERT(!fp->hasIMacroPC());

    /* Push void to initialize local variables. */
    Value *newslots = newfp->slots();
    Value *newsp = newslots + fun->u.i.nvars;
    for (Value *v = newslots; v != newsp; ++v)
        v->setUndefined();

    /* Scope with a call object parented by callee's parent. */
    if (fun->isHeavyweight() && !js_GetCallObject(cx, newfp))
        return false;

    /* :TODO: Switch version if currentVersion wasn't overridden. */
    newfp->setCallerVersion((JSVersion)cx->version);

    // Marker for debug support.
    if (JSInterpreterHook hook = cx->debugHooks->callHook) {
        newfp->setHookData(hook(cx, fp, JS_TRUE, 0,
                                cx->debugHooks->callHookData));
    } else {
        newfp->setHookData(NULL);
    }

    stack.pushInlineFrame(cx, fp, cx->regs->pc, newfp);
    f.regs.fp = newfp;

    return true;
}

static inline bool
UncachedInlineCall(VMFrame &f, uint32 flags, void **pret, uint32 argc)
{
    if (!CreateFrame(f, flags, argc))
        return false;

    JSContext *cx = f.cx;
    JSStackFrame *fp = cx->fp();
    JSScript *script = fp->getScript();
    f.regs.pc = script->code;
    f.regs.sp = fp->base();

    if (cx->options & JSOPTION_METHODJIT) {
        if (!script->ncode) {
            if (mjit::TryCompile(cx, script, fp->getFunction(), fp->getScopeChain()) == Compile_Error) {
                InlineReturn(f, JS_FALSE);
                return false;
            }
        }
        JS_ASSERT(script->ncode);
        if (script->ncode != JS_UNJITTABLE_METHOD) {
            *pret = script->jit->invoke;
            return true;
        }
    }

    bool ok = !!Interpret(cx, cx->fp());
    InlineReturn(f, JS_TRUE);

    *pret = NULL;
    return ok;
}

void * JS_FASTCALL
stubs::UncachedNew(VMFrame &f, uint32 argc)
{
    JSContext *cx = f.cx;

    Value *vp = f.regs.sp - (argc + 2);

    JSObject *obj;
    if (IsFunctionObject(*vp, &obj)) {
        JSFunction *fun = GET_FUNCTION_PRIVATE(cx, obj);

        if (fun->isInterpreted()) {
            JSScript *script = fun->u.i.script;
            if (!stubs::NewObject(f, argc))
                THROWV(NULL);

            if (script->isEmpty()) {
                vp[0] = vp[1];
                return NULL;
            }

            void *ret;
            if (!UncachedInlineCall(f, JSFRAME_CONSTRUCTING, &ret, argc))
                THROWV(NULL);

            return ret;
        }

        if (fun->isConstructor()) {
            vp[1].setMagicWithObjectOrNullPayload(NULL);
            Native fn = fun->u.n.native;
            if (!fn(cx, argc, vp))
                THROWV(NULL);
            JS_ASSERT(!vp->isPrimitive());
            return NULL;
        }
    }

    if (!InvokeConstructor(cx, InvokeArgsAlreadyOnTheStack(vp, argc)))
        THROWV(NULL);

    return NULL;
}

void * JS_FASTCALL
stubs::UncachedCall(VMFrame &f, uint32 argc)
{
    JSContext *cx = f.cx;

    Value *vp = f.regs.sp - (argc + 2);

    JSObject *obj;
    if (IsFunctionObject(*vp, &obj)) {
        JSFunction *fun = GET_FUNCTION_PRIVATE(cx, obj);

        if (fun->isInterpreted()) {
            void *ret;

            if (fun->u.i.script->isEmpty()) {
                vp->setUndefined();
                f.regs.sp = vp + 1;
                return NULL;
            }

            if (!UncachedInlineCall(f, 0, &ret, argc))
                THROWV(NULL);

            return ret;
        }

        if (fun->isNative()) {
            if (!fun->u.n.native(cx, argc, vp))
                THROWV(NULL);
            return NULL;
        }
    }

    if (!Invoke(f.cx, InvokeArgsAlreadyOnTheStack(vp, argc), 0))
        THROWV(NULL);

    return NULL;
}

void JS_FASTCALL
stubs::PutCallObject(VMFrame &f)
{
    JS_ASSERT(f.fp()->hasCallObj());
    js_PutCallObject(f.cx, f.fp());
    JS_ASSERT(!f.fp()->hasArgsObj());
}

void JS_FASTCALL
stubs::PutArgsObject(VMFrame &f)
{
    js_PutArgsObject(f.cx, f.fp());
}

extern "C" void *
js_InternalThrow(VMFrame &f)
{
    JSContext *cx = f.cx;

    // Make sure sp is up to date.
    JS_ASSERT(cx->regs == &f.regs);

    // Call the throw hook if necessary
    JSThrowHook handler = f.cx->debugHooks->throwHook;
    if (handler) {
        Value rval;
        switch (handler(cx, cx->fp()->getScript(), cx->regs->pc, Jsvalify(&rval),
                        cx->debugHooks->throwHookData)) {
          case JSTRAP_ERROR:
            cx->throwing = JS_FALSE;
            return NULL;

          case JSTRAP_RETURN:
            cx->throwing = JS_FALSE;
            cx->fp()->setReturnValue(rval);
            return JS_FUNC_TO_DATA_PTR(void *,
                   JS_METHODJIT_DATA(cx).trampolines.forceReturn);

          case JSTRAP_THROW:
            cx->exception = rval;
            break;

          default:
            break;
        }
    }

    jsbytecode *pc = NULL;
    for (;;) {
        pc = FindExceptionHandler(cx);
        if (pc)
            break;

        // If on the 'topmost' frame (where topmost means the first frame
        // called into through js_Interpret). In this case, we still unwind,
        // but we shouldn't return from a JS function, because we're not in a
        // JS function.
        bool lastFrame = (f.entryFp == f.fp());
        js_UnwindScope(cx, 0, cx->throwing);
        if (lastFrame)
            break;

        JS_ASSERT(f.regs.sp == cx->regs->sp);
        InlineReturn(f, JS_FALSE);
    }

    JS_ASSERT(f.regs.sp == cx->regs->sp);

    if (!pc) {
        *f.oldRegs = f.regs;
        f.cx->setCurrentRegs(f.oldRegs);
        return NULL;
    }

    return cx->fp()->getScript()->pcToNative(pc);
}

void JS_FASTCALL
stubs::GetCallObject(VMFrame &f)
{
    JS_ASSERT(f.fp()->getFunction()->isHeavyweight());
    if (!js_GetCallObject(f.cx, f.fp()))
        THROW();
}

static inline void
AdvanceReturnPC(JSContext *cx)
{
    /* Simulate an inline_return by advancing the pc. */
    JS_ASSERT(*cx->regs->pc == JSOP_CALL ||
              *cx->regs->pc == JSOP_NEW ||
              *cx->regs->pc == JSOP_EVAL ||
              *cx->regs->pc == JSOP_APPLY);
    cx->regs->pc += JSOP_CALL_LENGTH;
}

#ifdef JS_TRACER

static inline bool
SwallowErrors(VMFrame &f, JSStackFrame *stopFp)
{
    JSContext *cx = f.cx;

    /* Remove the bottom frame. */
    bool ok = false;
    for (;;) {
        JSStackFrame *fp = cx->fp();

        /* Look for an imacro with hard-coded exception handlers. */
        if (fp->hasIMacroPC() && cx->throwing) {
            cx->regs->pc = fp->getIMacroPC();
            fp->clearIMacroPC();
            if (ok)
                break;
        }
        JS_ASSERT(!fp->hasIMacroPC());

        /* If there's an exception and a handler, set the pc and leave. */
        jsbytecode *pc = FindExceptionHandler(cx);
        if (pc) {
            cx->regs->pc = pc;
            ok = true;
            break;
        }

        /* Don't unwind if this was the entry frame. */
        if (fp == stopFp)
            break;

        /* Unwind and return. */
        ok &= bool(js_UnwindScope(cx, 0, cx->throwing));
        InlineReturn(f, ok);
    }

    /* Update the VMFrame before leaving. */
    JS_ASSERT(&f.regs == cx->regs);

    JS_ASSERT_IF(!ok, cx->fp() == stopFp);
    return ok;
}

static inline bool
AtSafePoint(JSContext *cx)
{
    JSStackFrame *fp = cx->fp();
    if (fp->hasIMacroPC())
        return false;

    JSScript *script = fp->getScript();
    if (!script->nmap)
        return false;

    JS_ASSERT(cx->regs->pc >= script->code && cx->regs->pc < script->code + script->length);
    return !!script->nmap[cx->regs->pc - script->code];
}

static inline JSBool
PartialInterpret(VMFrame &f)
{
    JSContext *cx = f.cx;
    JSStackFrame *fp = cx->fp();

    JS_ASSERT(fp->hasIMacroPC() || !fp->getScript()->nmap ||
              !fp->getScript()->nmap[cx->regs->pc - fp->getScript()->code]);

    JSBool ok = JS_TRUE;
    ok = Interpret(cx, fp, 0, JSINTERP_SAFEPOINT);

    f.fp() = cx->fp();

    return ok;
}

JS_STATIC_ASSERT(JSOP_NOP == 0);

static inline JSOp
FrameIsFinished(JSContext *cx)
{
    JSOp op = JSOp(*cx->regs->pc);
    return (op == JSOP_RETURN ||
            op == JSOP_RETRVAL ||
            op == JSOP_STOP)
        ? op
        : JSOP_NOP;
}

static bool
RemoveExcessFrames(VMFrame &f, JSStackFrame *entryFrame)
{
    JSContext *cx = f.cx;
    while (cx->fp() != entryFrame || entryFrame->hasIMacroPC()) {
        JSStackFrame *fp = cx->fp();

        if (AtSafePoint(cx)) {
            JSScript *script = fp->getScript();
            if (!JaegerShotAtSafePoint(cx, script->nmap[cx->regs->pc - script->code])) {
                if (!SwallowErrors(f, entryFrame))
                    return false;

                /* Could be anywhere - restart outer loop. */
                continue;
            }
            InlineReturn(f, JS_TRUE);
            AdvanceReturnPC(cx);
        } else {
            if (!PartialInterpret(f)) {
                if (!SwallowErrors(f, entryFrame))
                    return false;
            } else if (cx->fp() != entryFrame) {
                /*
                 * Partial interpret could have dropped us anywhere. Deduce the
                 * edge case: at a RETURN, needing to pop a frame.
                 */
                JS_ASSERT(!cx->fp()->hasIMacroPC());
                if (FrameIsFinished(cx)) {
                    JSOp op = JSOp(*cx->regs->pc);
                    if (op == JSOP_RETURN && !(cx->fp()->flags & JSFRAME_BAILED_AT_RETURN))
                        fp->setReturnValue(f.regs.sp[-1]);
                    InlineReturn(f, JS_TRUE);
                    AdvanceReturnPC(cx);
                }
            }
        }
    }

    return true;
}

#if JS_MONOIC
static void
DisableTraceHint(VMFrame &f, ic::MICInfo &mic)
{
    JS_ASSERT(mic.kind == ic::MICInfo::TRACER);

    /*
     * Hack: The value that will be patched is before the executable address,
     * so to get protection right, just unprotect the general region around
     * the jump.
     */
    uint8 *addr = (uint8 *)(mic.traceHint.executableAddress());
    JSC::RepatchBuffer repatch(addr - 64, 128);
    repatch.relink(mic.traceHint, mic.load);

    JaegerSpew(JSpew_PICs, "relinking trace hint %p to %p\n", mic.traceHint.executableAddress(),
               mic.load.executableAddress());

    if (mic.u.hasSlowTraceHint) {
        addr = (uint8 *)(mic.slowTraceHint.executableAddress());
        JSC::RepatchBuffer repatch(addr - 64, 128);
        repatch.relink(mic.slowTraceHint, mic.load);

        JaegerSpew(JSpew_PICs, "relinking trace hint %p to %p\n",
                   mic.slowTraceHint.executableAddress(),
                   mic.load.executableAddress());
    }
}
#endif

#if JS_MONOIC
void *
RunTracer(VMFrame &f, ic::MICInfo &mic)
#else
void *
RunTracer(VMFrame &f)
#endif
{
    JSContext *cx = f.cx;
    JSStackFrame *entryFrame = f.fp();
    TracePointAction tpa;

    /* :TODO: nuke PIC? */
    if (!cx->jitEnabled)
        return NULL;

    bool blacklist;
    uintN inlineCallCount = 0;
    tpa = MonitorTracePoint(f.cx, inlineCallCount, blacklist);
    JS_ASSERT(!TRACE_RECORDER(cx));

#if JS_MONOIC
    if (blacklist)
        DisableTraceHint(f, mic);
#endif

    if ((tpa == TPA_RanStuff || tpa == TPA_Recorded) && cx->throwing)
        tpa = TPA_Error;

	/* Sync up the VMFrame's view of cx->fp(). */
	f.fp() = cx->fp();

    switch (tpa) {
      case TPA_Nothing:
        return NULL;

      case TPA_Error:
        if (!SwallowErrors(f, entryFrame))
            THROWV(NULL);
        JS_ASSERT(!cx->fp()->hasIMacroPC());
        break;

      case TPA_RanStuff:
      case TPA_Recorded:
        break;
    }

    /*
     * The tracer could have dropped us off on any frame at any position.
     * Well, it could not have removed frames (recursion is disabled).
     *
     * Frames after the entryFrame cannot be entered via JaegerShotAtSafePoint()
     * unless each is at a safe point. We can JaegerShotAtSafePoint these
     * frames individually, but we must unwind to the entryFrame.
     *
     * Note carefully that JaegerShotAtSafePoint can resume methods at
     * arbitrary safe points whereas JaegerShot cannot.
     *
     * If we land on entryFrame without a safe point in sight, we'll end up
     * at the RETURN op. This is an edge case with two paths:
     *
     * 1) The entryFrame is the last inline frame. If it fell on a RETURN,
     *    move the return value down.
     * 2) The entryFrame is NOT the last inline frame. Pop the frame.
     *
     * In both cases, we hijack the stub to return to InjectJaegerReturn. This
     * moves |oldFp->rval| into the scripted return registers.
     */

  restart:
    /* Step 1. Initial removal of excess frames. */
    if (!RemoveExcessFrames(f, entryFrame))
        THROWV(NULL);

    /* IMacros are guaranteed to have been removed by now. */
    JS_ASSERT(!entryFrame->hasIMacroPC());

    /* Step 2. If entryFrame is at a safe point, just leave. */
    if (AtSafePoint(cx)) {
        uint32 offs = uint32(cx->regs->pc - entryFrame->getScript()->code);
        JS_ASSERT(entryFrame->getScript()->nmap[offs]);
        return entryFrame->getScript()->nmap[offs];
    }

    /* Step 3. If entryFrame is at a RETURN, then leave slightly differently. */
    if (JSOp op = FrameIsFinished(cx)) {
        /* We're not guaranteed that the RETURN was run. */
        if (op == JSOP_RETURN && !(entryFrame->flags & JSFRAME_BAILED_AT_RETURN))
            entryFrame->setReturnValue(f.regs.sp[-1]);

        /* Don't pop the frame if it's maybe owned by an Invoke. */
        if (f.fp() != f.entryFp) {
            if (!InlineReturn(f, JS_TRUE))
                THROWV(NULL);
        }
        void *retPtr = JS_FUNC_TO_DATA_PTR(void *, InjectJaegerReturn);
        *f.returnAddressLocation() = retPtr;
        return NULL;
    }

    /* Step 4. Do a partial interp, then restart the whole process. */
    if (!PartialInterpret(f)) {
        if (!SwallowErrors(f, entryFrame))
            THROWV(NULL);
    }

    goto restart;
}

#endif /* JS_TRACER */

#if defined JS_TRACER
# if defined JS_MONOIC
void *JS_FASTCALL
stubs::InvokeTracer(VMFrame &f, uint32 index)
{
    JSScript *script = f.fp()->getScript();
    ic::MICInfo &mic = script->mics[index];

    JS_ASSERT(mic.kind == ic::MICInfo::TRACER);

    return RunTracer(f, mic);
}

# else

void *JS_FASTCALL
stubs::InvokeTracer(VMFrame &f)
{
    return RunTracer(f);
}
# endif /* JS_MONOIC */
#endif /* JS_TRACER */

