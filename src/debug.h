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

#ifndef V8_V8_DEBUG_H_
#define V8_V8_DEBUG_H_

#include "../include/v8-debug.h"
#include "assembler.h"
#include "code-stubs.h"
#include "debug-agent.h"
#include "execution.h"
#include "factory.h"
#include "platform.h"
#include "string-stream.h"
#include "v8threads.h"


namespace v8 { namespace internal {


// Forward declarations.
class EnterDebugger;


// Step actions. NOTE: These values are in macros.py as well.
enum StepAction {
  StepNone = -1,  // Stepping not prepared.
  StepOut = 0,   // Step out of the current function.
  StepNext = 1,  // Step to the next statement in the current function.
  StepIn = 2,    // Step into new functions invoked or the next statement
                 // in the current function.
  StepMin = 3,   // Perform a minimum step in the current function.
  StepInMin = 4  // Step into new functions invoked or perform a minimum step
                 // in the current function.
};


// Type of exception break. NOTE: These values are in macros.py as well.
enum ExceptionBreakType {
  BreakException = 0,
  BreakUncaughtException = 1
};


// Type of exception break. NOTE: These values are in macros.py as well.
enum BreakLocatorType {
  ALL_BREAK_LOCATIONS = 0,
  SOURCE_BREAK_LOCATIONS = 1
};


// Class for iterating through the break points in a function and changing
// them.
class BreakLocationIterator {
 public:
  explicit BreakLocationIterator(Handle<DebugInfo> debug_info,
                                 BreakLocatorType type);
  virtual ~BreakLocationIterator();

  void Next();
  void Next(int count);
  void FindBreakLocationFromAddress(Address pc);
  void FindBreakLocationFromPosition(int position);
  void Reset();
  bool Done() const;
  void SetBreakPoint(Handle<Object> break_point_object);
  void ClearBreakPoint(Handle<Object> break_point_object);
  void SetOneShot();
  void ClearOneShot();
  void PrepareStepIn();
  bool IsExit() const;
  bool HasBreakPoint();
  bool IsDebugBreak();
  Object* BreakPointObjects();
  void ClearAllDebugBreak();


  inline int code_position() { return pc() - debug_info_->code()->entry(); }
  inline int break_point() { return break_point_; }
  inline int position() { return position_; }
  inline int statement_position() { return statement_position_; }
  inline Address pc() { return reloc_iterator_->rinfo()->pc(); }
  inline Code* code() { return debug_info_->code(); }
  inline RelocInfo* rinfo() { return reloc_iterator_->rinfo(); }
  inline RelocInfo::Mode rmode() const {
    return reloc_iterator_->rinfo()->rmode();
  }
  inline RelocInfo* original_rinfo() {
    return reloc_iterator_original_->rinfo();
  }
  inline RelocInfo::Mode original_rmode() const {
    return reloc_iterator_original_->rinfo()->rmode();
  }

 protected:
  bool RinfoDone() const;
  void RinfoNext();

  BreakLocatorType type_;
  int break_point_;
  int position_;
  int statement_position_;
  Handle<DebugInfo> debug_info_;
  RelocIterator* reloc_iterator_;
  RelocIterator* reloc_iterator_original_;

 private:
  void SetDebugBreak();
  void ClearDebugBreak();
  bool IsDebugBreakAtReturn();
  void SetDebugBreakAtReturn();
  void ClearDebugBreakAtReturn();

  DISALLOW_COPY_AND_ASSIGN(BreakLocationIterator);
};


// Linked list holding debug info objects. The debug info objects are kept as
// weak handles to avoid a debug info object to keep a function alive.
class DebugInfoListNode {
 public:
  explicit DebugInfoListNode(DebugInfo* debug_info);
  virtual ~DebugInfoListNode();

  DebugInfoListNode* next() { return next_; }
  void set_next(DebugInfoListNode* next) { next_ = next; }
  Handle<DebugInfo> debug_info() { return debug_info_; }

 private:
  // Global (weak) handle to the debug info object.
  Handle<DebugInfo> debug_info_;

  // Next pointer for linked list.
  DebugInfoListNode* next_;
};


// This class contains the debugger support. The main purpose is to handle
// setting break points in the code.
//
// This class controls the debug info for all functions which currently have
// active breakpoints in them. This debug info is held in the heap root object
// debug_info which is a FixedArray. Each entry in this list is of class
// DebugInfo.
class Debug {
 public:
  static void Setup(bool create_heap_objects);
  static bool Load();
  static void Unload();
  static bool IsLoaded() { return !debug_context_.is_null(); }
  static bool InDebugger() { return thread_local_.debugger_entry_ != NULL; }
  static void PreemptionWhileInDebugger();
  static void Iterate(ObjectVisitor* v);

  static Object* Break(Arguments args);
  static void SetBreakPoint(Handle<SharedFunctionInfo> shared,
                            int source_position,
                            Handle<Object> break_point_object);
  static void ClearBreakPoint(Handle<Object> break_point_object);
  static void ClearAllBreakPoints();
  static void FloodWithOneShot(Handle<SharedFunctionInfo> shared);
  static void FloodHandlerWithOneShot();
  static void ChangeBreakOnException(ExceptionBreakType type, bool enable);
  static void PrepareStep(StepAction step_action, int step_count);
  static void ClearStepping();
  static bool StepNextContinue(BreakLocationIterator* break_location_iterator,
                               JavaScriptFrame* frame);
  static Handle<DebugInfo> GetDebugInfo(Handle<SharedFunctionInfo> shared);
  static bool HasDebugInfo(Handle<SharedFunctionInfo> shared);

  // Returns whether the operation succeeded.
  static bool EnsureDebugInfo(Handle<SharedFunctionInfo> shared);

  static bool IsDebugBreak(Address addr);
  static bool IsDebugBreakAtReturn(RelocInfo* rinfo);

  // Check whether a code stub with the specified major key is a possible break
  // point location.
  static bool IsSourceBreakStub(Code* code);
  static bool IsBreakStub(Code* code);

  // Find the builtin to use for invoking the debug break
  static Handle<Code> FindDebugBreak(RelocInfo* rinfo);

  static Handle<Object> GetSourceBreakLocations(
      Handle<SharedFunctionInfo> shared);

  // Getter for the debug_context.
  inline static Handle<Context> debug_context() { return debug_context_; }

  // Check whether a global object is the debug global object.
  static bool IsDebugGlobal(GlobalObject* global);

  // Fast check to see if any break points are active.
  inline static bool has_break_points() { return has_break_points_; }

  static void NewBreak(StackFrame::Id break_frame_id);
  static void SetBreak(StackFrame::Id break_frame_id, int break_id);
  static StackFrame::Id break_frame_id() {
    return thread_local_.break_frame_id_;
  }
  static int break_id() { return thread_local_.break_id_; }




  static bool StepInActive() { return thread_local_.step_into_fp_ != 0; }
  static void HandleStepIn(Handle<JSFunction> function,
                           Address fp,
                           bool is_constructor);
  static Address step_in_fp() { return thread_local_.step_into_fp_; }
  static Address* step_in_fp_addr() { return &thread_local_.step_into_fp_; }

  static EnterDebugger* debugger_entry() {
    return thread_local_.debugger_entry_;
  }
  static void set_debugger_entry(EnterDebugger* entry) {
    thread_local_.debugger_entry_ = entry;
  }

  static bool preemption_pending() {
    return thread_local_.preemption_pending_;
  }
  static void set_preemption_pending(bool preemption_pending) {
    thread_local_.preemption_pending_ = preemption_pending;
  }

  // Getter and setter for the disable break state.
  static bool disable_break() { return disable_break_; }
  static void set_disable_break(bool disable_break) {
    disable_break_ = disable_break;
  }

  // Getters for the current exception break state.
  static bool break_on_exception() { return break_on_exception_; }
  static bool break_on_uncaught_exception() {
    return break_on_uncaught_exception_;
  }

  enum AddressId {
    k_after_break_target_address,
    k_debug_break_return_address,
    k_register_address
  };

  // Support for setting the address to jump to when returning from break point.
  static Address* after_break_target_address() {
    return reinterpret_cast<Address*>(&thread_local_.after_break_target_);
  }

  // Support for saving/restoring registers when handling debug break calls.
  static Object** register_address(int r) {
    return &registers_[r];
  }

  // Address of the debug break return entry code.
  static Code* debug_break_return_entry() { return debug_break_return_entry_; }

  // Support for getting the address of the debug break on return code.
  static Code** debug_break_return_address() {
    return &debug_break_return_;
  }

  static const int kEstimatedNofDebugInfoEntries = 16;
  static const int kEstimatedNofBreakPointsInFunction = 16;

  static void HandleWeakDebugInfo(v8::Persistent<v8::Value> obj, void* data);

  friend class Debugger;
  friend Handle<FixedArray> GetDebuggedFunctions();  // In test-debug.cc
  friend void CheckDebuggerUnloaded(bool check_functions);  // In test-debug.cc

  // Threading support.
  static char* ArchiveDebug(char* to);
  static char* RestoreDebug(char* from);
  static int ArchiveSpacePerThread();

  // Mirror cache handling.
  static void ClearMirrorCache();

  // Code generation assumptions.
  static const int kIa32CallInstructionLength = 5;
  static const int kIa32JSReturnSequenceLength = 6;

  // Code generator routines.
  static void GenerateLoadICDebugBreak(MacroAssembler* masm);
  static void GenerateStoreICDebugBreak(MacroAssembler* masm);
  static void GenerateKeyedLoadICDebugBreak(MacroAssembler* masm);
  static void GenerateKeyedStoreICDebugBreak(MacroAssembler* masm);
  static void GenerateConstructCallDebugBreak(MacroAssembler* masm);
  static void GenerateReturnDebugBreak(MacroAssembler* masm);
  static void GenerateReturnDebugBreakEntry(MacroAssembler* masm);
  static void GenerateStubNoRegistersDebugBreak(MacroAssembler* masm);

  // Called from stub-cache.cc.
  static void GenerateCallICDebugBreak(MacroAssembler* masm);

 private:
  static bool CompileDebuggerScript(int index);
  static void ClearOneShot();
  static void ActivateStepIn(StackFrame* frame);
  static void ClearStepIn();
  static void ClearStepNext();
  // Returns whether the compile succeeded.
  static bool EnsureCompiled(Handle<SharedFunctionInfo> shared);
  static void RemoveDebugInfo(Handle<DebugInfo> debug_info);
  static void SetAfterBreakTarget(JavaScriptFrame* frame);
  static Handle<Object> CheckBreakPoints(Handle<Object> break_point);
  static bool CheckBreakPoint(Handle<Object> break_point_object);

  // Global handle to debug context where all the debugger JavaScript code is
  // loaded.
  static Handle<Context> debug_context_;

  // Boolean state indicating whether any break points are set.
  static bool has_break_points_;
  static DebugInfoListNode* debug_info_list_;

  static bool disable_break_;
  static bool break_on_exception_;
  static bool break_on_uncaught_exception_;

  // Per-thread data.
  class ThreadLocal {
   public:
    // Counter for generating next break id.
    int break_count_;

    // Current break id.
    int break_id_;

    // Frame id for the frame of the current break.
    StackFrame::Id break_frame_id_;

    // Step action for last step performed.
    StepAction last_step_action_;

    // Source statement position from last step next action.
    int last_statement_position_;

    // Number of steps left to perform before debug event.
    int step_count_;

    // Frame pointer from last step next action.
    Address last_fp_;

    // Frame pointer for frame from which step in was performed.
    Address step_into_fp_;

    // Storage location for jump when exiting debug break calls.
    Address after_break_target_;

    // Top debugger entry.
    EnterDebugger* debugger_entry_;

    // Preemption happened while debugging.
    bool preemption_pending_;
  };

  // Storage location for registers when handling debug break calls
  static JSCallerSavedBuffer registers_;
  static ThreadLocal thread_local_;
  static void ThreadInit();

  // Code object for debug break return entry code.
  static Code* debug_break_return_entry_;

  // Code to call for handling debug break on return.
  static Code* debug_break_return_;

  DISALLOW_COPY_AND_ASSIGN(Debug);
};


class DebugMessageThread;

class Debugger {
 public:
  static void DebugRequest(const uint16_t* json_request, int length);

  static Handle<Object> MakeJSObject(Vector<const char> constructor_name,
                                     int argc, Object*** argv,
                                     bool* caught_exception);
  static Handle<Object> MakeExecutionState(bool* caught_exception);
  static Handle<Object> MakeBreakEvent(Handle<Object> exec_state,
                                       Handle<Object> break_points_hit,
                                       bool* caught_exception);
  static Handle<Object> MakeExceptionEvent(Handle<Object> exec_state,
                                           Handle<Object> exception,
                                           bool uncaught,
                                           bool* caught_exception);
  static Handle<Object> MakeNewFunctionEvent(Handle<Object> func,
                                             bool* caught_exception);
  static Handle<Object> MakeCompileEvent(Handle<Script> script,
                                         bool before,
                                         bool* caught_exception);
  static void OnDebugBreak(Handle<Object> break_points_hit, bool auto_continue);
  static void OnException(Handle<Object> exception, bool uncaught);
  static void OnBeforeCompile(Handle<Script> script);
  static void OnAfterCompile(Handle<Script> script,
                           Handle<JSFunction> fun);
  static void OnNewFunction(Handle<JSFunction> fun);
  static void ProcessDebugEvent(v8::DebugEvent event,
                                Handle<Object> event_data,
                                bool auto_continue);
  static void SetEventListener(Handle<Object> callback, Handle<Object> data);
  static void SetMessageHandler(v8::DebugMessageHandler handler, void* data);
  static void SetHostDispatchHandler(v8::DebugHostDispatchHandler handler,
                                     void* data);
  static void SendMessage(Vector<uint16_t> message);
  static void ProcessCommand(Vector<const uint16_t> command);
  static bool HasCommands();
  static void ProcessHostDispatch(void* dispatch);
  static void UpdateActiveDebugger();
  static Handle<Object> Call(Handle<JSFunction> fun,
                             Handle<Object> data,
                             bool* pending_exception);

  // Start the debugger agent listening on the provided port.
  static bool StartAgent(const char* name, int port);

  // Stop the debugger agent.
  static void StopAgent();

  inline static bool EventActive(v8::DebugEvent event) {
    // Currently argument event is not used.
    return !Debugger::compiling_natives_ && Debugger::debugger_active_;
  }

  static void set_debugger_active(bool debugger_active) {
    Debugger::debugger_active_ = debugger_active;
  }
  static bool debugger_active() { return Debugger::debugger_active_; }
  static void set_compiling_natives(bool compiling_natives) {
    Debugger::compiling_natives_ = compiling_natives;
  }
  static bool compiling_natives() { return Debugger::compiling_natives_; }
  static void set_loading_debugger(bool v) { is_loading_debugger_ = v; }
  static bool is_loading_debugger() { return Debugger::is_loading_debugger_; }

 private:
  static Handle<Object> event_listener_;  // Global handle to listener
  static Handle<Object> event_listener_data_;
  static bool debugger_active_;  // Are there any active debugger?
  static bool compiling_natives_;  // Are we compiling natives?
  static bool is_loading_debugger_;  // Are we loading the debugger?
  static DebugMessageThread* message_thread_;
  static v8::DebugMessageHandler message_handler_;
  static void* message_handler_data_;
  static v8::DebugHostDispatchHandler host_dispatch_handler_;
  static void* host_dispatch_handler_data_;

  static DebuggerAgent* agent_;

  friend class DebugMessageThread;
};


// A Queue of Vector<uint16_t> objects.  A thread-safe version is
// LockingMessageQueue, based on this class.
class MessageQueue BASE_EMBEDDED {
 public:
  explicit MessageQueue(int size);
  ~MessageQueue();
  bool IsEmpty() const { return start_ == end_; }
  Vector<uint16_t> Get();
  void Put(const Vector<uint16_t>& message);
  void Clear() { start_ = end_ = 0; }  // Queue is empty after Clear().
 private:
  // Doubles the size of the message queue, and copies the messages.
  void Expand();

  Vector<uint16_t>* messages_;
  int start_;
  int end_;
  int size_;  // The size of the queue buffer.  Queue can hold size-1 messages.
};


// LockingMessageQueue is a thread-safe circular buffer of Vector<uint16_t>
// messages.  The message data is not managed by LockingMessageQueue.
// Pointers to the data are passed in and out. Implemented by adding a
// Mutex to MessageQueue.  Includes logging of all puts and gets.
class LockingMessageQueue BASE_EMBEDDED {
 public:
  explicit LockingMessageQueue(int size);
  ~LockingMessageQueue();
  bool IsEmpty() const;
  Vector<uint16_t> Get();
  void Put(const Vector<uint16_t>& message);
  void Clear();
 private:
  MessageQueue queue_;
  Mutex* lock_;
  DISALLOW_COPY_AND_ASSIGN(LockingMessageQueue);
};


/* This class is the data for a running thread that serializes
 * event messages and command processing for the debugger.
 * All uncommented methods are called only from this message thread.
 */
class DebugMessageThread: public Thread {
 public:
  DebugMessageThread();  // Called from API thread.
  virtual ~DebugMessageThread();  // Never called.
  // Called by V8 thread.  Reports events from V8 VM.
  // Also handles command processing in stopped state of V8,
  // when host_running_ is false.
  void DebugEvent(v8::DebugEvent,
                  Handle<Object> exec_state,
                  Handle<Object> event_data,
                  bool auto_continue);
  // Puts event on the output queue.  Called by V8.
  // This is where V8 hands off
  // processing of the event to the DebugMessageThread thread,
  // which forwards it to the debug_message_handler set by the API.
  void SendMessage(Vector<uint16_t> event_json);
  // Formats an event into JSON, and calls SendMessage.
  bool SetEventJSONFromEvent(Handle<Object> event_data);
  // Puts a command coming from the public API on the queue.  Called
  // by the API client thread.  This is where the API client hands off
  // processing of the command to the DebugMessageThread thread.
  void ProcessCommand(Vector<uint16_t> command);
  void ProcessHostDispatch(void* dispatch);
  void OnDebuggerInactive();

  // Main function of DebugMessageThread thread.
  void Run();

  // Check whether there are commands in the queue.
  bool HasCommands() { return !command_queue_.IsEmpty(); }

  bool host_running_;  // Is the debugging host running or stopped?
  Semaphore* command_received_;  // Non-zero when command queue is non-empty.
  Semaphore* message_received_;  // Exactly equal to message queue length.
 private:
  bool TwoByteEqualsAscii(Vector<uint16_t> two_byte, const char* ascii);

  static const int kQueueInitialSize = 4;
  LockingMessageQueue command_queue_;
  LockingMessageQueue message_queue_;
  DISALLOW_COPY_AND_ASSIGN(DebugMessageThread);
};


// This class is used for entering the debugger. Create an instance in the stack
// to enter the debugger. This will set the current break state, make sure the
// debugger is loaded and switch to the debugger context. If the debugger for
// some reason could not be entered FailedToEnter will return true.
class EnterDebugger BASE_EMBEDDED {
 public:
  EnterDebugger()
      : prev_(Debug::debugger_entry()),
        has_js_frames_(!it_.done()) {
    ASSERT(prev_ == NULL ? !Debug::preemption_pending() : true);

    // Link recursive debugger entry.
    Debug::set_debugger_entry(this);

    // Store the previous break id and frame id.
    break_id_ = Debug::break_id();
    break_frame_id_ = Debug::break_frame_id();

    // Create the new break info. If there is no JavaScript frames there is no
    // break frame id.
    if (has_js_frames_) {
      Debug::NewBreak(it_.frame()->id());
    } else {
      Debug::NewBreak(StackFrame::NO_ID);
    }

    // Make sure that debugger is loaded and enter the debugger context.
    load_failed_ = !Debug::Load();
    if (!load_failed_) {
      // NOTE the member variable save which saves the previous context before
      // this change.
      Top::set_context(*Debug::debug_context());
    }
  }

  ~EnterDebugger() {
    // Restore to the previous break state.
    Debug::SetBreak(break_frame_id_, break_id_);

    // Request preemption when leaving the last debugger entry and a preemption
    // had been recorded while debugging. This is to avoid starvation in some
    // debugging scenarios.
    if (prev_ == NULL && Debug::preemption_pending()) {
      StackGuard::Preempt();
      Debug::set_preemption_pending(false);
    }

    // If there are commands in the queue when leaving the debugger request that
    // these commands are processed.
    if (prev_ == NULL && Debugger::HasCommands()) {
      StackGuard::DebugCommand();
    }

    // Leaving this debugger entry.
    Debug::set_debugger_entry(prev_);
  }

  // Check whether the debugger could be entered.
  inline bool FailedToEnter() { return load_failed_; }

  // Check whether there are any JavaScript frames on the stack.
  inline bool HasJavaScriptFrames() { return has_js_frames_; }

 private:
  EnterDebugger* prev_;  // Previous debugger entry if entered recursively.
  JavaScriptFrameIterator it_;
  const bool has_js_frames_;  // Were there any JavaScript frames?
  StackFrame::Id break_frame_id_;  // Previous break frame id.
  int break_id_;  // Previous break id.
  bool load_failed_;  // Did the debugger fail to load?
  SaveContext save_;  // Saves previous context.
};


// Stack allocated class for disabling break.
class DisableBreak BASE_EMBEDDED {
 public:
  // Enter the debugger by storing the previous top context and setting the
  // current top context to the debugger context.
  explicit DisableBreak(bool disable_break)  {
    prev_disable_break_ = Debug::disable_break();
    Debug::set_disable_break(disable_break);
  }
  ~DisableBreak() {
    Debug::set_disable_break(prev_disable_break_);
  }

 private:
  // The previous state of the disable break used to restore the value when this
  // object is destructed.
  bool prev_disable_break_;
};


// Debug_Address encapsulates the Address pointers used in generating debug
// code.
class Debug_Address {
 public:
  Debug_Address(Debug::AddressId id, int reg = 0)
    : id_(id), reg_(reg) {
    ASSERT(reg == 0 || id == Debug::k_register_address);
  }

  static Debug_Address AfterBreakTarget() {
    return Debug_Address(Debug::k_after_break_target_address);
  }

  static Debug_Address DebugBreakReturn() {
    return Debug_Address(Debug::k_debug_break_return_address);
  }

  static Debug_Address Register(int reg) {
    return Debug_Address(Debug::k_register_address, reg);
  }

  Address address() const {
    switch (id_) {
      case Debug::k_after_break_target_address:
        return reinterpret_cast<Address>(Debug::after_break_target_address());
      case Debug::k_debug_break_return_address:
        return reinterpret_cast<Address>(Debug::debug_break_return_address());
      case Debug::k_register_address:
        return reinterpret_cast<Address>(Debug::register_address(reg_));
      default:
        UNREACHABLE();
        return NULL;
    }
  }
 private:
  Debug::AddressId id_;
  int reg_;
};


} }  // namespace v8::internal

#endif  // V8_V8_DEBUG_H_
