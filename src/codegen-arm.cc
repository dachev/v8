// Copyright 2006-2009 the V8 project authors. All rights reserved.
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

#include "bootstrapper.h"
#include "codegen-inl.h"
#include "debug.h"
#include "parser.h"
#include "register-allocator-inl.h"
#include "runtime.h"
#include "scopes.h"


namespace v8 { namespace internal {

#define __ masm_->

// -------------------------------------------------------------------------
// CodeGenState implementation.

CodeGenState::CodeGenState(CodeGenerator* owner)
    : owner_(owner),
      typeof_state_(NOT_INSIDE_TYPEOF),
      true_target_(NULL),
      false_target_(NULL),
      previous_(NULL) {
  owner_->set_state(this);
}


CodeGenState::CodeGenState(CodeGenerator* owner,
                           TypeofState typeof_state,
                           JumpTarget* true_target,
                           JumpTarget* false_target)
    : owner_(owner),
      typeof_state_(typeof_state),
      true_target_(true_target),
      false_target_(false_target),
      previous_(owner->state()) {
  owner_->set_state(this);
}


CodeGenState::~CodeGenState() {
  ASSERT(owner_->state() == this);
  owner_->set_state(previous_);
}


// -------------------------------------------------------------------------
// CodeGenerator implementation

CodeGenerator::CodeGenerator(int buffer_size, Handle<Script> script,
                             bool is_eval)
    : is_eval_(is_eval),
      script_(script),
      deferred_(8),
      masm_(new MacroAssembler(NULL, buffer_size)),
      scope_(NULL),
      frame_(NULL),
      allocator_(NULL),
      cc_reg_(al),
      state_(NULL),
      function_return_is_shadowed_(false),
      in_spilled_code_(false) {
}


// Calling conventions:
// fp: caller's frame pointer
// sp: stack pointer
// r1: called JS function
// cp: callee's context

void CodeGenerator::GenCode(FunctionLiteral* fun) {
  ZoneList<Statement*>* body = fun->body();

  // Initialize state.
  ASSERT(scope_ == NULL);
  scope_ = fun->scope();
  ASSERT(allocator_ == NULL);
  RegisterAllocator register_allocator(this);
  allocator_ = &register_allocator;
  ASSERT(frame_ == NULL);
  frame_ = new VirtualFrame(this);
  cc_reg_ = al;
  set_in_spilled_code(false);
  {
    CodeGenState state(this);

    // Entry:
    // Stack: receiver, arguments
    // lr: return address
    // fp: caller's frame pointer
    // sp: stack pointer
    // r1: called JS function
    // cp: callee's context
    allocator_->Initialize();
    frame_->Enter();
    // tos: code slot
#ifdef DEBUG
    if (strlen(FLAG_stop_at) > 0 &&
        fun->name()->IsEqualTo(CStrVector(FLAG_stop_at))) {
      frame_->SpillAll();
      __ stop("stop-at");
    }
#endif

    // Allocate space for locals and initialize them.
    frame_->AllocateStackSlots(scope_->num_stack_slots());
    // Initialize the function return target after the locals are set
    // up, because it needs the expected frame height from the frame.
    function_return_.Initialize(this, JumpTarget::BIDIRECTIONAL);
    function_return_is_shadowed_ = false;

    VirtualFrame::SpilledScope spilled_scope(this);
    if (scope_->num_heap_slots() > 0) {
      // Allocate local context.
      // Get outer context and create a new context based on it.
      __ ldr(r0, frame_->Function());
      frame_->EmitPush(r0);
      frame_->CallRuntime(Runtime::kNewContext, 1);  // r0 holds the result

      if (kDebug) {
        JumpTarget verified_true(this);
        __ cmp(r0, Operand(cp));
        verified_true.Branch(eq);
        __ stop("NewContext: r0 is expected to be the same as cp");
        verified_true.Bind();
      }
      // Update context local.
      __ str(cp, frame_->Context());
    }

    // TODO(1241774): Improve this code:
    // 1) only needed if we have a context
    // 2) no need to recompute context ptr every single time
    // 3) don't copy parameter operand code from SlotOperand!
    {
      Comment cmnt2(masm_, "[ copy context parameters into .context");

      // Note that iteration order is relevant here! If we have the same
      // parameter twice (e.g., function (x, y, x)), and that parameter
      // needs to be copied into the context, it must be the last argument
      // passed to the parameter that needs to be copied. This is a rare
      // case so we don't check for it, instead we rely on the copying
      // order: such a parameter is copied repeatedly into the same
      // context location and thus the last value is what is seen inside
      // the function.
      for (int i = 0; i < scope_->num_parameters(); i++) {
        Variable* par = scope_->parameter(i);
        Slot* slot = par->slot();
        if (slot != NULL && slot->type() == Slot::CONTEXT) {
          ASSERT(!scope_->is_global_scope());  // no parameters in global scope
          __ ldr(r1, frame_->ParameterAt(i));
          // Loads r2 with context; used below in RecordWrite.
          __ str(r1, SlotOperand(slot, r2));
          // Load the offset into r3.
          int slot_offset =
              FixedArray::kHeaderSize + slot->index() * kPointerSize;
          __ mov(r3, Operand(slot_offset));
          __ RecordWrite(r2, r3, r1);
        }
      }
    }

    // Store the arguments object.  This must happen after context
    // initialization because the arguments object may be stored in the
    // context.
    if (scope_->arguments() != NULL) {
      ASSERT(scope_->arguments_shadow() != NULL);
      Comment cmnt(masm_, "[ allocate arguments object");
      { Reference shadow_ref(this, scope_->arguments_shadow());
        { Reference arguments_ref(this, scope_->arguments());
          ArgumentsAccessStub stub(ArgumentsAccessStub::NEW_OBJECT);
          __ ldr(r2, frame_->Function());
          // The receiver is below the arguments, the return address,
          // and the frame pointer on the stack.
          const int kReceiverDisplacement = 2 + scope_->num_parameters();
          __ add(r1, fp, Operand(kReceiverDisplacement * kPointerSize));
          __ mov(r0, Operand(Smi::FromInt(scope_->num_parameters())));
          frame_->Adjust(3);
          __ stm(db_w, sp, r0.bit() | r1.bit() | r2.bit());
          frame_->CallStub(&stub, 3);
          frame_->EmitPush(r0);
          arguments_ref.SetValue(NOT_CONST_INIT);
        }
        shadow_ref.SetValue(NOT_CONST_INIT);
      }
      frame_->Drop();  // Value is no longer needed.
    }

    // Generate code to 'execute' declarations and initialize functions
    // (source elements). In case of an illegal redeclaration we need to
    // handle that instead of processing the declarations.
    if (scope_->HasIllegalRedeclaration()) {
      Comment cmnt(masm_, "[ illegal redeclarations");
      scope_->VisitIllegalRedeclaration(this);
    } else {
      Comment cmnt(masm_, "[ declarations");
      ProcessDeclarations(scope_->declarations());
      // Bail out if a stack-overflow exception occurred when processing
      // declarations.
      if (HasStackOverflow()) return;
    }

    if (FLAG_trace) {
      frame_->CallRuntime(Runtime::kTraceEnter, 0);
      // Ignore the return value.
    }
    CheckStack();

    // Compile the body of the function in a vanilla state. Don't
    // bother compiling all the code if the scope has an illegal
    // redeclaration.
    if (!scope_->HasIllegalRedeclaration()) {
      Comment cmnt(masm_, "[ function body");
#ifdef DEBUG
      bool is_builtin = Bootstrapper::IsActive();
      bool should_trace =
          is_builtin ? FLAG_trace_builtin_calls : FLAG_trace_calls;
      if (should_trace) {
        frame_->CallRuntime(Runtime::kDebugTrace, 0);
        // Ignore the return value.
      }
#endif
      VisitStatementsAndSpill(body);
    }
  }

  // Generate the return sequence if necessary.
  if (frame_ != NULL || function_return_.is_linked()) {
    // exit
    // r0: result
    // sp: stack pointer
    // fp: frame pointer
    // pp: parameter pointer
    // cp: callee's context
    __ mov(r0, Operand(Factory::undefined_value()));

    function_return_.Bind();
    if (FLAG_trace) {
      // Push the return value on the stack as the parameter.
      // Runtime::TraceExit returns the parameter as it is.
      frame_->EmitPush(r0);
      frame_->CallRuntime(Runtime::kTraceExit, 1);
    }

    // Tear down the frame which will restore the caller's frame pointer and
    // the link register.
    frame_->Exit();

    __ add(sp, sp, Operand((scope_->num_parameters() + 1) * kPointerSize));
    __ mov(pc, lr);
  }

  // Code generation state must be reset.
  ASSERT(!has_cc());
  ASSERT(state_ == NULL);
  ASSERT(!function_return_is_shadowed_);
  function_return_.Unuse();
  DeleteFrame();

  // Process any deferred code using the register allocator.
  if (HasStackOverflow()) {
    ClearDeferred();
  } else {
    ProcessDeferred();
  }

  allocator_ = NULL;
  scope_ = NULL;
}


MemOperand CodeGenerator::SlotOperand(Slot* slot, Register tmp) {
  // Currently, this assertion will fail if we try to assign to
  // a constant variable that is constant because it is read-only
  // (such as the variable referring to a named function expression).
  // We need to implement assignments to read-only variables.
  // Ideally, we should do this during AST generation (by converting
  // such assignments into expression statements); however, in general
  // we may not be able to make the decision until past AST generation,
  // that is when the entire program is known.
  ASSERT(slot != NULL);
  int index = slot->index();
  switch (slot->type()) {
    case Slot::PARAMETER:
      return frame_->ParameterAt(index);

    case Slot::LOCAL:
      return frame_->LocalAt(index);

    case Slot::CONTEXT: {
      // Follow the context chain if necessary.
      ASSERT(!tmp.is(cp));  // do not overwrite context register
      Register context = cp;
      int chain_length = scope()->ContextChainLength(slot->var()->scope());
      for (int i = 0; i < chain_length; i++) {
        // Load the closure.
        // (All contexts, even 'with' contexts, have a closure,
        // and it is the same for all contexts inside a function.
        // There is no need to go to the function context first.)
        __ ldr(tmp, ContextOperand(context, Context::CLOSURE_INDEX));
        // Load the function context (which is the incoming, outer context).
        __ ldr(tmp, FieldMemOperand(tmp, JSFunction::kContextOffset));
        context = tmp;
      }
      // We may have a 'with' context now. Get the function context.
      // (In fact this mov may never be the needed, since the scope analysis
      // may not permit a direct context access in this case and thus we are
      // always at a function context. However it is safe to dereference be-
      // cause the function context of a function context is itself. Before
      // deleting this mov we should try to create a counter-example first,
      // though...)
      __ ldr(tmp, ContextOperand(context, Context::FCONTEXT_INDEX));
      return ContextOperand(tmp, index);
    }

    default:
      UNREACHABLE();
      return MemOperand(r0, 0);
  }
}


MemOperand CodeGenerator::ContextSlotOperandCheckExtensions(
    Slot* slot,
    Register tmp,
    Register tmp2,
    JumpTarget* slow) {
  ASSERT(slot->type() == Slot::CONTEXT);
  Register context = cp;

  for (Scope* s = scope(); s != slot->var()->scope(); s = s->outer_scope()) {
    if (s->num_heap_slots() > 0) {
      if (s->calls_eval()) {
        // Check that extension is NULL.
        __ ldr(tmp2, ContextOperand(context, Context::EXTENSION_INDEX));
        __ tst(tmp2, tmp2);
        slow->Branch(ne);
      }
      __ ldr(tmp, ContextOperand(context, Context::CLOSURE_INDEX));
      __ ldr(tmp, FieldMemOperand(tmp, JSFunction::kContextOffset));
      context = tmp;
    }
  }
  // Check that last extension is NULL.
  __ ldr(tmp2, ContextOperand(context, Context::EXTENSION_INDEX));
  __ tst(tmp2, tmp2);
  slow->Branch(ne);
  __ ldr(tmp, ContextOperand(context, Context::FCONTEXT_INDEX));
  return ContextOperand(tmp, slot->index());
}


void CodeGenerator::LoadConditionAndSpill(Expression* expression,
                                          TypeofState typeof_state,
                                          JumpTarget* true_target,
                                          JumpTarget* false_target,
                                          bool force_control) {
  ASSERT(in_spilled_code());
  set_in_spilled_code(false);
  LoadCondition(expression, typeof_state, true_target, false_target,
                force_control);
  if (frame_ != NULL) {
    frame_->SpillAll();
  }
  set_in_spilled_code(true);
}


// Loads a value on TOS. If it is a boolean value, the result may have been
// (partially) translated into branches, or it may have set the condition
// code register. If force_cc is set, the value is forced to set the
// condition code register and no value is pushed. If the condition code
// register was set, has_cc() is true and cc_reg_ contains the condition to
// test for 'true'.
void CodeGenerator::LoadCondition(Expression* x,
                                  TypeofState typeof_state,
                                  JumpTarget* true_target,
                                  JumpTarget* false_target,
                                  bool force_cc) {
  ASSERT(!in_spilled_code());
  ASSERT(!has_cc());
  int original_height = frame_->height();

  { CodeGenState new_state(this, typeof_state, true_target, false_target);
    Visit(x);

    // If we hit a stack overflow, we may not have actually visited
    // the expression.  In that case, we ensure that we have a
    // valid-looking frame state because we will continue to generate
    // code as we unwind the C++ stack.
    //
    // It's possible to have both a stack overflow and a valid frame
    // state (eg, a subexpression overflowed, visiting it returned
    // with a dummied frame state, and visiting this expression
    // returned with a normal-looking state).
    if (HasStackOverflow() &&
        has_valid_frame() &&
        !has_cc() &&
        frame_->height() == original_height) {
      true_target->Jump();
    }
  }
  if (force_cc && frame_ != NULL && !has_cc()) {
    // Convert the TOS value to a boolean in the condition code register.
    ToBoolean(true_target, false_target);
  }
  ASSERT(!force_cc || !has_valid_frame() || has_cc());
  ASSERT(!has_valid_frame() ||
         (has_cc() && frame_->height() == original_height) ||
         (!has_cc() && frame_->height() == original_height + 1));
}


void CodeGenerator::LoadAndSpill(Expression* expression,
                                 TypeofState typeof_state) {
  ASSERT(in_spilled_code());
  set_in_spilled_code(false);
  Load(expression, typeof_state);
  frame_->SpillAll();
  set_in_spilled_code(true);
}


void CodeGenerator::Load(Expression* x, TypeofState typeof_state) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  ASSERT(!in_spilled_code());
  JumpTarget true_target(this);
  JumpTarget false_target(this);
  LoadCondition(x, typeof_state, &true_target, &false_target, false);

  if (has_cc()) {
    // Convert cc_reg_ into a boolean value.
    JumpTarget loaded(this);
    JumpTarget materialize_true(this);
    materialize_true.Branch(cc_reg_);
    __ mov(r0, Operand(Factory::false_value()));
    frame_->EmitPush(r0);
    loaded.Jump();
    materialize_true.Bind();
    __ mov(r0, Operand(Factory::true_value()));
    frame_->EmitPush(r0);
    loaded.Bind();
    cc_reg_ = al;
  }

  if (true_target.is_linked() || false_target.is_linked()) {
    // We have at least one condition value that has been "translated"
    // into a branch, thus it needs to be loaded explicitly.
    JumpTarget loaded(this);
    if (frame_ != NULL) {
      loaded.Jump();  // Don't lose the current TOS.
    }
    bool both = true_target.is_linked() && false_target.is_linked();
    // Load "true" if necessary.
    if (true_target.is_linked()) {
      true_target.Bind();
      __ mov(r0, Operand(Factory::true_value()));
      frame_->EmitPush(r0);
    }
    // If both "true" and "false" need to be loaded jump across the code for
    // "false".
    if (both) {
      loaded.Jump();
    }
    // Load "false" if necessary.
    if (false_target.is_linked()) {
      false_target.Bind();
      __ mov(r0, Operand(Factory::false_value()));
      frame_->EmitPush(r0);
    }
    // A value is loaded on all paths reaching this point.
    loaded.Bind();
  }
  ASSERT(has_valid_frame());
  ASSERT(!has_cc());
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::LoadGlobal() {
  VirtualFrame::SpilledScope spilled_scope(this);
  __ ldr(r0, GlobalObject());
  frame_->EmitPush(r0);
}


void CodeGenerator::LoadGlobalReceiver(Register scratch) {
  VirtualFrame::SpilledScope spilled_scope(this);
  __ ldr(scratch, ContextOperand(cp, Context::GLOBAL_INDEX));
  __ ldr(scratch,
         FieldMemOperand(scratch, GlobalObject::kGlobalReceiverOffset));
  frame_->EmitPush(scratch);
}


// TODO(1241834): Get rid of this function in favor of just using Load, now
// that we have the INSIDE_TYPEOF typeof state. => Need to handle global
// variables w/o reference errors elsewhere.
void CodeGenerator::LoadTypeofExpression(Expression* x) {
  VirtualFrame::SpilledScope spilled_scope(this);
  Variable* variable = x->AsVariableProxy()->AsVariable();
  if (variable != NULL && !variable->is_this() && variable->is_global()) {
    // NOTE: This is somewhat nasty. We force the compiler to load
    // the variable as if through '<global>.<variable>' to make sure we
    // do not get reference errors.
    Slot global(variable, Slot::CONTEXT, Context::GLOBAL_INDEX);
    Literal key(variable->name());
    // TODO(1241834): Fetch the position from the variable instead of using
    // no position.
    Property property(&global, &key, RelocInfo::kNoPosition);
    LoadAndSpill(&property);
  } else {
    LoadAndSpill(x, INSIDE_TYPEOF);
  }
}


Reference::Reference(CodeGenerator* cgen, Expression* expression)
    : cgen_(cgen), expression_(expression), type_(ILLEGAL) {
  cgen->LoadReference(this);
}


Reference::~Reference() {
  cgen_->UnloadReference(this);
}


void CodeGenerator::LoadReference(Reference* ref) {
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ LoadReference");
  Expression* e = ref->expression();
  Property* property = e->AsProperty();
  Variable* var = e->AsVariableProxy()->AsVariable();

  if (property != NULL) {
    // The expression is either a property or a variable proxy that rewrites
    // to a property.
    LoadAndSpill(property->obj());
    // We use a named reference if the key is a literal symbol, unless it is
    // a string that can be legally parsed as an integer.  This is because
    // otherwise we will not get into the slow case code that handles [] on
    // String objects.
    Literal* literal = property->key()->AsLiteral();
    uint32_t dummy;
    if (literal != NULL &&
        literal->handle()->IsSymbol() &&
        !String::cast(*(literal->handle()))->AsArrayIndex(&dummy)) {
      ref->set_type(Reference::NAMED);
    } else {
      LoadAndSpill(property->key());
      ref->set_type(Reference::KEYED);
    }
  } else if (var != NULL) {
    // The expression is a variable proxy that does not rewrite to a
    // property.  Global variables are treated as named property references.
    if (var->is_global()) {
      LoadGlobal();
      ref->set_type(Reference::NAMED);
    } else {
      ASSERT(var->slot() != NULL);
      ref->set_type(Reference::SLOT);
    }
  } else {
    // Anything else is a runtime error.
    LoadAndSpill(e);
    frame_->CallRuntime(Runtime::kThrowReferenceError, 1);
  }
}


void CodeGenerator::UnloadReference(Reference* ref) {
  VirtualFrame::SpilledScope spilled_scope(this);
  // Pop a reference from the stack while preserving TOS.
  Comment cmnt(masm_, "[ UnloadReference");
  int size = ref->size();
  if (size > 0) {
    frame_->EmitPop(r0);
    frame_->Drop(size);
    frame_->EmitPush(r0);
  }
}


// ECMA-262, section 9.2, page 30: ToBoolean(). Convert the given
// register to a boolean in the condition code register. The code
// may jump to 'false_target' in case the register converts to 'false'.
void CodeGenerator::ToBoolean(JumpTarget* true_target,
                              JumpTarget* false_target) {
  VirtualFrame::SpilledScope spilled_scope(this);
  // Note: The generated code snippet does not change stack variables.
  //       Only the condition code should be set.
  frame_->EmitPop(r0);

  // Fast case checks

  // Check if the value is 'false'.
  __ cmp(r0, Operand(Factory::false_value()));
  false_target->Branch(eq);

  // Check if the value is 'true'.
  __ cmp(r0, Operand(Factory::true_value()));
  true_target->Branch(eq);

  // Check if the value is 'undefined'.
  __ cmp(r0, Operand(Factory::undefined_value()));
  false_target->Branch(eq);

  // Check if the value is a smi.
  __ cmp(r0, Operand(Smi::FromInt(0)));
  false_target->Branch(eq);
  __ tst(r0, Operand(kSmiTagMask));
  true_target->Branch(eq);

  // Slow case: call the runtime.
  frame_->EmitPush(r0);
  frame_->CallRuntime(Runtime::kToBool, 1);
  // Convert the result (r0) to a condition code.
  __ cmp(r0, Operand(Factory::false_value()));

  cc_reg_ = ne;
}


class GetPropertyStub : public CodeStub {
 public:
  GetPropertyStub() { }

 private:
  Major MajorKey() { return GetProperty; }
  int MinorKey() { return 0; }
  void Generate(MacroAssembler* masm);
};


class SetPropertyStub : public CodeStub {
 public:
  SetPropertyStub() { }

 private:
  Major MajorKey() { return SetProperty; }
  int MinorKey() { return 0; }
  void Generate(MacroAssembler* masm);
};


class GenericBinaryOpStub : public CodeStub {
 public:
  explicit GenericBinaryOpStub(Token::Value op) : op_(op) { }

 private:
  Token::Value op_;

  Major MajorKey() { return GenericBinaryOp; }
  int MinorKey() { return static_cast<int>(op_); }
  void Generate(MacroAssembler* masm);

  const char* GetName() {
    switch (op_) {
      case Token::ADD: return "GenericBinaryOpStub_ADD";
      case Token::SUB: return "GenericBinaryOpStub_SUB";
      case Token::MUL: return "GenericBinaryOpStub_MUL";
      case Token::DIV: return "GenericBinaryOpStub_DIV";
      case Token::BIT_OR: return "GenericBinaryOpStub_BIT_OR";
      case Token::BIT_AND: return "GenericBinaryOpStub_BIT_AND";
      case Token::BIT_XOR: return "GenericBinaryOpStub_BIT_XOR";
      case Token::SAR: return "GenericBinaryOpStub_SAR";
      case Token::SHL: return "GenericBinaryOpStub_SHL";
      case Token::SHR: return "GenericBinaryOpStub_SHR";
      default:         return "GenericBinaryOpStub";
    }
  }

#ifdef DEBUG
  void Print() { PrintF("GenericBinaryOpStub (%s)\n", Token::String(op_)); }
#endif
};


void CodeGenerator::GenericBinaryOperation(Token::Value op) {
  VirtualFrame::SpilledScope spilled_scope(this);
  // sp[0] : y
  // sp[1] : x
  // result : r0

  // Stub is entered with a call: 'return address' is in lr.
  switch (op) {
    case Token::ADD:  // fall through.
    case Token::SUB:  // fall through.
    case Token::MUL:
    case Token::BIT_OR:
    case Token::BIT_AND:
    case Token::BIT_XOR:
    case Token::SHL:
    case Token::SHR:
    case Token::SAR: {
      frame_->EmitPop(r0);  // r0 : y
      frame_->EmitPop(r1);  // r1 : x
      GenericBinaryOpStub stub(op);
      frame_->CallStub(&stub, 0);
      break;
    }

    case Token::DIV: {
      Result arg_count = allocator_->Allocate(r0);
      ASSERT(arg_count.is_valid());
      __ mov(arg_count.reg(), Operand(1));
      frame_->InvokeBuiltin(Builtins::DIV, CALL_JS, &arg_count, 2);
      break;
    }

    case Token::MOD: {
      Result arg_count = allocator_->Allocate(r0);
      ASSERT(arg_count.is_valid());
      __ mov(arg_count.reg(), Operand(1));
      frame_->InvokeBuiltin(Builtins::MOD, CALL_JS, &arg_count, 2);
      break;
    }

    case Token::COMMA:
      frame_->EmitPop(r0);
      // simply discard left value
      frame_->Drop();
      break;

    default:
      // Other cases should have been handled before this point.
      UNREACHABLE();
      break;
  }
}


class DeferredInlineSmiOperation: public DeferredCode {
 public:
  DeferredInlineSmiOperation(CodeGenerator* generator,
                             Token::Value op,
                             int value,
                             bool reversed)
      : DeferredCode(generator),
        op_(op),
        value_(value),
        reversed_(reversed) {
    set_comment("[ DeferredInlinedSmiOperation");
  }

  virtual void Generate();

 private:
  Token::Value op_;
  int value_;
  bool reversed_;
};


void DeferredInlineSmiOperation::Generate() {
  enter()->Bind();
  VirtualFrame::SpilledScope spilled_scope(generator());

  switch (op_) {
    case Token::ADD: {
      if (reversed_) {
        // revert optimistic add
        __ sub(r0, r0, Operand(Smi::FromInt(value_)));
        __ mov(r1, Operand(Smi::FromInt(value_)));
      } else {
        // revert optimistic add
        __ sub(r1, r0, Operand(Smi::FromInt(value_)));
        __ mov(r0, Operand(Smi::FromInt(value_)));
      }
      break;
    }

    case Token::SUB: {
      if (reversed_) {
        // revert optimistic sub
        __ rsb(r0, r0, Operand(Smi::FromInt(value_)));
        __ mov(r1, Operand(Smi::FromInt(value_)));
      } else {
        __ add(r1, r0, Operand(Smi::FromInt(value_)));
        __ mov(r0, Operand(Smi::FromInt(value_)));
      }
      break;
    }

    case Token::BIT_OR:
    case Token::BIT_XOR:
    case Token::BIT_AND: {
      if (reversed_) {
        __ mov(r1, Operand(Smi::FromInt(value_)));
      } else {
        __ mov(r1, Operand(r0));
        __ mov(r0, Operand(Smi::FromInt(value_)));
      }
      break;
    }

    case Token::SHL:
    case Token::SHR:
    case Token::SAR: {
      if (!reversed_) {
        __ mov(r1, Operand(r0));
        __ mov(r0, Operand(Smi::FromInt(value_)));
      } else {
        UNREACHABLE();  // should have been handled in SmiOperation
      }
      break;
    }

    default:
      // other cases should have been handled before this point.
      UNREACHABLE();
      break;
  }

  GenericBinaryOpStub igostub(op_);
  Result arg0 = generator()->allocator()->Allocate(r1);
  ASSERT(arg0.is_valid());
  Result arg1 = generator()->allocator()->Allocate(r0);
  ASSERT(arg1.is_valid());
  generator()->frame()->CallStub(&igostub, &arg0, &arg1);
  exit_.Jump();
}


void CodeGenerator::SmiOperation(Token::Value op,
                                 Handle<Object> value,
                                 bool reversed) {
  VirtualFrame::SpilledScope spilled_scope(this);
  // NOTE: This is an attempt to inline (a bit) more of the code for
  // some possible smi operations (like + and -) when (at least) one
  // of the operands is a literal smi. With this optimization, the
  // performance of the system is increased by ~15%, and the generated
  // code size is increased by ~1% (measured on a combination of
  // different benchmarks).

  // sp[0] : operand

  int int_value = Smi::cast(*value)->value();

  JumpTarget exit(this);
  frame_->EmitPop(r0);

  switch (op) {
    case Token::ADD: {
      DeferredCode* deferred =
        new DeferredInlineSmiOperation(this, op, int_value, reversed);

      __ add(r0, r0, Operand(value), SetCC);
      deferred->enter()->Branch(vs);
      __ tst(r0, Operand(kSmiTagMask));
      deferred->enter()->Branch(ne);
      deferred->BindExit();
      break;
    }

    case Token::SUB: {
      DeferredCode* deferred =
        new DeferredInlineSmiOperation(this, op, int_value, reversed);

      if (!reversed) {
        __ sub(r0, r0, Operand(value), SetCC);
      } else {
        __ rsb(r0, r0, Operand(value), SetCC);
      }
      deferred->enter()->Branch(vs);
      __ tst(r0, Operand(kSmiTagMask));
      deferred->enter()->Branch(ne);
      deferred->BindExit();
      break;
    }

    case Token::BIT_OR:
    case Token::BIT_XOR:
    case Token::BIT_AND: {
      DeferredCode* deferred =
        new DeferredInlineSmiOperation(this, op, int_value, reversed);
      __ tst(r0, Operand(kSmiTagMask));
      deferred->enter()->Branch(ne);
      switch (op) {
        case Token::BIT_OR:  __ orr(r0, r0, Operand(value)); break;
        case Token::BIT_XOR: __ eor(r0, r0, Operand(value)); break;
        case Token::BIT_AND: __ and_(r0, r0, Operand(value)); break;
        default: UNREACHABLE();
      }
      deferred->BindExit();
      break;
    }

    case Token::SHL:
    case Token::SHR:
    case Token::SAR: {
      if (reversed) {
        __ mov(ip, Operand(value));
        frame_->EmitPush(ip);
        frame_->EmitPush(r0);
        GenericBinaryOperation(op);

      } else {
        int shift_value = int_value & 0x1f;  // least significant 5 bits
        DeferredCode* deferred =
          new DeferredInlineSmiOperation(this, op, shift_value, false);
        __ tst(r0, Operand(kSmiTagMask));
        deferred->enter()->Branch(ne);
        __ mov(r2, Operand(r0, ASR, kSmiTagSize));  // remove tags
        switch (op) {
          case Token::SHL: {
            __ mov(r2, Operand(r2, LSL, shift_value));
            // check that the *unsigned* result fits in a smi
            __ add(r3, r2, Operand(0x40000000), SetCC);
            deferred->enter()->Branch(mi);
            break;
          }
          case Token::SHR: {
            // LSR by immediate 0 means shifting 32 bits.
            if (shift_value != 0) {
              __ mov(r2, Operand(r2, LSR, shift_value));
            }
            // check that the *unsigned* result fits in a smi
            // neither of the two high-order bits can be set:
            // - 0x80000000: high bit would be lost when smi tagging
            // - 0x40000000: this number would convert to negative when
            // smi tagging these two cases can only happen with shifts
            // by 0 or 1 when handed a valid smi
            __ and_(r3, r2, Operand(0xc0000000), SetCC);
            deferred->enter()->Branch(ne);
            break;
          }
          case Token::SAR: {
            if (shift_value != 0) {
              // ASR by immediate 0 means shifting 32 bits.
              __ mov(r2, Operand(r2, ASR, shift_value));
            }
            break;
          }
          default: UNREACHABLE();
        }
        __ mov(r0, Operand(r2, LSL, kSmiTagSize));
        deferred->BindExit();
      }
      break;
    }

    default:
      if (!reversed) {
        frame_->EmitPush(r0);
        __ mov(r0, Operand(value));
        frame_->EmitPush(r0);
      } else {
        __ mov(ip, Operand(value));
        frame_->EmitPush(ip);
        frame_->EmitPush(r0);
      }
      GenericBinaryOperation(op);
      break;
  }

  exit.Bind();
}


void CodeGenerator::Comparison(Condition cc, bool strict) {
  VirtualFrame::SpilledScope spilled_scope(this);
  // sp[0] : y
  // sp[1] : x
  // result : cc register

  // Strict only makes sense for equality comparisons.
  ASSERT(!strict || cc == eq);

  JumpTarget exit(this);
  JumpTarget smi(this);
  // Implement '>' and '<=' by reversal to obtain ECMA-262 conversion order.
  if (cc == gt || cc == le) {
    cc = ReverseCondition(cc);
    frame_->EmitPop(r1);
    frame_->EmitPop(r0);
  } else {
    frame_->EmitPop(r0);
    frame_->EmitPop(r1);
  }
  __ orr(r2, r0, Operand(r1));
  __ tst(r2, Operand(kSmiTagMask));
  smi.Branch(eq);

  // Perform non-smi comparison by runtime call.
  frame_->EmitPush(r1);

  // Figure out which native to call and setup the arguments.
  Builtins::JavaScript native;
  int arg_count = 1;
  if (cc == eq) {
    native = strict ? Builtins::STRICT_EQUALS : Builtins::EQUALS;
  } else {
    native = Builtins::COMPARE;
    int ncr;  // NaN compare result
    if (cc == lt || cc == le) {
      ncr = GREATER;
    } else {
      ASSERT(cc == gt || cc == ge);  // remaining cases
      ncr = LESS;
    }
    frame_->EmitPush(r0);
    arg_count++;
    __ mov(r0, Operand(Smi::FromInt(ncr)));
  }

  // Call the native; it returns -1 (less), 0 (equal), or 1 (greater)
  // tagged as a small integer.
  frame_->EmitPush(r0);
  Result arg_count_register = allocator_->Allocate(r0);
  ASSERT(arg_count_register.is_valid());
  __ mov(arg_count_register.reg(), Operand(arg_count));
  Result result = frame_->InvokeBuiltin(native,
                                        CALL_JS,
                                        &arg_count_register,
                                        arg_count + 1);
  __ cmp(result.reg(), Operand(0));
  result.Unuse();
  exit.Jump();

  // test smi equality by pointer comparison.
  smi.Bind();
  __ cmp(r1, Operand(r0));

  exit.Bind();
  cc_reg_ = cc;
}


class CallFunctionStub: public CodeStub {
 public:
  explicit CallFunctionStub(int argc) : argc_(argc) {}

  void Generate(MacroAssembler* masm);

 private:
  int argc_;

#if defined(DEBUG)
  void Print() { PrintF("CallFunctionStub (argc %d)\n", argc_); }
#endif  // defined(DEBUG)

  Major MajorKey() { return CallFunction; }
  int MinorKey() { return argc_; }
};


// Call the function on the stack with the given arguments.
void CodeGenerator::CallWithArguments(ZoneList<Expression*>* args,
                                         int position) {
  VirtualFrame::SpilledScope spilled_scope(this);
  // Push the arguments ("left-to-right") on the stack.
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    LoadAndSpill(args->at(i));
  }

  // Record the position for debugging purposes.
  CodeForSourcePosition(position);

  // Use the shared code stub to call the function.
  CallFunctionStub call_function(arg_count);
  frame_->CallStub(&call_function, arg_count + 1);

  // Restore context and pop function from the stack.
  __ ldr(cp, frame_->Context());
  frame_->Drop();  // discard the TOS
}


void CodeGenerator::Branch(bool if_true, JumpTarget* target) {
  VirtualFrame::SpilledScope spilled_scope(this);
  ASSERT(has_cc());
  Condition cc = if_true ? cc_reg_ : NegateCondition(cc_reg_);
  target->Branch(cc);
  cc_reg_ = al;
}


void CodeGenerator::CheckStack() {
  VirtualFrame::SpilledScope spilled_scope(this);
  if (FLAG_check_stack) {
    Comment cmnt(masm_, "[ check stack");
    StackCheckStub stub;
    frame_->CallStub(&stub, 0);
  }
}


void CodeGenerator::VisitAndSpill(Statement* statement) {
  ASSERT(in_spilled_code());
  set_in_spilled_code(false);
  Visit(statement);
  if (frame_ != NULL) {
    frame_->SpillAll();
    }
  set_in_spilled_code(true);
}


void CodeGenerator::VisitStatementsAndSpill(ZoneList<Statement*>* statements) {
  ASSERT(in_spilled_code());
  set_in_spilled_code(false);
  VisitStatements(statements);
  if (frame_ != NULL) {
    frame_->SpillAll();
  }
  set_in_spilled_code(true);
}


void CodeGenerator::VisitStatements(ZoneList<Statement*>* statements) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  for (int i = 0; frame_ != NULL && i < statements->length(); i++) {
    VisitAndSpill(statements->at(i));
  }
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitBlock(Block* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ Block");
  CodeForStatementPosition(node);
  node->break_target()->Initialize(this);
  VisitStatementsAndSpill(node->statements());
  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  node->break_target()->Unuse();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::DeclareGlobals(Handle<FixedArray> pairs) {
  VirtualFrame::SpilledScope spilled_scope(this);
  __ mov(r0, Operand(pairs));
  frame_->EmitPush(r0);
  frame_->EmitPush(cp);
  __ mov(r0, Operand(Smi::FromInt(is_eval() ? 1 : 0)));
  frame_->EmitPush(r0);
  frame_->CallRuntime(Runtime::kDeclareGlobals, 3);
  // The result is discarded.
}


void CodeGenerator::VisitDeclaration(Declaration* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ Declaration");
  CodeForStatementPosition(node);
  Variable* var = node->proxy()->var();
  ASSERT(var != NULL);  // must have been resolved
  Slot* slot = var->slot();

  // If it was not possible to allocate the variable at compile time,
  // we need to "declare" it at runtime to make sure it actually
  // exists in the local context.
  if (slot != NULL && slot->type() == Slot::LOOKUP) {
    // Variables with a "LOOKUP" slot were introduced as non-locals
    // during variable resolution and must have mode DYNAMIC.
    ASSERT(var->is_dynamic());
    // For now, just do a runtime call.
    frame_->EmitPush(cp);
    __ mov(r0, Operand(var->name()));
    frame_->EmitPush(r0);
    // Declaration nodes are always declared in only two modes.
    ASSERT(node->mode() == Variable::VAR || node->mode() == Variable::CONST);
    PropertyAttributes attr = node->mode() == Variable::VAR ? NONE : READ_ONLY;
    __ mov(r0, Operand(Smi::FromInt(attr)));
    frame_->EmitPush(r0);
    // Push initial value, if any.
    // Note: For variables we must not push an initial value (such as
    // 'undefined') because we may have a (legal) redeclaration and we
    // must not destroy the current value.
    if (node->mode() == Variable::CONST) {
      __ mov(r0, Operand(Factory::the_hole_value()));
      frame_->EmitPush(r0);
    } else if (node->fun() != NULL) {
      LoadAndSpill(node->fun());
    } else {
      __ mov(r0, Operand(0));  // no initial value!
      frame_->EmitPush(r0);
    }
    frame_->CallRuntime(Runtime::kDeclareContextSlot, 4);
    // Ignore the return value (declarations are statements).
    ASSERT(frame_->height() == original_height);
    return;
  }

  ASSERT(!var->is_global());

  // If we have a function or a constant, we need to initialize the variable.
  Expression* val = NULL;
  if (node->mode() == Variable::CONST) {
    val = new Literal(Factory::the_hole_value());
  } else {
    val = node->fun();  // NULL if we don't have a function
  }

  if (val != NULL) {
    {
      // Set initial value.
      Reference target(this, node->proxy());
      LoadAndSpill(val);
      target.SetValue(NOT_CONST_INIT);
      // The reference is removed from the stack (preserving TOS) when
      // it goes out of scope.
    }
    // Get rid of the assigned value (declarations are statements).
    frame_->Drop();
  }
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitExpressionStatement(ExpressionStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ ExpressionStatement");
  CodeForStatementPosition(node);
  Expression* expression = node->expression();
  expression->MarkAsStatement();
  LoadAndSpill(expression);
  frame_->Drop();
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitEmptyStatement(EmptyStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "// EmptyStatement");
  CodeForStatementPosition(node);
  // nothing to do
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitIfStatement(IfStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ IfStatement");
  // Generate different code depending on which parts of the if statement
  // are present or not.
  bool has_then_stm = node->HasThenStatement();
  bool has_else_stm = node->HasElseStatement();

  CodeForStatementPosition(node);

  JumpTarget exit(this);
  if (has_then_stm && has_else_stm) {
    Comment cmnt(masm_, "[ IfThenElse");
    JumpTarget then(this);
    JumpTarget else_(this);
    // if (cond)
    LoadConditionAndSpill(node->condition(), NOT_INSIDE_TYPEOF,
                          &then, &else_, true);
    if (frame_ != NULL) {
      Branch(false, &else_);
    }
    // then
    if (frame_ != NULL || then.is_linked()) {
      then.Bind();
      VisitAndSpill(node->then_statement());
    }
    if (frame_ != NULL) {
      exit.Jump();
    }
    // else
    if (else_.is_linked()) {
      else_.Bind();
      VisitAndSpill(node->else_statement());
    }

  } else if (has_then_stm) {
    Comment cmnt(masm_, "[ IfThen");
    ASSERT(!has_else_stm);
    JumpTarget then(this);
    // if (cond)
    LoadConditionAndSpill(node->condition(), NOT_INSIDE_TYPEOF,
                          &then, &exit, true);
    if (frame_ != NULL) {
      Branch(false, &exit);
    }
    // then
    if (frame_ != NULL || then.is_linked()) {
      then.Bind();
      VisitAndSpill(node->then_statement());
    }

  } else if (has_else_stm) {
    Comment cmnt(masm_, "[ IfElse");
    ASSERT(!has_then_stm);
    JumpTarget else_(this);
    // if (!cond)
    LoadConditionAndSpill(node->condition(), NOT_INSIDE_TYPEOF,
                          &exit, &else_, true);
    if (frame_ != NULL) {
      Branch(true, &exit);
    }
    // else
    if (frame_ != NULL || else_.is_linked()) {
      else_.Bind();
      VisitAndSpill(node->else_statement());
    }

  } else {
    Comment cmnt(masm_, "[ If");
    ASSERT(!has_then_stm && !has_else_stm);
    // if (cond)
    LoadConditionAndSpill(node->condition(), NOT_INSIDE_TYPEOF,
                          &exit, &exit, false);
    if (frame_ != NULL) {
      if (has_cc()) {
        cc_reg_ = al;
      } else {
        frame_->Drop();
      }
    }
  }

  // end
  if (exit.is_linked()) {
    exit.Bind();
  }
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitContinueStatement(ContinueStatement* node) {
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ ContinueStatement");
  CodeForStatementPosition(node);
  node->target()->continue_target()->Jump();
}


void CodeGenerator::VisitBreakStatement(BreakStatement* node) {
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ BreakStatement");
  CodeForStatementPosition(node);
  node->target()->break_target()->Jump();
}


void CodeGenerator::VisitReturnStatement(ReturnStatement* node) {
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ ReturnStatement");

  if (function_return_is_shadowed_) {
    CodeForStatementPosition(node);
    LoadAndSpill(node->expression());
    frame_->EmitPop(r0);
    function_return_.Jump();
  } else {
    // Load the returned value.
    CodeForStatementPosition(node);
    LoadAndSpill(node->expression());

    // Pop the result from the frame and prepare the frame for
    // returning thus making it easier to merge.
    frame_->EmitPop(r0);
    frame_->PrepareForReturn();

    function_return_.Jump();
  }
}


void CodeGenerator::VisitWithEnterStatement(WithEnterStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ WithEnterStatement");
  CodeForStatementPosition(node);
  LoadAndSpill(node->expression());
  if (node->is_catch_block()) {
    frame_->CallRuntime(Runtime::kPushCatchContext, 1);
  } else {
    frame_->CallRuntime(Runtime::kPushContext, 1);
  }
  if (kDebug) {
    JumpTarget verified_true(this);
    __ cmp(r0, Operand(cp));
    verified_true.Branch(eq);
    __ stop("PushContext: r0 is expected to be the same as cp");
    verified_true.Bind();
  }
  // Update context local.
  __ str(cp, frame_->Context());
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitWithExitStatement(WithExitStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ WithExitStatement");
  CodeForStatementPosition(node);
  // Pop context.
  __ ldr(cp, ContextOperand(cp, Context::PREVIOUS_INDEX));
  // Update context local.
  __ str(cp, frame_->Context());
  ASSERT(frame_->height() == original_height);
}


int CodeGenerator::FastCaseSwitchMaxOverheadFactor() {
  return kFastSwitchMaxOverheadFactor;
}

int CodeGenerator::FastCaseSwitchMinCaseCount() {
  return kFastSwitchMinCaseCount;
}


void CodeGenerator::GenerateFastCaseSwitchJumpTable(
    SwitchStatement* node,
    int min_index,
    int range,
    Label* default_label,
    Vector<Label*> case_targets,
    Vector<Label> case_labels) {
  VirtualFrame::SpilledScope spilled_scope(this);
  JumpTarget setup_default(this);
  JumpTarget is_smi(this);

  // A non-null default label pointer indicates a default case among
  // the case labels.  Otherwise we use the break target as a
  // "default" for failure to hit the jump table.
  JumpTarget* default_target =
      (default_label == NULL) ? node->break_target() : &setup_default;

  ASSERT(kSmiTag == 0 && kSmiTagSize <= 2);
  frame_->EmitPop(r0);

  // Test for a Smi value in a HeapNumber.
  __ tst(r0, Operand(kSmiTagMask));
  is_smi.Branch(eq);
  __ ldr(r1, MemOperand(r0, HeapObject::kMapOffset - kHeapObjectTag));
  __ ldrb(r1, MemOperand(r1, Map::kInstanceTypeOffset - kHeapObjectTag));
  __ cmp(r1, Operand(HEAP_NUMBER_TYPE));
  default_target->Branch(ne);
  frame_->EmitPush(r0);
  frame_->CallRuntime(Runtime::kNumberToSmi, 1);
  is_smi.Bind();

  if (min_index != 0) {
    // Small positive numbers can be immediate operands.
    if (min_index < 0) {
      // If min_index is Smi::kMinValue, -min_index is not a Smi.
      if (Smi::IsValid(-min_index)) {
        __ add(r0, r0, Operand(Smi::FromInt(-min_index)));
      } else {
        __ add(r0, r0, Operand(Smi::FromInt(-min_index - 1)));
        __ add(r0, r0, Operand(Smi::FromInt(1)));
      }
    } else {
      __ sub(r0, r0, Operand(Smi::FromInt(min_index)));
    }
  }
  __ tst(r0, Operand(0x80000000 | kSmiTagMask));
  default_target->Branch(ne);
  __ cmp(r0, Operand(Smi::FromInt(range)));
  default_target->Branch(ge);
  VirtualFrame* start_frame = new VirtualFrame(frame_);
  __ SmiJumpTable(r0, case_targets);

  GenerateFastCaseSwitchCases(node, case_labels, start_frame);

  // If there was a default case among the case labels, we need to
  // emit code to jump to it from the default target used for failure
  // to hit the jump table.
  if (default_label != NULL) {
    if (has_valid_frame()) {
      node->break_target()->Jump();
    }
    setup_default.Bind();
    frame_->MergeTo(start_frame);
    __ b(default_label);
    DeleteFrame();
  }
  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }

  delete start_frame;
}


void CodeGenerator::VisitSwitchStatement(SwitchStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ SwitchStatement");
  CodeForStatementPosition(node);
  node->break_target()->Initialize(this);

  LoadAndSpill(node->tag());
  if (TryGenerateFastCaseSwitchStatement(node)) {
    ASSERT(!has_valid_frame() || frame_->height() == original_height);
    return;
  }

  JumpTarget next_test(this);
  JumpTarget fall_through(this);
  JumpTarget default_entry(this);
  JumpTarget default_exit(this, JumpTarget::BIDIRECTIONAL);
  ZoneList<CaseClause*>* cases = node->cases();
  int length = cases->length();
  CaseClause* default_clause = NULL;

  for (int i = 0; i < length; i++) {
    CaseClause* clause = cases->at(i);
    if (clause->is_default()) {
      // Remember the default clause and compile it at the end.
      default_clause = clause;
      continue;
    }

    Comment cmnt(masm_, "[ Case clause");
    // Compile the test.
    next_test.Bind();
    next_test.Unuse();
    // Duplicate TOS.
    __ ldr(r0, frame_->Top());
    frame_->EmitPush(r0);
    LoadAndSpill(clause->label());
    Comparison(eq, true);
    Branch(false, &next_test);

    // Before entering the body from the test, remove the switch value from
    // the stack.
    frame_->Drop();

    // Label the body so that fall through is enabled.
    if (i > 0 && cases->at(i - 1)->is_default()) {
      default_exit.Bind();
    } else {
      fall_through.Bind();
      fall_through.Unuse();
    }
    VisitStatementsAndSpill(clause->statements());

    // If control flow can fall through from the body, jump to the next body
    // or the end of the statement.
    if (frame_ != NULL) {
      if (i < length - 1 && cases->at(i + 1)->is_default()) {
        default_entry.Jump();
      } else {
        fall_through.Jump();
      }
    }
  }

  // The final "test" removes the switch value.
  next_test.Bind();
  frame_->Drop();

  // If there is a default clause, compile it.
  if (default_clause != NULL) {
    Comment cmnt(masm_, "[ Default clause");
    default_entry.Bind();
    VisitStatementsAndSpill(default_clause->statements());
    // If control flow can fall out of the default and there is a case after
    // it, jup to that case's body.
    if (frame_ != NULL && default_exit.is_bound()) {
      default_exit.Jump();
    }
  }

  if (fall_through.is_linked()) {
    fall_through.Bind();
  }

  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  node->break_target()->Unuse();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitLoopStatement(LoopStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ LoopStatement");
  CodeForStatementPosition(node);
  node->break_target()->Initialize(this);

  // Simple condition analysis.  ALWAYS_TRUE and ALWAYS_FALSE represent a
  // known result for the test expression, with no side effects.
  enum { ALWAYS_TRUE, ALWAYS_FALSE, DONT_KNOW } info = DONT_KNOW;
  if (node->cond() == NULL) {
    ASSERT(node->type() == LoopStatement::FOR_LOOP);
    info = ALWAYS_TRUE;
  } else {
    Literal* lit = node->cond()->AsLiteral();
    if (lit != NULL) {
      if (lit->IsTrue()) {
        info = ALWAYS_TRUE;
      } else if (lit->IsFalse()) {
        info = ALWAYS_FALSE;
      }
    }
  }

  switch (node->type()) {
    case LoopStatement::DO_LOOP: {
      JumpTarget body(this, JumpTarget::BIDIRECTIONAL);

      // Label the top of the loop for the backward CFG edge.  If the test
      // is always true we can use the continue target, and if the test is
      // always false there is no need.
      if (info == ALWAYS_TRUE) {
        node->continue_target()->Initialize(this, JumpTarget::BIDIRECTIONAL);
        node->continue_target()->Bind();
      } else if (info == ALWAYS_FALSE) {
        node->continue_target()->Initialize(this);
      } else {
        ASSERT(info == DONT_KNOW);
        node->continue_target()->Initialize(this);
        body.Bind();
      }

      CheckStack();  // TODO(1222600): ignore if body contains calls.
      VisitAndSpill(node->body());

      // Compile the test.
      if (info == ALWAYS_TRUE) {
        if (has_valid_frame()) {
          // If control can fall off the end of the body, jump back to the
          // top.
          node->continue_target()->Jump();
        }
      } else if (info == ALWAYS_FALSE) {
        // If we have a continue in the body, we only have to bind its jump
        // target.
        if (node->continue_target()->is_linked()) {
          node->continue_target()->Bind();
        }
      } else {
        ASSERT(info == DONT_KNOW);
        // We have to compile the test expression if it can be reached by
        // control flow falling out of the body or via continue.
        if (node->continue_target()->is_linked()) {
          node->continue_target()->Bind();
        }
        if (has_valid_frame()) {
          LoadConditionAndSpill(node->cond(), NOT_INSIDE_TYPEOF,
                                &body, node->break_target(), true);
          if (has_valid_frame()) {
            // A invalid frame here indicates that control did not
            // fall out of the test expression.
            Branch(true, &body);
          }
        }
      }
      break;
    }

    case LoopStatement::WHILE_LOOP: {
      // If the test is never true and has no side effects there is no need
      // to compile the test or body.
      if (info == ALWAYS_FALSE) break;

      // Label the top of the loop with the continue target for the backward
      // CFG edge.
      node->continue_target()->Initialize(this, JumpTarget::BIDIRECTIONAL);
      node->continue_target()->Bind();

      if (info == DONT_KNOW) {
        JumpTarget body(this);
        LoadConditionAndSpill(node->cond(), NOT_INSIDE_TYPEOF,
                              &body, node->break_target(), true);
        if (has_valid_frame()) {
          // A NULL frame indicates that control did not fall out of the
          // test expression.
          Branch(false, node->break_target());
        }
        if (has_valid_frame() || body.is_linked()) {
          body.Bind();
        }
      }

      if (has_valid_frame()) {
        CheckStack();  // TODO(1222600): ignore if body contains calls.
        VisitAndSpill(node->body());

        // If control flow can fall out of the body, jump back to the top.
        if (has_valid_frame()) {
          node->continue_target()->Jump();
        }
      }
      break;
    }

    case LoopStatement::FOR_LOOP: {
      JumpTarget loop(this, JumpTarget::BIDIRECTIONAL);

      if (node->init() != NULL) {
        VisitAndSpill(node->init());
      }

      // There is no need to compile the test or body.
      if (info == ALWAYS_FALSE) break;

      // If there is no update statement, label the top of the loop with the
      // continue target, otherwise with the loop target.
      if (node->next() == NULL) {
        node->continue_target()->Initialize(this, JumpTarget::BIDIRECTIONAL);
        node->continue_target()->Bind();
      } else {
        node->continue_target()->Initialize(this);
        loop.Bind();
      }

      // If the test is always true, there is no need to compile it.
      if (info == DONT_KNOW) {
        JumpTarget body(this);
        LoadConditionAndSpill(node->cond(), NOT_INSIDE_TYPEOF,
                              &body, node->break_target(), true);
        if (has_valid_frame()) {
          Branch(false, node->break_target());
        }
        if (has_valid_frame() || body.is_linked()) {
          body.Bind();
        }
      }

      if (has_valid_frame()) {
        CheckStack();  // TODO(1222600): ignore if body contains calls.
        VisitAndSpill(node->body());

        if (node->next() == NULL) {
          // If there is no update statement and control flow can fall out
          // of the loop, jump directly to the continue label.
          if (has_valid_frame()) {
            node->continue_target()->Jump();
          }
        } else {
          // If there is an update statement and control flow can reach it
          // via falling out of the body of the loop or continuing, we
          // compile the update statement.
          if (node->continue_target()->is_linked()) {
            node->continue_target()->Bind();
          }
          if (has_valid_frame()) {
            // Record source position of the statement as this code which is
            // after the code for the body actually belongs to the loop
            // statement and not the body.
            CodeForStatementPosition(node);
            VisitAndSpill(node->next());
            loop.Jump();
          }
        }
      }
      break;
    }
  }

  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  node->continue_target()->Unuse();
  node->break_target()->Unuse();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitForInStatement(ForInStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  ASSERT(!in_spilled_code());
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ ForInStatement");
  CodeForStatementPosition(node);

  JumpTarget primitive(this);
  JumpTarget jsobject(this);
  JumpTarget fixed_array(this);
  JumpTarget entry(this, JumpTarget::BIDIRECTIONAL);
  JumpTarget end_del_check(this);
  JumpTarget exit(this);

  // Get the object to enumerate over (converted to JSObject).
  LoadAndSpill(node->enumerable());

  // Both SpiderMonkey and kjs ignore null and undefined in contrast
  // to the specification.  12.6.4 mandates a call to ToObject.
  frame_->EmitPop(r0);
  __ cmp(r0, Operand(Factory::undefined_value()));
  exit.Branch(eq);
  __ cmp(r0, Operand(Factory::null_value()));
  exit.Branch(eq);

  // Stack layout in body:
  // [iteration counter (Smi)]
  // [length of array]
  // [FixedArray]
  // [Map or 0]
  // [Object]

  // Check if enumerable is already a JSObject
  __ tst(r0, Operand(kSmiTagMask));
  primitive.Branch(eq);
  __ ldr(r1, FieldMemOperand(r0, HeapObject::kMapOffset));
  __ ldrb(r1, FieldMemOperand(r1, Map::kInstanceTypeOffset));
  __ cmp(r1, Operand(FIRST_JS_OBJECT_TYPE));
  jsobject.Branch(hs);

  primitive.Bind();
  frame_->EmitPush(r0);
  Result arg_count = allocator_->Allocate(r0);
  ASSERT(arg_count.is_valid());
  __ mov(arg_count.reg(), Operand(0));
  frame_->InvokeBuiltin(Builtins::TO_OBJECT, CALL_JS, &arg_count, 1);

  jsobject.Bind();
  // Get the set of properties (as a FixedArray or Map).
  frame_->EmitPush(r0);  // duplicate the object being enumerated
  frame_->EmitPush(r0);
  frame_->CallRuntime(Runtime::kGetPropertyNamesFast, 1);

  // If we got a Map, we can do a fast modification check.
  // Otherwise, we got a FixedArray, and we have to do a slow check.
  __ mov(r2, Operand(r0));
  __ ldr(r1, FieldMemOperand(r2, HeapObject::kMapOffset));
  __ cmp(r1, Operand(Factory::meta_map()));
  fixed_array.Branch(ne);

  // Get enum cache
  __ mov(r1, Operand(r0));
  __ ldr(r1, FieldMemOperand(r1, Map::kInstanceDescriptorsOffset));
  __ ldr(r1, FieldMemOperand(r1, DescriptorArray::kEnumerationIndexOffset));
  __ ldr(r2,
         FieldMemOperand(r1, DescriptorArray::kEnumCacheBridgeCacheOffset));

  frame_->EmitPush(r0);  // map
  frame_->EmitPush(r2);  // enum cache bridge cache
  __ ldr(r0, FieldMemOperand(r2, FixedArray::kLengthOffset));
  __ mov(r0, Operand(r0, LSL, kSmiTagSize));
  frame_->EmitPush(r0);
  __ mov(r0, Operand(Smi::FromInt(0)));
  frame_->EmitPush(r0);
  entry.Jump();

  fixed_array.Bind();
  __ mov(r1, Operand(Smi::FromInt(0)));
  frame_->EmitPush(r1);  // insert 0 in place of Map
  frame_->EmitPush(r0);

  // Push the length of the array and the initial index onto the stack.
  __ ldr(r0, FieldMemOperand(r0, FixedArray::kLengthOffset));
  __ mov(r0, Operand(r0, LSL, kSmiTagSize));
  frame_->EmitPush(r0);
  __ mov(r0, Operand(Smi::FromInt(0)));  // init index
  frame_->EmitPush(r0);

  // Condition.
  entry.Bind();
  // sp[0] : index
  // sp[1] : array/enum cache length
  // sp[2] : array or enum cache
  // sp[3] : 0 or map
  // sp[4] : enumerable
  // Grab the current frame's height for the break and continue
  // targets only after all the state is pushed on the frame.
  node->break_target()->Initialize(this);
  node->continue_target()->Initialize(this);

  __ ldr(r0, frame_->ElementAt(0));  // load the current count
  __ ldr(r1, frame_->ElementAt(1));  // load the length
  __ cmp(r0, Operand(r1));  // compare to the array length
  node->break_target()->Branch(hs);

  __ ldr(r0, frame_->ElementAt(0));

  // Get the i'th entry of the array.
  __ ldr(r2, frame_->ElementAt(2));
  __ add(r2, r2, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  __ ldr(r3, MemOperand(r2, r0, LSL, kPointerSizeLog2 - kSmiTagSize));

  // Get Map or 0.
  __ ldr(r2, frame_->ElementAt(3));
  // Check if this (still) matches the map of the enumerable.
  // If not, we have to filter the key.
  __ ldr(r1, frame_->ElementAt(4));
  __ ldr(r1, FieldMemOperand(r1, HeapObject::kMapOffset));
  __ cmp(r1, Operand(r2));
  end_del_check.Branch(eq);

  // Convert the entry to a string (or null if it isn't a property anymore).
  __ ldr(r0, frame_->ElementAt(4));  // push enumerable
  frame_->EmitPush(r0);
  frame_->EmitPush(r3);  // push entry
  Result arg_count_register = allocator_->Allocate(r0);
  ASSERT(arg_count_register.is_valid());
  __ mov(arg_count_register.reg(), Operand(1));
  Result result = frame_->InvokeBuiltin(Builtins::FILTER_KEY,
                                        CALL_JS,
                                        &arg_count_register,
                                        2);
  __ mov(r3, Operand(result.reg()));
  result.Unuse();

  // If the property has been removed while iterating, we just skip it.
  __ cmp(r3, Operand(Factory::null_value()));
  node->continue_target()->Branch(eq);

  end_del_check.Bind();
  // Store the entry in the 'each' expression and take another spin in the
  // loop.  r3: i'th entry of the enum cache (or string there of)
  frame_->EmitPush(r3);  // push entry
  { Reference each(this, node->each());
    if (!each.is_illegal()) {
      if (each.size() > 0) {
        __ ldr(r0, frame_->ElementAt(each.size()));
        frame_->EmitPush(r0);
      }
      // If the reference was to a slot we rely on the convenient property
      // that it doesn't matter whether a value (eg, r3 pushed above) is
      // right on top of or right underneath a zero-sized reference.
      each.SetValue(NOT_CONST_INIT);
      if (each.size() > 0) {
        // It's safe to pop the value lying on top of the reference before
        // unloading the reference itself (which preserves the top of stack,
        // ie, now the topmost value of the non-zero sized reference), since
        // we will discard the top of stack after unloading the reference
        // anyway.
        frame_->EmitPop(r0);
      }
    }
  }
  // Discard the i'th entry pushed above or else the remainder of the
  // reference, whichever is currently on top of the stack.
  frame_->Drop();

  // Body.
  CheckStack();  // TODO(1222600): ignore if body contains calls.
  VisitAndSpill(node->body());

  // Next.  Reestablish a spilled frame in case we are coming here via
  // a continue in the body.
  node->continue_target()->Bind();
  frame_->SpillAll();
  frame_->EmitPop(r0);
  __ add(r0, r0, Operand(Smi::FromInt(1)));
  frame_->EmitPush(r0);
  entry.Jump();

  // Cleanup.  No need to spill because VirtualFrame::Drop is safe for
  // any frame.
  node->break_target()->Bind();
  frame_->Drop(5);

  // Exit.
  exit.Bind();
  node->continue_target()->Unuse();
  node->break_target()->Unuse();
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitTryCatch(TryCatch* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ TryCatch");
  CodeForStatementPosition(node);

  JumpTarget try_block(this);
  JumpTarget exit(this);

  try_block.Call();
  // --- Catch block ---
  frame_->EmitPush(r0);

  // Store the caught exception in the catch variable.
  { Reference ref(this, node->catch_var());
    ASSERT(ref.is_slot());
    // Here we make use of the convenient property that it doesn't matter
    // whether a value is immediately on top of or underneath a zero-sized
    // reference.
    ref.SetValue(NOT_CONST_INIT);
  }

  // Remove the exception from the stack.
  frame_->Drop();

  VisitStatementsAndSpill(node->catch_block()->statements());
  if (frame_ != NULL) {
    exit.Jump();
  }


  // --- Try block ---
  try_block.Bind();

  frame_->PushTryHandler(TRY_CATCH_HANDLER);
  int handler_height = frame_->height();

  // Shadow the labels for all escapes from the try block, including
  // returns. During shadowing, the original label is hidden as the
  // LabelShadow and operations on the original actually affect the
  // shadowing label.
  //
  // We should probably try to unify the escaping labels and the return
  // label.
  int nof_escapes = node->escaping_targets()->length();
  List<ShadowTarget*> shadows(1 + nof_escapes);

  // Add the shadow target for the function return.
  static const int kReturnShadowIndex = 0;
  shadows.Add(new ShadowTarget(&function_return_));
  bool function_return_was_shadowed = function_return_is_shadowed_;
  function_return_is_shadowed_ = true;
  ASSERT(shadows[kReturnShadowIndex]->other_target() == &function_return_);

  // Add the remaining shadow targets.
  for (int i = 0; i < nof_escapes; i++) {
    shadows.Add(new ShadowTarget(node->escaping_targets()->at(i)));
  }

  // Generate code for the statements in the try block.
  VisitStatementsAndSpill(node->try_block()->statements());

  // Stop the introduced shadowing and count the number of required unlinks.
  // After shadowing stops, the original labels are unshadowed and the
  // LabelShadows represent the formerly shadowing labels.
  bool has_unlinks = false;
  for (int i = 0; i < shadows.length(); i++) {
    shadows[i]->StopShadowing();
    has_unlinks = has_unlinks || shadows[i]->is_linked();
  }
  function_return_is_shadowed_ = function_return_was_shadowed;

  // Get an external reference to the handler address.
  ExternalReference handler_address(Top::k_handler_address);

  // The next handler address is at kNextIndex in the stack.
  const int kNextIndex = StackHandlerConstants::kNextOffset / kPointerSize;
  // If we can fall off the end of the try block, unlink from try chain.
  if (has_valid_frame()) {
    __ ldr(r1, frame_->ElementAt(kNextIndex));
    __ mov(r3, Operand(handler_address));
    __ str(r1, MemOperand(r3));
    frame_->Drop(StackHandlerConstants::kSize / kPointerSize);
    if (has_unlinks) {
      exit.Jump();
    }
  }

  // Generate unlink code for the (formerly) shadowing labels that have been
  // jumped to.  Deallocate each shadow target.
  for (int i = 0; i < shadows.length(); i++) {
    if (shadows[i]->is_linked()) {
      // Unlink from try chain;
      shadows[i]->Bind();
      // Because we can be jumping here (to spilled code) from unspilled
      // code, we need to reestablish a spilled frame at this block.
      frame_->SpillAll();

      // Reload sp from the top handler, because some statements that we
      // break from (eg, for...in) may have left stuff on the stack.
      __ mov(r3, Operand(handler_address));
      __ ldr(sp, MemOperand(r3));
      // The stack pointer was restored to just below the code slot
      // (the topmost slot) in the handler.
      frame_->Forget(frame_->height() - handler_height + 1);

      // kNextIndex is off by one because the code slot has already
      // been dropped.
      __ ldr(r1, frame_->ElementAt(kNextIndex - 1));
      __ str(r1, MemOperand(r3));
      // The code slot has already been dropped from the handler.
      frame_->Drop(StackHandlerConstants::kSize / kPointerSize - 1);

      if (!function_return_is_shadowed_ && i == kReturnShadowIndex) {
        frame_->PrepareForReturn();
      }
      shadows[i]->other_target()->Jump();
    }
    delete shadows[i];
  }

  exit.Bind();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitTryFinally(TryFinally* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ TryFinally");
  CodeForStatementPosition(node);

  // State: Used to keep track of reason for entering the finally
  // block. Should probably be extended to hold information for
  // break/continue from within the try block.
  enum { FALLING, THROWING, JUMPING };

  JumpTarget try_block(this);
  JumpTarget finally_block(this);

  try_block.Call();

  frame_->EmitPush(r0);  // save exception object on the stack
  // In case of thrown exceptions, this is where we continue.
  __ mov(r2, Operand(Smi::FromInt(THROWING)));
  finally_block.Jump();

  // --- Try block ---
  try_block.Bind();

  frame_->PushTryHandler(TRY_FINALLY_HANDLER);
  int handler_height = frame_->height();

  // Shadow the labels for all escapes from the try block, including
  // returns.  Shadowing hides the original label as the LabelShadow and
  // operations on the original actually affect the shadowing label.
  //
  // We should probably try to unify the escaping labels and the return
  // label.
  int nof_escapes = node->escaping_targets()->length();
  List<ShadowTarget*> shadows(1 + nof_escapes);

  // Add the shadow target for the function return.
  static const int kReturnShadowIndex = 0;
  shadows.Add(new ShadowTarget(&function_return_));
  bool function_return_was_shadowed = function_return_is_shadowed_;
  function_return_is_shadowed_ = true;
  ASSERT(shadows[kReturnShadowIndex]->other_target() == &function_return_);

  // Add the remaining shadow targets.
  for (int i = 0; i < nof_escapes; i++) {
    shadows.Add(new ShadowTarget(node->escaping_targets()->at(i)));
  }

  // Generate code for the statements in the try block.
  VisitStatementsAndSpill(node->try_block()->statements());

  // Stop the introduced shadowing and count the number of required unlinks.
  // After shadowing stops, the original labels are unshadowed and the
  // LabelShadows represent the formerly shadowing labels.
  int nof_unlinks = 0;
  for (int i = 0; i < shadows.length(); i++) {
    shadows[i]->StopShadowing();
    if (shadows[i]->is_linked()) nof_unlinks++;
  }
  function_return_is_shadowed_ = function_return_was_shadowed;

  // Get an external reference to the handler address.
  ExternalReference handler_address(Top::k_handler_address);

  // The next handler address is at kNextIndex in the stack.
  const int kNextIndex = StackHandlerConstants::kNextOffset / kPointerSize;
  // If we can fall off the end of the try block, unlink from the try
  // chain and set the state on the frame to FALLING.
  if (has_valid_frame()) {
    __ ldr(r1, frame_->ElementAt(kNextIndex));
    __ mov(r3, Operand(handler_address));
    __ str(r1, MemOperand(r3));
    frame_->Drop(StackHandlerConstants::kSize / kPointerSize);

    // Fake a top of stack value (unneeded when FALLING) and set the
    // state in r2, then jump around the unlink blocks if any.
    __ mov(r0, Operand(Factory::undefined_value()));
    frame_->EmitPush(r0);
    __ mov(r2, Operand(Smi::FromInt(FALLING)));
    if (nof_unlinks > 0) {
      finally_block.Jump();
    }
  }

  // Generate code to unlink and set the state for the (formerly)
  // shadowing targets that have been jumped to.
  for (int i = 0; i < shadows.length(); i++) {
    if (shadows[i]->is_linked()) {
      // If we have come from the shadowed return, the return value is
      // in (a non-refcounted reference to) r0.  We must preserve it
      // until it is pushed.
      //
      // Because we can be jumping here (to spilled code) from
      // unspilled code, we need to reestablish a spilled frame at
      // this block.
      shadows[i]->Bind();
      frame_->SpillAll();

      // Reload sp from the top handler, because some statements that
      // we break from (eg, for...in) may have left stuff on the
      // stack.
      __ mov(r3, Operand(handler_address));
      __ ldr(sp, MemOperand(r3));
      // The stack pointer was restored to the address slot in the handler.
      ASSERT(StackHandlerConstants::kNextOffset == 1 * kPointerSize);
      frame_->Forget(frame_->height() - handler_height + 1);

      // Unlink this handler and drop it from the frame.  The next
      // handler address is now on top of the frame.
      frame_->EmitPop(r1);
      __ str(r1, MemOperand(r3));
      // The top (code) and the second (handler) slot have both been
      // dropped already.
      frame_->Drop(StackHandlerConstants::kSize / kPointerSize - 2);

      if (i == kReturnShadowIndex) {
        // If this label shadowed the function return, materialize the
        // return value on the stack.
        frame_->EmitPush(r0);
      } else {
        // Fake TOS for targets that shadowed breaks and continues.
        __ mov(r0, Operand(Factory::undefined_value()));
        frame_->EmitPush(r0);
      }
      __ mov(r2, Operand(Smi::FromInt(JUMPING + i)));
      if (--nof_unlinks > 0) {
        // If this is not the last unlink block, jump around the next.
        finally_block.Jump();
      }
    }
  }

  // --- Finally block ---
  finally_block.Bind();

  // Push the state on the stack.
  frame_->EmitPush(r2);

  // We keep two elements on the stack - the (possibly faked) result
  // and the state - while evaluating the finally block.
  //
  // Generate code for the statements in the finally block.
  VisitStatementsAndSpill(node->finally_block()->statements());

  if (has_valid_frame()) {
    // Restore state and return value or faked TOS.
    frame_->EmitPop(r2);
    frame_->EmitPop(r0);
  }

  // Generate code to jump to the right destination for all used
  // formerly shadowing targets.  Deallocate each shadow target.
  for (int i = 0; i < shadows.length(); i++) {
    if (has_valid_frame() && shadows[i]->is_bound()) {
      JumpTarget* original = shadows[i]->other_target();
      __ cmp(r2, Operand(Smi::FromInt(JUMPING + i)));
      if (!function_return_is_shadowed_ && i == kReturnShadowIndex) {
        JumpTarget skip(this);
        skip.Branch(ne);
        frame_->PrepareForReturn();
        original->Jump();
        skip.Bind();
      } else {
        original->Branch(eq);
      }
    }
    delete shadows[i];
  }

  if (has_valid_frame()) {
    // Check if we need to rethrow the exception.
    JumpTarget exit(this);
    __ cmp(r2, Operand(Smi::FromInt(THROWING)));
    exit.Branch(ne);

    // Rethrow exception.
    frame_->EmitPush(r0);
    frame_->CallRuntime(Runtime::kReThrow, 1);

    // Done.
    exit.Bind();
  }
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitDebuggerStatement(DebuggerStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ DebuggerStatament");
  CodeForStatementPosition(node);
  frame_->CallRuntime(Runtime::kDebugBreak, 0);
  // Ignore the return value.
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::InstantiateBoilerplate(Handle<JSFunction> boilerplate) {
  VirtualFrame::SpilledScope spilled_scope(this);
  ASSERT(boilerplate->IsBoilerplate());

  // Push the boilerplate on the stack.
  __ mov(r0, Operand(boilerplate));
  frame_->EmitPush(r0);

  // Create a new closure.
  frame_->EmitPush(cp);
  frame_->CallRuntime(Runtime::kNewClosure, 2);
  frame_->EmitPush(r0);
}


void CodeGenerator::VisitFunctionLiteral(FunctionLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ FunctionLiteral");

  // Build the function boilerplate and instantiate it.
  Handle<JSFunction> boilerplate = BuildBoilerplate(node);
  // Check for stack-overflow exception.
  if (HasStackOverflow()) {
    ASSERT(frame_->height() == original_height);
    return;
  }
  InstantiateBoilerplate(boilerplate);
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitFunctionBoilerplateLiteral(
    FunctionBoilerplateLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ FunctionBoilerplateLiteral");
  InstantiateBoilerplate(node->boilerplate());
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitConditional(Conditional* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ Conditional");
  JumpTarget then(this);
  JumpTarget else_(this);
  JumpTarget exit(this);
  LoadConditionAndSpill(node->condition(), NOT_INSIDE_TYPEOF,
                        &then, &else_, true);
  Branch(false, &else_);
  then.Bind();
  LoadAndSpill(node->then_expression(), typeof_state());
  exit.Jump();
  else_.Bind();
  LoadAndSpill(node->else_expression(), typeof_state());
  exit.Bind();
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::LoadFromSlot(Slot* slot, TypeofState typeof_state) {
  VirtualFrame::SpilledScope spilled_scope(this);
  if (slot->type() == Slot::LOOKUP) {
    ASSERT(slot->var()->is_dynamic());

    JumpTarget slow(this);
    JumpTarget done(this);

    // Generate fast-case code for variables that might be shadowed by
    // eval-introduced variables.  Eval is used a lot without
    // introducing variables.  In those cases, we do not want to
    // perform a runtime call for all variables in the scope
    // containing the eval.
    if (slot->var()->mode() == Variable::DYNAMIC_GLOBAL) {
      LoadFromGlobalSlotCheckExtensions(slot, typeof_state, r1, r2, &slow);
      // If there was no control flow to slow, we can exit early.
      if (!slow.is_linked()) {
        frame_->EmitPush(r0);
        return;
      }

      done.Jump();

    } else if (slot->var()->mode() == Variable::DYNAMIC_LOCAL) {
      Slot* potential_slot = slot->var()->local_if_not_shadowed()->slot();
      // Only generate the fast case for locals that rewrite to slots.
      // This rules out argument loads.
      if (potential_slot != NULL) {
        __ ldr(r0,
               ContextSlotOperandCheckExtensions(potential_slot,
                                                 r1,
                                                 r2,
                                                 &slow));
        // There is always control flow to slow from
        // ContextSlotOperandCheckExtensions.
        done.Jump();
      }
    }

    slow.Bind();
    frame_->EmitPush(cp);
    __ mov(r0, Operand(slot->var()->name()));
    frame_->EmitPush(r0);

    if (typeof_state == INSIDE_TYPEOF) {
      frame_->CallRuntime(Runtime::kLoadContextSlotNoReferenceError, 2);
    } else {
      frame_->CallRuntime(Runtime::kLoadContextSlot, 2);
    }

    done.Bind();
    frame_->EmitPush(r0);

  } else {
    // Note: We would like to keep the assert below, but it fires because of
    // some nasty code in LoadTypeofExpression() which should be removed...
    // ASSERT(!slot->var()->is_dynamic());

    // Special handling for locals allocated in registers.
    __ ldr(r0, SlotOperand(slot, r2));
    frame_->EmitPush(r0);
    if (slot->var()->mode() == Variable::CONST) {
      // Const slots may contain 'the hole' value (the constant hasn't been
      // initialized yet) which needs to be converted into the 'undefined'
      // value.
      Comment cmnt(masm_, "[ Unhole const");
      frame_->EmitPop(r0);
      __ cmp(r0, Operand(Factory::the_hole_value()));
      __ mov(r0, Operand(Factory::undefined_value()), LeaveCC, eq);
      frame_->EmitPush(r0);
    }
  }
}


void CodeGenerator::LoadFromGlobalSlotCheckExtensions(Slot* slot,
                                                      TypeofState typeof_state,
                                                      Register tmp,
                                                      Register tmp2,
                                                      JumpTarget* slow) {
  // Check that no extension objects have been created by calls to
  // eval from the current scope to the global scope.
  Register context = cp;
  Scope* s = scope();
  while (s != NULL) {
    if (s->num_heap_slots() > 0) {
      if (s->calls_eval()) {
        // Check that extension is NULL.
        __ ldr(tmp2, ContextOperand(context, Context::EXTENSION_INDEX));
        __ tst(tmp2, tmp2);
        slow->Branch(ne);
      }
      // Load next context in chain.
      __ ldr(tmp, ContextOperand(context, Context::CLOSURE_INDEX));
      __ ldr(tmp, FieldMemOperand(tmp, JSFunction::kContextOffset));
      context = tmp;
    }
    // If no outer scope calls eval, we do not need to check more
    // context extensions.
    if (!s->outer_scope_calls_eval() || s->is_eval_scope()) break;
    s = s->outer_scope();
  }

  if (s->is_eval_scope()) {
    Label next, fast;
    if (!context.is(tmp)) __ mov(tmp, Operand(context));
    __ bind(&next);
    // Terminate at global context.
    __ ldr(tmp2, FieldMemOperand(tmp, HeapObject::kMapOffset));
    __ cmp(tmp2, Operand(Factory::global_context_map()));
    __ b(eq, &fast);
    // Check that extension is NULL.
    __ ldr(tmp2, ContextOperand(tmp, Context::EXTENSION_INDEX));
    __ tst(tmp2, tmp2);
    slow->Branch(ne);
    // Load next context in chain.
    __ ldr(tmp, ContextOperand(tmp, Context::CLOSURE_INDEX));
    __ ldr(tmp, FieldMemOperand(tmp, JSFunction::kContextOffset));
    __ b(&next);
    __ bind(&fast);
  }

  // All extension objects were empty and it is safe to use a global
  // load IC call.
  Handle<Code> ic(Builtins::builtin(Builtins::LoadIC_Initialize));
  // Load the global object.
  LoadGlobal();
  // Setup the name register.
  Result name = allocator_->Allocate(r2);
  ASSERT(name.is_valid());  // We are in spilled code.
  __ mov(name.reg(), Operand(slot->var()->name()));
  // Call IC stub.
  if (typeof_state == INSIDE_TYPEOF) {
    frame_->CallCodeObject(ic, RelocInfo::CODE_TARGET, &name, 0);
  } else {
    frame_->CallCodeObject(ic, RelocInfo::CODE_TARGET_CONTEXT, &name, 0);
  }

  // Drop the global object. The result is in r0.
  frame_->Drop();
}


void CodeGenerator::VisitSlot(Slot* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ Slot");
  LoadFromSlot(node, typeof_state());
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitVariableProxy(VariableProxy* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ VariableProxy");

  Variable* var = node->var();
  Expression* expr = var->rewrite();
  if (expr != NULL) {
    Visit(expr);
  } else {
    ASSERT(var->is_global());
    Reference ref(this, node);
    ref.GetValueAndSpill(typeof_state());
  }
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitLiteral(Literal* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ Literal");
  __ mov(r0, Operand(node->handle()));
  frame_->EmitPush(r0);
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitRegExpLiteral(RegExpLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ RexExp Literal");

  // Retrieve the literal array and check the allocated entry.

  // Load the function of this activation.
  __ ldr(r1, frame_->Function());

  // Load the literals array of the function.
  __ ldr(r1, FieldMemOperand(r1, JSFunction::kLiteralsOffset));

  // Load the literal at the ast saved index.
  int literal_offset =
      FixedArray::kHeaderSize + node->literal_index() * kPointerSize;
  __ ldr(r2, FieldMemOperand(r1, literal_offset));

  JumpTarget done(this);
  __ cmp(r2, Operand(Factory::undefined_value()));
  done.Branch(ne);

  // If the entry is undefined we call the runtime system to computed
  // the literal.
  frame_->EmitPush(r1);  // literal array  (0)
  __ mov(r0, Operand(Smi::FromInt(node->literal_index())));
  frame_->EmitPush(r0);  // literal index  (1)
  __ mov(r0, Operand(node->pattern()));  // RegExp pattern (2)
  frame_->EmitPush(r0);
  __ mov(r0, Operand(node->flags()));  // RegExp flags   (3)
  frame_->EmitPush(r0);
  frame_->CallRuntime(Runtime::kMaterializeRegExpLiteral, 4);
  __ mov(r2, Operand(r0));

  done.Bind();
  // Push the literal.
  frame_->EmitPush(r2);
  ASSERT(frame_->height() == original_height + 1);
}


// This deferred code stub will be used for creating the boilerplate
// by calling Runtime_CreateObjectLiteralBoilerplate.
// Each created boilerplate is stored in the JSFunction and they are
// therefore context dependent.
class DeferredObjectLiteral: public DeferredCode {
 public:
  DeferredObjectLiteral(CodeGenerator* generator, ObjectLiteral* node)
      : DeferredCode(generator), node_(node) {
    set_comment("[ DeferredObjectLiteral");
  }

  virtual void Generate();

 private:
  ObjectLiteral* node_;
};


void DeferredObjectLiteral::Generate() {
  // Argument is passed in r1.
  enter()->Bind();
  VirtualFrame::SpilledScope spilled_scope(generator());

  // If the entry is undefined we call the runtime system to compute
  // the literal.

  VirtualFrame* frame = generator()->frame();
  // Literal array (0).
  frame->EmitPush(r1);
  // Literal index (1).
  __ mov(r0, Operand(Smi::FromInt(node_->literal_index())));
  frame->EmitPush(r0);
  // Constant properties (2).
  __ mov(r0, Operand(node_->constant_properties()));
  frame->EmitPush(r0);
  Result boilerplate =
      frame->CallRuntime(Runtime::kCreateObjectLiteralBoilerplate, 3);
  __ mov(r2, Operand(boilerplate.reg()));
  // Result is returned in r2.
  exit_.Jump();
}


void CodeGenerator::VisitObjectLiteral(ObjectLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ ObjectLiteral");

  DeferredObjectLiteral* deferred = new DeferredObjectLiteral(this, node);

  // Retrieve the literal array and check the allocated entry.

  // Load the function of this activation.
  __ ldr(r1, frame_->Function());

  // Load the literals array of the function.
  __ ldr(r1, FieldMemOperand(r1, JSFunction::kLiteralsOffset));

  // Load the literal at the ast saved index.
  int literal_offset =
      FixedArray::kHeaderSize + node->literal_index() * kPointerSize;
  __ ldr(r2, FieldMemOperand(r1, literal_offset));

  // Check whether we need to materialize the object literal boilerplate.
  // If so, jump to the deferred code.
  __ cmp(r2, Operand(Factory::undefined_value()));
  deferred->enter()->Branch(eq);
  deferred->BindExit();

  // Push the object literal boilerplate.
  frame_->EmitPush(r2);

  // Clone the boilerplate object.
  Runtime::FunctionId clone_function_id = Runtime::kCloneLiteralBoilerplate;
  if (node->depth() == 1) {
    clone_function_id = Runtime::kCloneShallowLiteralBoilerplate;
  }
  frame_->CallRuntime(clone_function_id, 1);
  frame_->EmitPush(r0);  // save the result
  // r0: cloned object literal

  for (int i = 0; i < node->properties()->length(); i++) {
    ObjectLiteral::Property* property = node->properties()->at(i);
    Literal* key = property->key();
    Expression* value = property->value();
    switch (property->kind()) {
      case ObjectLiteral::Property::CONSTANT:
        break;
      case ObjectLiteral::Property::MATERIALIZED_LITERAL:
        if (CompileTimeValue::IsCompileTimeValue(property->value())) break;
        // else fall through
      case ObjectLiteral::Property::COMPUTED:  // fall through
      case ObjectLiteral::Property::PROTOTYPE: {
        frame_->EmitPush(r0);  // dup the result
        LoadAndSpill(key);
        LoadAndSpill(value);
        frame_->CallRuntime(Runtime::kSetProperty, 3);
        // restore r0
        __ ldr(r0, frame_->Top());
        break;
      }
      case ObjectLiteral::Property::SETTER: {
        frame_->EmitPush(r0);
        LoadAndSpill(key);
        __ mov(r0, Operand(Smi::FromInt(1)));
        frame_->EmitPush(r0);
        LoadAndSpill(value);
        frame_->CallRuntime(Runtime::kDefineAccessor, 4);
        __ ldr(r0, frame_->Top());
        break;
      }
      case ObjectLiteral::Property::GETTER: {
        frame_->EmitPush(r0);
        LoadAndSpill(key);
        __ mov(r0, Operand(Smi::FromInt(0)));
        frame_->EmitPush(r0);
        LoadAndSpill(value);
        frame_->CallRuntime(Runtime::kDefineAccessor, 4);
        __ ldr(r0, frame_->Top());
        break;
      }
    }
  }
  ASSERT(frame_->height() == original_height + 1);
}


// This deferred code stub will be used for creating the boilerplate
// by calling Runtime_CreateArrayLiteralBoilerplate.
// Each created boilerplate is stored in the JSFunction and they are
// therefore context dependent.
class DeferredArrayLiteral: public DeferredCode {
 public:
  DeferredArrayLiteral(CodeGenerator* generator, ArrayLiteral* node)
      : DeferredCode(generator), node_(node) {
    set_comment("[ DeferredArrayLiteral");
  }

  virtual void Generate();

 private:
  ArrayLiteral* node_;
};


void DeferredArrayLiteral::Generate() {
  // Argument is passed in r1.
  enter()->Bind();
  VirtualFrame::SpilledScope spilled_scope(generator());

  // If the entry is undefined we call the runtime system to computed
  // the literal.

  VirtualFrame* frame = generator()->frame();
  // Literal array (0).
  frame->EmitPush(r1);
  // Literal index (1).
  __ mov(r0, Operand(Smi::FromInt(node_->literal_index())));
  frame->EmitPush(r0);
  // Constant properties (2).
  __ mov(r0, Operand(node_->literals()));
  frame->EmitPush(r0);
  Result boilerplate =
      frame->CallRuntime(Runtime::kCreateArrayLiteralBoilerplate, 3);
  __ mov(r2, Operand(boilerplate.reg()));
  // Result is returned in r2.
  exit_.Jump();
}


void CodeGenerator::VisitArrayLiteral(ArrayLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ ArrayLiteral");

  DeferredArrayLiteral* deferred = new DeferredArrayLiteral(this, node);

  // Retrieve the literal array and check the allocated entry.

  // Load the function of this activation.
  __ ldr(r1, frame_->Function());

  // Load the literals array of the function.
  __ ldr(r1, FieldMemOperand(r1, JSFunction::kLiteralsOffset));

  // Load the literal at the ast saved index.
  int literal_offset =
      FixedArray::kHeaderSize + node->literal_index() * kPointerSize;
  __ ldr(r2, FieldMemOperand(r1, literal_offset));

  // Check whether we need to materialize the object literal boilerplate.
  // If so, jump to the deferred code.
  __ cmp(r2, Operand(Factory::undefined_value()));
  deferred->enter()->Branch(eq);
  deferred->BindExit();

  // Push the object literal boilerplate.
  frame_->EmitPush(r2);

  // Clone the boilerplate object.
  Runtime::FunctionId clone_function_id = Runtime::kCloneLiteralBoilerplate;
  if (node->depth() == 1) {
    clone_function_id = Runtime::kCloneShallowLiteralBoilerplate;
  }
  frame_->CallRuntime(clone_function_id, 1);
  frame_->EmitPush(r0);  // save the result
  // r0: cloned object literal

  // Generate code to set the elements in the array that are not
  // literals.
  for (int i = 0; i < node->values()->length(); i++) {
    Expression* value = node->values()->at(i);

    // If value is a literal the property value is already set in the
    // boilerplate object.
    if (value->AsLiteral() != NULL) continue;
    // If value is a materialized literal the property value is already set
    // in the boilerplate object if it is simple.
    if (CompileTimeValue::IsCompileTimeValue(value)) continue;

    // The property must be set by generated code.
    LoadAndSpill(value);
    frame_->EmitPop(r0);

    // Fetch the object literal.
    __ ldr(r1, frame_->Top());
    // Get the elements array.
    __ ldr(r1, FieldMemOperand(r1, JSObject::kElementsOffset));

    // Write to the indexed properties array.
    int offset = i * kPointerSize + Array::kHeaderSize;
    __ str(r0, FieldMemOperand(r1, offset));

    // Update the write barrier for the array address.
    __ mov(r3, Operand(offset));
    __ RecordWrite(r1, r3, r2);
  }
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitCatchExtensionObject(CatchExtensionObject* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  ASSERT(!in_spilled_code());
  VirtualFrame::SpilledScope spilled_scope(this);
  // Call runtime routine to allocate the catch extension object and
  // assign the exception value to the catch variable.
  Comment cmnt(masm_, "[ CatchExtensionObject");
  LoadAndSpill(node->key());
  LoadAndSpill(node->value());
  Result result =
      frame_->CallRuntime(Runtime::kCreateCatchExtensionObject, 2);
  frame_->EmitPush(result.reg());
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitAssignment(Assignment* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ Assignment");
  CodeForStatementPosition(node);

  { Reference target(this, node->target());
    if (target.is_illegal()) {
      // Fool the virtual frame into thinking that we left the assignment's
      // value on the frame.
      __ mov(r0, Operand(Smi::FromInt(0)));
      frame_->EmitPush(r0);
      ASSERT(frame_->height() == original_height + 1);
      return;
    }

    if (node->op() == Token::ASSIGN ||
        node->op() == Token::INIT_VAR ||
        node->op() == Token::INIT_CONST) {
      LoadAndSpill(node->value());

    } else {
      target.GetValueAndSpill(NOT_INSIDE_TYPEOF);
      Literal* literal = node->value()->AsLiteral();
      if (literal != NULL && literal->handle()->IsSmi()) {
        SmiOperation(node->binary_op(), literal->handle(), false);
        frame_->EmitPush(r0);

      } else {
        LoadAndSpill(node->value());
        GenericBinaryOperation(node->binary_op());
        frame_->EmitPush(r0);
      }
    }

    Variable* var = node->target()->AsVariableProxy()->AsVariable();
    if (var != NULL &&
        (var->mode() == Variable::CONST) &&
        node->op() != Token::INIT_VAR && node->op() != Token::INIT_CONST) {
      // Assignment ignored - leave the value on the stack.

    } else {
      CodeForSourcePosition(node->position());
      if (node->op() == Token::INIT_CONST) {
        // Dynamic constant initializations must use the function context
        // and initialize the actual constant declared. Dynamic variable
        // initializations are simply assignments and use SetValue.
        target.SetValue(CONST_INIT);
      } else {
        target.SetValue(NOT_CONST_INIT);
      }
    }
  }
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitThrow(Throw* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ Throw");

  LoadAndSpill(node->exception());
  CodeForSourcePosition(node->position());
  frame_->CallRuntime(Runtime::kThrow, 1);
  frame_->EmitPush(r0);
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitProperty(Property* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ Property");

  { Reference property(this, node);
    property.GetValueAndSpill(typeof_state());
  }
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitCall(Call* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ Call");

  ZoneList<Expression*>* args = node->arguments();

  CodeForStatementPosition(node);
  // Standard function call.

  // Check if the function is a variable or a property.
  Expression* function = node->expression();
  Variable* var = function->AsVariableProxy()->AsVariable();
  Property* property = function->AsProperty();

  // ------------------------------------------------------------------------
  // Fast-case: Use inline caching.
  // ---
  // According to ECMA-262, section 11.2.3, page 44, the function to call
  // must be resolved after the arguments have been evaluated. The IC code
  // automatically handles this by loading the arguments before the function
  // is resolved in cache misses (this also holds for megamorphic calls).
  // ------------------------------------------------------------------------

  if (var != NULL && !var->is_this() && var->is_global()) {
    // ----------------------------------
    // JavaScript example: 'foo(1, 2, 3)'  // foo is global
    // ----------------------------------

    // Push the name of the function and the receiver onto the stack.
    __ mov(r0, Operand(var->name()));
    frame_->EmitPush(r0);

    // Pass the global object as the receiver and let the IC stub
    // patch the stack to use the global proxy as 'this' in the
    // invoked function.
    LoadGlobal();

    // Load the arguments.
    int arg_count = args->length();
    for (int i = 0; i < arg_count; i++) {
      LoadAndSpill(args->at(i));
    }

    // Setup the receiver register and call the IC initialization code.
    Handle<Code> stub = ComputeCallInitialize(arg_count);
    CodeForSourcePosition(node->position());
    frame_->CallCodeObject(stub, RelocInfo::CODE_TARGET_CONTEXT,
                           arg_count + 1);
    __ ldr(cp, frame_->Context());
    // Remove the function from the stack.
    frame_->Drop();
    frame_->EmitPush(r0);

  } else if (var != NULL && var->slot() != NULL &&
             var->slot()->type() == Slot::LOOKUP) {
    // ----------------------------------
    // JavaScript example: 'with (obj) foo(1, 2, 3)'  // foo is in obj
    // ----------------------------------

    // Load the function
    frame_->EmitPush(cp);
    __ mov(r0, Operand(var->name()));
    frame_->EmitPush(r0);
    frame_->CallRuntime(Runtime::kLoadContextSlot, 2);
    // r0: slot value; r1: receiver

    // Load the receiver.
    frame_->EmitPush(r0);  // function
    frame_->EmitPush(r1);  // receiver

    // Call the function.
    CallWithArguments(args, node->position());
    frame_->EmitPush(r0);

  } else if (property != NULL) {
    // Check if the key is a literal string.
    Literal* literal = property->key()->AsLiteral();

    if (literal != NULL && literal->handle()->IsSymbol()) {
      // ------------------------------------------------------------------
      // JavaScript example: 'object.foo(1, 2, 3)' or 'map["key"](1, 2, 3)'
      // ------------------------------------------------------------------

      // Push the name of the function and the receiver onto the stack.
      __ mov(r0, Operand(literal->handle()));
      frame_->EmitPush(r0);
      LoadAndSpill(property->obj());

      // Load the arguments.
      int arg_count = args->length();
      for (int i = 0; i < arg_count; i++) {
        LoadAndSpill(args->at(i));
      }

      // Set the receiver register and call the IC initialization code.
      Handle<Code> stub = ComputeCallInitialize(arg_count);
      CodeForSourcePosition(node->position());
      frame_->CallCodeObject(stub, RelocInfo::CODE_TARGET, arg_count + 1);
      __ ldr(cp, frame_->Context());

      // Remove the function from the stack.
      frame_->Drop();

      frame_->EmitPush(r0);  // push after get rid of function from the stack

    } else {
      // -------------------------------------------
      // JavaScript example: 'array[index](1, 2, 3)'
      // -------------------------------------------

      // Load the function to call from the property through a reference.
      Reference ref(this, property);
      ref.GetValueAndSpill(NOT_INSIDE_TYPEOF);  // receiver

      // Pass receiver to called function.
      if (property->is_synthetic()) {
        LoadGlobalReceiver(r0);
      } else {
        __ ldr(r0, frame_->ElementAt(ref.size()));
        frame_->EmitPush(r0);
      }

      // Call the function.
      CallWithArguments(args, node->position());
      frame_->EmitPush(r0);
    }

  } else {
    // ----------------------------------
    // JavaScript example: 'foo(1, 2, 3)'  // foo is not global
    // ----------------------------------

    // Load the function.
    LoadAndSpill(function);

    // Pass the global proxy as the receiver.
    LoadGlobalReceiver(r0);

    // Call the function.
    CallWithArguments(args, node->position());
    frame_->EmitPush(r0);
  }
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitCallEval(CallEval* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ CallEval");

  // In a call to eval, we first call %ResolvePossiblyDirectEval to resolve
  // the function we need to call and the receiver of the call.
  // Then we call the resolved function using the given arguments.

  ZoneList<Expression*>* args = node->arguments();
  Expression* function = node->expression();

  CodeForStatementPosition(node);

  // Prepare stack for call to resolved function.
  LoadAndSpill(function);
  __ mov(r2, Operand(Factory::undefined_value()));
  frame_->EmitPush(r2);  // Slot for receiver
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    LoadAndSpill(args->at(i));
  }

  // Prepare stack for call to ResolvePossiblyDirectEval.
  __ ldr(r1, MemOperand(sp, arg_count * kPointerSize + kPointerSize));
  frame_->EmitPush(r1);
  if (arg_count > 0) {
    __ ldr(r1, MemOperand(sp, arg_count * kPointerSize));
    frame_->EmitPush(r1);
  } else {
    frame_->EmitPush(r2);
  }

  // Resolve the call.
  frame_->CallRuntime(Runtime::kResolvePossiblyDirectEval, 2);

  // Touch up stack with the right values for the function and the receiver.
  __ ldr(r1, FieldMemOperand(r0, FixedArray::kHeaderSize));
  __ str(r1, MemOperand(sp, (arg_count + 1) * kPointerSize));
  __ ldr(r1, FieldMemOperand(r0, FixedArray::kHeaderSize + kPointerSize));
  __ str(r1, MemOperand(sp, arg_count * kPointerSize));

  // Call the function.
  CodeForSourcePosition(node->position());

  CallFunctionStub call_function(arg_count);
  frame_->CallStub(&call_function, arg_count + 1);

  __ ldr(cp, frame_->Context());
  // Remove the function from the stack.
  frame_->Drop();
  frame_->EmitPush(r0);
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitCallNew(CallNew* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ CallNew");
  CodeForStatementPosition(node);

  // According to ECMA-262, section 11.2.2, page 44, the function
  // expression in new calls must be evaluated before the
  // arguments. This is different from ordinary calls, where the
  // actual function to call is resolved after the arguments have been
  // evaluated.

  // Compute function to call and use the global object as the
  // receiver. There is no need to use the global proxy here because
  // it will always be replaced with a newly allocated object.
  LoadAndSpill(node->expression());
  LoadGlobal();

  // Push the arguments ("left-to-right") on the stack.
  ZoneList<Expression*>* args = node->arguments();
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    LoadAndSpill(args->at(i));
  }

  // r0: the number of arguments.
  Result num_args = allocator_->Allocate(r0);
  ASSERT(num_args.is_valid());
  __ mov(num_args.reg(), Operand(arg_count));

  // Load the function into r1 as per calling convention.
  Result function = allocator_->Allocate(r1);
  ASSERT(function.is_valid());
  __ ldr(function.reg(), frame_->ElementAt(arg_count + 1));

  // Call the construct call builtin that handles allocation and
  // constructor invocation.
  CodeForSourcePosition(node->position());
  Handle<Code> ic(Builtins::builtin(Builtins::JSConstructCall));
  Result result = frame_->CallCodeObject(ic,
                                         RelocInfo::CONSTRUCT_CALL,
                                         &num_args,
                                         &function,
                                         arg_count + 1);

  // Discard old TOS value and push r0 on the stack (same as Pop(), push(r0)).
  __ str(r0, frame_->Top());
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::GenerateValueOf(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(this);
  ASSERT(args->length() == 1);
  JumpTarget leave(this);
  LoadAndSpill(args->at(0));
  frame_->EmitPop(r0);  // r0 contains object.
  // if (object->IsSmi()) return the object.
  __ tst(r0, Operand(kSmiTagMask));
  leave.Branch(eq);
  // It is a heap object - get map.
  __ ldr(r1, FieldMemOperand(r0, HeapObject::kMapOffset));
  __ ldrb(r1, FieldMemOperand(r1, Map::kInstanceTypeOffset));
  // if (!object->IsJSValue()) return the object.
  __ cmp(r1, Operand(JS_VALUE_TYPE));
  leave.Branch(ne);
  // Load the value.
  __ ldr(r0, FieldMemOperand(r0, JSValue::kValueOffset));
  leave.Bind();
  frame_->EmitPush(r0);
}


void CodeGenerator::GenerateSetValueOf(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(this);
  ASSERT(args->length() == 2);
  JumpTarget leave(this);
  LoadAndSpill(args->at(0));  // Load the object.
  LoadAndSpill(args->at(1));  // Load the value.
  frame_->EmitPop(r0);  // r0 contains value
  frame_->EmitPop(r1);  // r1 contains object
  // if (object->IsSmi()) return object.
  __ tst(r1, Operand(kSmiTagMask));
  leave.Branch(eq);
  // It is a heap object - get map.
  __ ldr(r2, FieldMemOperand(r1, HeapObject::kMapOffset));
  __ ldrb(r2, FieldMemOperand(r2, Map::kInstanceTypeOffset));
  // if (!object->IsJSValue()) return object.
  __ cmp(r2, Operand(JS_VALUE_TYPE));
  leave.Branch(ne);
  // Store the value.
  __ str(r0, FieldMemOperand(r1, JSValue::kValueOffset));
  // Update the write barrier.
  __ mov(r2, Operand(JSValue::kValueOffset - kHeapObjectTag));
  __ RecordWrite(r1, r2, r3);
  // Leave.
  leave.Bind();
  frame_->EmitPush(r0);
}


void CodeGenerator::GenerateIsSmi(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(this);
  ASSERT(args->length() == 1);
  LoadAndSpill(args->at(0));
  frame_->EmitPop(r0);
  __ tst(r0, Operand(kSmiTagMask));
  cc_reg_ = eq;
}


void CodeGenerator::GenerateLog(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(this);
  // See comment in CodeGenerator::GenerateLog in codegen-ia32.cc.
  ASSERT_EQ(args->length(), 3);
#ifdef ENABLE_LOGGING_AND_PROFILING
  if (ShouldGenerateLog(args->at(0))) {
    LoadAndSpill(args->at(1));
    LoadAndSpill(args->at(2));
    __ CallRuntime(Runtime::kLog, 2);
  }
#endif
  __ mov(r0, Operand(Factory::undefined_value()));
  frame_->EmitPush(r0);
}


void CodeGenerator::GenerateIsNonNegativeSmi(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(this);
  ASSERT(args->length() == 1);
  LoadAndSpill(args->at(0));
  frame_->EmitPop(r0);
  __ tst(r0, Operand(kSmiTagMask | 0x80000000));
  cc_reg_ = eq;
}


// This should generate code that performs a charCodeAt() call or returns
// undefined in order to trigger the slow case, Runtime_StringCharCodeAt.
// It is not yet implemented on ARM, so it always goes to the slow case.
void CodeGenerator::GenerateFastCharCodeAt(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(this);
  ASSERT(args->length() == 2);
  __ mov(r0, Operand(Factory::undefined_value()));
  frame_->EmitPush(r0);
}


void CodeGenerator::GenerateIsArray(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(this);
  ASSERT(args->length() == 1);
  LoadAndSpill(args->at(0));
  JumpTarget answer(this);
  // We need the CC bits to come out as not_equal in the case where the
  // object is a smi.  This can't be done with the usual test opcode so
  // we use XOR to get the right CC bits.
  frame_->EmitPop(r0);
  __ and_(r1, r0, Operand(kSmiTagMask));
  __ eor(r1, r1, Operand(kSmiTagMask), SetCC);
  answer.Branch(ne);
  // It is a heap object - get the map.
  __ ldr(r1, FieldMemOperand(r0, HeapObject::kMapOffset));
  __ ldrb(r1, FieldMemOperand(r1, Map::kInstanceTypeOffset));
  // Check if the object is a JS array or not.
  __ cmp(r1, Operand(JS_ARRAY_TYPE));
  answer.Bind();
  cc_reg_ = eq;
}


void CodeGenerator::GenerateArgumentsLength(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(this);
  ASSERT(args->length() == 0);

  // Seed the result with the formal parameters count, which will be used
  // in case no arguments adaptor frame is found below the current frame.
  __ mov(r0, Operand(Smi::FromInt(scope_->num_parameters())));

  // Call the shared stub to get to the arguments.length.
  ArgumentsAccessStub stub(ArgumentsAccessStub::READ_LENGTH);
  frame_->CallStub(&stub, 0);
  frame_->EmitPush(r0);
}


void CodeGenerator::GenerateArgumentsAccess(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(this);
  ASSERT(args->length() == 1);

  // Satisfy contract with ArgumentsAccessStub:
  // Load the key into r1 and the formal parameters count into r0.
  LoadAndSpill(args->at(0));
  frame_->EmitPop(r1);
  __ mov(r0, Operand(Smi::FromInt(scope_->num_parameters())));

  // Call the shared stub to get to arguments[key].
  ArgumentsAccessStub stub(ArgumentsAccessStub::READ_ELEMENT);
  frame_->CallStub(&stub, 0);
  frame_->EmitPush(r0);
}


void CodeGenerator::GenerateObjectEquals(ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(this);
  ASSERT(args->length() == 2);

  // Load the two objects into registers and perform the comparison.
  LoadAndSpill(args->at(0));
  LoadAndSpill(args->at(1));
  frame_->EmitPop(r0);
  frame_->EmitPop(r1);
  __ cmp(r0, Operand(r1));
  cc_reg_ = eq;
}


void CodeGenerator::VisitCallRuntime(CallRuntime* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  if (CheckForInlineRuntimeCall(node)) {
    ASSERT((has_cc() && frame_->height() == original_height) ||
           (!has_cc() && frame_->height() == original_height + 1));
    return;
  }

  ZoneList<Expression*>* args = node->arguments();
  Comment cmnt(masm_, "[ CallRuntime");
  Runtime::Function* function = node->function();

  if (function == NULL) {
    // Prepare stack for calling JS runtime function.
    __ mov(r0, Operand(node->name()));
    frame_->EmitPush(r0);
    // Push the builtins object found in the current global object.
    __ ldr(r1, GlobalObject());
    __ ldr(r0, FieldMemOperand(r1, GlobalObject::kBuiltinsOffset));
    frame_->EmitPush(r0);
  }

  // Push the arguments ("left-to-right").
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    LoadAndSpill(args->at(i));
  }

  if (function == NULL) {
    // Call the JS runtime function.
    Handle<Code> stub = ComputeCallInitialize(arg_count);
    frame_->CallCodeObject(stub, RelocInfo::CODE_TARGET, arg_count + 1);
    __ ldr(cp, frame_->Context());
    frame_->Drop();
    frame_->EmitPush(r0);
  } else {
    // Call the C runtime function.
    frame_->CallRuntime(function, arg_count);
    frame_->EmitPush(r0);
  }
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitUnaryOperation(UnaryOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ UnaryOperation");

  Token::Value op = node->op();

  if (op == Token::NOT) {
    LoadConditionAndSpill(node->expression(),
                          NOT_INSIDE_TYPEOF,
                          false_target(),
                          true_target(),
                          true);
    cc_reg_ = NegateCondition(cc_reg_);

  } else if (op == Token::DELETE) {
    Property* property = node->expression()->AsProperty();
    Variable* variable = node->expression()->AsVariableProxy()->AsVariable();
    if (property != NULL) {
      LoadAndSpill(property->obj());
      LoadAndSpill(property->key());
      Result arg_count = allocator_->Allocate(r0);
      ASSERT(arg_count.is_valid());
      __ mov(arg_count.reg(), Operand(1));  // not counting receiver
      frame_->InvokeBuiltin(Builtins::DELETE, CALL_JS, &arg_count, 2);

    } else if (variable != NULL) {
      Slot* slot = variable->slot();
      if (variable->is_global()) {
        LoadGlobal();
        __ mov(r0, Operand(variable->name()));
        frame_->EmitPush(r0);
        Result arg_count = allocator_->Allocate(r0);
        ASSERT(arg_count.is_valid());
        __ mov(arg_count.reg(), Operand(1));  // not counting receiver
        frame_->InvokeBuiltin(Builtins::DELETE, CALL_JS, &arg_count, 2);

      } else if (slot != NULL && slot->type() == Slot::LOOKUP) {
        // lookup the context holding the named variable
        frame_->EmitPush(cp);
        __ mov(r0, Operand(variable->name()));
        frame_->EmitPush(r0);
        frame_->CallRuntime(Runtime::kLookupContext, 2);
        // r0: context
        frame_->EmitPush(r0);
        __ mov(r0, Operand(variable->name()));
        frame_->EmitPush(r0);
        Result arg_count = allocator_->Allocate(r0);
        ASSERT(arg_count.is_valid());
        __ mov(arg_count.reg(), Operand(1));  // not counting receiver
        frame_->InvokeBuiltin(Builtins::DELETE, CALL_JS, &arg_count, 2);

      } else {
        // Default: Result of deleting non-global, not dynamically
        // introduced variables is false.
        __ mov(r0, Operand(Factory::false_value()));
      }

    } else {
      // Default: Result of deleting expressions is true.
      LoadAndSpill(node->expression());  // may have side-effects
      frame_->Drop();
      __ mov(r0, Operand(Factory::true_value()));
    }
    frame_->EmitPush(r0);

  } else if (op == Token::TYPEOF) {
    // Special case for loading the typeof expression; see comment on
    // LoadTypeofExpression().
    LoadTypeofExpression(node->expression());
    frame_->CallRuntime(Runtime::kTypeof, 1);
    frame_->EmitPush(r0);  // r0 has result

  } else {
    LoadAndSpill(node->expression());
    frame_->EmitPop(r0);
    switch (op) {
      case Token::NOT:
      case Token::DELETE:
      case Token::TYPEOF:
        UNREACHABLE();  // handled above
        break;

      case Token::SUB: {
        UnarySubStub stub;
        frame_->CallStub(&stub, 0);
        break;
      }

      case Token::BIT_NOT: {
        // smi check
        JumpTarget smi_label(this);
        JumpTarget continue_label(this);
        __ tst(r0, Operand(kSmiTagMask));
        smi_label.Branch(eq);

        frame_->EmitPush(r0);
        Result arg_count = allocator_->Allocate(r0);
        ASSERT(arg_count.is_valid());
        __ mov(arg_count.reg(), Operand(0));  // not counting receiver
        frame_->InvokeBuiltin(Builtins::BIT_NOT, CALL_JS, &arg_count, 1);

        continue_label.Jump();
        smi_label.Bind();
        __ mvn(r0, Operand(r0));
        __ bic(r0, r0, Operand(kSmiTagMask));  // bit-clear inverted smi-tag
        continue_label.Bind();
        break;
      }

      case Token::VOID:
        // since the stack top is cached in r0, popping and then
        // pushing a value can be done by just writing to r0.
        __ mov(r0, Operand(Factory::undefined_value()));
        break;

      case Token::ADD: {
        // Smi check.
        JumpTarget continue_label(this);
        __ tst(r0, Operand(kSmiTagMask));
        continue_label.Branch(eq);
        frame_->EmitPush(r0);
        Result arg_count = allocator_->Allocate(r0);
        ASSERT(arg_count.is_valid());
        __ mov(arg_count.reg(), Operand(0));  // not counting receiver
        frame_->InvokeBuiltin(Builtins::TO_NUMBER, CALL_JS, &arg_count, 1);
        continue_label.Bind();
        break;
      }
      default:
        UNREACHABLE();
    }
    frame_->EmitPush(r0);  // r0 has result
  }
  ASSERT((has_cc() && frame_->height() == original_height) ||
         (!has_cc() && frame_->height() == original_height + 1));
}


void CodeGenerator::VisitCountOperation(CountOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ CountOperation");

  bool is_postfix = node->is_postfix();
  bool is_increment = node->op() == Token::INC;

  Variable* var = node->expression()->AsVariableProxy()->AsVariable();
  bool is_const = (var != NULL && var->mode() == Variable::CONST);

  // Postfix: Make room for the result.
  if (is_postfix) {
     __ mov(r0, Operand(0));
     frame_->EmitPush(r0);
  }

  { Reference target(this, node->expression());
    if (target.is_illegal()) {
      // Spoof the virtual frame to have the expected height (one higher
      // than on entry).
      if (!is_postfix) {
        __ mov(r0, Operand(Smi::FromInt(0)));
        frame_->EmitPush(r0);
      }
      ASSERT(frame_->height() == original_height + 1);
      return;
    }
    target.GetValueAndSpill(NOT_INSIDE_TYPEOF);
    frame_->EmitPop(r0);

    JumpTarget slow(this);
    JumpTarget exit(this);

    // Load the value (1) into register r1.
    __ mov(r1, Operand(Smi::FromInt(1)));

    // Check for smi operand.
    __ tst(r0, Operand(kSmiTagMask));
    slow.Branch(ne);

    // Postfix: Store the old value as the result.
    if (is_postfix) {
      __ str(r0, frame_->ElementAt(target.size()));
    }

    // Perform optimistic increment/decrement.
    if (is_increment) {
      __ add(r0, r0, Operand(r1), SetCC);
    } else {
      __ sub(r0, r0, Operand(r1), SetCC);
    }

    // If the increment/decrement didn't overflow, we're done.
    exit.Branch(vc);

    // Revert optimistic increment/decrement.
    if (is_increment) {
      __ sub(r0, r0, Operand(r1));
    } else {
      __ add(r0, r0, Operand(r1));
    }

    // Slow case: Convert to number.
    slow.Bind();
    {
      // Convert the operand to a number.
      frame_->EmitPush(r0);
      Result arg_count = allocator_->Allocate(r0);
      ASSERT(arg_count.is_valid());
      __ mov(arg_count.reg(), Operand(0));
      frame_->InvokeBuiltin(Builtins::TO_NUMBER, CALL_JS, &arg_count, 1);
    }
    if (is_postfix) {
      // Postfix: store to result (on the stack).
      __ str(r0, frame_->ElementAt(target.size()));
    }

    // Compute the new value.
    __ mov(r1, Operand(Smi::FromInt(1)));
    frame_->EmitPush(r0);
    frame_->EmitPush(r1);
    if (is_increment) {
      frame_->CallRuntime(Runtime::kNumberAdd, 2);
    } else {
      frame_->CallRuntime(Runtime::kNumberSub, 2);
    }

    // Store the new value in the target if not const.
    exit.Bind();
    frame_->EmitPush(r0);
    if (!is_const) target.SetValue(NOT_CONST_INIT);
  }

  // Postfix: Discard the new value and use the old.
  if (is_postfix) frame_->EmitPop(r0);
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitBinaryOperation(BinaryOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ BinaryOperation");
  Token::Value op = node->op();

  // According to ECMA-262 section 11.11, page 58, the binary logical
  // operators must yield the result of one of the two expressions
  // before any ToBoolean() conversions. This means that the value
  // produced by a && or || operator is not necessarily a boolean.

  // NOTE: If the left hand side produces a materialized value (not in
  // the CC register), we force the right hand side to do the
  // same. This is necessary because we may have to branch to the exit
  // after evaluating the left hand side (due to the shortcut
  // semantics), but the compiler must (statically) know if the result
  // of compiling the binary operation is materialized or not.

  if (op == Token::AND) {
    JumpTarget is_true(this);
    LoadConditionAndSpill(node->left(),
                          NOT_INSIDE_TYPEOF,
                          &is_true,
                          false_target(),
                          false);
    if (has_cc()) {
      Branch(false, false_target());

      // Evaluate right side expression.
      is_true.Bind();
      LoadConditionAndSpill(node->right(),
                            NOT_INSIDE_TYPEOF,
                            true_target(),
                            false_target(),
                            false);

    } else {
      JumpTarget pop_and_continue(this);
      JumpTarget exit(this);

      __ ldr(r0, frame_->Top());  // dup the stack top
      frame_->EmitPush(r0);
      // Avoid popping the result if it converts to 'false' using the
      // standard ToBoolean() conversion as described in ECMA-262,
      // section 9.2, page 30.
      ToBoolean(&pop_and_continue, &exit);
      Branch(false, &exit);

      // Pop the result of evaluating the first part.
      pop_and_continue.Bind();
      frame_->EmitPop(r0);

      // Evaluate right side expression.
      is_true.Bind();
      LoadAndSpill(node->right());

      // Exit (always with a materialized value).
      exit.Bind();
    }

  } else if (op == Token::OR) {
    JumpTarget is_false(this);
    LoadConditionAndSpill(node->left(),
                          NOT_INSIDE_TYPEOF,
                          true_target(),
                          &is_false,
                          false);
    if (has_cc()) {
      Branch(true, true_target());

      // Evaluate right side expression.
      is_false.Bind();
      LoadConditionAndSpill(node->right(),
                            NOT_INSIDE_TYPEOF,
                            true_target(),
                            false_target(),
                            false);

    } else {
      JumpTarget pop_and_continue(this);
      JumpTarget exit(this);

      __ ldr(r0, frame_->Top());
      frame_->EmitPush(r0);
      // Avoid popping the result if it converts to 'true' using the
      // standard ToBoolean() conversion as described in ECMA-262,
      // section 9.2, page 30.
      ToBoolean(&exit, &pop_and_continue);
      Branch(true, &exit);

      // Pop the result of evaluating the first part.
      pop_and_continue.Bind();
      frame_->EmitPop(r0);

      // Evaluate right side expression.
      is_false.Bind();
      LoadAndSpill(node->right());

      // Exit (always with a materialized value).
      exit.Bind();
    }

  } else {
    // Optimize for the case where (at least) one of the expressions
    // is a literal small integer.
    Literal* lliteral = node->left()->AsLiteral();
    Literal* rliteral = node->right()->AsLiteral();

    if (rliteral != NULL && rliteral->handle()->IsSmi()) {
      LoadAndSpill(node->left());
      SmiOperation(node->op(), rliteral->handle(), false);

    } else if (lliteral != NULL && lliteral->handle()->IsSmi()) {
      LoadAndSpill(node->right());
      SmiOperation(node->op(), lliteral->handle(), true);

    } else {
      LoadAndSpill(node->left());
      LoadAndSpill(node->right());
      GenericBinaryOperation(node->op());
    }
    frame_->EmitPush(r0);
  }
  ASSERT((has_cc() && frame_->height() == original_height) ||
         (!has_cc() && frame_->height() == original_height + 1));
}


void CodeGenerator::VisitThisFunction(ThisFunction* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  __ ldr(r0, frame_->Function());
  frame_->EmitPush(r0);
  ASSERT(frame_->height() == original_height + 1);
}


void CodeGenerator::VisitCompareOperation(CompareOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(this);
  Comment cmnt(masm_, "[ CompareOperation");

  // Get the expressions from the node.
  Expression* left = node->left();
  Expression* right = node->right();
  Token::Value op = node->op();

  // To make null checks efficient, we check if either left or right is the
  // literal 'null'. If so, we optimize the code by inlining a null check
  // instead of calling the (very) general runtime routine for checking
  // equality.
  if (op == Token::EQ || op == Token::EQ_STRICT) {
    bool left_is_null =
        left->AsLiteral() != NULL && left->AsLiteral()->IsNull();
    bool right_is_null =
        right->AsLiteral() != NULL && right->AsLiteral()->IsNull();
    // The 'null' value can only be equal to 'null' or 'undefined'.
    if (left_is_null || right_is_null) {
      LoadAndSpill(left_is_null ? right : left);
      frame_->EmitPop(r0);
      __ cmp(r0, Operand(Factory::null_value()));

      // The 'null' value is only equal to 'undefined' if using non-strict
      // comparisons.
      if (op != Token::EQ_STRICT) {
        true_target()->Branch(eq);

        __ cmp(r0, Operand(Factory::undefined_value()));
        true_target()->Branch(eq);

        __ tst(r0, Operand(kSmiTagMask));
        false_target()->Branch(eq);

        // It can be an undetectable object.
        __ ldr(r0, FieldMemOperand(r0, HeapObject::kMapOffset));
        __ ldrb(r0, FieldMemOperand(r0, Map::kBitFieldOffset));
        __ and_(r0, r0, Operand(1 << Map::kIsUndetectable));
        __ cmp(r0, Operand(1 << Map::kIsUndetectable));
      }

      cc_reg_ = eq;
      ASSERT(has_cc() && frame_->height() == original_height);
      return;
    }
  }

  // To make typeof testing for natives implemented in JavaScript really
  // efficient, we generate special code for expressions of the form:
  // 'typeof <expression> == <string>'.
  UnaryOperation* operation = left->AsUnaryOperation();
  if ((op == Token::EQ || op == Token::EQ_STRICT) &&
      (operation != NULL && operation->op() == Token::TYPEOF) &&
      (right->AsLiteral() != NULL &&
       right->AsLiteral()->handle()->IsString())) {
    Handle<String> check(String::cast(*right->AsLiteral()->handle()));

    // Load the operand, move it to register r1.
    LoadTypeofExpression(operation->expression());
    frame_->EmitPop(r1);

    if (check->Equals(Heap::number_symbol())) {
      __ tst(r1, Operand(kSmiTagMask));
      true_target()->Branch(eq);
      __ ldr(r1, FieldMemOperand(r1, HeapObject::kMapOffset));
      __ cmp(r1, Operand(Factory::heap_number_map()));
      cc_reg_ = eq;

    } else if (check->Equals(Heap::string_symbol())) {
      __ tst(r1, Operand(kSmiTagMask));
      false_target()->Branch(eq);

      __ ldr(r1, FieldMemOperand(r1, HeapObject::kMapOffset));

      // It can be an undetectable string object.
      __ ldrb(r2, FieldMemOperand(r1, Map::kBitFieldOffset));
      __ and_(r2, r2, Operand(1 << Map::kIsUndetectable));
      __ cmp(r2, Operand(1 << Map::kIsUndetectable));
      false_target()->Branch(eq);

      __ ldrb(r2, FieldMemOperand(r1, Map::kInstanceTypeOffset));
      __ cmp(r2, Operand(FIRST_NONSTRING_TYPE));
      cc_reg_ = lt;

    } else if (check->Equals(Heap::boolean_symbol())) {
      __ cmp(r1, Operand(Factory::true_value()));
      true_target()->Branch(eq);
      __ cmp(r1, Operand(Factory::false_value()));
      cc_reg_ = eq;

    } else if (check->Equals(Heap::undefined_symbol())) {
      __ cmp(r1, Operand(Factory::undefined_value()));
      true_target()->Branch(eq);

      __ tst(r1, Operand(kSmiTagMask));
      false_target()->Branch(eq);

      // It can be an undetectable object.
      __ ldr(r1, FieldMemOperand(r1, HeapObject::kMapOffset));
      __ ldrb(r2, FieldMemOperand(r1, Map::kBitFieldOffset));
      __ and_(r2, r2, Operand(1 << Map::kIsUndetectable));
      __ cmp(r2, Operand(1 << Map::kIsUndetectable));

      cc_reg_ = eq;

    } else if (check->Equals(Heap::function_symbol())) {
      __ tst(r1, Operand(kSmiTagMask));
      false_target()->Branch(eq);
      __ ldr(r1, FieldMemOperand(r1, HeapObject::kMapOffset));
      __ ldrb(r1, FieldMemOperand(r1, Map::kInstanceTypeOffset));
      __ cmp(r1, Operand(JS_FUNCTION_TYPE));
      cc_reg_ = eq;

    } else if (check->Equals(Heap::object_symbol())) {
      __ tst(r1, Operand(kSmiTagMask));
      false_target()->Branch(eq);

      __ ldr(r2, FieldMemOperand(r1, HeapObject::kMapOffset));
      __ cmp(r1, Operand(Factory::null_value()));
      true_target()->Branch(eq);

      // It can be an undetectable object.
      __ ldrb(r1, FieldMemOperand(r2, Map::kBitFieldOffset));
      __ and_(r1, r1, Operand(1 << Map::kIsUndetectable));
      __ cmp(r1, Operand(1 << Map::kIsUndetectable));
      false_target()->Branch(eq);

      __ ldrb(r2, FieldMemOperand(r2, Map::kInstanceTypeOffset));
      __ cmp(r2, Operand(FIRST_JS_OBJECT_TYPE));
      false_target()->Branch(lt);
      __ cmp(r2, Operand(LAST_JS_OBJECT_TYPE));
      cc_reg_ = le;

    } else {
      // Uncommon case: typeof testing against a string literal that is
      // never returned from the typeof operator.
      false_target()->Jump();
    }
    ASSERT(!has_valid_frame() ||
           (has_cc() && frame_->height() == original_height));
    return;
  }

  LoadAndSpill(left);
  LoadAndSpill(right);
  switch (op) {
    case Token::EQ:
      Comparison(eq, false);
      break;

    case Token::LT:
      Comparison(lt);
      break;

    case Token::GT:
      Comparison(gt);
      break;

    case Token::LTE:
      Comparison(le);
      break;

    case Token::GTE:
      Comparison(ge);
      break;

    case Token::EQ_STRICT:
      Comparison(eq, true);
      break;

    case Token::IN: {
      Result arg_count = allocator_->Allocate(r0);
      ASSERT(arg_count.is_valid());
      __ mov(arg_count.reg(), Operand(1));  // not counting receiver
      Result result = frame_->InvokeBuiltin(Builtins::IN,
                                            CALL_JS,
                                            &arg_count,
                                            2);
      frame_->EmitPush(result.reg());
      break;
    }

    case Token::INSTANCEOF: {
      Result arg_count = allocator_->Allocate(r0);
      ASSERT(arg_count.is_valid());
      __ mov(arg_count.reg(), Operand(1));  // not counting receiver
      Result result = frame_->InvokeBuiltin(Builtins::INSTANCE_OF,
                                            CALL_JS,
                                            &arg_count,
                                            2);
      __ tst(result.reg(), Operand(result.reg()));
      cc_reg_ = eq;
      break;
    }

    default:
      UNREACHABLE();
  }
  ASSERT((has_cc() && frame_->height() == original_height) ||
         (!has_cc() && frame_->height() == original_height + 1));
}


#ifdef DEBUG
bool CodeGenerator::HasValidEntryRegisters() { return true; }
#endif


#undef __
#define __ masm->

Handle<String> Reference::GetName() {
  ASSERT(type_ == NAMED);
  Property* property = expression_->AsProperty();
  if (property == NULL) {
    // Global variable reference treated as a named property reference.
    VariableProxy* proxy = expression_->AsVariableProxy();
    ASSERT(proxy->AsVariable() != NULL);
    ASSERT(proxy->AsVariable()->is_global());
    return proxy->name();
  } else {
    Literal* raw_name = property->key()->AsLiteral();
    ASSERT(raw_name != NULL);
    return Handle<String>(String::cast(*raw_name->handle()));
  }
}


void Reference::GetValueAndSpill(TypeofState typeof_state) {
  ASSERT(cgen_->in_spilled_code());
  cgen_->set_in_spilled_code(false);
  GetValue(typeof_state);
  cgen_->frame()->SpillAll();
  cgen_->set_in_spilled_code(true);
}


void Reference::GetValue(TypeofState typeof_state) {
  ASSERT(!cgen_->in_spilled_code());
  ASSERT(cgen_->HasValidEntryRegisters());
  ASSERT(!is_illegal());
  ASSERT(!cgen_->has_cc());
  MacroAssembler* masm = cgen_->masm();
  Property* property = expression_->AsProperty();
  if (property != NULL) {
    cgen_->CodeForSourcePosition(property->position());
  }

  switch (type_) {
    case SLOT: {
      Comment cmnt(masm, "[ Load from Slot");
      Slot* slot = expression_->AsVariableProxy()->AsVariable()->slot();
      ASSERT(slot != NULL);
      cgen_->LoadFromSlot(slot, typeof_state);
      break;
    }

    case NAMED: {
      // TODO(1241834): Make sure that this it is safe to ignore the
      // distinction between expressions in a typeof and not in a typeof. If
      // there is a chance that reference errors can be thrown below, we
      // must distinguish between the two kinds of loads (typeof expression
      // loads must not throw a reference error).
      VirtualFrame* frame = cgen_->frame();
      Comment cmnt(masm, "[ Load from named Property");
      Handle<String> name(GetName());
      Variable* var = expression_->AsVariableProxy()->AsVariable();
      Handle<Code> ic(Builtins::builtin(Builtins::LoadIC_Initialize));
      // Setup the name register.
      Result name_reg = cgen_->allocator()->Allocate(r2);
      ASSERT(name_reg.is_valid());
      __ mov(name_reg.reg(), Operand(name));
      ASSERT(var == NULL || var->is_global());
      RelocInfo::Mode rmode = (var == NULL)
                            ? RelocInfo::CODE_TARGET
                            : RelocInfo::CODE_TARGET_CONTEXT;
      Result answer = frame->CallCodeObject(ic, rmode, &name_reg, 0);
      frame->EmitPush(answer.reg());
      break;
    }

    case KEYED: {
      // TODO(1241834): Make sure that this it is safe to ignore the
      // distinction between expressions in a typeof and not in a typeof.

      // TODO(181): Implement inlined version of array indexing once
      // loop nesting is properly tracked on ARM.
      VirtualFrame* frame = cgen_->frame();
      Comment cmnt(masm, "[ Load from keyed Property");
      ASSERT(property != NULL);
      Handle<Code> ic(Builtins::builtin(Builtins::KeyedLoadIC_Initialize));
      Variable* var = expression_->AsVariableProxy()->AsVariable();
      ASSERT(var == NULL || var->is_global());
      RelocInfo::Mode rmode = (var == NULL)
                            ? RelocInfo::CODE_TARGET
                            : RelocInfo::CODE_TARGET_CONTEXT;
      Result answer = frame->CallCodeObject(ic, rmode, 0);
      frame->EmitPush(answer.reg());
      break;
    }

    default:
      UNREACHABLE();
  }
}


void Reference::SetValue(InitState init_state) {
  ASSERT(!is_illegal());
  ASSERT(!cgen_->has_cc());
  MacroAssembler* masm = cgen_->masm();
  VirtualFrame* frame = cgen_->frame();
  Property* property = expression_->AsProperty();
  if (property != NULL) {
    cgen_->CodeForSourcePosition(property->position());
  }

  switch (type_) {
    case SLOT: {
      Comment cmnt(masm, "[ Store to Slot");
      Slot* slot = expression_->AsVariableProxy()->AsVariable()->slot();
      ASSERT(slot != NULL);
      if (slot->type() == Slot::LOOKUP) {
        ASSERT(slot->var()->is_dynamic());

        // For now, just do a runtime call.
        frame->EmitPush(cp);
        __ mov(r0, Operand(slot->var()->name()));
        frame->EmitPush(r0);

        if (init_state == CONST_INIT) {
          // Same as the case for a normal store, but ignores attribute
          // (e.g. READ_ONLY) of context slot so that we can initialize
          // const properties (introduced via eval("const foo = (some
          // expr);")). Also, uses the current function context instead of
          // the top context.
          //
          // Note that we must declare the foo upon entry of eval(), via a
          // context slot declaration, but we cannot initialize it at the
          // same time, because the const declaration may be at the end of
          // the eval code (sigh...) and the const variable may have been
          // used before (where its value is 'undefined'). Thus, we can only
          // do the initialization when we actually encounter the expression
          // and when the expression operands are defined and valid, and
          // thus we need the split into 2 operations: declaration of the
          // context slot followed by initialization.
          frame->CallRuntime(Runtime::kInitializeConstContextSlot, 3);
        } else {
          frame->CallRuntime(Runtime::kStoreContextSlot, 3);
        }
        // Storing a variable must keep the (new) value on the expression
        // stack. This is necessary for compiling assignment expressions.
        frame->EmitPush(r0);

      } else {
        ASSERT(!slot->var()->is_dynamic());

        JumpTarget exit(cgen_);
        if (init_state == CONST_INIT) {
          ASSERT(slot->var()->mode() == Variable::CONST);
          // Only the first const initialization must be executed (the slot
          // still contains 'the hole' value). When the assignment is
          // executed, the code is identical to a normal store (see below).
          Comment cmnt(masm, "[ Init const");
          __ ldr(r2, cgen_->SlotOperand(slot, r2));
          __ cmp(r2, Operand(Factory::the_hole_value()));
          exit.Branch(ne);
        }

        // We must execute the store.  Storing a variable must keep the
        // (new) value on the stack. This is necessary for compiling
        // assignment expressions.
        //
        // Note: We will reach here even with slot->var()->mode() ==
        // Variable::CONST because of const declarations which will
        // initialize consts to 'the hole' value and by doing so, end up
        // calling this code.  r2 may be loaded with context; used below in
        // RecordWrite.
        frame->EmitPop(r0);
        __ str(r0, cgen_->SlotOperand(slot, r2));
        frame->EmitPush(r0);
        if (slot->type() == Slot::CONTEXT) {
          // Skip write barrier if the written value is a smi.
          __ tst(r0, Operand(kSmiTagMask));
          exit.Branch(eq);
          // r2 is loaded with context when calling SlotOperand above.
          int offset = FixedArray::kHeaderSize + slot->index() * kPointerSize;
          __ mov(r3, Operand(offset));
          __ RecordWrite(r2, r3, r1);
        }
        // If we definitely did not jump over the assignment, we do not need
        // to bind the exit label.  Doing so can defeat peephole
        // optimization.
        if (init_state == CONST_INIT || slot->type() == Slot::CONTEXT) {
          exit.Bind();
        }
      }
      break;
    }

    case NAMED: {
      Comment cmnt(masm, "[ Store to named Property");
      // Call the appropriate IC code.
      Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Initialize));
      Handle<String> name(GetName());

      Result value = cgen_->allocator()->Allocate(r0);
      ASSERT(value.is_valid());
      frame->EmitPop(value.reg());

      // Setup the name register.
      Result property_name = cgen_->allocator()->Allocate(r2);
      ASSERT(property_name.is_valid());
      __ mov(property_name.reg(), Operand(name));
      Result answer = frame->CallCodeObject(ic,
                                            RelocInfo::CODE_TARGET,
                                            &value,
                                            &property_name,
                                            0);
      frame->EmitPush(answer.reg());
      break;
    }

    case KEYED: {
      Comment cmnt(masm, "[ Store to keyed Property");
      Property* property = expression_->AsProperty();
      ASSERT(property != NULL);
      cgen_->CodeForSourcePosition(property->position());

      // Call IC code.
      Handle<Code> ic(Builtins::builtin(Builtins::KeyedStoreIC_Initialize));
      // TODO(1222589): Make the IC grab the values from the stack.
      Result value = cgen_->allocator()->Allocate(r0);
      ASSERT(value.is_valid());
      frame->EmitPop(value.reg());  // value
      Result result =
          frame->CallCodeObject(ic, RelocInfo::CODE_TARGET, &value, 0);
      frame->EmitPush(result.reg());
      break;
    }

    default:
      UNREACHABLE();
  }
}


void GetPropertyStub::Generate(MacroAssembler* masm) {
  // sp[0]: key
  // sp[1]: receiver
  Label slow, fast;
  // Get the key and receiver object from the stack.
  __ ldm(ia, sp, r0.bit() | r1.bit());
  // Check that the key is a smi.
  __ tst(r0, Operand(kSmiTagMask));
  __ b(ne, &slow);
  __ mov(r0, Operand(r0, ASR, kSmiTagSize));
  // Check that the object isn't a smi.
  __ tst(r1, Operand(kSmiTagMask));
  __ b(eq, &slow);

  // Check that the object is some kind of JS object EXCEPT JS Value type.
  // In the case that the object is a value-wrapper object,
  // we enter the runtime system to make sure that indexing into string
  // objects work as intended.
  ASSERT(JS_OBJECT_TYPE > JS_VALUE_TYPE);
  __ ldr(r2, FieldMemOperand(r1, HeapObject::kMapOffset));
  __ ldrb(r2, FieldMemOperand(r2, Map::kInstanceTypeOffset));
  __ cmp(r2, Operand(JS_OBJECT_TYPE));
  __ b(lt, &slow);

  // Get the elements array of the object.
  __ ldr(r1, FieldMemOperand(r1, JSObject::kElementsOffset));
  // Check that the object is in fast mode (not dictionary).
  __ ldr(r3, FieldMemOperand(r1, HeapObject::kMapOffset));
  __ cmp(r3, Operand(Factory::hash_table_map()));
  __ b(eq, &slow);
  // Check that the key (index) is within bounds.
  __ ldr(r3, FieldMemOperand(r1, Array::kLengthOffset));
  __ cmp(r0, Operand(r3));
  __ b(lo, &fast);

  // Slow case: Push extra copies of the arguments (2).
  __ bind(&slow);
  __ ldm(ia, sp, r0.bit() | r1.bit());
  __ stm(db_w, sp, r0.bit() | r1.bit());
  // Do tail-call to runtime routine.
  __ TailCallRuntime(ExternalReference(Runtime::kGetProperty), 2);

  // Fast case: Do the load.
  __ bind(&fast);
  __ add(r3, r1, Operand(Array::kHeaderSize - kHeapObjectTag));
  __ ldr(r0, MemOperand(r3, r0, LSL, kPointerSizeLog2));
  __ cmp(r0, Operand(Factory::the_hole_value()));
  // In case the loaded value is the_hole we have to consult GetProperty
  // to ensure the prototype chain is searched.
  __ b(eq, &slow);

  __ StubReturn(1);
}


void SetPropertyStub::Generate(MacroAssembler* masm) {
  // r0 : value
  // sp[0] : key
  // sp[1] : receiver

  Label slow, fast, array, extra, exit;
  // Get the key and the object from the stack.
  __ ldm(ia, sp, r1.bit() | r3.bit());  // r1 = key, r3 = receiver
  // Check that the key is a smi.
  __ tst(r1, Operand(kSmiTagMask));
  __ b(ne, &slow);
  // Check that the object isn't a smi.
  __ tst(r3, Operand(kSmiTagMask));
  __ b(eq, &slow);
  // Get the type of the object from its map.
  __ ldr(r2, FieldMemOperand(r3, HeapObject::kMapOffset));
  __ ldrb(r2, FieldMemOperand(r2, Map::kInstanceTypeOffset));
  // Check if the object is a JS array or not.
  __ cmp(r2, Operand(JS_ARRAY_TYPE));
  __ b(eq, &array);
  // Check that the object is some kind of JS object.
  __ cmp(r2, Operand(FIRST_JS_OBJECT_TYPE));
  __ b(lt, &slow);


  // Object case: Check key against length in the elements array.
  __ ldr(r3, FieldMemOperand(r3, JSObject::kElementsOffset));
  // Check that the object is in fast mode (not dictionary).
  __ ldr(r2, FieldMemOperand(r3, HeapObject::kMapOffset));
  __ cmp(r2, Operand(Factory::hash_table_map()));
  __ b(eq, &slow);
  // Untag the key (for checking against untagged length in the fixed array).
  __ mov(r1, Operand(r1, ASR, kSmiTagSize));
  // Compute address to store into and check array bounds.
  __ add(r2, r3, Operand(Array::kHeaderSize - kHeapObjectTag));
  __ add(r2, r2, Operand(r1, LSL, kPointerSizeLog2));
  __ ldr(ip, FieldMemOperand(r3, Array::kLengthOffset));
  __ cmp(r1, Operand(ip));
  __ b(lo, &fast);


  // Slow case: Push extra copies of the arguments (3).
  __ bind(&slow);
  __ ldm(ia, sp, r1.bit() | r3.bit());  // r0 == value, r1 == key, r3 == object
  __ stm(db_w, sp, r0.bit() | r1.bit() | r3.bit());
  // Do tail-call to runtime routine.
  __ TailCallRuntime(ExternalReference(Runtime::kSetProperty), 3);


  // Extra capacity case: Check if there is extra capacity to
  // perform the store and update the length. Used for adding one
  // element to the array by writing to array[array.length].
  // r0 == value, r1 == key, r2 == elements, r3 == object
  __ bind(&extra);
  __ b(ne, &slow);  // do not leave holes in the array
  __ mov(r1, Operand(r1, ASR, kSmiTagSize));  // untag
  __ ldr(ip, FieldMemOperand(r2, Array::kLengthOffset));
  __ cmp(r1, Operand(ip));
  __ b(hs, &slow);
  __ mov(r1, Operand(r1, LSL, kSmiTagSize));  // restore tag
  __ add(r1, r1, Operand(1 << kSmiTagSize));  // and increment
  __ str(r1, FieldMemOperand(r3, JSArray::kLengthOffset));
  __ mov(r3, Operand(r2));
  // NOTE: Computing the address to store into must take the fact
  // that the key has been incremented into account.
  int displacement = Array::kHeaderSize - kHeapObjectTag -
      ((1 << kSmiTagSize) * 2);
  __ add(r2, r2, Operand(displacement));
  __ add(r2, r2, Operand(r1, LSL, kPointerSizeLog2 - kSmiTagSize));
  __ b(&fast);


  // Array case: Get the length and the elements array from the JS
  // array. Check that the array is in fast mode; if it is the
  // length is always a smi.
  // r0 == value, r3 == object
  __ bind(&array);
  __ ldr(r2, FieldMemOperand(r3, JSObject::kElementsOffset));
  __ ldr(r1, FieldMemOperand(r2, HeapObject::kMapOffset));
  __ cmp(r1, Operand(Factory::hash_table_map()));
  __ b(eq, &slow);

  // Check the key against the length in the array, compute the
  // address to store into and fall through to fast case.
  __ ldr(r1, MemOperand(sp));
  // r0 == value, r1 == key, r2 == elements, r3 == object.
  __ ldr(ip, FieldMemOperand(r3, JSArray::kLengthOffset));
  __ cmp(r1, Operand(ip));
  __ b(hs, &extra);
  __ mov(r3, Operand(r2));
  __ add(r2, r2, Operand(Array::kHeaderSize - kHeapObjectTag));
  __ add(r2, r2, Operand(r1, LSL, kPointerSizeLog2 - kSmiTagSize));


  // Fast case: Do the store.
  // r0 == value, r2 == address to store into, r3 == elements
  __ bind(&fast);
  __ str(r0, MemOperand(r2));
  // Skip write barrier if the written value is a smi.
  __ tst(r0, Operand(kSmiTagMask));
  __ b(eq, &exit);
  // Update write barrier for the elements array address.
  __ sub(r1, r2, Operand(r3));
  __ RecordWrite(r3, r1, r2);
  __ bind(&exit);
  __ StubReturn(1);
}


void GenericBinaryOpStub::Generate(MacroAssembler* masm) {
  // r1 : x
  // r0 : y
  // result : r0

  switch (op_) {
    case Token::ADD: {
      Label slow, exit;
      // fast path
      __ orr(r2, r1, Operand(r0));  // r2 = x | y;
      __ add(r0, r1, Operand(r0), SetCC);  // add y optimistically
      // go slow-path in case of overflow
      __ b(vs, &slow);
      // go slow-path in case of non-smi operands
      ASSERT(kSmiTag == 0);  // adjust code below
      __ tst(r2, Operand(kSmiTagMask));
      __ b(eq, &exit);
      // slow path
      __ bind(&slow);
      __ sub(r0, r0, Operand(r1));  // revert optimistic add
      __ push(r1);
      __ push(r0);
      __ mov(r0, Operand(1));  // set number of arguments
      __ InvokeBuiltin(Builtins::ADD, JUMP_JS);
      // done
      __ bind(&exit);
      break;
    }

    case Token::SUB: {
      Label slow, exit;
      // fast path
      __ orr(r2, r1, Operand(r0));  // r2 = x | y;
      __ sub(r3, r1, Operand(r0), SetCC);  // subtract y optimistically
      // go slow-path in case of overflow
      __ b(vs, &slow);
      // go slow-path in case of non-smi operands
      ASSERT(kSmiTag == 0);  // adjust code below
      __ tst(r2, Operand(kSmiTagMask));
      __ mov(r0, Operand(r3), LeaveCC, eq);  // conditionally set r0 to result
      __ b(eq, &exit);
      // slow path
      __ bind(&slow);
      __ push(r1);
      __ push(r0);
      __ mov(r0, Operand(1));  // set number of arguments
      __ InvokeBuiltin(Builtins::SUB, JUMP_JS);
      // done
      __ bind(&exit);
      break;
    }

    case Token::MUL: {
      Label slow, exit;
      // tag check
      __ orr(r2, r1, Operand(r0));  // r2 = x | y;
      ASSERT(kSmiTag == 0);  // adjust code below
      __ tst(r2, Operand(kSmiTagMask));
      __ b(ne, &slow);
      // remove tag from one operand (but keep sign), so that result is smi
      __ mov(ip, Operand(r0, ASR, kSmiTagSize));
      // do multiplication
      __ smull(r3, r2, r1, ip);  // r3 = lower 32 bits of ip*r1
      // go slow on overflows (overflow bit is not set)
      __ mov(ip, Operand(r3, ASR, 31));
      __ cmp(ip, Operand(r2));  // no overflow if higher 33 bits are identical
      __ b(ne, &slow);
      // go slow on zero result to handle -0
      __ tst(r3, Operand(r3));
      __ mov(r0, Operand(r3), LeaveCC, ne);
      __ b(ne, &exit);
      // slow case
      __ bind(&slow);
      __ push(r1);
      __ push(r0);
      __ mov(r0, Operand(1));  // set number of arguments
      __ InvokeBuiltin(Builtins::MUL, JUMP_JS);
      // done
      __ bind(&exit);
      break;
    }

    case Token::BIT_OR:
    case Token::BIT_AND:
    case Token::BIT_XOR: {
      Label slow, exit;
      // tag check
      __ orr(r2, r1, Operand(r0));  // r2 = x | y;
      ASSERT(kSmiTag == 0);  // adjust code below
      __ tst(r2, Operand(kSmiTagMask));
      __ b(ne, &slow);
      switch (op_) {
        case Token::BIT_OR:  __ orr(r0, r0, Operand(r1)); break;
        case Token::BIT_AND: __ and_(r0, r0, Operand(r1)); break;
        case Token::BIT_XOR: __ eor(r0, r0, Operand(r1)); break;
        default: UNREACHABLE();
      }
      __ b(&exit);
      __ bind(&slow);
      __ push(r1);  // restore stack
      __ push(r0);
      __ mov(r0, Operand(1));  // 1 argument (not counting receiver).
      switch (op_) {
        case Token::BIT_OR:
          __ InvokeBuiltin(Builtins::BIT_OR, JUMP_JS);
          break;
        case Token::BIT_AND:
          __ InvokeBuiltin(Builtins::BIT_AND, JUMP_JS);
          break;
        case Token::BIT_XOR:
          __ InvokeBuiltin(Builtins::BIT_XOR, JUMP_JS);
          break;
        default:
          UNREACHABLE();
      }
      __ bind(&exit);
      break;
    }

    case Token::SHL:
    case Token::SHR:
    case Token::SAR: {
      Label slow, exit;
      // tag check
      __ orr(r2, r1, Operand(r0));  // r2 = x | y;
      ASSERT(kSmiTag == 0);  // adjust code below
      __ tst(r2, Operand(kSmiTagMask));
      __ b(ne, &slow);
      // remove tags from operands (but keep sign)
      __ mov(r3, Operand(r1, ASR, kSmiTagSize));  // x
      __ mov(r2, Operand(r0, ASR, kSmiTagSize));  // y
      // use only the 5 least significant bits of the shift count
      __ and_(r2, r2, Operand(0x1f));
      // perform operation
      switch (op_) {
        case Token::SAR:
          __ mov(r3, Operand(r3, ASR, r2));
          // no checks of result necessary
          break;

        case Token::SHR:
          __ mov(r3, Operand(r3, LSR, r2));
          // check that the *unsigned* result fits in a smi
          // neither of the two high-order bits can be set:
          // - 0x80000000: high bit would be lost when smi tagging
          // - 0x40000000: this number would convert to negative when
          // smi tagging these two cases can only happen with shifts
          // by 0 or 1 when handed a valid smi
          __ and_(r2, r3, Operand(0xc0000000), SetCC);
          __ b(ne, &slow);
          break;

        case Token::SHL:
          __ mov(r3, Operand(r3, LSL, r2));
          // check that the *signed* result fits in a smi
          __ add(r2, r3, Operand(0x40000000), SetCC);
          __ b(mi, &slow);
          break;

        default: UNREACHABLE();
      }
      // tag result and store it in r0
      ASSERT(kSmiTag == 0);  // adjust code below
      __ mov(r0, Operand(r3, LSL, kSmiTagSize));
      __ b(&exit);
      // slow case
      __ bind(&slow);
      __ push(r1);  // restore stack
      __ push(r0);
      __ mov(r0, Operand(1));  // 1 argument (not counting receiver).
      switch (op_) {
        case Token::SAR: __ InvokeBuiltin(Builtins::SAR, JUMP_JS); break;
        case Token::SHR: __ InvokeBuiltin(Builtins::SHR, JUMP_JS); break;
        case Token::SHL: __ InvokeBuiltin(Builtins::SHL, JUMP_JS); break;
        default: UNREACHABLE();
      }
      __ bind(&exit);
      break;
    }

    default: UNREACHABLE();
  }
  __ Ret();
}


void StackCheckStub::Generate(MacroAssembler* masm) {
  Label within_limit;
  __ mov(ip, Operand(ExternalReference::address_of_stack_guard_limit()));
  __ ldr(ip, MemOperand(ip));
  __ cmp(sp, Operand(ip));
  __ b(hs, &within_limit);
  // Do tail-call to runtime routine.
  __ push(r0);
  __ TailCallRuntime(ExternalReference(Runtime::kStackGuard), 1);
  __ bind(&within_limit);

  __ StubReturn(1);
}


void UnarySubStub::Generate(MacroAssembler* masm) {
  Label undo;
  Label slow;
  Label done;

  // Enter runtime system if the value is not a smi.
  __ tst(r0, Operand(kSmiTagMask));
  __ b(ne, &slow);

  // Enter runtime system if the value of the expression is zero
  // to make sure that we switch between 0 and -0.
  __ cmp(r0, Operand(0));
  __ b(eq, &slow);

  // The value of the expression is a smi that is not zero.  Try
  // optimistic subtraction '0 - value'.
  __ rsb(r1, r0, Operand(0), SetCC);
  __ b(vs, &slow);

  // If result is a smi we are done.
  __ tst(r1, Operand(kSmiTagMask));
  __ mov(r0, Operand(r1), LeaveCC, eq);  // conditionally set r0 to result
  __ b(eq, &done);

  // Enter runtime system.
  __ bind(&slow);
  __ push(r0);
  __ mov(r0, Operand(0));  // set number of arguments
  __ InvokeBuiltin(Builtins::UNARY_MINUS, JUMP_JS);

  __ bind(&done);
  __ StubReturn(1);
}


void CEntryStub::GenerateThrowTOS(MacroAssembler* masm) {
  // r0 holds exception
  ASSERT(StackHandlerConstants::kSize == 6 * kPointerSize);  // adjust this code
  __ mov(r3, Operand(ExternalReference(Top::k_handler_address)));
  __ ldr(sp, MemOperand(r3));
  __ pop(r2);  // pop next in chain
  __ str(r2, MemOperand(r3));
  // restore parameter- and frame-pointer and pop state.
  __ ldm(ia_w, sp, r3.bit() | pp.bit() | fp.bit());
  // Before returning we restore the context from the frame pointer if not NULL.
  // The frame pointer is NULL in the exception handler of a JS entry frame.
  __ cmp(fp, Operand(0));
  // Set cp to NULL if fp is NULL.
  __ mov(cp, Operand(0), LeaveCC, eq);
  // Restore cp otherwise.
  __ ldr(cp, MemOperand(fp, StandardFrameConstants::kContextOffset), ne);
  if (kDebug && FLAG_debug_code) __ mov(lr, Operand(pc));
  __ pop(pc);
}


void CEntryStub::GenerateThrowOutOfMemory(MacroAssembler* masm) {
  // Fetch top stack handler.
  __ mov(r3, Operand(ExternalReference(Top::k_handler_address)));
  __ ldr(r3, MemOperand(r3));

  // Unwind the handlers until the ENTRY handler is found.
  Label loop, done;
  __ bind(&loop);
  // Load the type of the current stack handler.
  const int kStateOffset = StackHandlerConstants::kAddressDisplacement +
      StackHandlerConstants::kStateOffset;
  __ ldr(r2, MemOperand(r3, kStateOffset));
  __ cmp(r2, Operand(StackHandler::ENTRY));
  __ b(eq, &done);
  // Fetch the next handler in the list.
  const int kNextOffset =  StackHandlerConstants::kAddressDisplacement +
      StackHandlerConstants::kNextOffset;
  __ ldr(r3, MemOperand(r3, kNextOffset));
  __ jmp(&loop);
  __ bind(&done);

  // Set the top handler address to next handler past the current ENTRY handler.
  __ ldr(r0, MemOperand(r3, kNextOffset));
  __ mov(r2, Operand(ExternalReference(Top::k_handler_address)));
  __ str(r0, MemOperand(r2));

  // Set external caught exception to false.
  __ mov(r0, Operand(false));
  ExternalReference external_caught(Top::k_external_caught_exception_address);
  __ mov(r2, Operand(external_caught));
  __ str(r0, MemOperand(r2));

  // Set pending exception and r0 to out of memory exception.
  Failure* out_of_memory = Failure::OutOfMemoryException();
  __ mov(r0, Operand(reinterpret_cast<int32_t>(out_of_memory)));
  __ mov(r2, Operand(ExternalReference(Top::k_pending_exception_address)));
  __ str(r0, MemOperand(r2));

  // Restore the stack to the address of the ENTRY handler
  __ mov(sp, Operand(r3));

  // Stack layout at this point. See also PushTryHandler
  // r3, sp ->   next handler
  //             state (ENTRY)
  //             pp
  //             fp
  //             lr

  // Discard ENTRY state (r2 is not used), and restore parameter-
  // and frame-pointer and pop state.
  __ ldm(ia_w, sp, r2.bit() | r3.bit() | pp.bit() | fp.bit());
  // Before returning we restore the context from the frame pointer if not NULL.
  // The frame pointer is NULL in the exception handler of a JS entry frame.
  __ cmp(fp, Operand(0));
  // Set cp to NULL if fp is NULL.
  __ mov(cp, Operand(0), LeaveCC, eq);
  // Restore cp otherwise.
  __ ldr(cp, MemOperand(fp, StandardFrameConstants::kContextOffset), ne);
  if (kDebug && FLAG_debug_code) __ mov(lr, Operand(pc));
  __ pop(pc);
}


void CEntryStub::GenerateCore(MacroAssembler* masm,
                              Label* throw_normal_exception,
                              Label* throw_out_of_memory_exception,
                              StackFrame::Type frame_type,
                              bool do_gc,
                              bool always_allocate) {
  // r0: result parameter for PerformGC, if any
  // r4: number of arguments including receiver  (C callee-saved)
  // r5: pointer to builtin function  (C callee-saved)
  // r6: pointer to the first argument (C callee-saved)

  if (do_gc) {
    // Passing r0.
    __ Call(FUNCTION_ADDR(Runtime::PerformGC), RelocInfo::RUNTIME_ENTRY);
  }

  ExternalReference scope_depth =
      ExternalReference::heap_always_allocate_scope_depth();
  if (always_allocate) {
    __ mov(r0, Operand(scope_depth));
    __ ldr(r1, MemOperand(r0));
    __ add(r1, r1, Operand(1));
    __ str(r1, MemOperand(r0));
  }

  // Call C built-in.
  // r0 = argc, r1 = argv
  __ mov(r0, Operand(r4));
  __ mov(r1, Operand(r6));

  // TODO(1242173): To let the GC traverse the return address of the exit
  // frames, we need to know where the return address is. Right now,
  // we push it on the stack to be able to find it again, but we never
  // restore from it in case of changes, which makes it impossible to
  // support moving the C entry code stub. This should be fixed, but currently
  // this is OK because the CEntryStub gets generated so early in the V8 boot
  // sequence that it is not moving ever.
  __ add(lr, pc, Operand(4));  // compute return address: (pc + 8) + 4
  __ push(lr);
#if !defined(__arm__)
  // Notify the simulator of the transition to C code.
  __ swi(assembler::arm::call_rt_r5);
#else /* !defined(__arm__) */
  __ Jump(r5);
#endif /* !defined(__arm__) */

  if (always_allocate) {
    // It's okay to clobber r2 and r3 here. Don't mess with r0 and r1
    // though (contain the result).
    __ mov(r2, Operand(scope_depth));
    __ ldr(r3, MemOperand(r2));
    __ sub(r3, r3, Operand(1));
    __ str(r3, MemOperand(r2));
  }

  // check for failure result
  Label failure_returned;
  ASSERT(((kFailureTag + 1) & kFailureTagMask) == 0);
  // Lower 2 bits of r2 are 0 iff r0 has failure tag.
  __ add(r2, r0, Operand(1));
  __ tst(r2, Operand(kFailureTagMask));
  __ b(eq, &failure_returned);

  // Exit C frame and return.
  // r0:r1: result
  // sp: stack pointer
  // fp: frame pointer
  // pp: caller's parameter pointer pp  (restored as C callee-saved)
  __ LeaveExitFrame(frame_type);

  // check if we should retry or throw exception
  Label retry;
  __ bind(&failure_returned);
  ASSERT(Failure::RETRY_AFTER_GC == 0);
  __ tst(r0, Operand(((1 << kFailureTypeTagSize) - 1) << kFailureTagSize));
  __ b(eq, &retry);

  Label continue_exception;
  // If the returned failure is EXCEPTION then promote Top::pending_exception().
  __ cmp(r0, Operand(reinterpret_cast<int32_t>(Failure::Exception())));
  __ b(ne, &continue_exception);

  // Retrieve the pending exception and clear the variable.
  __ mov(ip, Operand(ExternalReference::the_hole_value_location()));
  __ ldr(r3, MemOperand(ip));
  __ mov(ip, Operand(ExternalReference(Top::k_pending_exception_address)));
  __ ldr(r0, MemOperand(ip));
  __ str(r3, MemOperand(ip));

  __ bind(&continue_exception);
  // Special handling of out of memory exception.
  Failure* out_of_memory = Failure::OutOfMemoryException();
  __ cmp(r0, Operand(reinterpret_cast<int32_t>(out_of_memory)));
  __ b(eq, throw_out_of_memory_exception);

  // Handle normal exception.
  __ jmp(throw_normal_exception);

  __ bind(&retry);  // pass last failure (r0) as parameter (r0) when retrying
}


void CEntryStub::GenerateBody(MacroAssembler* masm, bool is_debug_break) {
  // Called from JavaScript; parameters are on stack as if calling JS function
  // r0: number of arguments including receiver
  // r1: pointer to builtin function
  // fp: frame pointer  (restored after C call)
  // sp: stack pointer  (restored as callee's pp after C call)
  // cp: current context  (C callee-saved)
  // pp: caller's parameter pointer pp  (C callee-saved)

  // NOTE: Invocations of builtins may return failure objects
  // instead of a proper result. The builtin entry handles
  // this by performing a garbage collection and retrying the
  // builtin once.

  StackFrame::Type frame_type = is_debug_break
      ? StackFrame::EXIT_DEBUG
      : StackFrame::EXIT;

  // Enter the exit frame that transitions from JavaScript to C++.
  __ EnterExitFrame(frame_type);

  // r4: number of arguments (C callee-saved)
  // r5: pointer to builtin function (C callee-saved)
  // r6: pointer to first argument (C callee-saved)

  Label throw_out_of_memory_exception;
  Label throw_normal_exception;

  // Call into the runtime system. Collect garbage before the call if
  // running with --gc-greedy set.
  if (FLAG_gc_greedy) {
    Failure* failure = Failure::RetryAfterGC(0);
    __ mov(r0, Operand(reinterpret_cast<intptr_t>(failure)));
  }
  GenerateCore(masm, &throw_normal_exception,
               &throw_out_of_memory_exception,
               frame_type,
               FLAG_gc_greedy,
               false);

  // Do space-specific GC and retry runtime call.
  GenerateCore(masm,
               &throw_normal_exception,
               &throw_out_of_memory_exception,
               frame_type,
               true,
               false);

  // Do full GC and retry runtime call one final time.
  Failure* failure = Failure::InternalError();
  __ mov(r0, Operand(reinterpret_cast<int32_t>(failure)));
  GenerateCore(masm,
               &throw_normal_exception,
               &throw_out_of_memory_exception,
               frame_type,
               true,
               true);

  __ bind(&throw_out_of_memory_exception);
  GenerateThrowOutOfMemory(masm);
  // control flow for generated will not return.

  __ bind(&throw_normal_exception);
  GenerateThrowTOS(masm);
}


void JSEntryStub::GenerateBody(MacroAssembler* masm, bool is_construct) {
  // r0: code entry
  // r1: function
  // r2: receiver
  // r3: argc
  // [sp+0]: argv

  Label invoke, exit;

  // Called from C, so do not pop argc and args on exit (preserve sp)
  // No need to save register-passed args
  // Save callee-saved registers (incl. cp, pp, and fp), sp, and lr
  __ stm(db_w, sp, kCalleeSaved | lr.bit());

  // Get address of argv, see stm above.
  // r0: code entry
  // r1: function
  // r2: receiver
  // r3: argc
  __ add(r4, sp, Operand((kNumCalleeSaved + 1)*kPointerSize));
  __ ldr(r4, MemOperand(r4));  // argv

  // Push a frame with special values setup to mark it as an entry frame.
  // r0: code entry
  // r1: function
  // r2: receiver
  // r3: argc
  // r4: argv
  int marker = is_construct ? StackFrame::ENTRY_CONSTRUCT : StackFrame::ENTRY;
  __ mov(r8, Operand(-1));  // Push a bad frame pointer to fail if it is used.
  __ mov(r7, Operand(~ArgumentsAdaptorFrame::SENTINEL));
  __ mov(r6, Operand(Smi::FromInt(marker)));
  __ mov(r5, Operand(ExternalReference(Top::k_c_entry_fp_address)));
  __ ldr(r5, MemOperand(r5));
  __ stm(db_w, sp, r5.bit() | r6.bit() | r7.bit() | r8.bit());

  // Setup frame pointer for the frame to be pushed.
  __ add(fp, sp, Operand(-EntryFrameConstants::kCallerFPOffset));

  // Call a faked try-block that does the invoke.
  __ bl(&invoke);

  // Caught exception: Store result (exception) in the pending
  // exception field in the JSEnv and return a failure sentinel.
  // Coming in here the fp will be invalid because the PushTryHandler below
  // sets it to 0 to signal the existence of the JSEntry frame.
  __ mov(ip, Operand(ExternalReference(Top::k_pending_exception_address)));
  __ str(r0, MemOperand(ip));
  __ mov(r0, Operand(reinterpret_cast<int32_t>(Failure::Exception())));
  __ b(&exit);

  // Invoke: Link this frame into the handler chain.
  __ bind(&invoke);
  // Must preserve r0-r4, r5-r7 are available.
  __ PushTryHandler(IN_JS_ENTRY, JS_ENTRY_HANDLER);
  // If an exception not caught by another handler occurs, this handler returns
  // control to the code after the bl(&invoke) above, which restores all
  // kCalleeSaved registers (including cp, pp and fp) to their saved values
  // before returning a failure to C.

  // Clear any pending exceptions.
  __ mov(ip, Operand(ExternalReference::the_hole_value_location()));
  __ ldr(r5, MemOperand(ip));
  __ mov(ip, Operand(ExternalReference(Top::k_pending_exception_address)));
  __ str(r5, MemOperand(ip));

  // Invoke the function by calling through JS entry trampoline builtin.
  // Notice that we cannot store a reference to the trampoline code directly in
  // this stub, because runtime stubs are not traversed when doing GC.

  // Expected registers by Builtins::JSEntryTrampoline
  // r0: code entry
  // r1: function
  // r2: receiver
  // r3: argc
  // r4: argv
  if (is_construct) {
    ExternalReference construct_entry(Builtins::JSConstructEntryTrampoline);
    __ mov(ip, Operand(construct_entry));
  } else {
    ExternalReference entry(Builtins::JSEntryTrampoline);
    __ mov(ip, Operand(entry));
  }
  __ ldr(ip, MemOperand(ip));  // deref address

  // Branch and link to JSEntryTrampoline
  __ mov(lr, Operand(pc));
  __ add(pc, ip, Operand(Code::kHeaderSize - kHeapObjectTag));

  // Unlink this frame from the handler chain. When reading the
  // address of the next handler, there is no need to use the address
  // displacement since the current stack pointer (sp) points directly
  // to the stack handler.
  __ ldr(r3, MemOperand(sp, StackHandlerConstants::kNextOffset));
  __ mov(ip, Operand(ExternalReference(Top::k_handler_address)));
  __ str(r3, MemOperand(ip));
  // No need to restore registers
  __ add(sp, sp, Operand(StackHandlerConstants::kSize));

  __ bind(&exit);  // r0 holds result
  // Restore the top frame descriptors from the stack.
  __ pop(r3);
  __ mov(ip, Operand(ExternalReference(Top::k_c_entry_fp_address)));
  __ str(r3, MemOperand(ip));

  // Reset the stack to the callee saved registers.
  __ add(sp, sp, Operand(-EntryFrameConstants::kCallerFPOffset));

  // Restore callee-saved registers and return.
#ifdef DEBUG
  if (FLAG_debug_code) __ mov(lr, Operand(pc));
#endif
  __ ldm(ia_w, sp, kCalleeSaved | pc.bit());
}


void ArgumentsAccessStub::GenerateReadLength(MacroAssembler* masm) {
  // Check if the calling frame is an arguments adaptor frame.
  Label adaptor;
  __ ldr(r2, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
  __ ldr(r3, MemOperand(r2, StandardFrameConstants::kContextOffset));
  __ cmp(r3, Operand(ArgumentsAdaptorFrame::SENTINEL));
  __ b(eq, &adaptor);

  // Nothing to do: The formal number of parameters has already been
  // passed in register r0 by calling function. Just return it.
  __ mov(pc, lr);

  // Arguments adaptor case: Read the arguments length from the
  // adaptor frame and return it.
  __ bind(&adaptor);
  __ ldr(r0, MemOperand(r2, ArgumentsAdaptorFrameConstants::kLengthOffset));
  __ mov(pc, lr);
}


void ArgumentsAccessStub::GenerateReadElement(MacroAssembler* masm) {
  // The displacement is the offset of the last parameter (if any)
  // relative to the frame pointer.
  static const int kDisplacement =
      StandardFrameConstants::kCallerSPOffset - kPointerSize;

  // Check that the key is a smi.
  Label slow;
  __ tst(r1, Operand(kSmiTagMask));
  __ b(ne, &slow);

  // Check if the calling frame is an arguments adaptor frame.
  Label adaptor;
  __ ldr(r2, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
  __ ldr(r3, MemOperand(r2, StandardFrameConstants::kContextOffset));
  __ cmp(r3, Operand(ArgumentsAdaptorFrame::SENTINEL));
  __ b(eq, &adaptor);

  // Check index against formal parameters count limit passed in
  // through register eax. Use unsigned comparison to get negative
  // check for free.
  __ cmp(r1, r0);
  __ b(cs, &slow);

  // Read the argument from the stack and return it.
  __ sub(r3, r0, r1);
  __ add(r3, fp, Operand(r3, LSL, kPointerSizeLog2 - kSmiTagSize));
  __ ldr(r0, MemOperand(r3, kDisplacement));
  __ mov(pc, lr);

  // Arguments adaptor case: Check index against actual arguments
  // limit found in the arguments adaptor frame. Use unsigned
  // comparison to get negative check for free.
  __ bind(&adaptor);
  __ ldr(r0, MemOperand(r2, ArgumentsAdaptorFrameConstants::kLengthOffset));
  __ cmp(r1, r0);
  __ b(cs, &slow);

  // Read the argument from the adaptor frame and return it.
  __ sub(r3, r0, r1);
  __ add(r3, r2, Operand(r3, LSL, kPointerSizeLog2 - kSmiTagSize));
  __ ldr(r0, MemOperand(r3, kDisplacement));
  __ mov(pc, lr);

  // Slow-case: Handle non-smi or out-of-bounds access to arguments
  // by calling the runtime system.
  __ bind(&slow);
  __ push(r1);
  __ TailCallRuntime(ExternalReference(Runtime::kGetArgumentsProperty), 1);
}


void ArgumentsAccessStub::GenerateNewObject(MacroAssembler* masm) {
  // Check if the calling frame is an arguments adaptor frame.
  Label runtime;
  __ ldr(r2, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
  __ ldr(r3, MemOperand(r2, StandardFrameConstants::kContextOffset));
  __ cmp(r3, Operand(ArgumentsAdaptorFrame::SENTINEL));
  __ b(ne, &runtime);

  // Patch the arguments.length and the parameters pointer.
  __ ldr(r0, MemOperand(r2, ArgumentsAdaptorFrameConstants::kLengthOffset));
  __ str(r0, MemOperand(sp, 0 * kPointerSize));
  __ add(r3, r2, Operand(r0, LSL, kPointerSizeLog2 - kSmiTagSize));
  __ add(r3, r3, Operand(StandardFrameConstants::kCallerSPOffset));
  __ str(r3, MemOperand(sp, 1 * kPointerSize));

  // Do the runtime call to allocate the arguments object.
  __ bind(&runtime);
  __ TailCallRuntime(ExternalReference(Runtime::kNewArgumentsFast), 3);
}


void CallFunctionStub::Generate(MacroAssembler* masm) {
  Label slow;
  // Get the function to call from the stack.
  // function, receiver [, arguments]
  __ ldr(r1, MemOperand(sp, (argc_ + 1) * kPointerSize));

  // Check that the function is really a JavaScript function.
  // r1: pushed function (to be verified)
  __ tst(r1, Operand(kSmiTagMask));
  __ b(eq, &slow);
  // Get the map of the function object.
  __ ldr(r2, FieldMemOperand(r1, HeapObject::kMapOffset));
  __ ldrb(r2, FieldMemOperand(r2, Map::kInstanceTypeOffset));
  __ cmp(r2, Operand(JS_FUNCTION_TYPE));
  __ b(ne, &slow);

  // Fast-case: Invoke the function now.
  // r1: pushed function
  ParameterCount actual(argc_);
  __ InvokeFunction(r1, actual, JUMP_FUNCTION);

  // Slow-case: Non-function called.
  __ bind(&slow);
  __ mov(r0, Operand(argc_));  // Setup the number of arguments.
  __ mov(r2, Operand(0));
  __ GetBuiltinEntry(r3, Builtins::CALL_NON_FUNCTION);
  __ Jump(Handle<Code>(Builtins::builtin(Builtins::ArgumentsAdaptorTrampoline)),
          RelocInfo::CODE_TARGET);
}


#undef __

} }  // namespace v8::internal
