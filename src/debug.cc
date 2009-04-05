// Copyright 2006-2008 the V8 project authors. All rights reserved.
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

#include "api.h"
#include "arguments.h"
#include "bootstrapper.h"
#include "code-stubs.h"
#include "compiler.h"
#include "debug.h"
#include "execution.h"
#include "global-handles.h"
#include "natives.h"
#include "stub-cache.h"
#include "log.h"

namespace v8 { namespace internal {

static void PrintLn(v8::Local<v8::Value> value) {
  v8::Local<v8::String> s = value->ToString();
  char* data = NewArray<char>(s->Length() + 1);
  if (data == NULL) {
    V8::FatalProcessOutOfMemory("PrintLn");
    return;
  }
  s->WriteAscii(data);
  PrintF("%s\n", data);
  DeleteArray(data);
}


static Handle<Code> ComputeCallDebugBreak(int argc) {
  CALL_HEAP_FUNCTION(StubCache::ComputeCallDebugBreak(argc), Code);
}


static Handle<Code> ComputeCallDebugPrepareStepIn(int argc) {
  CALL_HEAP_FUNCTION(StubCache::ComputeCallDebugPrepareStepIn(argc), Code);
}


BreakLocationIterator::BreakLocationIterator(Handle<DebugInfo> debug_info,
                                             BreakLocatorType type) {
  debug_info_ = debug_info;
  type_ = type;
  reloc_iterator_ = NULL;
  reloc_iterator_original_ = NULL;
  Reset();  // Initialize the rest of the member variables.
}


BreakLocationIterator::~BreakLocationIterator() {
  ASSERT(reloc_iterator_ != NULL);
  ASSERT(reloc_iterator_original_ != NULL);
  delete reloc_iterator_;
  delete reloc_iterator_original_;
}


void BreakLocationIterator::Next() {
  AssertNoAllocation nogc;
  ASSERT(!RinfoDone());

  // Iterate through reloc info for code and original code stopping at each
  // breakable code target.
  bool first = break_point_ == -1;
  while (!RinfoDone()) {
    if (!first) RinfoNext();
    first = false;
    if (RinfoDone()) return;

    // Whenever a statement position or (plain) position is passed update the
    // current value of these.
    if (RelocInfo::IsPosition(rmode())) {
      if (RelocInfo::IsStatementPosition(rmode())) {
        statement_position_ =
            rinfo()->data() - debug_info_->shared()->start_position();
      }
      // Always update the position as we don't want that to be before the
      // statement position.
      position_ = rinfo()->data() - debug_info_->shared()->start_position();
      ASSERT(position_ >= 0);
      ASSERT(statement_position_ >= 0);
    }

    // Check for breakable code target. Look in the original code as setting
    // break points can cause the code targets in the running (debugged) code to
    // be of a different kind than in the original code.
    if (RelocInfo::IsCodeTarget(rmode())) {
      Address target = original_rinfo()->target_address();
      Code* code = Code::GetCodeFromTargetAddress(target);
      if (code->is_inline_cache_stub() || RelocInfo::IsConstructCall(rmode())) {
        break_point_++;
        return;
      }
      if (code->kind() == Code::STUB) {
        if (type_ == ALL_BREAK_LOCATIONS) {
          if (Debug::IsBreakStub(code)) {
            break_point_++;
            return;
          }
        } else {
          ASSERT(type_ == SOURCE_BREAK_LOCATIONS);
          if (Debug::IsSourceBreakStub(code)) {
            break_point_++;
            return;
          }
        }
      }
    }

    // Check for break at return.
    if (RelocInfo::IsJSReturn(rmode())) {
      // Set the positions to the end of the function.
      if (debug_info_->shared()->HasSourceCode()) {
        position_ = debug_info_->shared()->end_position() -
                    debug_info_->shared()->start_position();
      } else {
        position_ = 0;
      }
      statement_position_ = position_;
      break_point_++;
      return;
    }
  }
}


void BreakLocationIterator::Next(int count) {
  while (count > 0) {
    Next();
    count--;
  }
}


// Find the break point closest to the supplied address.
void BreakLocationIterator::FindBreakLocationFromAddress(Address pc) {
  // Run through all break points to locate the one closest to the address.
  int closest_break_point = 0;
  int distance = kMaxInt;
  while (!Done()) {
    // Check if this break point is closer that what was previously found.
    if (this->pc() < pc && pc - this->pc() < distance) {
      closest_break_point = break_point();
      distance = pc - this->pc();
      // Check whether we can't get any closer.
      if (distance == 0) break;
    }
    Next();
  }

  // Move to the break point found.
  Reset();
  Next(closest_break_point);
}


// Find the break point closest to the supplied source position.
void BreakLocationIterator::FindBreakLocationFromPosition(int position) {
  // Run through all break points to locate the one closest to the source
  // position.
  int closest_break_point = 0;
  int distance = kMaxInt;
  while (!Done()) {
    // Check if this break point is closer that what was previously found.
    if (position <= statement_position() &&
        statement_position() - position < distance) {
      closest_break_point = break_point();
      distance = statement_position() - position;
      // Check whether we can't get any closer.
      if (distance == 0) break;
    }
    Next();
  }

  // Move to the break point found.
  Reset();
  Next(closest_break_point);
}


void BreakLocationIterator::Reset() {
  // Create relocation iterators for the two code objects.
  if (reloc_iterator_ != NULL) delete reloc_iterator_;
  if (reloc_iterator_original_ != NULL) delete reloc_iterator_original_;
  reloc_iterator_ = new RelocIterator(debug_info_->code());
  reloc_iterator_original_ = new RelocIterator(debug_info_->original_code());

  // Position at the first break point.
  break_point_ = -1;
  position_ = 1;
  statement_position_ = 1;
  Next();
}


bool BreakLocationIterator::Done() const {
  return RinfoDone();
}


void BreakLocationIterator::SetBreakPoint(Handle<Object> break_point_object) {
  // If there is not already a real break point here patch code with debug
  // break.
  if (!HasBreakPoint()) {
    SetDebugBreak();
  }
  ASSERT(IsDebugBreak());
  // Set the break point information.
  DebugInfo::SetBreakPoint(debug_info_, code_position(),
                           position(), statement_position(),
                           break_point_object);
}


void BreakLocationIterator::ClearBreakPoint(Handle<Object> break_point_object) {
  // Clear the break point information.
  DebugInfo::ClearBreakPoint(debug_info_, code_position(), break_point_object);
  // If there are no more break points here remove the debug break.
  if (!HasBreakPoint()) {
    ClearDebugBreak();
    ASSERT(!IsDebugBreak());
  }
}


void BreakLocationIterator::SetOneShot() {
  // If there is a real break point here no more to do.
  if (HasBreakPoint()) {
    ASSERT(IsDebugBreak());
    return;
  }

  // Patch code with debug break.
  SetDebugBreak();
}


void BreakLocationIterator::ClearOneShot() {
  // If there is a real break point here no more to do.
  if (HasBreakPoint()) {
    ASSERT(IsDebugBreak());
    return;
  }

  // Patch code removing debug break.
  ClearDebugBreak();
  ASSERT(!IsDebugBreak());
}


void BreakLocationIterator::SetDebugBreak() {
  // If there is already a break point here just return. This might happen if
  // the same code is flooded with break points twice. Flooding the same
  // function twice might happen when stepping in a function with an exception
  // handler as the handler and the function is the same.
  if (IsDebugBreak()) {
    return;
  }

  if (RelocInfo::IsJSReturn(rmode())) {
    // Patch the frame exit code with a break point.
    SetDebugBreakAtReturn();
  } else {
    // Patch the original code with the current address as the current address
    // might have changed by the inline caching since the code was copied.
    original_rinfo()->set_target_address(rinfo()->target_address());

    // Patch the code to invoke the builtin debug break function matching the
    // calling convention used by the call site.
    Handle<Code> dbgbrk_code(Debug::FindDebugBreak(rinfo()));
    rinfo()->set_target_address(dbgbrk_code->entry());
  }
  ASSERT(IsDebugBreak());
}


void BreakLocationIterator::ClearDebugBreak() {
  if (RelocInfo::IsJSReturn(rmode())) {
    // Restore the frame exit code.
    ClearDebugBreakAtReturn();
  } else {
    // Patch the code to the original invoke.
    rinfo()->set_target_address(original_rinfo()->target_address());
  }
  ASSERT(!IsDebugBreak());
}


void BreakLocationIterator::PrepareStepIn() {
  HandleScope scope;

  // Step in can only be prepared if currently positioned on an IC call or
  // construct call.
  Address target = rinfo()->target_address();
  Code* code = Code::GetCodeFromTargetAddress(target);
  if (code->is_call_stub()) {
    // Step in through IC call is handled by the runtime system. Therefore make
    // sure that the any current IC is cleared and the runtime system is
    // called. If the executing code has a debug break at the location change
    // the call in the original code as it is the code there that will be
    // executed in place of the debug break call.
    Handle<Code> stub = ComputeCallDebugPrepareStepIn(code->arguments_count());
    if (IsDebugBreak()) {
      original_rinfo()->set_target_address(stub->entry());
    } else {
      rinfo()->set_target_address(stub->entry());
    }
  } else {
    // Step in through constructs call requires no changes to the running code.
    ASSERT(RelocInfo::IsConstructCall(rmode()));
  }
}


// Check whether the break point is at a position which will exit the function.
bool BreakLocationIterator::IsExit() const {
  return (RelocInfo::IsJSReturn(rmode()));
}


bool BreakLocationIterator::HasBreakPoint() {
  return debug_info_->HasBreakPoint(code_position());
}


// Check whether there is a debug break at the current position.
bool BreakLocationIterator::IsDebugBreak() {
  if (RelocInfo::IsJSReturn(rmode())) {
    return IsDebugBreakAtReturn();
  } else {
    return Debug::IsDebugBreak(rinfo()->target_address());
  }
}


Object* BreakLocationIterator::BreakPointObjects() {
  return debug_info_->GetBreakPointObjects(code_position());
}


// Clear out all the debug break code. This is ONLY supposed to be used when
// shutting down the debugger as it will leave the break point information in
// DebugInfo even though the code is patched back to the non break point state.
void BreakLocationIterator::ClearAllDebugBreak() {
  while (!Done()) {
    ClearDebugBreak();
    Next();
  }
}


bool BreakLocationIterator::RinfoDone() const {
  ASSERT(reloc_iterator_->done() == reloc_iterator_original_->done());
  return reloc_iterator_->done();
}


void BreakLocationIterator::RinfoNext() {
  reloc_iterator_->next();
  reloc_iterator_original_->next();
#ifdef DEBUG
  ASSERT(reloc_iterator_->done() == reloc_iterator_original_->done());
  if (!reloc_iterator_->done()) {
    ASSERT(rmode() == original_rmode());
  }
#endif
}


bool Debug::has_break_points_ = false;
DebugInfoListNode* Debug::debug_info_list_ = NULL;


// Threading support.
void Debug::ThreadInit() {
  thread_local_.break_count_ = 0;
  thread_local_.break_id_ = 0;
  thread_local_.break_frame_id_ = StackFrame::NO_ID;
  thread_local_.last_step_action_ = StepNone;
  thread_local_.last_statement_position_ = RelocInfo::kNoPosition;
  thread_local_.step_count_ = 0;
  thread_local_.last_fp_ = 0;
  thread_local_.step_into_fp_ = 0;
  thread_local_.after_break_target_ = 0;
  thread_local_.debugger_entry_ = NULL;
  thread_local_.preemption_pending_ = false;
}


JSCallerSavedBuffer Debug::registers_;
Debug::ThreadLocal Debug::thread_local_;


char* Debug::ArchiveDebug(char* storage) {
  char* to = storage;
  memcpy(to, reinterpret_cast<char*>(&thread_local_), sizeof(ThreadLocal));
  to += sizeof(ThreadLocal);
  memcpy(to, reinterpret_cast<char*>(&registers_), sizeof(registers_));
  ThreadInit();
  ASSERT(to <= storage + ArchiveSpacePerThread());
  return storage + ArchiveSpacePerThread();
}


char* Debug::RestoreDebug(char* storage) {
  char* from = storage;
  memcpy(reinterpret_cast<char*>(&thread_local_), from, sizeof(ThreadLocal));
  from += sizeof(ThreadLocal);
  memcpy(reinterpret_cast<char*>(&registers_), from, sizeof(registers_));
  ASSERT(from <= storage + ArchiveSpacePerThread());
  return storage + ArchiveSpacePerThread();
}


int Debug::ArchiveSpacePerThread() {
  return sizeof(ThreadLocal) + sizeof(registers_);
}


// Default break enabled.
bool Debug::disable_break_ = false;

// Default call debugger on uncaught exception.
bool Debug::break_on_exception_ = false;
bool Debug::break_on_uncaught_exception_ = true;

Handle<Context> Debug::debug_context_ = Handle<Context>();
Code* Debug::debug_break_return_entry_ = NULL;
Code* Debug::debug_break_return_ = NULL;


void Debug::HandleWeakDebugInfo(v8::Persistent<v8::Value> obj, void* data) {
  DebugInfoListNode* node = reinterpret_cast<DebugInfoListNode*>(data);
  RemoveDebugInfo(node->debug_info());
#ifdef DEBUG
  node = Debug::debug_info_list_;
  while (node != NULL) {
    ASSERT(node != reinterpret_cast<DebugInfoListNode*>(data));
    node = node->next();
  }
#endif
}


DebugInfoListNode::DebugInfoListNode(DebugInfo* debug_info): next_(NULL) {
  // Globalize the request debug info object and make it weak.
  debug_info_ = Handle<DebugInfo>::cast((GlobalHandles::Create(debug_info)));
  GlobalHandles::MakeWeak(reinterpret_cast<Object**>(debug_info_.location()),
                          this, Debug::HandleWeakDebugInfo);
}


DebugInfoListNode::~DebugInfoListNode() {
  GlobalHandles::Destroy(reinterpret_cast<Object**>(debug_info_.location()));
}


void Debug::Setup(bool create_heap_objects) {
  ThreadInit();
  if (create_heap_objects) {
    // Get code to handle entry to debug break on return.
    debug_break_return_entry_ =
        Builtins::builtin(Builtins::Return_DebugBreakEntry);
    ASSERT(debug_break_return_entry_->IsCode());

    // Get code to handle debug break on return.
    debug_break_return_ =
        Builtins::builtin(Builtins::Return_DebugBreak);
    ASSERT(debug_break_return_->IsCode());
  }
}


bool Debug::CompileDebuggerScript(int index) {
  HandleScope scope;

  // Bail out if the index is invalid.
  if (index == -1) {
    return false;
  }

  // Find source and name for the requested script.
  Handle<String> source_code = Bootstrapper::NativesSourceLookup(index);
  Vector<const char> name = Natives::GetScriptName(index);
  Handle<String> script_name = Factory::NewStringFromAscii(name);

  // Compile the script.
  bool allow_natives_syntax = FLAG_allow_natives_syntax;
  FLAG_allow_natives_syntax = true;
  Handle<JSFunction> boilerplate;
  boilerplate = Compiler::Compile(source_code, script_name, 0, 0, NULL, NULL);
  FLAG_allow_natives_syntax = allow_natives_syntax;

  // Silently ignore stack overflows during compilation.
  if (boilerplate.is_null()) {
    ASSERT(Top::has_pending_exception());
    Top::clear_pending_exception();
    return false;
  }

  // Execute the boilerplate function in the debugger context.
  Handle<Context> context = Top::global_context();
  bool caught_exception = false;
  Handle<JSFunction> function =
      Factory::NewFunctionFromBoilerplate(boilerplate, context);
  Handle<Object> result =
      Execution::TryCall(function, Handle<Object>(context->global()),
                         0, NULL, &caught_exception);

  // Check for caught exceptions.
  if (caught_exception) {
    Handle<Object> message = MessageHandler::MakeMessageObject(
        "error_loading_debugger", NULL, HandleVector<Object>(&result, 1),
        Handle<String>());
    MessageHandler::ReportMessage(NULL, message);
    return false;
  }

  // Mark this script as native and return successfully.
  Handle<Script> script(Script::cast(function->shared()->script()));
  script->set_type(Smi::FromInt(SCRIPT_TYPE_NATIVE));
  return true;
}


bool Debug::Load() {
  // Return if debugger is already loaded.
  if (IsLoaded()) return true;

  // Bail out if we're already in the process of compiling the native
  // JavaScript source code for the debugger.
  if (Debugger::compiling_natives() || Debugger::is_loading_debugger())
    return false;
  Debugger::set_loading_debugger(true);

  // Disable breakpoints and interrupts while compiling and running the
  // debugger scripts including the context creation code.
  DisableBreak disable(true);
  PostponeInterruptsScope postpone;

  // Create the debugger context.
  HandleScope scope;
  Handle<Context> context =
      Bootstrapper::CreateEnvironment(Handle<Object>::null(),
                                      v8::Handle<ObjectTemplate>(),
                                      NULL);

  // Use the debugger context.
  SaveContext save;
  Top::set_context(*context);

  // Expose the builtins object in the debugger context.
  Handle<String> key = Factory::LookupAsciiSymbol("builtins");
  Handle<GlobalObject> global = Handle<GlobalObject>(context->global());
  SetProperty(global, key, Handle<Object>(global->builtins()), NONE);

  // Compile the JavaScript for the debugger in the debugger context.
  Debugger::set_compiling_natives(true);
  bool caught_exception =
      !CompileDebuggerScript(Natives::GetIndex("mirror")) ||
      !CompileDebuggerScript(Natives::GetIndex("debug"));
  Debugger::set_compiling_natives(false);

  // Make sure we mark the debugger as not loading before we might
  // return.
  Debugger::set_loading_debugger(false);

  // Check for caught exceptions.
  if (caught_exception) return false;

  // Debugger loaded.
  debug_context_ = Handle<Context>::cast(GlobalHandles::Create(*context));
  return true;
}


void Debug::Unload() {
  // Return debugger is not loaded.
  if (!IsLoaded()) {
    return;
  }

  // Clear debugger context global handle.
  GlobalHandles::Destroy(reinterpret_cast<Object**>(debug_context_.location()));
  debug_context_ = Handle<Context>();
}


// Set the flag indicating that preemption happened during debugging.
void Debug::PreemptionWhileInDebugger() {
  ASSERT(InDebugger());
  Debug::set_preemption_pending(true);
}


void Debug::Iterate(ObjectVisitor* v) {
  v->VisitPointer(bit_cast<Object**, Code**>(&(debug_break_return_entry_)));
  v->VisitPointer(bit_cast<Object**, Code**>(&(debug_break_return_)));
}


Object* Debug::Break(Arguments args) {
  HandleScope scope;
  ASSERT(args.length() == 0);

  // Get the top-most JavaScript frame.
  JavaScriptFrameIterator it;
  JavaScriptFrame* frame = it.frame();

  // Just continue if breaks are disabled or debugger cannot be loaded.
  if (disable_break() || !Load()) {
    SetAfterBreakTarget(frame);
    return Heap::undefined_value();
  }

  // Enter the debugger.
  EnterDebugger debugger;
  if (debugger.FailedToEnter()) {
    return Heap::undefined_value();
  }

  // Postpone interrupt during breakpoint processing.
  PostponeInterruptsScope postpone;

  // Get the debug info (create it if it does not exist).
  Handle<SharedFunctionInfo> shared =
      Handle<SharedFunctionInfo>(JSFunction::cast(frame->function())->shared());
  Handle<DebugInfo> debug_info = GetDebugInfo(shared);

  // Find the break point where execution has stopped.
  BreakLocationIterator break_location_iterator(debug_info,
                                                ALL_BREAK_LOCATIONS);
  break_location_iterator.FindBreakLocationFromAddress(frame->pc());

  // Check whether step next reached a new statement.
  if (!StepNextContinue(&break_location_iterator, frame)) {
    // Decrease steps left if performing multiple steps.
    if (thread_local_.step_count_ > 0) {
      thread_local_.step_count_--;
    }
  }

  // If there is one or more real break points check whether any of these are
  // triggered.
  Handle<Object> break_points_hit(Heap::undefined_value());
  if (break_location_iterator.HasBreakPoint()) {
    Handle<Object> break_point_objects =
        Handle<Object>(break_location_iterator.BreakPointObjects());
    break_points_hit = CheckBreakPoints(break_point_objects);
  }

  // Notify debugger if a real break point is triggered or if performing single
  // stepping with no more steps to perform. Otherwise do another step.
  if (!break_points_hit->IsUndefined() ||
    (thread_local_.last_step_action_ != StepNone &&
     thread_local_.step_count_ == 0)) {
    // Clear all current stepping setup.
    ClearStepping();

    // Notify the debug event listeners.
    Debugger::OnDebugBreak(break_points_hit, false);
  } else if (thread_local_.last_step_action_ != StepNone) {
    // Hold on to last step action as it is cleared by the call to
    // ClearStepping.
    StepAction step_action = thread_local_.last_step_action_;
    int step_count = thread_local_.step_count_;

    // Clear all current stepping setup.
    ClearStepping();

    // Set up for the remaining steps.
    PrepareStep(step_action, step_count);
  }

  // Install jump to the call address which was overwritten.
  SetAfterBreakTarget(frame);

  return Heap::undefined_value();
}


// Check the break point objects for whether one or more are actually
// triggered. This function returns a JSArray with the break point objects
// which is triggered.
Handle<Object> Debug::CheckBreakPoints(Handle<Object> break_point_objects) {
  int break_points_hit_count = 0;
  Handle<JSArray> break_points_hit = Factory::NewJSArray(1);

  // If there are multiple break points they are in a FixedArray.
  ASSERT(!break_point_objects->IsUndefined());
  if (break_point_objects->IsFixedArray()) {
    Handle<FixedArray> array(FixedArray::cast(*break_point_objects));
    for (int i = 0; i < array->length(); i++) {
      Handle<Object> o(array->get(i));
      if (CheckBreakPoint(o)) {
        break_points_hit->SetElement(break_points_hit_count++, *o);
      }
    }
  } else {
    if (CheckBreakPoint(break_point_objects)) {
      break_points_hit->SetElement(break_points_hit_count++,
                                   *break_point_objects);
    }
  }

  // Return undefined if no break points where triggered.
  if (break_points_hit_count == 0) {
    return Factory::undefined_value();
  }
  return break_points_hit;
}


// Check whether a single break point object is triggered.
bool Debug::CheckBreakPoint(Handle<Object> break_point_object) {
  HandleScope scope;

  // Ignore check if break point object is not a JSObject.
  if (!break_point_object->IsJSObject()) return true;

  // Get the function CheckBreakPoint (defined in debug.js).
  Handle<JSFunction> check_break_point =
    Handle<JSFunction>(JSFunction::cast(
      debug_context()->global()->GetProperty(
          *Factory::LookupAsciiSymbol("IsBreakPointTriggered"))));

  // Get the break id as an object.
  Handle<Object> break_id = Factory::NewNumberFromInt(Debug::break_id());

  // Call HandleBreakPointx.
  bool caught_exception = false;
  const int argc = 2;
  Object** argv[argc] = {
    break_id.location(),
    reinterpret_cast<Object**>(break_point_object.location())
  };
  Handle<Object> result = Execution::TryCall(check_break_point,
                                             Top::builtins(), argc, argv,
                                             &caught_exception);

  // If exception or non boolean result handle as not triggered
  if (caught_exception || !result->IsBoolean()) {
    return false;
  }

  // Return whether the break point is triggered.
  return *result == Heap::true_value();
}


// Check whether the function has debug information.
bool Debug::HasDebugInfo(Handle<SharedFunctionInfo> shared) {
  return !shared->debug_info()->IsUndefined();
}


// Return the debug info for this function. EnsureDebugInfo must be called
// prior to ensure the debug info has been generated for shared.
Handle<DebugInfo> Debug::GetDebugInfo(Handle<SharedFunctionInfo> shared) {
  ASSERT(HasDebugInfo(shared));
  return Handle<DebugInfo>(DebugInfo::cast(shared->debug_info()));
}


void Debug::SetBreakPoint(Handle<SharedFunctionInfo> shared,
                          int source_position,
                          Handle<Object> break_point_object) {
  HandleScope scope;

  if (!EnsureDebugInfo(shared)) {
    // Return if retrieving debug info failed.
    return;
  }

  Handle<DebugInfo> debug_info = GetDebugInfo(shared);
  // Source positions starts with zero.
  ASSERT(source_position >= 0);

  // Find the break point and change it.
  BreakLocationIterator it(debug_info, SOURCE_BREAK_LOCATIONS);
  it.FindBreakLocationFromPosition(source_position);
  it.SetBreakPoint(break_point_object);

  // At least one active break point now.
  ASSERT(debug_info->GetBreakPointCount() > 0);
}


void Debug::ClearBreakPoint(Handle<Object> break_point_object) {
  HandleScope scope;

  DebugInfoListNode* node = debug_info_list_;
  while (node != NULL) {
    Object* result = DebugInfo::FindBreakPointInfo(node->debug_info(),
                                                   break_point_object);
    if (!result->IsUndefined()) {
      // Get information in the break point.
      BreakPointInfo* break_point_info = BreakPointInfo::cast(result);
      Handle<DebugInfo> debug_info = node->debug_info();
      Handle<SharedFunctionInfo> shared(debug_info->shared());
      int source_position =  break_point_info->statement_position()->value();

      // Source positions starts with zero.
      ASSERT(source_position >= 0);

      // Find the break point and clear it.
      BreakLocationIterator it(debug_info, SOURCE_BREAK_LOCATIONS);
      it.FindBreakLocationFromPosition(source_position);
      it.ClearBreakPoint(break_point_object);

      // If there are no more break points left remove the debug info for this
      // function.
      if (debug_info->GetBreakPointCount() == 0) {
        RemoveDebugInfo(debug_info);
      }

      return;
    }
    node = node->next();
  }
}


void Debug::ClearAllBreakPoints() {
  DebugInfoListNode* node = debug_info_list_;
  while (node != NULL) {
    // Remove all debug break code.
    BreakLocationIterator it(node->debug_info(), ALL_BREAK_LOCATIONS);
    it.ClearAllDebugBreak();
    node = node->next();
  }

  // Remove all debug info.
  while (debug_info_list_ != NULL) {
    RemoveDebugInfo(debug_info_list_->debug_info());
  }
}


void Debug::FloodWithOneShot(Handle<SharedFunctionInfo> shared) {
  // Make sure the function has setup the debug info.
  if (!EnsureDebugInfo(shared)) {
    // Return if we failed to retrieve the debug info.
    return;
  }

  // Flood the function with break points.
  BreakLocationIterator it(GetDebugInfo(shared), ALL_BREAK_LOCATIONS);
  while (!it.Done()) {
    it.SetOneShot();
    it.Next();
  }
}


void Debug::FloodHandlerWithOneShot() {
  // Iterate through the JavaScript stack looking for handlers.
  StackFrame::Id id = break_frame_id();
  if (id == StackFrame::NO_ID) {
    // If there is no JavaScript stack don't do anything.
    return;
  }
  for (JavaScriptFrameIterator it(id); !it.done(); it.Advance()) {
    JavaScriptFrame* frame = it.frame();
    if (frame->HasHandler()) {
      Handle<SharedFunctionInfo> shared =
          Handle<SharedFunctionInfo>(
              JSFunction::cast(frame->function())->shared());
      // Flood the function with the catch block with break points
      FloodWithOneShot(shared);
      return;
    }
  }
}


void Debug::ChangeBreakOnException(ExceptionBreakType type, bool enable) {
  if (type == BreakUncaughtException) {
    break_on_uncaught_exception_ = enable;
  } else {
    break_on_exception_ = enable;
  }
}


void Debug::PrepareStep(StepAction step_action, int step_count) {
  HandleScope scope;
  ASSERT(Debug::InDebugger());

  // Remember this step action and count.
  thread_local_.last_step_action_ = step_action;
  thread_local_.step_count_ = step_count;

  // Get the frame where the execution has stopped and skip the debug frame if
  // any. The debug frame will only be present if execution was stopped due to
  // hitting a break point. In other situations (e.g. unhandled exception) the
  // debug frame is not present.
  StackFrame::Id id = break_frame_id();
  if (id == StackFrame::NO_ID) {
    // If there is no JavaScript stack don't do anything.
    return;
  }
  JavaScriptFrameIterator frames_it(id);
  JavaScriptFrame* frame = frames_it.frame();

  // First of all ensure there is one-shot break points in the top handler
  // if any.
  FloodHandlerWithOneShot();

  // If the function on the top frame is unresolved perform step out. This will
  // be the case when calling unknown functions and having the debugger stopped
  // in an unhandled exception.
  if (!frame->function()->IsJSFunction()) {
    // Step out: Find the calling JavaScript frame and flood it with
    // breakpoints.
    frames_it.Advance();
    // Fill the function to return to with one-shot break points.
    JSFunction* function = JSFunction::cast(frames_it.frame()->function());
    FloodWithOneShot(Handle<SharedFunctionInfo>(function->shared()));
    return;
  }

  // Get the debug info (create it if it does not exist).
  Handle<SharedFunctionInfo> shared =
      Handle<SharedFunctionInfo>(JSFunction::cast(frame->function())->shared());
  if (!EnsureDebugInfo(shared)) {
    // Return if ensuring debug info failed.
    return;
  }
  Handle<DebugInfo> debug_info = GetDebugInfo(shared);

  // Find the break location where execution has stopped.
  BreakLocationIterator it(debug_info, ALL_BREAK_LOCATIONS);
  it.FindBreakLocationFromAddress(frame->pc());

  // Compute whether or not the target is a call target.
  bool is_call_target = false;
  if (RelocInfo::IsCodeTarget(it.rinfo()->rmode())) {
    Address target = it.rinfo()->target_address();
    Code* code = Code::GetCodeFromTargetAddress(target);
    if (code->is_call_stub()) is_call_target = true;
  }

  // If this is the last break code target step out is the only possibility.
  if (it.IsExit() || step_action == StepOut) {
    // Step out: If there is a JavaScript caller frame, we need to
    // flood it with breakpoints.
    frames_it.Advance();
    if (!frames_it.done()) {
      // Fill the function to return to with one-shot break points.
      JSFunction* function = JSFunction::cast(frames_it.frame()->function());
      FloodWithOneShot(Handle<SharedFunctionInfo>(function->shared()));
    }
  } else if (!(is_call_target || RelocInfo::IsConstructCall(it.rmode())) ||
             step_action == StepNext || step_action == StepMin) {
    // Step next or step min.

    // Fill the current function with one-shot break points.
    FloodWithOneShot(shared);

    // Remember source position and frame to handle step next.
    thread_local_.last_statement_position_ =
        debug_info->code()->SourceStatementPosition(frame->pc());
    thread_local_.last_fp_ = frame->fp();
  } else {
    // Fill the current function with one-shot break points even for step in on
    // a call target as the function called might be a native function for
    // which step in will not stop.
    FloodWithOneShot(shared);

    // Step in or Step in min
    it.PrepareStepIn();
    ActivateStepIn(frame);
  }
}


// Check whether the current debug break should be reported to the debugger. It
// is used to have step next and step in only report break back to the debugger
// if on a different frame or in a different statement. In some situations
// there will be several break points in the same statement when the code is
// flooded with one-shot break points. This function helps to perform several
// steps before reporting break back to the debugger.
bool Debug::StepNextContinue(BreakLocationIterator* break_location_iterator,
                             JavaScriptFrame* frame) {
  // If the step last action was step next or step in make sure that a new
  // statement is hit.
  if (thread_local_.last_step_action_ == StepNext ||
      thread_local_.last_step_action_ == StepIn) {
    // Never continue if returning from function.
    if (break_location_iterator->IsExit()) return false;

    // Continue if we are still on the same frame and in the same statement.
    int current_statement_position =
        break_location_iterator->code()->SourceStatementPosition(frame->pc());
    return thread_local_.last_fp_ == frame->fp() &&
        thread_local_.last_statement_position_ == current_statement_position;
  }

  // No step next action - don't continue.
  return false;
}


// Check whether the code object at the specified address is a debug break code
// object.
bool Debug::IsDebugBreak(Address addr) {
  Code* code = Code::GetCodeFromTargetAddress(addr);
  return code->ic_state() == DEBUG_BREAK;
}


// Check whether a code stub with the specified major key is a possible break
// point location when looking for source break locations.
bool Debug::IsSourceBreakStub(Code* code) {
  CodeStub::Major major_key = code->major_key();
  return major_key == CodeStub::CallFunction;
}


// Check whether a code stub with the specified major key is a possible break
// location.
bool Debug::IsBreakStub(Code* code) {
  CodeStub::Major major_key = code->major_key();
  return major_key == CodeStub::CallFunction ||
         major_key == CodeStub::StackCheck;
}


// Find the builtin to use for invoking the debug break
Handle<Code> Debug::FindDebugBreak(RelocInfo* rinfo) {
  // Find the builtin debug break function matching the calling convention
  // used by the call site.
  RelocInfo::Mode mode = rinfo->rmode();

  if (RelocInfo::IsCodeTarget(mode)) {
    Address target = rinfo->target_address();
    Code* code = Code::GetCodeFromTargetAddress(target);
    if (code->is_inline_cache_stub()) {
      if (code->is_call_stub()) {
        return ComputeCallDebugBreak(code->arguments_count());
      }
      if (code->is_load_stub()) {
        return Handle<Code>(Builtins::builtin(Builtins::LoadIC_DebugBreak));
      }
      if (code->is_store_stub()) {
        return Handle<Code>(Builtins::builtin(Builtins::StoreIC_DebugBreak));
      }
      if (code->is_keyed_load_stub()) {
        Handle<Code> result =
            Handle<Code>(Builtins::builtin(Builtins::KeyedLoadIC_DebugBreak));
        return result;
      }
      if (code->is_keyed_store_stub()) {
        Handle<Code> result =
            Handle<Code>(Builtins::builtin(Builtins::KeyedStoreIC_DebugBreak));
        return result;
      }
    }
    if (RelocInfo::IsConstructCall(mode)) {
      Handle<Code> result =
          Handle<Code>(Builtins::builtin(Builtins::ConstructCall_DebugBreak));
      return result;
    }
    if (code->kind() == Code::STUB) {
      ASSERT(code->major_key() == CodeStub::CallFunction ||
             code->major_key() == CodeStub::StackCheck);
      Handle<Code> result =
          Handle<Code>(Builtins::builtin(Builtins::StubNoRegisters_DebugBreak));
      return result;
    }
  }

  UNREACHABLE();
  return Handle<Code>::null();
}


// Simple function for returning the source positions for active break points.
Handle<Object> Debug::GetSourceBreakLocations(
    Handle<SharedFunctionInfo> shared) {
  if (!HasDebugInfo(shared)) return Handle<Object>(Heap::undefined_value());
  Handle<DebugInfo> debug_info = GetDebugInfo(shared);
  if (debug_info->GetBreakPointCount() == 0) {
    return Handle<Object>(Heap::undefined_value());
  }
  Handle<FixedArray> locations =
      Factory::NewFixedArray(debug_info->GetBreakPointCount());
  int count = 0;
  for (int i = 0; i < debug_info->break_points()->length(); i++) {
    if (!debug_info->break_points()->get(i)->IsUndefined()) {
      BreakPointInfo* break_point_info =
          BreakPointInfo::cast(debug_info->break_points()->get(i));
      if (break_point_info->GetBreakPointCount() > 0) {
        locations->set(count++, break_point_info->statement_position());
      }
    }
  }
  return locations;
}


void Debug::NewBreak(StackFrame::Id break_frame_id) {
  thread_local_.break_frame_id_ = break_frame_id;
  thread_local_.break_id_ = ++thread_local_.break_count_;
}


void Debug::SetBreak(StackFrame::Id break_frame_id, int break_id) {
  thread_local_.break_frame_id_ = break_frame_id;
  thread_local_.break_id_ = break_id;
}


// Handle stepping into a function.
void Debug::HandleStepIn(Handle<JSFunction> function,
                         Address fp,
                         bool is_constructor) {
  // If the frame pointer is not supplied by the caller find it.
  if (fp == 0) {
    StackFrameIterator it;
    it.Advance();
    // For constructor functions skip another frame.
    if (is_constructor) {
      ASSERT(it.frame()->is_construct());
      it.Advance();
    }
    fp = it.frame()->fp();
  }

  // Flood the function with one-shot break points if it is called from where
  // step into was requested.
  if (fp == Debug::step_in_fp()) {
    // Don't allow step into functions in the native context.
    if (function->context()->global() != Top::context()->builtins()) {
      Debug::FloodWithOneShot(Handle<SharedFunctionInfo>(function->shared()));
    }
  }
}


void Debug::ClearStepping() {
  // Clear the various stepping setup.
  ClearOneShot();
  ClearStepIn();
  ClearStepNext();

  // Clear multiple step counter.
  thread_local_.step_count_ = 0;
}

// Clears all the one-shot break points that are currently set. Normally this
// function is called each time a break point is hit as one shot break points
// are used to support stepping.
void Debug::ClearOneShot() {
  // The current implementation just runs through all the breakpoints. When the
  // last break point for a function is removed that function is automatically
  // removed from the list.

  DebugInfoListNode* node = debug_info_list_;
  while (node != NULL) {
    BreakLocationIterator it(node->debug_info(), ALL_BREAK_LOCATIONS);
    while (!it.Done()) {
      it.ClearOneShot();
      it.Next();
    }
    node = node->next();
  }
}


void Debug::ActivateStepIn(StackFrame* frame) {
  thread_local_.step_into_fp_ = frame->fp();
}


void Debug::ClearStepIn() {
  thread_local_.step_into_fp_ = 0;
}


void Debug::ClearStepNext() {
  thread_local_.last_step_action_ = StepNone;
  thread_local_.last_statement_position_ = RelocInfo::kNoPosition;
  thread_local_.last_fp_ = 0;
}


bool Debug::EnsureCompiled(Handle<SharedFunctionInfo> shared) {
  if (shared->is_compiled()) return true;
  return CompileLazyShared(shared, CLEAR_EXCEPTION, 0);
}


// Ensures the debug information is present for shared.
bool Debug::EnsureDebugInfo(Handle<SharedFunctionInfo> shared) {
  // Return if we already have the debug info for shared.
  if (HasDebugInfo(shared)) return true;

  // Ensure shared in compiled. Return false if this failed.
  if (!EnsureCompiled(shared)) return false;

  // Create the debug info object.
  Handle<DebugInfo> debug_info = Factory::NewDebugInfo(shared);

  // Add debug info to the list.
  DebugInfoListNode* node = new DebugInfoListNode(*debug_info);
  node->set_next(debug_info_list_);
  debug_info_list_ = node;

  // Now there is at least one break point.
  has_break_points_ = true;

  return true;
}


void Debug::RemoveDebugInfo(Handle<DebugInfo> debug_info) {
  ASSERT(debug_info_list_ != NULL);
  // Run through the debug info objects to find this one and remove it.
  DebugInfoListNode* prev = NULL;
  DebugInfoListNode* current = debug_info_list_;
  while (current != NULL) {
    if (*current->debug_info() == *debug_info) {
      // Unlink from list. If prev is NULL we are looking at the first element.
      if (prev == NULL) {
        debug_info_list_ = current->next();
      } else {
        prev->set_next(current->next());
      }
      current->debug_info()->shared()->set_debug_info(Heap::undefined_value());
      delete current;

      // If there are no more debug info objects there are not more break
      // points.
      has_break_points_ = debug_info_list_ != NULL;

      return;
    }
    // Move to next in list.
    prev = current;
    current = current->next();
  }
  UNREACHABLE();
}


void Debug::SetAfterBreakTarget(JavaScriptFrame* frame) {
  HandleScope scope;

  // Get the executing function in which the debug break occurred.
  Handle<SharedFunctionInfo> shared =
      Handle<SharedFunctionInfo>(JSFunction::cast(frame->function())->shared());
  if (!EnsureDebugInfo(shared)) {
    // Return if we failed to retrieve the debug info.
    return;
  }
  Handle<DebugInfo> debug_info = GetDebugInfo(shared);
  Handle<Code> code(debug_info->code());
  Handle<Code> original_code(debug_info->original_code());
#ifdef DEBUG
  // Get the code which is actually executing.
  Handle<Code> frame_code(frame->code());
  ASSERT(frame_code.is_identical_to(code));
#endif

  // Find the call address in the running code. This address holds the call to
  // either a DebugBreakXXX or to the debug break return entry code if the
  // break point is still active after processing the break point.
  Address addr = frame->pc() - Assembler::kTargetAddrToReturnAddrDist;

  // Check if the location is at JS exit.
  bool at_js_exit = false;
  RelocIterator it(debug_info->code());
  while (!it.done()) {
    if (RelocInfo::IsJSReturn(it.rinfo()->rmode())) {
      at_js_exit = it.rinfo()->pc() == addr - 1;
    }
    it.next();
  }

  // Handle the jump to continue execution after break point depending on the
  // break location.
  if (at_js_exit) {
    // First check if the call in the code is still the debug break return
    // entry code. If it is the break point is still active. If not the break
    // point was removed during break point processing.
    if (Assembler::target_address_at(addr) ==
        debug_break_return_entry()->entry()) {
      // Break point still active. Jump to the corresponding place in the
      // original code.
      addr +=  original_code->instruction_start() - code->instruction_start();
    }

    // Move one byte back to where the call instruction was placed.
    thread_local_.after_break_target_ = addr - 1;
  } else {
    // Check if there still is a debug break call at the target address. If the
    // break point has been removed it will have disappeared. If it have
    // disappeared don't try to look in the original code as the running code
    // will have the right address. This takes care of the case where the last
    // break point is removed from the function and therefore no "original code"
    // is available. If the debug break call is still there find the address in
    // the original code.
    if (IsDebugBreak(Assembler::target_address_at(addr))) {
      // If the break point is still there find the call address which was
      // overwritten in the original code by the call to DebugBreakXXX.

      // Find the corresponding address in the original code.
      addr += original_code->instruction_start() - code->instruction_start();
    }

    // Install jump to the call address in the original code. This will be the
    // call which was overwritten by the call to DebugBreakXXX.
    thread_local_.after_break_target_ = Assembler::target_address_at(addr);
  }
}


bool Debug::IsDebugGlobal(GlobalObject* global) {
  return IsLoaded() && global == Debug::debug_context()->global();
}


void Debug::ClearMirrorCache() {
  HandleScope scope;
  ASSERT(Top::context() == *Debug::debug_context());

  // Clear the mirror cache.
  Handle<String> function_name =
      Factory::LookupSymbol(CStrVector("ClearMirrorCache"));
  Handle<Object> fun(Top::global()->GetProperty(*function_name));
  ASSERT(fun->IsJSFunction());
  bool caught_exception;
  Handle<Object> js_object = Execution::TryCall(
      Handle<JSFunction>::cast(fun),
      Handle<JSObject>(Debug::debug_context()->global()),
      0, NULL, &caught_exception);
}


Mutex* Debugger::debugger_access_ = OS::CreateMutex();
Handle<Object> Debugger::event_listener_ = Handle<Object>();
Handle<Object> Debugger::event_listener_data_ = Handle<Object>();
bool Debugger::compiling_natives_ = false;
bool Debugger::is_loading_debugger_ = false;
bool Debugger::never_unload_debugger_ = false;
DebugMessageThread* Debugger::message_thread_ = NULL;
v8::DebugMessageHandler Debugger::message_handler_ = NULL;
bool Debugger::message_handler_cleared_ = false;
void* Debugger::message_handler_data_ = NULL;
v8::DebugHostDispatchHandler Debugger::host_dispatch_handler_ = NULL;
void* Debugger::host_dispatch_handler_data_ = NULL;
DebuggerAgent* Debugger::agent_ = NULL;
LockingMessageQueue Debugger::command_queue_(kQueueInitialSize);
LockingMessageQueue Debugger::message_queue_(kQueueInitialSize);
Semaphore* Debugger::command_received_ = OS::CreateSemaphore(0);
Semaphore* Debugger::message_received_ = OS::CreateSemaphore(0);


Handle<Object> Debugger::MakeJSObject(Vector<const char> constructor_name,
                                      int argc, Object*** argv,
                                      bool* caught_exception) {
  ASSERT(Top::context() == *Debug::debug_context());

  // Create the execution state object.
  Handle<String> constructor_str = Factory::LookupSymbol(constructor_name);
  Handle<Object> constructor(Top::global()->GetProperty(*constructor_str));
  ASSERT(constructor->IsJSFunction());
  if (!constructor->IsJSFunction()) {
    *caught_exception = true;
    return Factory::undefined_value();
  }
  Handle<Object> js_object = Execution::TryCall(
      Handle<JSFunction>::cast(constructor),
      Handle<JSObject>(Debug::debug_context()->global()), argc, argv,
      caught_exception);
  return js_object;
}


Handle<Object> Debugger::MakeExecutionState(bool* caught_exception) {
  // Create the execution state object.
  Handle<Object> break_id = Factory::NewNumberFromInt(Debug::break_id());
  const int argc = 1;
  Object** argv[argc] = { break_id.location() };
  return MakeJSObject(CStrVector("MakeExecutionState"),
                      argc, argv, caught_exception);
}


Handle<Object> Debugger::MakeBreakEvent(Handle<Object> exec_state,
                                        Handle<Object> break_points_hit,
                                        bool* caught_exception) {
  // Create the new break event object.
  const int argc = 2;
  Object** argv[argc] = { exec_state.location(),
                          break_points_hit.location() };
  return MakeJSObject(CStrVector("MakeBreakEvent"),
                      argc,
                      argv,
                      caught_exception);
}


Handle<Object> Debugger::MakeExceptionEvent(Handle<Object> exec_state,
                                            Handle<Object> exception,
                                            bool uncaught,
                                            bool* caught_exception) {
  // Create the new exception event object.
  const int argc = 3;
  Object** argv[argc] = { exec_state.location(),
                          exception.location(),
                          uncaught ? Factory::true_value().location() :
                                     Factory::false_value().location()};
  return MakeJSObject(CStrVector("MakeExceptionEvent"),
                      argc, argv, caught_exception);
}


Handle<Object> Debugger::MakeNewFunctionEvent(Handle<Object> function,
                                              bool* caught_exception) {
  // Create the new function event object.
  const int argc = 1;
  Object** argv[argc] = { function.location() };
  return MakeJSObject(CStrVector("MakeNewFunctionEvent"),
                      argc, argv, caught_exception);
}


Handle<Object> Debugger::MakeCompileEvent(Handle<Script> script,
                                          bool before,
                                          bool* caught_exception) {
  // Create the compile event object.
  Handle<Object> exec_state = MakeExecutionState(caught_exception);
  Handle<Object> script_wrapper = GetScriptWrapper(script);
  const int argc = 3;
  Object** argv[argc] = { exec_state.location(),
                          script_wrapper.location(),
                          before ? Factory::true_value().location() :
                                   Factory::false_value().location() };

  return MakeJSObject(CStrVector("MakeCompileEvent"),
                      argc,
                      argv,
                      caught_exception);
}


void Debugger::OnException(Handle<Object> exception, bool uncaught) {
  HandleScope scope;

  // Bail out based on state or if there is no listener for this event
  if (Debug::InDebugger()) return;
  if (!Debugger::EventActive(v8::Exception)) return;

  // Bail out if exception breaks are not active
  if (uncaught) {
    // Uncaught exceptions are reported by either flags.
    if (!(Debug::break_on_uncaught_exception() ||
          Debug::break_on_exception())) return;
  } else {
    // Caught exceptions are reported is activated.
    if (!Debug::break_on_exception()) return;
  }

  // Enter the debugger.
  EnterDebugger debugger;
  if (debugger.FailedToEnter()) return;

  // Clear all current stepping setup.
  Debug::ClearStepping();
  // Create the event data object.
  bool caught_exception = false;
  Handle<Object> exec_state = MakeExecutionState(&caught_exception);
  Handle<Object> event_data;
  if (!caught_exception) {
    event_data = MakeExceptionEvent(exec_state, exception, uncaught,
                                    &caught_exception);
  }
  // Bail out and don't call debugger if exception.
  if (caught_exception) {
    return;
  }

  // Process debug event
  ProcessDebugEvent(v8::Exception, event_data, false);
  // Return to continue execution from where the exception was thrown.
}


void Debugger::OnDebugBreak(Handle<Object> break_points_hit,
                            bool auto_continue) {
  HandleScope scope;

  // Debugger has already been entered by caller.
  ASSERT(Top::context() == *Debug::debug_context());

  // Bail out if there is no listener for this event
  if (!Debugger::EventActive(v8::Break)) return;

  // Debugger must be entered in advance.
  ASSERT(Top::context() == *Debug::debug_context());

  // Create the event data object.
  bool caught_exception = false;
  Handle<Object> exec_state = MakeExecutionState(&caught_exception);
  Handle<Object> event_data;
  if (!caught_exception) {
    event_data = MakeBreakEvent(exec_state, break_points_hit,
                                &caught_exception);
  }
  // Bail out and don't call debugger if exception.
  if (caught_exception) {
    return;
  }

  // Process debug event
  ProcessDebugEvent(v8::Break, event_data, auto_continue);
}


void Debugger::OnBeforeCompile(Handle<Script> script) {
  HandleScope scope;

  // Bail out based on state or if there is no listener for this event
  if (Debug::InDebugger()) return;
  if (compiling_natives()) return;
  if (!EventActive(v8::BeforeCompile)) return;

  // Enter the debugger.
  EnterDebugger debugger;
  if (debugger.FailedToEnter()) return;

  // Create the event data object.
  bool caught_exception = false;
  Handle<Object> event_data = MakeCompileEvent(script, true, &caught_exception);
  // Bail out and don't call debugger if exception.
  if (caught_exception) {
    return;
  }

  // Process debug event
  ProcessDebugEvent(v8::BeforeCompile, event_data, false);
}


// Handle debugger actions when a new script is compiled.
void Debugger::OnAfterCompile(Handle<Script> script, Handle<JSFunction> fun) {
  HandleScope scope;

  // No compile events while compiling natives.
  if (compiling_natives()) return;

  // No more to do if not debugging.
  if (!IsDebuggerActive()) return;

  // Store whether in debugger before entering debugger.
  bool in_debugger = Debug::InDebugger();

  // Enter the debugger.
  EnterDebugger debugger;
  if (debugger.FailedToEnter()) return;

  // If debugging there might be script break points registered for this
  // script. Make sure that these break points are set.

  // Get the function UpdateScriptBreakPoints (defined in debug-delay.js).
  Handle<Object> update_script_break_points =
      Handle<Object>(Debug::debug_context()->global()->GetProperty(
          *Factory::LookupAsciiSymbol("UpdateScriptBreakPoints")));
  if (!update_script_break_points->IsJSFunction()) {
    return;
  }
  ASSERT(update_script_break_points->IsJSFunction());

  // Wrap the script object in a proper JS object before passing it
  // to JavaScript.
  Handle<JSValue> wrapper = GetScriptWrapper(script);

  // Call UpdateScriptBreakPoints expect no exceptions.
  bool caught_exception = false;
  const int argc = 1;
  Object** argv[argc] = { reinterpret_cast<Object**>(wrapper.location()) };
  Handle<Object> result = Execution::TryCall(
      Handle<JSFunction>::cast(update_script_break_points),
      Top::builtins(), argc, argv,
      &caught_exception);
  if (caught_exception) {
    return;
  }
  // Bail out based on state or if there is no listener for this event
  if (in_debugger) return;
  if (!Debugger::EventActive(v8::AfterCompile)) return;

  // Create the compile state object.
  Handle<Object> event_data = MakeCompileEvent(script,
                                               false,
                                               &caught_exception);
  // Bail out and don't call debugger if exception.
  if (caught_exception) {
    return;
  }
  // Process debug event
  ProcessDebugEvent(v8::AfterCompile, event_data, false);
}


void Debugger::OnNewFunction(Handle<JSFunction> function) {
  return;
  HandleScope scope;

  // Bail out based on state or if there is no listener for this event
  if (Debug::InDebugger()) return;
  if (compiling_natives()) return;
  if (!Debugger::EventActive(v8::NewFunction)) return;

  // Enter the debugger.
  EnterDebugger debugger;
  if (debugger.FailedToEnter()) return;

  // Create the event object.
  bool caught_exception = false;
  Handle<Object> event_data = MakeNewFunctionEvent(function, &caught_exception);
  // Bail out and don't call debugger if exception.
  if (caught_exception) {
    return;
  }
  // Process debug event.
  ProcessDebugEvent(v8::NewFunction, event_data, false);
}


void Debugger::ProcessDebugEvent(v8::DebugEvent event,
                                 Handle<Object> event_data,
                                 bool auto_continue) {
  HandleScope scope;

  // Create the execution state.
  bool caught_exception = false;
  Handle<Object> exec_state = MakeExecutionState(&caught_exception);
  if (caught_exception) {
    return;
  }
  // First notify the message handler if any.
  if (message_handler_ != NULL) {
    NotifyMessageHandler(event, exec_state, event_data, auto_continue);
  }
  // Notify registered debug event listener. This can be either a C or a
  // JavaScript function.
  if (!event_listener_.is_null()) {
    if (event_listener_->IsProxy()) {
      // C debug event listener.
      Handle<Proxy> callback_obj(Handle<Proxy>::cast(event_listener_));
      v8::DebugEventCallback callback =
            FUNCTION_CAST<v8::DebugEventCallback>(callback_obj->proxy());
      callback(event,
               v8::Utils::ToLocal(Handle<JSObject>::cast(exec_state)),
               v8::Utils::ToLocal(Handle<JSObject>::cast(event_data)),
               v8::Utils::ToLocal(Handle<Object>::cast(event_listener_data_)));
    } else {
      // JavaScript debug event listener.
      ASSERT(event_listener_->IsJSFunction());
      Handle<JSFunction> fun(Handle<JSFunction>::cast(event_listener_));

      // Invoke the JavaScript debug event listener.
      const int argc = 4;
      Object** argv[argc] = { Handle<Object>(Smi::FromInt(event)).location(),
                              exec_state.location(),
                              event_data.location(),
                              event_listener_data_.location() };
      Handle<Object> result = Execution::TryCall(fun, Top::global(),
                                                 argc, argv, &caught_exception);
      if (caught_exception) {
        // Silently ignore exceptions from debug event listeners.
      }
    }
  }

  // Clear the mirror cache.
  Debug::ClearMirrorCache();
}


void Debugger::UnloadDebugger() {
  // Make sure that there are no breakpoints left.
  Debug::ClearAllBreakPoints();

  // Unload the debugger if feasible.
  if (!never_unload_debugger_) {
    Debug::Unload();
  }

  // Clear the flag indicating that the message handler was recently cleared.
  message_handler_cleared_ = false;
}


void Debugger::NotifyMessageHandler(v8::DebugEvent event,
                                    Handle<Object> exec_state,
                                    Handle<Object> event_data,
                                    bool auto_continue) {
  HandleScope scope;

  if (!Debug::Load()) return;

  // Process the individual events.
  bool interactive = false;
  switch (event) {
    case v8::Break:
      interactive = true;  // Break event is always interactive
      break;
    case v8::Exception:
      interactive = true;  // Exception event is always interactive
      break;
    case v8::BeforeCompile:
      break;
    case v8::AfterCompile:
      break;
    case v8::NewFunction:
      break;
    default:
      UNREACHABLE();
  }

  // Done if not interactive.
  if (!interactive) return;

  // Get the DebugCommandProcessor.
  v8::Local<v8::Object> api_exec_state =
      v8::Utils::ToLocal(Handle<JSObject>::cast(exec_state));
  v8::Local<v8::String> fun_name =
      v8::String::New("debugCommandProcessor");
  v8::Local<v8::Function> fun =
      v8::Function::Cast(*api_exec_state->Get(fun_name));
  v8::TryCatch try_catch;
  v8::Local<v8::Object> cmd_processor =
      v8::Object::Cast(*fun->Call(api_exec_state, 0, NULL));
  if (try_catch.HasCaught()) {
    PrintLn(try_catch.Exception());
    return;
  }

  // Notify the debugger that a debug event has occurred unless auto continue is
  // active in which case no event is send.
  if (!auto_continue) {
    bool success = SendEventMessage(event_data);
    if (!success) {
      // If failed to notify debugger just continue running.
      return;
    }
  }

  // Process requests from the debugger.
  while (true) {
    // Wait for new command in the queue.
    command_received_->Wait();

    // The debug command interrupt flag might have been set when the command was
    // added.
    StackGuard::Continue(DEBUGCOMMAND);

    // Get the command from the queue.
    Vector<uint16_t> command = command_queue_.Get();
    Logger::DebugTag("Got request from command queue, in interactive loop.");
    if (!Debugger::IsDebuggerActive()) {
      return;
    }

    // Check if the command is a host dispatch.
    if (command[0] == 0) {
      if (Debugger::host_dispatch_handler_) {
        int32_t dispatch = (command[1] << 16) | command[2];
        Debugger::host_dispatch_handler_(reinterpret_cast<void*>(dispatch),
                                         Debugger::host_dispatch_handler_data_);
      }
      continue;
    }

    // Invoke JavaScript to process the debug request.
    v8::Local<v8::String> fun_name;
    v8::Local<v8::Function> fun;
    v8::Local<v8::Value> request;
    v8::TryCatch try_catch;
    fun_name = v8::String::New("processDebugRequest");
    fun = v8::Function::Cast(*cmd_processor->Get(fun_name));
    request = v8::String::New(reinterpret_cast<uint16_t*>(command.start()),
                              command.length());
    static const int kArgc = 1;
    v8::Handle<Value> argv[kArgc] = { request };
    v8::Local<v8::Value> response_val = fun->Call(cmd_processor, kArgc, argv);

    // Get the response.
    v8::Local<v8::String> response;
    bool running = false;
    if (!try_catch.HasCaught()) {
      // Get response string.
      if (!response_val->IsUndefined()) {
        response = v8::String::Cast(*response_val);
      } else {
        response = v8::String::New("");
      }

      // Log the JSON request/response.
      if (FLAG_trace_debug_json) {
        PrintLn(request);
        PrintLn(response);
      }

      // Get the running state.
      fun_name = v8::String::New("isRunning");
      fun = v8::Function::Cast(*cmd_processor->Get(fun_name));
      static const int kArgc = 1;
      v8::Handle<Value> argv[kArgc] = { response };
      v8::Local<v8::Value> running_val = fun->Call(cmd_processor, kArgc, argv);
      if (!try_catch.HasCaught()) {
        running = running_val->ToBoolean()->Value();
      }
    } else {
      // In case of failure the result text is the exception text.
      response = try_catch.Exception()->ToString();
    }

    // Convert text result to C string.
    v8::String::Value val(response);
    Vector<uint16_t> str(reinterpret_cast<uint16_t*>(*val),
                        response->Length());

    // Return the result.
    SendMessage(str);

    // Return from debug event processing if either the VM is put into the
    // runnning state (through a continue command) or auto continue is active
    // and there are no more commands queued.
    if (running || (auto_continue && !HasCommands())) {
      return;
    }
  }
}


void Debugger::SetEventListener(Handle<Object> callback,
                                Handle<Object> data) {
  HandleScope scope;

  // Clear the global handles for the event listener and the event listener data
  // object.
  if (!event_listener_.is_null()) {
    GlobalHandles::Destroy(
        reinterpret_cast<Object**>(event_listener_.location()));
    event_listener_ = Handle<Object>();
  }
  if (!event_listener_data_.is_null()) {
    GlobalHandles::Destroy(
        reinterpret_cast<Object**>(event_listener_data_.location()));
    event_listener_data_ = Handle<Object>();
  }

  // If there is a new debug event listener register it together with its data
  // object.
  if (!callback->IsUndefined() && !callback->IsNull()) {
    event_listener_ = Handle<Object>::cast(GlobalHandles::Create(*callback));
    if (data.is_null()) {
      data = Factory::undefined_value();
    }
    event_listener_data_ = Handle<Object>::cast(GlobalHandles::Create(*data));
  }

  // Unload the debugger if event listener cleared.
  if (callback->IsUndefined()) {
    UnloadDebugger();
  }
}


void Debugger::SetMessageHandler(v8::DebugMessageHandler handler, void* data,
                                 bool message_handler_thread) {
  ScopedLock with(debugger_access_);

  message_handler_ = handler;
  message_handler_data_ = data;
  if (handler != NULL) {
    if (!message_thread_ && message_handler_thread) {
      message_thread_ = new DebugMessageThread();
      message_thread_->Start();
    }
  } else {
    // Indicate that the message handler was recently cleared.
    message_handler_cleared_ = true;

    // Send an empty command to the debugger if in a break to make JavaScript
    // run again if the debugger is closed.
    if (Debug::InDebugger()) {
      ProcessCommand(Vector<const uint16_t>::empty());
    }
  }
}


void Debugger::SetHostDispatchHandler(v8::DebugHostDispatchHandler handler,
                                      void* data) {
  host_dispatch_handler_ = handler;
  host_dispatch_handler_data_ = data;
}


// Calls the registered debug message handler. This callback is part of the
// public API. Messages are kept internally as Vector<uint16_t> strings, which
// are allocated in various places and deallocated by the calling function
// sometime after this call.
void Debugger::InvokeMessageHandler(Vector<uint16_t> message) {
  ScopedLock with(debugger_access_);

  if (message_handler_ != NULL) {
    message_handler_(message.start(), message.length(), message_handler_data_);
  }
}


void Debugger::SendMessage(Vector<uint16_t> message) {
  if (message_thread_ == NULL) {
    // If there is no message thread just invoke the message handler from the
    // V8 thread.
    InvokeMessageHandler(message);
  } else {
    // Put a copy of the message coming from V8 on the queue. The new copy of
    // the event string is destroyed by the message thread.
    Vector<uint16_t> message_copy = message.Clone();
    Logger::DebugTag("Put message on event message_queue.");
    message_queue_.Put(message_copy);
    message_received_->Signal();
  }
}


bool Debugger::SendEventMessage(Handle<Object> event_data) {
  v8::HandleScope scope;
  // Call toJSONProtocol on the debug event object.
  v8::Local<v8::Object> api_event_data =
      v8::Utils::ToLocal(Handle<JSObject>::cast(event_data));
  v8::Local<v8::String> fun_name = v8::String::New("toJSONProtocol");
  v8::Local<v8::Function> fun =
      v8::Function::Cast(*api_event_data->Get(fun_name));
  v8::TryCatch try_catch;
  v8::Local<v8::Value> json_event = *fun->Call(api_event_data, 0, NULL);
  v8::Local<v8::String> json_event_string;
  if (!try_catch.HasCaught()) {
    if (!json_event->IsUndefined()) {
      json_event_string = json_event->ToString();
      if (FLAG_trace_debug_json) {
        PrintLn(json_event_string);
      }
      v8::String::Value val(json_event_string);
      Vector<uint16_t> str(reinterpret_cast<uint16_t*>(*val),
                           json_event_string->Length());
      SendMessage(str);
    } else {
      SendMessage(Vector<uint16_t>::empty());
    }
  } else {
    PrintLn(try_catch.Exception());
    return false;
  }
  return true;
}


// Puts a command coming from the public API on the queue.  Creates
// a copy of the command string managed by the debugger.  Up to this
// point, the command data was managed by the API client.  Called
// by the API client thread.  This is where the API client hands off
// processing of the command to the DebugMessageThread thread.
// The new copy of the command is destroyed in HandleCommand().
void Debugger::ProcessCommand(Vector<const uint16_t> command) {
  // Make a copy of the command. Need to cast away const for Clone to work.
  Vector<uint16_t> command_copy =
      Vector<uint16_t>(const_cast<uint16_t*>(command.start()),
                       command.length()).Clone();
  Logger::DebugTag("Put command on command_queue.");
  command_queue_.Put(command_copy);
  command_received_->Signal();
  if (!Debug::InDebugger()) {
    StackGuard::DebugCommand();
  }
}


bool Debugger::HasCommands() {
  return !command_queue_.IsEmpty();
}


void Debugger::ProcessHostDispatch(void* dispatch) {
// Puts a host dispatch comming from the public API on the queue.
  uint16_t hack[3];
  hack[0] = 0;
  hack[1] = reinterpret_cast<uint32_t>(dispatch) >> 16;
  hack[2] = reinterpret_cast<uint32_t>(dispatch) & 0xFFFF;
  Logger::DebugTag("Put dispatch on command_queue.");
  command_queue_.Put(Vector<uint16_t>(hack, 3).Clone());
  command_received_->Signal();
}


bool Debugger::IsDebuggerActive() {
  ScopedLock with(debugger_access_);

  return message_handler_ != NULL || !event_listener_.is_null();
}


Handle<Object> Debugger::Call(Handle<JSFunction> fun,
                              Handle<Object> data,
                              bool* pending_exception) {
  // When calling functions in the debugger prevent it from beeing unloaded.
  Debugger::never_unload_debugger_ = true;

  // Enter the debugger.
  EnterDebugger debugger;
  if (debugger.FailedToEnter() || !debugger.HasJavaScriptFrames()) {
    return Factory::undefined_value();
  }

  // Create the execution state.
  bool caught_exception = false;
  Handle<Object> exec_state = MakeExecutionState(&caught_exception);
  if (caught_exception) {
    return Factory::undefined_value();
  }

  static const int kArgc = 2;
  Object** argv[kArgc] = { exec_state.location(), data.location() };
  Handle<Object> result = Execution::Call(fun, Factory::undefined_value(),
                                          kArgc, argv, pending_exception);
  return result;
}


bool Debugger::StartAgent(const char* name, int port) {
  if (Socket::Setup()) {
    agent_ = new DebuggerAgent(name, port);
    agent_->Start();
    return true;
  }

  return false;
}


void Debugger::StopAgent() {
  if (agent_ != NULL) {
    agent_->Shutdown();
    agent_->Join();
    delete agent_;
    agent_ = NULL;
  }
}


void Debugger::TearDown() {
  if (message_thread_ != NULL) {
    message_thread_->Stop();
    delete message_thread_;
    message_thread_ = NULL;
  }
}


void DebugMessageThread::Run() {
  // Sends debug events to an installed debugger message callback.
  while (keep_running_) {
    // Wait and Get are paired so that semaphore count equals queue length.
    Debugger::message_received_->Wait();
    Logger::DebugTag("Get message from event message_queue.");
    Vector<uint16_t> message = Debugger::message_queue_.Get();
    if (message.length() > 0) {
      Debugger::InvokeMessageHandler(message);
    }
  }
}


void DebugMessageThread::Stop() {
  keep_running_ = false;
  Debugger::SendMessage(Vector<uint16_t>(NULL, 0));
  Join();
}


MessageQueue::MessageQueue(int size) : start_(0), end_(0), size_(size) {
  messages_ = NewArray<Vector<uint16_t> >(size);
}


MessageQueue::~MessageQueue() {
  DeleteArray(messages_);
}


Vector<uint16_t> MessageQueue::Get() {
  ASSERT(!IsEmpty());
  int result = start_;
  start_ = (start_ + 1) % size_;
  return messages_[result];
}


void MessageQueue::Put(const Vector<uint16_t>& message) {
  if ((end_ + 1) % size_ == start_) {
    Expand();
  }
  messages_[end_] = message;
  end_ = (end_ + 1) % size_;
}


void MessageQueue::Expand() {
  MessageQueue new_queue(size_ * 2);
  while (!IsEmpty()) {
    new_queue.Put(Get());
  }
  Vector<uint16_t>* array_to_free = messages_;
  *this = new_queue;
  new_queue.messages_ = array_to_free;
  // Automatic destructor called on new_queue, freeing array_to_free.
}


LockingMessageQueue::LockingMessageQueue(int size) : queue_(size) {
  lock_ = OS::CreateMutex();
}


LockingMessageQueue::~LockingMessageQueue() {
  delete lock_;
}


bool LockingMessageQueue::IsEmpty() const {
  ScopedLock sl(lock_);
  return queue_.IsEmpty();
}


Vector<uint16_t> LockingMessageQueue::Get() {
  ScopedLock sl(lock_);
  Vector<uint16_t> result = queue_.Get();
  Logger::DebugEvent("Get", result);
  return result;
}


void LockingMessageQueue::Put(const Vector<uint16_t>& message) {
  ScopedLock sl(lock_);
  queue_.Put(message);
  Logger::DebugEvent("Put", message);
}


void LockingMessageQueue::Clear() {
  ScopedLock sl(lock_);
  queue_.Clear();
}


} }  // namespace v8::internal
