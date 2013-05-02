// Copyright 2012 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "v8.h"

#include "accessors.h"
#include "api.h"
#include "arguments.h"
#include "codegen.h"
#include "execution.h"
#include "ic-inl.h"
#include "runtime.h"
#include "stub-cache.h"

namespace v8 {
namespace internal {

#ifdef DEBUG
char IC::TransitionMarkFromState(IC::State state) {
  switch (state) {
    case UNINITIALIZED: return '0';
    case PREMONOMORPHIC: return '.';
    case MONOMORPHIC: return '1';
    case MONOMORPHIC_PROTOTYPE_FAILURE: return '^';
    case POLYMORPHIC: return 'P';
    case MEGAMORPHIC: return 'N';
    case GENERIC: return 'G';

    // We never see the debugger states here, because the state is
    // computed from the original code - not the patched code. Let
    // these cases fall through to the unreachable code below.
    case DEBUG_STUB: break;
  }
  UNREACHABLE();
  return 0;
}

void IC::TraceIC(const char* type,
                 Handle<Object> name,
                 State old_state,
                 Code* new_target) {
  if (FLAG_trace_ic) {
    Object* undef = new_target->GetHeap()->undefined_value();
    State new_state = StateFrom(new_target, undef, undef);
    PrintF("[%s in ", type);
    Isolate* isolate = new_target->GetIsolate();
    StackFrameIterator it(isolate);
    while (it.frame()->fp() != this->fp()) it.Advance();
    StackFrame* raw_frame = it.frame();
    if (raw_frame->is_internal()) {
      Code* apply_builtin = isolate->builtins()->builtin(
          Builtins::kFunctionApply);
      if (raw_frame->unchecked_code() == apply_builtin) {
        PrintF("apply from ");
        it.Advance();
        raw_frame = it.frame();
      }
    }
    JavaScriptFrame::PrintTop(isolate, stdout, false, true);
    bool new_can_grow =
        Code::GetKeyedAccessGrowMode(new_target->extra_ic_state()) ==
        ALLOW_JSARRAY_GROWTH;
    PrintF(" (%c->%c%s)",
           TransitionMarkFromState(old_state),
           TransitionMarkFromState(new_state),
           new_can_grow ? ".GROW" : "");
    name->Print();
    PrintF("]\n");
  }
}

#define TRACE_GENERIC_IC(isolate, type, reason)                 \
  do {                                                          \
    if (FLAG_trace_ic) {                                        \
      PrintF("[%s patching generic stub in ", type);            \
      JavaScriptFrame::PrintTop(isolate, stdout, false, true);  \
      PrintF(" (%s)]\n", reason);                               \
    }                                                           \
  } while (false)

#else
#define TRACE_GENERIC_IC(isolate, type, reason)
#endif  // DEBUG

#define TRACE_IC(type, name, old_state, new_target)             \
  ASSERT((TraceIC(type, name, old_state, new_target), true))

IC::IC(FrameDepth depth, Isolate* isolate) : isolate_(isolate) {
  // To improve the performance of the (much used) IC code, we unfold a few
  // levels of the stack frame iteration code. This yields a ~35% speedup when
  // running DeltaBlue and a ~25% speedup of gbemu with the '--nouse-ic' flag.
  const Address entry =
      Isolate::c_entry_fp(isolate->thread_local_top());
  Address* pc_address =
      reinterpret_cast<Address*>(entry + ExitFrameConstants::kCallerPCOffset);
  Address fp = Memory::Address_at(entry + ExitFrameConstants::kCallerFPOffset);
  // If there's another JavaScript frame on the stack or a
  // StubFailureTrampoline, we need to look one frame further down the stack to
  // find the frame pointer and the return address stack slot.
  if (depth == EXTRA_CALL_FRAME) {
    const int kCallerPCOffset = StandardFrameConstants::kCallerPCOffset;
    pc_address = reinterpret_cast<Address*>(fp + kCallerPCOffset);
    fp = Memory::Address_at(fp + StandardFrameConstants::kCallerFPOffset);
  }
#ifdef DEBUG
  StackFrameIterator it(isolate);
  for (int i = 0; i < depth + 1; i++) it.Advance();
  StackFrame* frame = it.frame();
  ASSERT(fp == frame->fp() && pc_address == frame->pc_address());
#endif
  fp_ = fp;
  pc_address_ = pc_address;
}


#ifdef ENABLE_DEBUGGER_SUPPORT
Address IC::OriginalCodeAddress() const {
  HandleScope scope(isolate());
  // Compute the JavaScript frame for the frame pointer of this IC
  // structure. We need this to be able to find the function
  // corresponding to the frame.
  StackFrameIterator it(isolate());
  while (it.frame()->fp() != this->fp()) it.Advance();
  JavaScriptFrame* frame = JavaScriptFrame::cast(it.frame());
  // Find the function on the stack and both the active code for the
  // function and the original code.
  JSFunction* function = JSFunction::cast(frame->function());
  Handle<SharedFunctionInfo> shared(function->shared());
  Code* code = shared->code();
  ASSERT(Debug::HasDebugInfo(shared));
  Code* original_code = Debug::GetDebugInfo(shared)->original_code();
  ASSERT(original_code->IsCode());
  // Get the address of the call site in the active code. This is the
  // place where the call to DebugBreakXXX is and where the IC
  // normally would be.
  Address addr = Assembler::target_address_from_return_address(pc());
  // Return the address in the original code. This is the place where
  // the call which has been overwritten by the DebugBreakXXX resides
  // and the place where the inline cache system should look.
  intptr_t delta =
      original_code->instruction_start() - code->instruction_start();
  return addr + delta;
}
#endif


static bool TryRemoveInvalidPrototypeDependentStub(Code* target,
                                                   Object* receiver,
                                                   Object* name) {
  InlineCacheHolderFlag cache_holder =
      Code::ExtractCacheHolderFromFlags(target->flags());

  Isolate* isolate = target->GetIsolate();
  if (cache_holder == OWN_MAP && !receiver->IsJSObject()) {
    // The stub was generated for JSObject but called for non-JSObject.
    // IC::GetCodeCacheHolder is not applicable.
    return false;
  } else if (cache_holder == PROTOTYPE_MAP &&
             receiver->GetPrototype(isolate)->IsNull()) {
    // IC::GetCodeCacheHolder is not applicable.
    return false;
  }
  Map* map = IC::GetCodeCacheHolder(isolate, receiver, cache_holder)->map();

  // Decide whether the inline cache failed because of changes to the
  // receiver itself or changes to one of its prototypes.
  //
  // If there are changes to the receiver itself, the map of the
  // receiver will have changed and the current target will not be in
  // the receiver map's code cache.  Therefore, if the current target
  // is in the receiver map's code cache, the inline cache failed due
  // to prototype check failure.
  int index = map->IndexInCodeCache(name, target);
  if (index >= 0) {
    map->RemoveFromCodeCache(String::cast(name), target, index);
    return true;
  }

  return false;
}


IC::State IC::StateFrom(Code* target, Object* receiver, Object* name) {
  IC::State state = target->ic_state();

  if (state != MONOMORPHIC || !name->IsString()) return state;
  if (receiver->IsUndefined() || receiver->IsNull()) return state;

  // For keyed load/store/call, the most likely cause of cache failure is
  // that the key has changed.  We do not distinguish between
  // prototype and non-prototype failures for keyed access.
  Code::Kind kind = target->kind();
  if (kind == Code::KEYED_LOAD_IC ||
      kind == Code::KEYED_STORE_IC ||
      kind == Code::KEYED_CALL_IC) {
    return MONOMORPHIC;
  }

  // Remove the target from the code cache if it became invalid
  // because of changes in the prototype chain to avoid hitting it
  // again.
  // Call stubs handle this later to allow extra IC state
  // transitions.
  if (kind != Code::CALL_IC &&
      TryRemoveInvalidPrototypeDependentStub(target, receiver, name)) {
    return MONOMORPHIC_PROTOTYPE_FAILURE;
  }

  // The builtins object is special.  It only changes when JavaScript
  // builtins are loaded lazily.  It is important to keep inline
  // caches for the builtins object monomorphic.  Therefore, if we get
  // an inline cache miss for the builtins object after lazily loading
  // JavaScript builtins, we return uninitialized as the state to
  // force the inline cache back to monomorphic state.
  if (receiver->IsJSBuiltinsObject()) {
    return UNINITIALIZED;
  }

  return MONOMORPHIC;
}


RelocInfo::Mode IC::ComputeMode() {
  Address addr = address();
  Code* code = Code::cast(isolate()->heap()->FindCodeObject(addr));
  for (RelocIterator it(code, RelocInfo::kCodeTargetMask);
       !it.done(); it.next()) {
    RelocInfo* info = it.rinfo();
    if (info->pc() == addr) return info->rmode();
  }
  UNREACHABLE();
  return RelocInfo::NONE32;
}


Failure* IC::TypeError(const char* type,
                       Handle<Object> object,
                       Handle<Object> key) {
  HandleScope scope(isolate());
  Handle<Object> args[2] = { key, object };
  Handle<Object> error = isolate()->factory()->NewTypeError(
      type, HandleVector(args, 2));
  return isolate()->Throw(*error);
}


Failure* IC::ReferenceError(const char* type, Handle<String> name) {
  HandleScope scope(isolate());
  Handle<Object> error = isolate()->factory()->NewReferenceError(
      type, HandleVector(&name, 1));
  return isolate()->Throw(*error);
}


static int ComputeTypeInfoCountDelta(IC::State old_state, IC::State new_state) {
  bool was_uninitialized =
      old_state == UNINITIALIZED || old_state == PREMONOMORPHIC;
  bool is_uninitialized =
      new_state == UNINITIALIZED || new_state == PREMONOMORPHIC;
  return (was_uninitialized && !is_uninitialized) ?  1 :
         (!was_uninitialized && is_uninitialized) ? -1 : 0;
}


void IC::PostPatching(Address address, Code* target, Code* old_target) {
  if (FLAG_type_info_threshold == 0 && !FLAG_watch_ic_patching) {
    return;
  }
  Isolate* isolate = target->GetHeap()->isolate();
  Code* host = isolate->
      inner_pointer_to_code_cache()->GetCacheEntry(address)->code;
  if (host->kind() != Code::FUNCTION) return;

  if (FLAG_type_info_threshold > 0 &&
      old_target->is_inline_cache_stub() &&
      target->is_inline_cache_stub()) {
    int delta = ComputeTypeInfoCountDelta(old_target->ic_state(),
                                          target->ic_state());
    // Not all Code objects have TypeFeedbackInfo.
    if (host->type_feedback_info()->IsTypeFeedbackInfo() && delta != 0) {
      TypeFeedbackInfo* info =
          TypeFeedbackInfo::cast(host->type_feedback_info());
      info->change_ic_with_type_info_count(delta);
    }
  }
  if (host->type_feedback_info()->IsTypeFeedbackInfo()) {
    TypeFeedbackInfo* info =
        TypeFeedbackInfo::cast(host->type_feedback_info());
    info->change_own_type_change_checksum();
  }
  if (FLAG_watch_ic_patching) {
    host->set_profiler_ticks(0);
    isolate->runtime_profiler()->NotifyICChanged();
  }
  // TODO(2029): When an optimized function is patched, it would
  // be nice to propagate the corresponding type information to its
  // unoptimized version for the benefit of later inlining.
}


void IC::Clear(Address address) {
  Code* target = GetTargetAtAddress(address);

  // Don't clear debug break inline cache as it will remove the break point.
  if (target->is_debug_break()) return;

  switch (target->kind()) {
    case Code::LOAD_IC: return LoadIC::Clear(address, target);
    case Code::KEYED_LOAD_IC: return KeyedLoadIC::Clear(address, target);
    case Code::STORE_IC: return StoreIC::Clear(address, target);
    case Code::KEYED_STORE_IC: return KeyedStoreIC::Clear(address, target);
    case Code::CALL_IC: return CallIC::Clear(address, target);
    case Code::KEYED_CALL_IC:  return KeyedCallIC::Clear(address, target);
    case Code::COMPARE_IC: return CompareIC::Clear(address, target);
    case Code::UNARY_OP_IC:
    case Code::BINARY_OP_IC:
    case Code::TO_BOOLEAN_IC:
      // Clearing these is tricky and does not
      // make any performance difference.
      return;
    default: UNREACHABLE();
  }
}


void CallICBase::Clear(Address address, Code* target) {
  if (target->ic_state() == UNINITIALIZED) return;
  bool contextual = CallICBase::Contextual::decode(target->extra_ic_state());
  Code* code =
      Isolate::Current()->stub_cache()->FindCallInitialize(
          target->arguments_count(),
          contextual ? RelocInfo::CODE_TARGET_CONTEXT : RelocInfo::CODE_TARGET,
          target->kind());
  SetTargetAtAddress(address, code);
}


void KeyedLoadIC::Clear(Address address, Code* target) {
  if (target->ic_state() == UNINITIALIZED) return;
  // Make sure to also clear the map used in inline fast cases.  If we
  // do not clear these maps, cached code can keep objects alive
  // through the embedded maps.
  SetTargetAtAddress(address, *initialize_stub());
}


void LoadIC::Clear(Address address, Code* target) {
  if (target->ic_state() == UNINITIALIZED) return;
  SetTargetAtAddress(address, *initialize_stub());
}


void StoreIC::Clear(Address address, Code* target) {
  if (target->ic_state() == UNINITIALIZED) return;
  SetTargetAtAddress(address,
      (Code::GetStrictMode(target->extra_ic_state()) == kStrictMode)
        ? *initialize_stub_strict()
        : *initialize_stub());
}


void KeyedStoreIC::Clear(Address address, Code* target) {
  if (target->ic_state() == UNINITIALIZED) return;
  SetTargetAtAddress(address,
      (Code::GetStrictMode(target->extra_ic_state()) == kStrictMode)
        ? *initialize_stub_strict()
        : *initialize_stub());
}


void CompareIC::Clear(Address address, Code* target) {
  ASSERT(target->major_key() == CodeStub::CompareIC);
  CompareIC::State handler_state;
  Token::Value op;
  ICCompareStub::DecodeMinorKey(target->stub_info(), NULL, NULL,
                                &handler_state, &op);
  // Only clear CompareICs that can retain objects.
  if (handler_state != KNOWN_OBJECT) return;
  SetTargetAtAddress(address, GetRawUninitialized(op));
  PatchInlinedSmiCode(address, DISABLE_INLINED_SMI_CHECK);
}


static bool HasInterceptorGetter(JSObject* object) {
  return !object->GetNamedInterceptor()->getter()->IsUndefined();
}


static void LookupForRead(Handle<Object> object,
                          Handle<String> name,
                          LookupResult* lookup) {
  // Skip all the objects with named interceptors, but
  // without actual getter.
  while (true) {
    object->Lookup(*name, lookup);
    // Besides normal conditions (property not found or it's not
    // an interceptor), bail out if lookup is not cacheable: we won't
    // be able to IC it anyway and regular lookup should work fine.
    if (!lookup->IsInterceptor() || !lookup->IsCacheable()) {
      return;
    }

    Handle<JSObject> holder(lookup->holder());
    if (HasInterceptorGetter(*holder)) {
      return;
    }

    holder->LocalLookupRealNamedProperty(*name, lookup);
    if (lookup->IsFound()) {
      ASSERT(!lookup->IsInterceptor());
      return;
    }

    Handle<Object> proto(holder->GetPrototype(), name->GetIsolate());
    if (proto->IsNull()) {
      ASSERT(!lookup->IsFound());
      return;
    }

    object = proto;
  }
}


Handle<Object> CallICBase::TryCallAsFunction(Handle<Object> object) {
  Handle<Object> delegate = Execution::GetFunctionDelegate(object);

  if (delegate->IsJSFunction() && !object->IsJSFunctionProxy()) {
    // Patch the receiver and use the delegate as the function to
    // invoke. This is used for invoking objects as if they were functions.
    const int argc = target()->arguments_count();
    StackFrameLocator locator(isolate());
    JavaScriptFrame* frame = locator.FindJavaScriptFrame(0);
    int index = frame->ComputeExpressionsCount() - (argc + 1);
    frame->SetExpression(index, *object);
  }

  return delegate;
}


void CallICBase::ReceiverToObjectIfRequired(Handle<Object> callee,
                                            Handle<Object> object) {
  while (callee->IsJSFunctionProxy()) {
    callee = Handle<Object>(JSFunctionProxy::cast(*callee)->call_trap(),
                            isolate());
  }

  if (callee->IsJSFunction()) {
    Handle<JSFunction> function = Handle<JSFunction>::cast(callee);
    if (!function->shared()->is_classic_mode() || function->IsBuiltin()) {
      // Do not wrap receiver for strict mode functions or for builtins.
      return;
    }
  }

  // And only wrap string, number or boolean.
  if (object->IsString() || object->IsNumber() || object->IsBoolean()) {
    // Change the receiver to the result of calling ToObject on it.
    const int argc = this->target()->arguments_count();
    StackFrameLocator locator(isolate());
    JavaScriptFrame* frame = locator.FindJavaScriptFrame(0);
    int index = frame->ComputeExpressionsCount() - (argc + 1);
    frame->SetExpression(index, *isolate()->factory()->ToObject(object));
  }
}


MaybeObject* CallICBase::LoadFunction(State state,
                                      Code::ExtraICState extra_ic_state,
                                      Handle<Object> object,
                                      Handle<String> name) {
  // If the object is undefined or null it's illegal to try to get any
  // of its properties; throw a TypeError in that case.
  if (object->IsUndefined() || object->IsNull()) {
    return TypeError("non_object_property_call", object, name);
  }

  // Check if the name is trivially convertible to an index and get
  // the element if so.
  uint32_t index;
  if (name->AsArrayIndex(&index)) {
    Handle<Object> result = Object::GetElement(object, index);
    RETURN_IF_EMPTY_HANDLE(isolate(), result);
    if (result->IsJSFunction()) return *result;

    // Try to find a suitable function delegate for the object at hand.
    result = TryCallAsFunction(result);
    if (result->IsJSFunction()) return *result;

    // Otherwise, it will fail in the lookup step.
  }

  // Lookup the property in the object.
  LookupResult lookup(isolate());
  LookupForRead(object, name, &lookup);

  if (!lookup.IsFound()) {
    // If the object does not have the requested property, check which
    // exception we need to throw.
    return IsUndeclaredGlobal(object)
        ? ReferenceError("not_defined", name)
        : TypeError("undefined_method", object, name);
  }

  // Lookup is valid: Update inline cache and stub cache.
  if (FLAG_use_ic) {
    UpdateCaches(&lookup, state, extra_ic_state, object, name);
  }

  // Get the property.
  PropertyAttributes attr;
  Handle<Object> result =
      Object::GetProperty(object, object, &lookup, name, &attr);
  RETURN_IF_EMPTY_HANDLE(isolate(), result);

  if (lookup.IsInterceptor() && attr == ABSENT) {
    // If the object does not have the requested property, check which
    // exception we need to throw.
    return IsUndeclaredGlobal(object)
        ? ReferenceError("not_defined", name)
        : TypeError("undefined_method", object, name);
  }

  ASSERT(!result->IsTheHole());

  // Make receiver an object if the callee requires it. Strict mode or builtin
  // functions do not wrap the receiver, non-strict functions and objects
  // called as functions do.
  ReceiverToObjectIfRequired(result, object);

  if (result->IsJSFunction()) {
    Handle<JSFunction> function = Handle<JSFunction>::cast(result);
#ifdef ENABLE_DEBUGGER_SUPPORT
    // Handle stepping into a function if step into is active.
    Debug* debug = isolate()->debug();
    if (debug->StepInActive()) {
      // Protect the result in a handle as the debugger can allocate and might
      // cause GC.
      debug->HandleStepIn(function, object, fp(), false);
    }
#endif
    return *function;
  }

  // Try to find a suitable function delegate for the object at hand.
  result = TryCallAsFunction(result);
  if (result->IsJSFunction()) return *result;

  return TypeError("property_not_function", object, name);
}


bool CallICBase::TryUpdateExtraICState(LookupResult* lookup,
                                       Handle<Object> object,
                                       Code::ExtraICState* extra_ic_state) {
  ASSERT(kind_ == Code::CALL_IC);
  if (lookup->type() != CONSTANT_FUNCTION) return false;
  JSFunction* function = lookup->GetConstantFunction();
  if (!function->shared()->HasBuiltinFunctionId()) return false;

  // Fetch the arguments passed to the called function.
  const int argc = target()->arguments_count();
  Address entry = isolate()->c_entry_fp(isolate()->thread_local_top());
  Address fp = Memory::Address_at(entry + ExitFrameConstants::kCallerFPOffset);
  Arguments args(argc + 1,
                 &Memory::Object_at(fp +
                                    StandardFrameConstants::kCallerSPOffset +
                                    argc * kPointerSize));
  switch (function->shared()->builtin_function_id()) {
    case kStringCharCodeAt:
    case kStringCharAt:
      if (object->IsString()) {
        String* string = String::cast(*object);
        // Check there's the right string value or wrapper in the receiver slot.
        ASSERT(string == args[0] || string == JSValue::cast(args[0])->value());
        // If we're in the default (fastest) state and the index is
        // out of bounds, update the state to record this fact.
        if (StringStubState::decode(*extra_ic_state) == DEFAULT_STRING_STUB &&
            argc >= 1 && args[1]->IsNumber()) {
          double index = DoubleToInteger(args.number_at(1));
          if (index < 0 || index >= string->length()) {
            *extra_ic_state =
                StringStubState::update(*extra_ic_state,
                                        STRING_INDEX_OUT_OF_BOUNDS);
            return true;
          }
        }
      }
      break;
    default:
      return false;
  }
  return false;
}


Handle<Code> CallICBase::ComputeMonomorphicStub(LookupResult* lookup,
                                                State state,
                                                Code::ExtraICState extra_state,
                                                Handle<Object> object,
                                                Handle<String> name) {
  int argc = target()->arguments_count();
  Handle<JSObject> holder(lookup->holder());
  switch (lookup->type()) {
    case FIELD: {
      PropertyIndex index = lookup->GetFieldIndex();
      return isolate()->stub_cache()->ComputeCallField(
          argc, kind_, extra_state, name, object, holder, index);
    }
    case CONSTANT_FUNCTION: {
      // Get the constant function and compute the code stub for this
      // call; used for rewriting to monomorphic state and making sure
      // that the code stub is in the stub cache.
      Handle<JSFunction> function(lookup->GetConstantFunction());
      return isolate()->stub_cache()->ComputeCallConstant(
          argc, kind_, extra_state, name, object, holder, function);
    }
    case NORMAL: {
      // If we return a null handle, the IC will not be patched.
      if (!object->IsJSObject()) return Handle<Code>::null();
      Handle<JSObject> receiver = Handle<JSObject>::cast(object);

      if (holder->IsGlobalObject()) {
        Handle<GlobalObject> global = Handle<GlobalObject>::cast(holder);
        Handle<JSGlobalPropertyCell> cell(global->GetPropertyCell(lookup));
        if (!cell->value()->IsJSFunction()) return Handle<Code>::null();
        Handle<JSFunction> function(JSFunction::cast(cell->value()));
        return isolate()->stub_cache()->ComputeCallGlobal(
            argc, kind_, extra_state, name, receiver, global, cell, function);
      } else {
        // There is only one shared stub for calling normalized
        // properties. It does not traverse the prototype chain, so the
        // property must be found in the receiver for the stub to be
        // applicable.
        if (!holder.is_identical_to(receiver)) return Handle<Code>::null();
        return isolate()->stub_cache()->ComputeCallNormal(
            argc, kind_, extra_state);
      }
      break;
    }
    case INTERCEPTOR:
      ASSERT(HasInterceptorGetter(*holder));
      return isolate()->stub_cache()->ComputeCallInterceptor(
          argc, kind_, extra_state, name, object, holder);
    default:
      return Handle<Code>::null();
  }
}


void CallICBase::UpdateCaches(LookupResult* lookup,
                              State state,
                              Code::ExtraICState extra_ic_state,
                              Handle<Object> object,
                              Handle<String> name) {
  // Bail out if we didn't find a result.
  if (!lookup->IsProperty() || !lookup->IsCacheable()) return;

  // Compute the number of arguments.
  int argc = target()->arguments_count();
  Handle<Code> code;
  if (state == UNINITIALIZED) {
    // This is the first time we execute this inline cache.
    // Set the target to the pre monomorphic stub to delay
    // setting the monomorphic state.
    code = isolate()->stub_cache()->ComputeCallPreMonomorphic(
        argc, kind_, extra_ic_state);
  } else if (state == MONOMORPHIC) {
    if (kind_ == Code::CALL_IC &&
        TryUpdateExtraICState(lookup, object, &extra_ic_state)) {
      code = ComputeMonomorphicStub(lookup, state, extra_ic_state,
                                    object, name);
    } else if (kind_ == Code::CALL_IC &&
               TryRemoveInvalidPrototypeDependentStub(target(),
                                                      *object,
                                                      *name)) {
      state = MONOMORPHIC_PROTOTYPE_FAILURE;
      code = ComputeMonomorphicStub(lookup, state, extra_ic_state,
                                    object, name);
    } else {
      code = isolate()->stub_cache()->ComputeCallMegamorphic(
          argc, kind_, extra_ic_state);
    }
  } else {
    code = ComputeMonomorphicStub(lookup, state, extra_ic_state,
                                  object, name);
  }

  // If there's no appropriate stub we simply avoid updating the caches.
  if (code.is_null()) return;

  // Patch the call site depending on the state of the cache.
  switch (state) {
    case UNINITIALIZED:
    case MONOMORPHIC_PROTOTYPE_FAILURE:
    case PREMONOMORPHIC:
      set_target(*code);
      break;
    case MONOMORPHIC:
      if (code->ic_state() != MONOMORPHIC) {
        Map* map = target()->FindFirstMap();
        if (map != NULL) {
          UpdateMegamorphicCache(map, *name, target());
        }
      }
      set_target(*code);
      break;
    case MEGAMORPHIC: {
      // Cache code holding map should be consistent with
      // GenerateMonomorphicCacheProbe. It is not the map which holds the stub.
      Handle<JSObject> cache_object = object->IsJSObject()
          ? Handle<JSObject>::cast(object)
          : Handle<JSObject>(JSObject::cast(object->GetPrototype(isolate())));
      // Update the stub cache.
      UpdateMegamorphicCache(cache_object->map(), *name, *code);
      break;
    }
    case DEBUG_STUB:
      break;
    case POLYMORPHIC:
    case GENERIC:
      UNREACHABLE();
      break;
  }

  TRACE_IC(kind_ == Code::CALL_IC ? "CallIC" : "KeyedCallIC",
           name, state, target());
}


MaybeObject* KeyedCallIC::LoadFunction(State state,
                                       Handle<Object> object,
                                       Handle<Object> key) {
  if (key->IsSymbol()) {
    return CallICBase::LoadFunction(state,
                                    Code::kNoExtraICState,
                                    object,
                                    Handle<String>::cast(key));
  }

  if (object->IsUndefined() || object->IsNull()) {
    return TypeError("non_object_property_call", object, key);
  }

  bool use_ic = FLAG_use_ic && !object->IsAccessCheckNeeded();
  ASSERT(!(use_ic && object->IsJSGlobalProxy()));

  if (use_ic && state != MEGAMORPHIC) {
    int argc = target()->arguments_count();
    Handle<Code> stub = isolate()->stub_cache()->ComputeCallMegamorphic(
        argc, Code::KEYED_CALL_IC, Code::kNoExtraICState);
    if (object->IsJSObject()) {
      Handle<JSObject> receiver = Handle<JSObject>::cast(object);
      if (receiver->elements()->map() ==
          isolate()->heap()->non_strict_arguments_elements_map()) {
        stub = isolate()->stub_cache()->ComputeCallArguments(argc);
      }
    }
    ASSERT(!stub.is_null());
    set_target(*stub);
    TRACE_IC("KeyedCallIC", key, state, target());
  }

  Handle<Object> result = GetProperty(isolate(), object, key);
  RETURN_IF_EMPTY_HANDLE(isolate(), result);

  // Make receiver an object if the callee requires it. Strict mode or builtin
  // functions do not wrap the receiver, non-strict functions and objects
  // called as functions do.
  ReceiverToObjectIfRequired(result, object);
  if (result->IsJSFunction()) return *result;

  result = TryCallAsFunction(result);
  if (result->IsJSFunction()) return *result;

  return TypeError("property_not_function", object, key);
}


MaybeObject* LoadIC::Load(State state,
                          Handle<Object> object,
                          Handle<String> name) {
  // If the object is undefined or null it's illegal to try to get any
  // of its properties; throw a TypeError in that case.
  if (object->IsUndefined() || object->IsNull()) {
    return TypeError("non_object_property_load", object, name);
  }

  if (FLAG_use_ic) {
    // Use specialized code for getting the length of strings and
    // string wrapper objects.  The length property of string wrapper
    // objects is read-only and therefore always returns the length of
    // the underlying string value.  See ECMA-262 15.5.5.1.
    if ((object->IsString() || object->IsStringWrapper()) &&
        name->Equals(isolate()->heap()->length_symbol())) {
      Handle<Code> stub;
      if (state == UNINITIALIZED) {
        stub = pre_monomorphic_stub();
      } else if (state == PREMONOMORPHIC) {
        StringLengthStub string_length_stub(kind(), !object->IsString());
        stub = string_length_stub.GetCode(isolate());
      } else if (state == MONOMORPHIC && object->IsStringWrapper()) {
        StringLengthStub string_length_stub(kind(), true);
        stub = string_length_stub.GetCode(isolate());
      } else if (state != MEGAMORPHIC) {
        ASSERT(state != GENERIC);
        stub = megamorphic_stub();
      }
      if (!stub.is_null()) {
        set_target(*stub);
#ifdef DEBUG
        if (FLAG_trace_ic) PrintF("[LoadIC : +#length /string]\n");
#endif
      }
      // Get the string if we have a string wrapper object.
      Handle<Object> string = object->IsJSValue()
          ? Handle<Object>(Handle<JSValue>::cast(object)->value(), isolate())
          : object;
      return Smi::FromInt(String::cast(*string)->length());
    }

    // Use specialized code for getting the length of arrays.
    if (object->IsJSArray() &&
        name->Equals(isolate()->heap()->length_symbol())) {
      Handle<Code> stub;
      if (state == UNINITIALIZED) {
        stub = pre_monomorphic_stub();
      } else if (state == PREMONOMORPHIC) {
        ArrayLengthStub array_length_stub(kind());
        stub = array_length_stub.GetCode(isolate());
      } else if (state != MEGAMORPHIC) {
        ASSERT(state != GENERIC);
        stub = megamorphic_stub();
      }
      if (!stub.is_null()) {
        set_target(*stub);
#ifdef DEBUG
        if (FLAG_trace_ic) PrintF("[LoadIC : +#length /array]\n");
#endif
      }
      return JSArray::cast(*object)->length();
    }

    // Use specialized code for getting prototype of functions.
    if (object->IsJSFunction() &&
        name->Equals(isolate()->heap()->prototype_symbol()) &&
        Handle<JSFunction>::cast(object)->should_have_prototype()) {
      Handle<Code> stub;
      if (state == UNINITIALIZED) {
        stub = pre_monomorphic_stub();
      } else if (state == PREMONOMORPHIC) {
        FunctionPrototypeStub function_prototype_stub(kind());
        stub = function_prototype_stub.GetCode(isolate());
      } else if (state != MEGAMORPHIC) {
        ASSERT(state != GENERIC);
        stub = megamorphic_stub();
      }
      if (!stub.is_null()) {
        set_target(*stub);
#ifdef DEBUG
        if (FLAG_trace_ic) PrintF("[LoadIC : +#prototype /function]\n");
#endif
      }
      return Accessors::FunctionGetPrototype(*object, 0);
    }
  }

  // Check if the name is trivially convertible to an index and get
  // the element or char if so.
  uint32_t index;
  if (kind() == Code::KEYED_LOAD_IC && name->AsArrayIndex(&index)) {
    // Rewrite to the generic keyed load stub.
    if (FLAG_use_ic) set_target(*generic_stub());
    return Runtime::GetElementOrCharAt(isolate(), object, index);
  }

  // Named lookup in the object.
  LookupResult lookup(isolate());
  LookupForRead(object, name, &lookup);

  // If we did not find a property, check if we need to throw an exception.
  if (!lookup.IsFound()) {
    if (IsUndeclaredGlobal(object)) {
      return ReferenceError("not_defined", name);
    }
    LOG(isolate(), SuspectReadEvent(*name, *object));
  }

  // Update inline cache and stub cache.
  if (FLAG_use_ic) {
    UpdateCaches(&lookup, state, object, name);
  }

  PropertyAttributes attr;
  if (lookup.IsInterceptor() || lookup.IsHandler()) {
    // Get the property.
    Handle<Object> result =
        Object::GetProperty(object, object, &lookup, name, &attr);
    RETURN_IF_EMPTY_HANDLE(isolate(), result);
    // If the property is not present, check if we need to throw an
    // exception.
    if (attr == ABSENT && IsUndeclaredGlobal(object)) {
      return ReferenceError("not_defined", name);
    }
    return *result;
  }

  // Get the property.
  return object->GetProperty(*object, &lookup, *name, &attr);
}


void IC::PatchCache(State state,
                    StrictModeFlag strict_mode,
                    Handle<JSObject> receiver,
                    Handle<String> name,
                    Handle<Code> code) {
  switch (state) {
    case UNINITIALIZED:
    case PREMONOMORPHIC:
    case MONOMORPHIC_PROTOTYPE_FAILURE:
      set_target(*code);
      break;
    case MONOMORPHIC:
      // Only move to megamorphic if the target changes.
      if (target() != *code) {
        // We are transitioning from monomorphic to megamorphic case.
        // Place the current monomorphic stub and stub compiled for
        // the receiver into stub cache.
        Map* map = target()->FindFirstMap();
        if (map != NULL) {
          UpdateMegamorphicCache(map, *name, target());
        }
        UpdateMegamorphicCache(receiver->map(), *name, *code);
        set_target((strict_mode == kStrictMode)
                     ? *megamorphic_stub_strict()
                     : *megamorphic_stub());
      }
      break;
    case MEGAMORPHIC:
      // Update the stub cache.
      UpdateMegamorphicCache(receiver->map(), *name, *code);
      break;
    case POLYMORPHIC:
      // When trying to patch a polymorphic stub with anything other than
      // another polymorphic stub, go generic.
      // TODO(verwaest): Currently we always go generic since no polymorphic
      // stubs enter this code path. Replace with proper updating once named
      // load/store can also be polymorphic.
      set_target((strict_mode == kStrictMode)
                 ? *generic_stub_strict()
                 : *generic_stub());
      break;
    case DEBUG_STUB:
      break;
    case GENERIC:
      UNREACHABLE();
      break;
  }
}


void LoadIC::UpdateCaches(LookupResult* lookup,
                          State state,
                          Handle<Object> object,
                          Handle<String> name) {
  // Bail out if the result is not cacheable.
  if (!lookup->IsCacheable()) return;

  // Loading properties from values is not common, so don't try to
  // deal with non-JS objects here.
  if (!object->IsJSObject()) return;

  Handle<JSObject> receiver = Handle<JSObject>::cast(object);
  Handle<Code> code;
  if (state == UNINITIALIZED) {
    // This is the first time we execute this inline cache.
    // Set the target to the pre monomorphic stub to delay
    // setting the monomorphic state.
    code = pre_monomorphic_stub();
  } else {
    code = ComputeLoadMonomorphic(lookup, receiver, name);
    if (code.is_null()) return;
  }

  PatchCache(state, kNonStrictMode, receiver, name, code);
  TRACE_IC("LoadIC", name, state, target());
}


void IC::UpdateMegamorphicCache(Map* map, String* name, Code* code) {
  // Cache code holding map should be consistent with
  // GenerateMonomorphicCacheProbe.
  isolate()->stub_cache()->Set(name, map, code);
}


Handle<Code> LoadIC::ComputeLoadMonomorphic(LookupResult* lookup,
                                            Handle<JSObject> receiver,
                                            Handle<String> name) {
  if (!lookup->IsProperty()) {
    // Nonexistent property. The result is undefined.
    return isolate()->stub_cache()->ComputeLoadNonexistent(name, receiver);
  }

  // Compute monomorphic stub.
  Handle<JSObject> holder(lookup->holder());
  switch (lookup->type()) {
    case FIELD:
      return isolate()->stub_cache()->ComputeLoadField(
          name, receiver, holder, lookup->GetFieldIndex());
    case CONSTANT_FUNCTION: {
      Handle<JSFunction> constant(lookup->GetConstantFunction());
      return isolate()->stub_cache()->ComputeLoadConstant(
          name, receiver, holder, constant);
    }
    case NORMAL:
      if (holder->IsGlobalObject()) {
        Handle<GlobalObject> global = Handle<GlobalObject>::cast(holder);
        Handle<JSGlobalPropertyCell> cell(global->GetPropertyCell(lookup));
        return isolate()->stub_cache()->ComputeLoadGlobal(
            name, receiver, global, cell, lookup->IsDontDelete());
      }
      // There is only one shared stub for loading normalized
      // properties. It does not traverse the prototype chain, so the
      // property must be found in the receiver for the stub to be
      // applicable.
      if (!holder.is_identical_to(receiver)) break;
      return isolate()->stub_cache()->ComputeLoadNormal();
    case CALLBACKS: {
      Handle<Object> callback(lookup->GetCallbackObject(), isolate());
      if (callback->IsExecutableAccessorInfo()) {
        Handle<ExecutableAccessorInfo> info =
            Handle<ExecutableAccessorInfo>::cast(callback);
        if (v8::ToCData<Address>(info->getter()) == 0) break;
        if (!info->IsCompatibleReceiver(*receiver)) break;
        return isolate()->stub_cache()->ComputeLoadCallback(
            name, receiver, holder, info);
      } else if (callback->IsAccessorPair()) {
        Handle<Object> getter(Handle<AccessorPair>::cast(callback)->getter(),
                              isolate());
        if (!getter->IsJSFunction()) break;
        if (holder->IsGlobalObject()) break;
        if (!holder->HasFastProperties()) break;
        return isolate()->stub_cache()->ComputeLoadViaGetter(
            name, receiver, holder, Handle<JSFunction>::cast(getter));
      }
      // TODO(dcarney): Handle correctly.
      if (callback->IsDeclaredAccessorInfo()) break;
      ASSERT(callback->IsForeign());
      // No IC support for old-style native accessors.
      break;
    }
    case INTERCEPTOR:
      ASSERT(HasInterceptorGetter(*holder));
      return isolate()->stub_cache()->ComputeLoadInterceptor(
          name, receiver, holder);
    default:
      break;
  }
  return Handle<Code>::null();
}


static Handle<Object> TryConvertKey(Handle<Object> key, Isolate* isolate) {
  // This helper implements a few common fast cases for converting
  // non-smi keys of keyed loads/stores to a smi or a string.
  if (key->IsHeapNumber()) {
    double value = Handle<HeapNumber>::cast(key)->value();
    if (isnan(value)) {
      key = isolate->factory()->nan_symbol();
    } else {
      int int_value = FastD2I(value);
      if (value == int_value && Smi::IsValid(int_value)) {
        key = Handle<Smi>(Smi::FromInt(int_value), isolate);
      }
    }
  } else if (key->IsUndefined()) {
    key = isolate->factory()->undefined_symbol();
  }
  return key;
}


static bool AddOneReceiverMapIfMissing(MapHandleList* receiver_maps,
                                       Handle<Map> new_receiver_map) {
  ASSERT(!new_receiver_map.is_null());
  for (int current = 0; current < receiver_maps->length(); ++current) {
    if (!receiver_maps->at(current).is_null() &&
        receiver_maps->at(current).is_identical_to(new_receiver_map)) {
      return false;
    }
  }
  receiver_maps->Add(new_receiver_map);
  return true;
}


static void GetReceiverMapsForStub(Handle<Code> stub,
                                   MapHandleList* result) {
  ASSERT(stub->is_inline_cache_stub());
  ASSERT(stub->is_keyed_load_stub() || stub->is_keyed_store_stub());
  switch (stub->ic_state()) {
    case MONOMORPHIC: {
      Map* map = stub->FindFirstMap();
      if (map != NULL) {
        result->Add(Handle<Map>(map));
      }
      break;
    }
    case POLYMORPHIC: {
      AssertNoAllocation no_allocation;
      int mask = RelocInfo::ModeMask(RelocInfo::EMBEDDED_OBJECT);
      for (RelocIterator it(*stub, mask); !it.done(); it.next()) {
        RelocInfo* info = it.rinfo();
        Handle<Object> object(info->target_object(), stub->GetIsolate());
        ASSERT(object->IsMap());
        AddOneReceiverMapIfMissing(result, Handle<Map>::cast(object));
      }
      break;
    }
    case MEGAMORPHIC:
      break;
    case UNINITIALIZED:
    case PREMONOMORPHIC:
    case MONOMORPHIC_PROTOTYPE_FAILURE:
    case GENERIC:
    case DEBUG_STUB:
      UNREACHABLE();
      break;
  }
}


Handle<Code> KeyedLoadIC::LoadElementStub(Handle<JSObject> receiver) {
  State ic_state = target()->ic_state();

  // Don't handle megamorphic property accesses for INTERCEPTORS or CALLBACKS
  // via megamorphic stubs, since they don't have a map in their relocation info
  // and so the stubs can't be harvested for the object needed for a map check.
  if (target()->type() != Code::NORMAL) {
    TRACE_GENERIC_IC(isolate(), "KeyedIC", "non-NORMAL target type");
    return generic_stub();
  }

  Handle<Map> receiver_map(receiver->map());
  MapHandleList target_receiver_maps;
  if (ic_state == UNINITIALIZED || ic_state == PREMONOMORPHIC) {
    // Optimistically assume that ICs that haven't reached the MONOMORPHIC state
    // yet will do so and stay there.
    return isolate()->stub_cache()->ComputeKeyedLoadElement(receiver_map);
  }

  if (target() == *string_stub()) {
    target_receiver_maps.Add(isolate()->factory()->string_map());
  } else {
    GetReceiverMapsForStub(Handle<Code>(target()), &target_receiver_maps);
    if (target_receiver_maps.length() == 0) {
      return isolate()->stub_cache()->ComputeKeyedLoadElement(receiver_map);
    }
  }

  // The first time a receiver is seen that is a transitioned version of the
  // previous monomorphic receiver type, assume the new ElementsKind is the
  // monomorphic type. This benefits global arrays that only transition
  // once, and all call sites accessing them are faster if they remain
  // monomorphic. If this optimistic assumption is not true, the IC will
  // miss again and it will become polymorphic and support both the
  // untransitioned and transitioned maps.
  if (ic_state == MONOMORPHIC &&
      IsMoreGeneralElementsKindTransition(
          target_receiver_maps.at(0)->elements_kind(),
          receiver->GetElementsKind())) {
    return isolate()->stub_cache()->ComputeKeyedLoadElement(receiver_map);
  }

  ASSERT(ic_state != GENERIC);

  // Determine the list of receiver maps that this call site has seen,
  // adding the map that was just encountered.
  if (!AddOneReceiverMapIfMissing(&target_receiver_maps, receiver_map)) {
    // If the miss wasn't due to an unseen map, a polymorphic stub
    // won't help, use the generic stub.
    TRACE_GENERIC_IC(isolate(), "KeyedIC", "same map added twice");
    return generic_stub();
  }

  // If the maximum number of receiver maps has been exceeded, use the generic
  // version of the IC.
  if (target_receiver_maps.length() > kMaxKeyedPolymorphism) {
    TRACE_GENERIC_IC(isolate(), "KeyedIC", "max polymorph exceeded");
    return generic_stub();
  }

  return isolate()->stub_cache()->ComputeLoadElementPolymorphic(
      &target_receiver_maps);
}


MaybeObject* KeyedLoadIC::Load(State state,
                               Handle<Object> object,
                               Handle<Object> key,
                               ICMissMode miss_mode) {
  // Check for values that can be converted into a symbol directly or
  // is representable as a smi.
  key = TryConvertKey(key, isolate());

  if (key->IsSymbol()) {
    return LoadIC::Load(state, object, Handle<String>::cast(key));
  }

  bool use_ic = FLAG_use_ic && !object->IsAccessCheckNeeded();
  ASSERT(!(use_ic && object->IsJSGlobalProxy()));

  if (use_ic) {
    Handle<Code> stub = generic_stub();
    if (miss_mode != MISS_FORCE_GENERIC) {
      if (object->IsString() && key->IsNumber()) {
        if (state == UNINITIALIZED) {
          stub = string_stub();
        }
      } else if (object->IsJSObject()) {
        Handle<JSObject> receiver = Handle<JSObject>::cast(object);
        if (receiver->elements()->map() ==
            isolate()->heap()->non_strict_arguments_elements_map()) {
          stub = non_strict_arguments_stub();
        } else if (receiver->HasIndexedInterceptor()) {
          stub = indexed_interceptor_stub();
        } else if (key->IsSmi() && (target() != *non_strict_arguments_stub())) {
          stub = LoadElementStub(receiver);
        }
      }
    } else {
      TRACE_GENERIC_IC(isolate(), "KeyedLoadIC", "force generic");
    }
    ASSERT(!stub.is_null());
    set_target(*stub);
    TRACE_IC("KeyedLoadIC", key, state, target());
  }


  return Runtime::GetObjectProperty(isolate(), object, key);
}


Handle<Code> KeyedLoadIC::ComputeLoadMonomorphic(LookupResult* lookup,
                                                 Handle<JSObject> receiver,
                                                 Handle<String> name) {
  // Bail out if we didn't find a result.
  if (!lookup->IsProperty()) return Handle<Code>::null();

  // Compute a monomorphic stub.
  Handle<JSObject> holder(lookup->holder());
  switch (lookup->type()) {
    case FIELD:
      return isolate()->stub_cache()->ComputeKeyedLoadField(
          name, receiver, holder, lookup->GetFieldIndex());
    case CONSTANT_FUNCTION: {
      Handle<JSFunction> constant(lookup->GetConstantFunction());
      return isolate()->stub_cache()->ComputeKeyedLoadConstant(
          name, receiver, holder, constant);
    }
    case CALLBACKS: {
      Handle<Object> callback_object(lookup->GetCallbackObject(), isolate());
      // TODO(dcarney): Handle DeclaredAccessorInfo correctly.
      if (!callback_object->IsExecutableAccessorInfo()) break;
      Handle<ExecutableAccessorInfo> callback =
          Handle<ExecutableAccessorInfo>::cast(callback_object);
      if (v8::ToCData<Address>(callback->getter()) == 0) break;
      if (!callback->IsCompatibleReceiver(*receiver)) break;
      return isolate()->stub_cache()->ComputeKeyedLoadCallback(
          name, receiver, holder, callback);
    }
    case INTERCEPTOR:
      ASSERT(HasInterceptorGetter(lookup->holder()));
      return isolate()->stub_cache()->ComputeKeyedLoadInterceptor(
          name, receiver, holder);
    default:
      // Always rewrite to the generic case so that we do not
      // repeatedly try to rewrite.
      return generic_stub();
  }
  return Handle<Code>::null();
}


static bool StoreICableLookup(LookupResult* lookup) {
  // Bail out if we didn't find a result.
  if (!lookup->IsFound()) return false;

  // Bail out if inline caching is not allowed.
  if (!lookup->IsCacheable()) return false;

  // If the property is read-only, we leave the IC in its current state.
  if (lookup->IsTransition()) {
    return !lookup->GetTransitionDetails().IsReadOnly();
  }
  return !lookup->IsReadOnly();
}


static bool LookupForWrite(Handle<JSObject> receiver,
                           Handle<String> name,
                           LookupResult* lookup) {
  receiver->LocalLookup(*name, lookup);
  if (!lookup->IsFound()) {
    receiver->map()->LookupTransition(*receiver, *name, lookup);
  }
  if (!StoreICableLookup(lookup)) {
    // 2nd chance: There can be accessors somewhere in the prototype chain.
    receiver->Lookup(*name, lookup);
    return lookup->IsPropertyCallbacks() && StoreICableLookup(lookup);
  }

  if (lookup->IsInterceptor() &&
      receiver->GetNamedInterceptor()->setter()->IsUndefined()) {
    receiver->LocalLookupRealNamedProperty(*name, lookup);
    return StoreICableLookup(lookup);
  }

  return true;
}


MaybeObject* StoreIC::Store(State state,
                            StrictModeFlag strict_mode,
                            Handle<Object> object,
                            Handle<String> name,
                            Handle<Object> value,
                            JSReceiver::StoreFromKeyed store_mode) {
  // Handle proxies.
  if (object->IsJSProxy()) {
    return JSProxy::cast(*object)->
        SetProperty(*name, *value, NONE, strict_mode);
  }

  // If the object is undefined or null it's illegal to try to set any
  // properties on it; throw a TypeError in that case.
  if (object->IsUndefined() || object->IsNull()) {
    return TypeError("non_object_property_store", object, name);
  }

  // The length property of string values is read-only. Throw in strict mode.
  if (strict_mode == kStrictMode && object->IsString() &&
      name->Equals(isolate()->heap()->length_symbol())) {
    return TypeError("strict_read_only_property", object, name);
  }

  // Ignore other stores where the receiver is not a JSObject.
  // TODO(1475): Must check prototype chains of object wrappers.
  if (!object->IsJSObject()) return *value;

  Handle<JSObject> receiver = Handle<JSObject>::cast(object);

  // Check if the given name is an array index.
  uint32_t index;
  if (name->AsArrayIndex(&index)) {
    Handle<Object> result =
        JSObject::SetElement(receiver, index, value, NONE, strict_mode);
    RETURN_IF_EMPTY_HANDLE(isolate(), result);
    return *value;
  }

  // Observed objects are always modified through the runtime.
  if (FLAG_harmony_observation && receiver->map()->is_observed()) {
    return receiver->SetProperty(*name, *value, NONE, strict_mode, store_mode);
  }

  // Use specialized code for setting the length of arrays with fast
  // properties. Slow properties might indicate redefinition of the length
  // property.
  if (FLAG_use_ic &&
      receiver->IsJSArray() &&
      name->Equals(isolate()->heap()->length_symbol()) &&
      Handle<JSArray>::cast(receiver)->AllowsSetElementsLength() &&
      receiver->HasFastProperties()) {
    Handle<Code> stub =
        StoreArrayLengthStub(kind(), strict_mode).GetCode(isolate());
    set_target(*stub);
    TRACE_IC("StoreIC", name, state, *stub);
    return receiver->SetProperty(*name, *value, NONE, strict_mode, store_mode);
  }

  if (receiver->IsJSGlobalProxy()) {
    if (FLAG_use_ic && kind() != Code::KEYED_STORE_IC) {
      // Generate a generic stub that goes to the runtime when we see a global
      // proxy as receiver.
      Handle<Code> stub = (strict_mode == kStrictMode)
          ? global_proxy_stub_strict()
          : global_proxy_stub();
      set_target(*stub);
      TRACE_IC("StoreIC", name, state, *stub);
    }
    return receiver->SetProperty(*name, *value, NONE, strict_mode, store_mode);
  }

  LookupResult lookup(isolate());
  if (LookupForWrite(receiver, name, &lookup)) {
    if (FLAG_use_ic) {
      UpdateCaches(&lookup, state, strict_mode, receiver, name, value);
    }
  } else if (strict_mode == kStrictMode &&
             !(lookup.IsProperty() && lookup.IsReadOnly()) &&
             IsUndeclaredGlobal(object)) {
    // Strict mode doesn't allow setting non-existent global property.
    return ReferenceError("not_defined", name);
  }

  // Set the property.
  return receiver->SetProperty(*name, *value, NONE, strict_mode, store_mode);
}


void StoreIC::UpdateCaches(LookupResult* lookup,
                           State state,
                           StrictModeFlag strict_mode,
                           Handle<JSObject> receiver,
                           Handle<String> name,
                           Handle<Object> value) {
  ASSERT(!receiver->IsJSGlobalProxy());
  ASSERT(StoreICableLookup(lookup));
  ASSERT(lookup->IsFound());

  // These are not cacheable, so we never see such LookupResults here.
  ASSERT(!lookup->IsHandler());

  Handle<Code> code =
      ComputeStoreMonomorphic(lookup, strict_mode, receiver, name);
  if (code.is_null()) return;

  PatchCache(state, strict_mode, receiver, name, code);
  TRACE_IC("StoreIC", name, state, target());
}


Handle<Code> StoreIC::ComputeStoreMonomorphic(LookupResult* lookup,
                                              StrictModeFlag strict_mode,
                                              Handle<JSObject> receiver,
                                              Handle<String> name) {
  Handle<JSObject> holder(lookup->holder());
  switch (lookup->type()) {
    case FIELD:
      return isolate()->stub_cache()->ComputeStoreField(
          name, receiver, lookup->GetFieldIndex().field_index(),
          Handle<Map>::null(), strict_mode);
    case NORMAL:
      if (receiver->IsGlobalObject()) {
        // The stub generated for the global object picks the value directly
        // from the property cell. So the property must be directly on the
        // global object.
        Handle<GlobalObject> global = Handle<GlobalObject>::cast(receiver);
        Handle<JSGlobalPropertyCell> cell(global->GetPropertyCell(lookup));
        return isolate()->stub_cache()->ComputeStoreGlobal(
            name, global, cell, strict_mode);
      }
      if (!holder.is_identical_to(receiver)) break;
      return isolate()->stub_cache()->ComputeStoreNormal(strict_mode);
    case CALLBACKS: {
      Handle<Object> callback(lookup->GetCallbackObject(), isolate());
      if (callback->IsExecutableAccessorInfo()) {
        Handle<ExecutableAccessorInfo> info =
            Handle<ExecutableAccessorInfo>::cast(callback);
        if (v8::ToCData<Address>(info->setter()) == 0) break;
        if (!holder->HasFastProperties()) break;
        if (!info->IsCompatibleReceiver(*receiver)) break;
        return isolate()->stub_cache()->ComputeStoreCallback(
            name, receiver, holder, info, strict_mode);
      } else if (callback->IsAccessorPair()) {
        Handle<Object> setter(Handle<AccessorPair>::cast(callback)->setter(),
                              isolate());
        if (!setter->IsJSFunction()) break;
        if (holder->IsGlobalObject()) break;
        if (!holder->HasFastProperties()) break;
        return isolate()->stub_cache()->ComputeStoreViaSetter(
            name, receiver, holder, Handle<JSFunction>::cast(setter),
            strict_mode);
      }
      // TODO(dcarney): Handle correctly.
      if (callback->IsDeclaredAccessorInfo()) break;
      ASSERT(callback->IsForeign());
      // No IC support for old-style native accessors.
      break;
    }
    case INTERCEPTOR:
      ASSERT(!receiver->GetNamedInterceptor()->setter()->IsUndefined());
      return isolate()->stub_cache()->ComputeStoreInterceptor(
          name, receiver, strict_mode);
    case CONSTANT_FUNCTION:
      break;
    case TRANSITION: {
      Handle<Map> transition(lookup->GetTransitionTarget());
      int descriptor = transition->LastAdded();

      DescriptorArray* target_descriptors = transition->instance_descriptors();
      PropertyDetails details = target_descriptors->GetDetails(descriptor);

      if (details.type() != FIELD || details.attributes() != NONE) break;

      int field_index = target_descriptors->GetFieldIndex(descriptor);
      return isolate()->stub_cache()->ComputeStoreField(
          name, receiver, field_index, transition, strict_mode);
    }
    case NONEXISTENT:
    case HANDLER:
      UNREACHABLE();
      break;
  }
  return Handle<Code>::null();
}


Handle<Code> KeyedStoreIC::StoreElementStub(Handle<JSObject> receiver,
                                            StubKind stub_kind,
                                            StrictModeFlag strict_mode) {
  State ic_state = target()->ic_state();
  KeyedAccessGrowMode grow_mode = IsGrowStubKind(stub_kind)
      ? ALLOW_JSARRAY_GROWTH
      : DO_NOT_ALLOW_JSARRAY_GROWTH;

  // Don't handle megamorphic property accesses for INTERCEPTORS or CALLBACKS
  // via megamorphic stubs, since they don't have a map in their relocation info
  // and so the stubs can't be harvested for the object needed for a map check.
  if (target()->type() != Code::NORMAL) {
    TRACE_GENERIC_IC(isolate(), "KeyedIC", "non-NORMAL target type");
    return strict_mode == kStrictMode ? generic_stub_strict() : generic_stub();
  }

  Handle<Map> receiver_map(receiver->map());
  MapHandleList target_receiver_maps;
  if (ic_state == UNINITIALIZED || ic_state == PREMONOMORPHIC) {
    // Optimistically assume that ICs that haven't reached the MONOMORPHIC state
    // yet will do so and stay there.
    Handle<Map> monomorphic_map = ComputeTransitionedMap(receiver, stub_kind);
    stub_kind = GetNoTransitionStubKind(stub_kind);
    return isolate()->stub_cache()->ComputeKeyedStoreElement(
        monomorphic_map, stub_kind, strict_mode, grow_mode);
  }

  GetReceiverMapsForStub(Handle<Code>(target()), &target_receiver_maps);
  if (target_receiver_maps.length() == 0) {
    // Optimistically assume that ICs that haven't reached the MONOMORPHIC state
    // yet will do so and stay there.
    stub_kind = GetNoTransitionStubKind(stub_kind);
    return isolate()->stub_cache()->ComputeKeyedStoreElement(
        receiver_map, stub_kind, strict_mode, grow_mode);
  }
  // The first time a receiver is seen that is a transitioned version of the
  // previous monomorphic receiver type, assume the new ElementsKind is the
  // monomorphic type. This benefits global arrays that only transition
  // once, and all call sites accessing them are faster if they remain
  // monomorphic. If this optimistic assumption is not true, the IC will
  // miss again and it will become polymorphic and support both the
  // untransitioned and transitioned maps.
  if (ic_state == MONOMORPHIC &&
      IsTransitionStubKind(stub_kind) &&
      IsMoreGeneralElementsKindTransition(
          target_receiver_maps.at(0)->elements_kind(),
          receiver->GetElementsKind())) {
    Handle<Map> monomorphic_map = ComputeTransitionedMap(receiver, stub_kind);
    ASSERT(*monomorphic_map != *receiver_map);
    stub_kind = GetNoTransitionStubKind(stub_kind);
    return isolate()->stub_cache()->ComputeKeyedStoreElement(
        monomorphic_map, stub_kind, strict_mode, grow_mode);
  }

  ASSERT(ic_state != GENERIC);

  bool map_added =
      AddOneReceiverMapIfMissing(&target_receiver_maps, receiver_map);

  if (IsTransitionStubKind(stub_kind)) {
    Handle<Map> new_map = ComputeTransitionedMap(receiver, stub_kind);
    map_added |= AddOneReceiverMapIfMissing(&target_receiver_maps, new_map);
  }

  if (!map_added) {
    // If the miss wasn't due to an unseen map, a polymorphic stub
    // won't help, use the generic stub.
    TRACE_GENERIC_IC(isolate(), "KeyedIC", "same map added twice");
    return strict_mode == kStrictMode ? generic_stub_strict() : generic_stub();
  }

  // If the maximum number of receiver maps has been exceeded, use the generic
  // version of the IC.
  if (target_receiver_maps.length() > kMaxKeyedPolymorphism) {
    TRACE_GENERIC_IC(isolate(), "KeyedIC", "max polymorph exceeded");
    return strict_mode == kStrictMode ? generic_stub_strict() : generic_stub();
  }

  if ((Code::GetKeyedAccessGrowMode(target()->extra_ic_state()) ==
       ALLOW_JSARRAY_GROWTH)) {
    grow_mode = ALLOW_JSARRAY_GROWTH;
  }

  return isolate()->stub_cache()->ComputeStoreElementPolymorphic(
      &target_receiver_maps, grow_mode, strict_mode);
}


Handle<Map> KeyedStoreIC::ComputeTransitionedMap(Handle<JSObject> receiver,
                                                 StubKind stub_kind) {
  switch (stub_kind) {
    case STORE_TRANSITION_SMI_TO_OBJECT:
    case STORE_TRANSITION_DOUBLE_TO_OBJECT:
    case STORE_AND_GROW_TRANSITION_SMI_TO_OBJECT:
    case STORE_AND_GROW_TRANSITION_DOUBLE_TO_OBJECT:
      return JSObject::GetElementsTransitionMap(receiver, FAST_ELEMENTS);
    case STORE_TRANSITION_SMI_TO_DOUBLE:
    case STORE_AND_GROW_TRANSITION_SMI_TO_DOUBLE:
      return JSObject::GetElementsTransitionMap(receiver, FAST_DOUBLE_ELEMENTS);
    case STORE_TRANSITION_HOLEY_SMI_TO_OBJECT:
    case STORE_TRANSITION_HOLEY_DOUBLE_TO_OBJECT:
    case STORE_AND_GROW_TRANSITION_HOLEY_SMI_TO_OBJECT:
    case STORE_AND_GROW_TRANSITION_HOLEY_DOUBLE_TO_OBJECT:
      return JSObject::GetElementsTransitionMap(receiver,
                                                FAST_HOLEY_ELEMENTS);
    case STORE_TRANSITION_HOLEY_SMI_TO_DOUBLE:
    case STORE_AND_GROW_TRANSITION_HOLEY_SMI_TO_DOUBLE:
      return JSObject::GetElementsTransitionMap(receiver,
                                                FAST_HOLEY_DOUBLE_ELEMENTS);
    case STORE_NO_TRANSITION:
    case STORE_AND_GROW_NO_TRANSITION:
      return Handle<Map>(receiver->map());
  }
  return Handle<Map>::null();
}


KeyedStoreIC::StubKind KeyedStoreIC::GetStubKind(Handle<JSObject> receiver,
                                                 Handle<Object> key,
                                                 Handle<Object> value) {
  ASSERT(key->IsSmi());
  int index = Smi::cast(*key)->value();
  bool allow_growth = receiver->IsJSArray() &&
      JSArray::cast(*receiver)->length()->IsSmi() &&
      index >= Smi::cast(JSArray::cast(*receiver)->length())->value();

  if (allow_growth) {
    // Handle growing array in stub if necessary.
    if (receiver->HasFastSmiElements()) {
      if (value->IsHeapNumber()) {
        if (receiver->HasFastHoleyElements()) {
          return STORE_AND_GROW_TRANSITION_HOLEY_SMI_TO_DOUBLE;
        } else {
          return STORE_AND_GROW_TRANSITION_SMI_TO_DOUBLE;
        }
      }
      if (value->IsHeapObject()) {
        if (receiver->HasFastHoleyElements()) {
          return STORE_AND_GROW_TRANSITION_HOLEY_SMI_TO_OBJECT;
        } else {
          return STORE_AND_GROW_TRANSITION_SMI_TO_OBJECT;
        }
      }
    } else if (receiver->HasFastDoubleElements()) {
      if (!value->IsSmi() && !value->IsHeapNumber()) {
        if (receiver->HasFastHoleyElements()) {
          return STORE_AND_GROW_TRANSITION_HOLEY_DOUBLE_TO_OBJECT;
        } else {
          return STORE_AND_GROW_TRANSITION_DOUBLE_TO_OBJECT;
        }
      }
    }
    return STORE_AND_GROW_NO_TRANSITION;
  } else {
    // Handle only in-bounds elements accesses.
    if (receiver->HasFastSmiElements()) {
      if (value->IsHeapNumber()) {
        if (receiver->HasFastHoleyElements()) {
          return STORE_TRANSITION_HOLEY_SMI_TO_DOUBLE;
        } else {
          return STORE_TRANSITION_SMI_TO_DOUBLE;
        }
      } else if (value->IsHeapObject()) {
        if (receiver->HasFastHoleyElements()) {
          return STORE_TRANSITION_HOLEY_SMI_TO_OBJECT;
        } else {
          return STORE_TRANSITION_SMI_TO_OBJECT;
        }
      }
    } else if (receiver->HasFastDoubleElements()) {
      if (!value->IsSmi() && !value->IsHeapNumber()) {
        if (receiver->HasFastHoleyElements()) {
          return STORE_TRANSITION_HOLEY_DOUBLE_TO_OBJECT;
        } else {
          return STORE_TRANSITION_DOUBLE_TO_OBJECT;
        }
      }
    }
    return STORE_NO_TRANSITION;
  }
}


MaybeObject* KeyedStoreIC::Store(State state,
                                 StrictModeFlag strict_mode,
                                 Handle<Object> object,
                                 Handle<Object> key,
                                 Handle<Object> value,
                                 ICMissMode miss_mode) {
  // Check for values that can be converted into a symbol directly or
  // is representable as a smi.
  key = TryConvertKey(key, isolate());

  if (key->IsSymbol()) {
    return StoreIC::Store(state,
                          strict_mode,
                          object,
                          Handle<String>::cast(key),
                          value,
                          JSReceiver::MAY_BE_STORE_FROM_KEYED);
  }

  bool use_ic = FLAG_use_ic && !object->IsAccessCheckNeeded() &&
      !(FLAG_harmony_observation && object->IsJSObject() &&
          JSObject::cast(*object)->map()->is_observed());
  ASSERT(!(use_ic && object->IsJSGlobalProxy()));

  if (use_ic) {
    Handle<Code> stub = (strict_mode == kStrictMode)
        ? generic_stub_strict()
        : generic_stub();
    if (miss_mode != MISS_FORCE_GENERIC) {
      if (object->IsJSObject()) {
        Handle<JSObject> receiver = Handle<JSObject>::cast(object);
        if (receiver->elements()->map() ==
            isolate()->heap()->non_strict_arguments_elements_map()) {
          stub = non_strict_arguments_stub();
        } else if (key->IsSmi() && (target() != *non_strict_arguments_stub())) {
          StubKind stub_kind = GetStubKind(receiver, key, value);
          stub = StoreElementStub(receiver, stub_kind, strict_mode);
        }
      }
    } else {
      TRACE_GENERIC_IC(isolate(), "KeyedStoreIC", "force generic");
    }
    ASSERT(!stub.is_null());
    set_target(*stub);
    TRACE_IC("KeyedStoreIC", key, state, target());
  }

  return Runtime::SetObjectProperty(
      isolate(), object , key, value, NONE, strict_mode);
}


Handle<Code> KeyedStoreIC::ComputeStoreMonomorphic(LookupResult* lookup,
                                                   StrictModeFlag strict_mode,
                                                   Handle<JSObject> receiver,
                                                   Handle<String> name) {
  // If the property has a non-field type allowing map transitions
  // where there is extra room in the object, we leave the IC in its
  // current state.
  switch (lookup->type()) {
    case FIELD:
      return isolate()->stub_cache()->ComputeKeyedStoreField(
          name, receiver, lookup->GetFieldIndex().field_index(),
          Handle<Map>::null(), strict_mode);
    case TRANSITION: {
      Handle<Map> transition(lookup->GetTransitionTarget());
      int descriptor = transition->LastAdded();

      DescriptorArray* target_descriptors = transition->instance_descriptors();
      PropertyDetails details = target_descriptors->GetDetails(descriptor);

      if (details.type() == FIELD && details.attributes() == NONE) {
        int field_index = target_descriptors->GetFieldIndex(descriptor);
        return isolate()->stub_cache()->ComputeKeyedStoreField(
            name, receiver, field_index, transition, strict_mode);
      }
      // fall through.
    }
    case NORMAL:
    case CONSTANT_FUNCTION:
    case CALLBACKS:
    case INTERCEPTOR:
      // Always rewrite to the generic case so that we do not
      // repeatedly try to rewrite.
      return (strict_mode == kStrictMode)
          ? generic_stub_strict()
          : generic_stub();
    case HANDLER:
    case NONEXISTENT:
      UNREACHABLE();
      break;
  }
  return Handle<Code>::null();
}


#undef TRACE_IC


// ----------------------------------------------------------------------------
// Static IC stub generators.
//

// Used from ic-<arch>.cc.
RUNTIME_FUNCTION(MaybeObject*, CallIC_Miss) {
  HandleScope scope(isolate);
  ASSERT(args.length() == 2);
  CallIC ic(isolate);
  IC::State state = IC::StateFrom(ic.target(), args[0], args[1]);
  Code::ExtraICState extra_ic_state = ic.target()->extra_ic_state();
  MaybeObject* maybe_result = ic.LoadFunction(state,
                                              extra_ic_state,
                                              args.at<Object>(0),
                                              args.at<String>(1));
  JSFunction* raw_function;
  if (!maybe_result->To(&raw_function)) return maybe_result;

  // The first time the inline cache is updated may be the first time the
  // function it references gets called. If the function is lazily compiled
  // then the first call will trigger a compilation. We check for this case
  // and we do the compilation immediately, instead of waiting for the stub
  // currently attached to the JSFunction object to trigger compilation.
  if (raw_function->is_compiled()) return raw_function;

  Handle<JSFunction> function(raw_function);
  JSFunction::CompileLazy(function, CLEAR_EXCEPTION);
  return *function;
}


// Used from ic-<arch>.cc.
RUNTIME_FUNCTION(MaybeObject*, KeyedCallIC_Miss) {
  HandleScope scope(isolate);
  ASSERT(args.length() == 2);
  KeyedCallIC ic(isolate);
  IC::State state = IC::StateFrom(ic.target(), args[0], args[1]);
  MaybeObject* maybe_result =
      ic.LoadFunction(state, args.at<Object>(0), args.at<Object>(1));
  // Result could be a function or a failure.
  JSFunction* raw_function = NULL;
  if (!maybe_result->To(&raw_function)) return maybe_result;

  if (raw_function->is_compiled()) return raw_function;

  Handle<JSFunction> function(raw_function);
  JSFunction::CompileLazy(function, CLEAR_EXCEPTION);
  return *function;
}


// Used from ic-<arch>.cc.
RUNTIME_FUNCTION(MaybeObject*, LoadIC_Miss) {
  HandleScope scope(isolate);
  ASSERT(args.length() == 2);
  LoadIC ic(IC::NO_EXTRA_FRAME, isolate);
  IC::State state = IC::StateFrom(ic.target(), args[0], args[1]);
  return ic.Load(state, args.at<Object>(0), args.at<String>(1));
}


// Used from ic-<arch>.cc
RUNTIME_FUNCTION(MaybeObject*, KeyedLoadIC_Miss) {
  HandleScope scope(isolate);
  ASSERT(args.length() == 2);
  KeyedLoadIC ic(IC::NO_EXTRA_FRAME, isolate);
  IC::State state = IC::StateFrom(ic.target(), args[0], args[1]);
  return ic.Load(state, args.at<Object>(0), args.at<Object>(1), MISS);
}


RUNTIME_FUNCTION(MaybeObject*, KeyedLoadIC_MissFromStubFailure) {
  HandleScope scope(isolate);
  ASSERT(args.length() == 2);
  KeyedLoadIC ic(IC::EXTRA_CALL_FRAME, isolate);
  IC::State state = IC::StateFrom(ic.target(), args[0], args[1]);
  return ic.Load(state, args.at<Object>(0), args.at<Object>(1), MISS);
}


RUNTIME_FUNCTION(MaybeObject*, KeyedLoadIC_MissForceGeneric) {
  HandleScope scope(isolate);
  ASSERT(args.length() == 2);
  KeyedLoadIC ic(IC::NO_EXTRA_FRAME, isolate);
  IC::State state = IC::StateFrom(ic.target(), args[0], args[1]);
  return ic.Load(state,
                 args.at<Object>(0),
                 args.at<Object>(1),
                 MISS_FORCE_GENERIC);
}


// Used from ic-<arch>.cc.
RUNTIME_FUNCTION(MaybeObject*, StoreIC_Miss) {
  HandleScope scope(isolate);
  ASSERT(args.length() == 3);
  StoreIC ic(isolate);
  IC::State state = IC::StateFrom(ic.target(), args[0], args[1]);
  Code::ExtraICState extra_ic_state = ic.target()->extra_ic_state();
  return ic.Store(state,
                  Code::GetStrictMode(extra_ic_state),
                  args.at<Object>(0),
                  args.at<String>(1),
                  args.at<Object>(2));
}


RUNTIME_FUNCTION(MaybeObject*, StoreIC_ArrayLength) {
  NoHandleAllocation nha(isolate);

  ASSERT(args.length() == 2);
  JSArray* receiver = JSArray::cast(args[0]);
  Object* len = args[1];

  // The generated code should filter out non-Smis before we get here.
  ASSERT(len->IsSmi());

#ifdef DEBUG
  // The length property has to be a writable callback property.
  LookupResult debug_lookup(isolate);
  receiver->LocalLookup(isolate->heap()->length_symbol(), &debug_lookup);
  ASSERT(debug_lookup.IsPropertyCallbacks() && !debug_lookup.IsReadOnly());
#endif

  Object* result;
  MaybeObject* maybe_result = receiver->SetElementsLength(len);
  if (!maybe_result->To(&result)) return maybe_result;

  return len;
}


// Extend storage is called in a store inline cache when
// it is necessary to extend the properties array of a
// JSObject.
RUNTIME_FUNCTION(MaybeObject*, SharedStoreIC_ExtendStorage) {
  NoHandleAllocation na(isolate);
  ASSERT(args.length() == 3);

  // Convert the parameters
  JSObject* object = JSObject::cast(args[0]);
  Map* transition = Map::cast(args[1]);
  Object* value = args[2];

  // Check the object has run out out property space.
  ASSERT(object->HasFastProperties());
  ASSERT(object->map()->unused_property_fields() == 0);

  // Expand the properties array.
  FixedArray* old_storage = object->properties();
  int new_unused = transition->unused_property_fields();
  int new_size = old_storage->length() + new_unused + 1;
  Object* result;
  { MaybeObject* maybe_result = old_storage->CopySize(new_size);
    if (!maybe_result->ToObject(&result)) return maybe_result;
  }
  FixedArray* new_storage = FixedArray::cast(result);
  new_storage->set(old_storage->length(), value);

  // Set the new property value and do the map transition.
  object->set_properties(new_storage);
  object->set_map(transition);

  // Return the stored value.
  return value;
}


// Used from ic-<arch>.cc.
RUNTIME_FUNCTION(MaybeObject*, KeyedStoreIC_Miss) {
  HandleScope scope(isolate);
  ASSERT(args.length() == 3);
  KeyedStoreIC ic(isolate);
  IC::State state = IC::StateFrom(ic.target(), args[0], args[1]);
  Code::ExtraICState extra_ic_state = ic.target()->extra_ic_state();
  return ic.Store(state,
                  Code::GetStrictMode(extra_ic_state),
                  args.at<Object>(0),
                  args.at<Object>(1),
                  args.at<Object>(2),
                  MISS);
}


RUNTIME_FUNCTION(MaybeObject*, KeyedStoreIC_Slow) {
  NoHandleAllocation na(isolate);
  ASSERT(args.length() == 3);
  KeyedStoreIC ic(isolate);
  Code::ExtraICState extra_ic_state = ic.target()->extra_ic_state();
  Handle<Object> object = args.at<Object>(0);
  Handle<Object> key = args.at<Object>(1);
  Handle<Object> value = args.at<Object>(2);
  StrictModeFlag strict_mode = Code::GetStrictMode(extra_ic_state);
  return Runtime::SetObjectProperty(isolate,
                                    object,
                                    key,
                                    value,
                                    NONE,
                                    strict_mode);
}


RUNTIME_FUNCTION(MaybeObject*, KeyedStoreIC_MissForceGeneric) {
  HandleScope scope(isolate);
  ASSERT(args.length() == 3);
  KeyedStoreIC ic(isolate);
  IC::State state = IC::StateFrom(ic.target(), args[0], args[1]);
  Code::ExtraICState extra_ic_state = ic.target()->extra_ic_state();
  return ic.Store(state,
                  Code::GetStrictMode(extra_ic_state),
                  args.at<Object>(0),
                  args.at<Object>(1),
                  args.at<Object>(2),
                  MISS_FORCE_GENERIC);
}


void UnaryOpIC::patch(Code* code) {
  set_target(code);
}


const char* UnaryOpIC::GetName(TypeInfo type_info) {
  switch (type_info) {
    case UNINITIALIZED: return "Uninitialized";
    case SMI: return "Smi";
    case NUMBER: return "Number";
    case GENERIC: return "Generic";
    default: return "Invalid";
  }
}


UnaryOpIC::State UnaryOpIC::ToState(TypeInfo type_info) {
  switch (type_info) {
    case UNINITIALIZED:
      return ::v8::internal::UNINITIALIZED;
    case SMI:
    case NUMBER:
      return MONOMORPHIC;
    case GENERIC:
      return ::v8::internal::GENERIC;
  }
  UNREACHABLE();
  return ::v8::internal::UNINITIALIZED;
}

UnaryOpIC::TypeInfo UnaryOpIC::GetTypeInfo(Handle<Object> operand) {
  ::v8::internal::TypeInfo operand_type =
      ::v8::internal::TypeInfo::TypeFromValue(operand);
  if (operand_type.IsSmi()) {
    return SMI;
  } else if (operand_type.IsNumber()) {
    return NUMBER;
  } else {
    return GENERIC;
  }
}


UnaryOpIC::TypeInfo UnaryOpIC::ComputeNewType(
    TypeInfo current_type,
    TypeInfo previous_type) {
  switch (previous_type) {
    case UNINITIALIZED:
      return current_type;
    case SMI:
      return (current_type == GENERIC) ? GENERIC : NUMBER;
    case NUMBER:
      return GENERIC;
    case GENERIC:
      // We should never do patching if we are in GENERIC state.
      UNREACHABLE();
      return GENERIC;
  }
  UNREACHABLE();
  return GENERIC;
}


void BinaryOpIC::patch(Code* code) {
  set_target(code);
}


const char* BinaryOpIC::GetName(TypeInfo type_info) {
  switch (type_info) {
    case UNINITIALIZED: return "Uninitialized";
    case SMI: return "Smi";
    case INT32: return "Int32";
    case NUMBER: return "Number";
    case ODDBALL: return "Oddball";
    case STRING: return "String";
    case GENERIC: return "Generic";
    default: return "Invalid";
  }
}


BinaryOpIC::State BinaryOpIC::ToState(TypeInfo type_info) {
  switch (type_info) {
    case UNINITIALIZED:
      return ::v8::internal::UNINITIALIZED;
    case SMI:
    case INT32:
    case NUMBER:
    case ODDBALL:
    case STRING:
      return MONOMORPHIC;
    case GENERIC:
      return ::v8::internal::GENERIC;
  }
  UNREACHABLE();
  return ::v8::internal::UNINITIALIZED;
}


RUNTIME_FUNCTION(MaybeObject*, UnaryOp_Patch) {
  ASSERT(args.length() == 4);

  HandleScope scope(isolate);
  Handle<Object> operand = args.at<Object>(0);
  Token::Value op = static_cast<Token::Value>(args.smi_at(1));
  UnaryOverwriteMode mode = static_cast<UnaryOverwriteMode>(args.smi_at(2));
  UnaryOpIC::TypeInfo previous_type =
      static_cast<UnaryOpIC::TypeInfo>(args.smi_at(3));

  UnaryOpIC::TypeInfo type = UnaryOpIC::GetTypeInfo(operand);
  type = UnaryOpIC::ComputeNewType(type, previous_type);

  UnaryOpStub stub(op, mode, type);
  Handle<Code> code = stub.GetCode(isolate);
  if (!code.is_null()) {
    if (FLAG_trace_ic) {
      PrintF("[UnaryOpIC in ");
      JavaScriptFrame::PrintTop(isolate, stdout, false, true);
      PrintF(" (%s->%s)#%s @ %p]\n",
             UnaryOpIC::GetName(previous_type),
             UnaryOpIC::GetName(type),
             Token::Name(op),
             static_cast<void*>(*code));
    }
    UnaryOpIC ic(isolate);
    ic.patch(*code);
  }

  Handle<JSBuiltinsObject> builtins = Handle<JSBuiltinsObject>(
      isolate->thread_local_top()->context_->builtins(), isolate);
  Object* builtin = NULL;  // Initialization calms down the compiler.
  switch (op) {
    case Token::SUB:
      builtin = builtins->javascript_builtin(Builtins::UNARY_MINUS);
      break;
    case Token::BIT_NOT:
      builtin = builtins->javascript_builtin(Builtins::BIT_NOT);
      break;
    default:
      UNREACHABLE();
  }

  Handle<JSFunction> builtin_function(JSFunction::cast(builtin), isolate);

  bool caught_exception;
  Handle<Object> result = Execution::Call(builtin_function, operand, 0, NULL,
                                          &caught_exception);
  if (caught_exception) {
    return Failure::Exception();
  }
  return *result;
}


static BinaryOpIC::TypeInfo TypeInfoFromValue(Handle<Object> value,
                                              Token::Value op) {
  ::v8::internal::TypeInfo type =
      ::v8::internal::TypeInfo::TypeFromValue(value);
  if (type.IsSmi()) return BinaryOpIC::SMI;
  if (type.IsInteger32()) {
    if (kSmiValueSize == 32) return BinaryOpIC::SMI;
    return BinaryOpIC::INT32;
  }
  if (type.IsNumber()) return BinaryOpIC::NUMBER;
  if (type.IsString()) return BinaryOpIC::STRING;
  if (value->IsUndefined()) {
    if (op == Token::BIT_AND ||
        op == Token::BIT_OR ||
        op == Token::BIT_XOR ||
        op == Token::SAR ||
        op == Token::SHL ||
        op == Token::SHR) {
      if (kSmiValueSize == 32) return BinaryOpIC::SMI;
      return BinaryOpIC::INT32;
    }
    return BinaryOpIC::ODDBALL;
  }
  return BinaryOpIC::GENERIC;
}


static BinaryOpIC::TypeInfo InputState(BinaryOpIC::TypeInfo old_type,
                                       Handle<Object> value,
                                       Token::Value op) {
  BinaryOpIC::TypeInfo new_type = TypeInfoFromValue(value, op);
  if (old_type == BinaryOpIC::STRING) {
    if (new_type == BinaryOpIC::STRING) return new_type;
    return BinaryOpIC::GENERIC;
  }
  return Max(old_type, new_type);
}


RUNTIME_FUNCTION(MaybeObject*, BinaryOp_Patch) {
  ASSERT(args.length() == 3);

  HandleScope scope(isolate);
  Handle<Object> left = args.at<Object>(0);
  Handle<Object> right = args.at<Object>(1);
  int key = args.smi_at(2);
  Token::Value op = BinaryOpStub::decode_op_from_minor_key(key);
  BinaryOpIC::TypeInfo previous_left, previous_right, unused_previous_result;
  BinaryOpStub::decode_types_from_minor_key(
      key, &previous_left, &previous_right, &unused_previous_result);

  BinaryOpIC::TypeInfo new_left = InputState(previous_left, left, op);
  BinaryOpIC::TypeInfo new_right = InputState(previous_right, right, op);
  BinaryOpIC::TypeInfo result_type = BinaryOpIC::UNINITIALIZED;

  // STRING is only used for ADD operations.
  if ((new_left == BinaryOpIC::STRING || new_right == BinaryOpIC::STRING) &&
      op != Token::ADD) {
    new_left = new_right = BinaryOpIC::GENERIC;
  }

  BinaryOpIC::TypeInfo new_overall = Max(new_left, new_right);
  BinaryOpIC::TypeInfo previous_overall = Max(previous_left, previous_right);

  if (new_overall == BinaryOpIC::SMI && previous_overall == BinaryOpIC::SMI) {
    if (op == Token::DIV ||
        op == Token::MUL ||
        op == Token::SHR ||
        kSmiValueSize == 32) {
      // Arithmetic on two Smi inputs has yielded a heap number.
      // That is the only way to get here from the Smi stub.
      // With 32-bit Smis, all overflows give heap numbers, but with
      // 31-bit Smis, most operations overflow to int32 results.
      result_type = BinaryOpIC::NUMBER;
    } else {
      // Other operations on SMIs that overflow yield int32s.
      result_type = BinaryOpIC::INT32;
    }
  }
  if (new_overall == BinaryOpIC::INT32 &&
      previous_overall == BinaryOpIC::INT32) {
    if (new_left == previous_left && new_right == previous_right) {
      result_type = BinaryOpIC::NUMBER;
    }
  }

  BinaryOpStub stub(key, new_left, new_right, result_type);
  Handle<Code> code = stub.GetCode(isolate);
  if (!code.is_null()) {
#ifdef DEBUG
    if (FLAG_trace_ic) {
      PrintF("[BinaryOpIC in ");
      JavaScriptFrame::PrintTop(isolate, stdout, false, true);
      PrintF(" ((%s+%s)->((%s+%s)->%s))#%s @ %p]\n",
             BinaryOpIC::GetName(previous_left),
             BinaryOpIC::GetName(previous_right),
             BinaryOpIC::GetName(new_left),
             BinaryOpIC::GetName(new_right),
             BinaryOpIC::GetName(result_type),
             Token::Name(op),
             static_cast<void*>(*code));
    }
#endif
    BinaryOpIC ic(isolate);
    ic.patch(*code);

    // Activate inlined smi code.
    if (previous_overall == BinaryOpIC::UNINITIALIZED) {
      PatchInlinedSmiCode(ic.address(), ENABLE_INLINED_SMI_CHECK);
    }
  }

  Handle<JSBuiltinsObject> builtins = Handle<JSBuiltinsObject>(
      isolate->thread_local_top()->context_->builtins(), isolate);
  Object* builtin = NULL;  // Initialization calms down the compiler.
  switch (op) {
    case Token::ADD:
      builtin = builtins->javascript_builtin(Builtins::ADD);
      break;
    case Token::SUB:
      builtin = builtins->javascript_builtin(Builtins::SUB);
      break;
    case Token::MUL:
      builtin = builtins->javascript_builtin(Builtins::MUL);
      break;
    case Token::DIV:
      builtin = builtins->javascript_builtin(Builtins::DIV);
      break;
    case Token::MOD:
      builtin = builtins->javascript_builtin(Builtins::MOD);
      break;
    case Token::BIT_AND:
      builtin = builtins->javascript_builtin(Builtins::BIT_AND);
      break;
    case Token::BIT_OR:
      builtin = builtins->javascript_builtin(Builtins::BIT_OR);
      break;
    case Token::BIT_XOR:
      builtin = builtins->javascript_builtin(Builtins::BIT_XOR);
      break;
    case Token::SHR:
      builtin = builtins->javascript_builtin(Builtins::SHR);
      break;
    case Token::SAR:
      builtin = builtins->javascript_builtin(Builtins::SAR);
      break;
    case Token::SHL:
      builtin = builtins->javascript_builtin(Builtins::SHL);
      break;
    default:
      UNREACHABLE();
  }

  Handle<JSFunction> builtin_function(JSFunction::cast(builtin), isolate);

  bool caught_exception;
  Handle<Object> builtin_args[] = { right };
  Handle<Object> result = Execution::Call(builtin_function,
                                          left,
                                          ARRAY_SIZE(builtin_args),
                                          builtin_args,
                                          &caught_exception);
  if (caught_exception) {
    return Failure::Exception();
  }
  return *result;
}


Code* CompareIC::GetRawUninitialized(Token::Value op) {
  ICCompareStub stub(op, UNINITIALIZED, UNINITIALIZED, UNINITIALIZED);
  Code* code = NULL;
  CHECK(stub.FindCodeInCache(&code, Isolate::Current()));
  return code;
}


Handle<Code> CompareIC::GetUninitialized(Isolate* isolate, Token::Value op) {
  ICCompareStub stub(op, UNINITIALIZED, UNINITIALIZED, UNINITIALIZED);
  return stub.GetCode(isolate);
}


const char* CompareIC::GetStateName(State state) {
  switch (state) {
    case UNINITIALIZED: return "UNINITIALIZED";
    case SMI: return "SMI";
    case NUMBER: return "NUMBER";
    case SYMBOL: return "SYMBOL";
    case STRING: return "STRING";
    case OBJECT: return "OBJECT";
    case KNOWN_OBJECT: return "KNOWN_OBJECT";
    case GENERIC: return "GENERIC";
    default:
      UNREACHABLE();
      return NULL;
  }
}


static CompareIC::State InputState(CompareIC::State old_state,
                                   Handle<Object> value) {
  switch (old_state) {
    case CompareIC::UNINITIALIZED:
      if (value->IsSmi()) return CompareIC::SMI;
      if (value->IsHeapNumber()) return CompareIC::NUMBER;
      if (value->IsSymbol()) return CompareIC::SYMBOL;
      if (value->IsString()) return CompareIC::STRING;
      if (value->IsJSObject()) return CompareIC::OBJECT;
      break;
    case CompareIC::SMI:
      if (value->IsSmi()) return CompareIC::SMI;
      if (value->IsHeapNumber()) return CompareIC::NUMBER;
      break;
    case CompareIC::NUMBER:
      if (value->IsNumber()) return CompareIC::NUMBER;
      break;
    case CompareIC::SYMBOL:
      if (value->IsSymbol()) return CompareIC::SYMBOL;
      if (value->IsString()) return CompareIC::STRING;
      break;
    case CompareIC::STRING:
      if (value->IsSymbol() || value->IsString()) return CompareIC::STRING;
      break;
    case CompareIC::OBJECT:
      if (value->IsJSObject()) return CompareIC::OBJECT;
      break;
    case CompareIC::GENERIC:
      break;
    case CompareIC::KNOWN_OBJECT:
      UNREACHABLE();
      break;
  }
  return CompareIC::GENERIC;
}


CompareIC::State CompareIC::TargetState(State old_state,
                                        State old_left,
                                        State old_right,
                                        bool has_inlined_smi_code,
                                        Handle<Object> x,
                                        Handle<Object> y) {
  switch (old_state) {
    case UNINITIALIZED:
      if (x->IsSmi() && y->IsSmi()) return SMI;
      if (x->IsNumber() && y->IsNumber()) return NUMBER;
      if (Token::IsOrderedRelationalCompareOp(op_)) {
        // Ordered comparisons treat undefined as NaN, so the
        // NUMBER stub will do the right thing.
        if ((x->IsNumber() && y->IsUndefined()) ||
            (y->IsNumber() && x->IsUndefined())) {
          return NUMBER;
        }
      }
      if (x->IsSymbol() && y->IsSymbol()) {
        // We compare symbols as strings if we need to determine
        // the order in a non-equality compare.
        return Token::IsEqualityOp(op_) ? SYMBOL : STRING;
      }
      if (x->IsString() && y->IsString()) return STRING;
      if (!Token::IsEqualityOp(op_)) return GENERIC;
      if (x->IsJSObject() && y->IsJSObject()) {
        if (Handle<JSObject>::cast(x)->map() ==
            Handle<JSObject>::cast(y)->map()) {
          return KNOWN_OBJECT;
        } else {
          return OBJECT;
        }
      }
      return GENERIC;
    case SMI:
      return x->IsNumber() && y->IsNumber() ? NUMBER : GENERIC;
    case SYMBOL:
      ASSERT(Token::IsEqualityOp(op_));
      return x->IsString() && y->IsString() ? STRING : GENERIC;
    case NUMBER:
      // If the failure was due to one side changing from smi to heap number,
      // then keep the state (if other changed at the same time, we will get
      // a second miss and then go to generic).
      if (old_left == SMI && x->IsHeapNumber()) return NUMBER;
      if (old_right == SMI && y->IsHeapNumber()) return NUMBER;
      return GENERIC;
    case KNOWN_OBJECT:
      ASSERT(Token::IsEqualityOp(op_));
      if (x->IsJSObject() && y->IsJSObject()) return OBJECT;
      return GENERIC;
    case STRING:
    case OBJECT:
    case GENERIC:
      return GENERIC;
  }
  UNREACHABLE();
  return GENERIC;  // Make the compiler happy.
}


void CompareIC::UpdateCaches(Handle<Object> x, Handle<Object> y) {
  HandleScope scope(isolate());
  State previous_left, previous_right, previous_state;
  ICCompareStub::DecodeMinorKey(target()->stub_info(), &previous_left,
                                &previous_right, &previous_state, NULL);
  State new_left = InputState(previous_left, x);
  State new_right = InputState(previous_right, y);
  State state = TargetState(previous_state, previous_left, previous_right,
                            HasInlinedSmiCode(address()), x, y);
  ICCompareStub stub(op_, new_left, new_right, state);
  if (state == KNOWN_OBJECT) {
    stub.set_known_map(Handle<Map>(Handle<JSObject>::cast(x)->map()));
  }
  set_target(*stub.GetCode(isolate()));

#ifdef DEBUG
  if (FLAG_trace_ic) {
    PrintF("[CompareIC in ");
    JavaScriptFrame::PrintTop(isolate(), stdout, false, true);
    PrintF(" ((%s+%s=%s)->(%s+%s=%s))#%s @ %p]\n",
           GetStateName(previous_left),
           GetStateName(previous_right),
           GetStateName(previous_state),
           GetStateName(new_left),
           GetStateName(new_right),
           GetStateName(state),
           Token::Name(op_),
           static_cast<void*>(*stub.GetCode(isolate())));
  }
#endif

  // Activate inlined smi code.
  if (previous_state == UNINITIALIZED) {
    PatchInlinedSmiCode(address(), ENABLE_INLINED_SMI_CHECK);
  }
}


// Used from ICCompareStub::GenerateMiss in code-stubs-<arch>.cc.
RUNTIME_FUNCTION(Code*, CompareIC_Miss) {
  NoHandleAllocation na(isolate);
  ASSERT(args.length() == 3);
  CompareIC ic(isolate, static_cast<Token::Value>(args.smi_at(2)));
  ic.UpdateCaches(args.at<Object>(0), args.at<Object>(1));
  return ic.target();
}


RUNTIME_FUNCTION(MaybeObject*, ToBoolean_Patch) {
  ASSERT(args.length() == 3);

  HandleScope scope(isolate);
  Handle<Object> object = args.at<Object>(0);
  Register tos = Register::from_code(args.smi_at(1));
  ToBooleanStub::Types old_types(args.smi_at(2));

  ToBooleanStub::Types new_types(old_types);
  bool to_boolean_value = new_types.Record(object);
  old_types.TraceTransition(new_types);

  ToBooleanStub stub(tos, new_types);
  Handle<Code> code = stub.GetCode(isolate);
  ToBooleanIC ic(isolate);
  ic.patch(*code);
  return Smi::FromInt(to_boolean_value ? 1 : 0);
}


void ToBooleanIC::patch(Code* code) {
  set_target(code);
}


static const Address IC_utilities[] = {
#define ADDR(name) FUNCTION_ADDR(name),
    IC_UTIL_LIST(ADDR)
    NULL
#undef ADDR
};


Address IC::AddressFromUtilityId(IC::UtilityId id) {
  return IC_utilities[id];
}


} }  // namespace v8::internal