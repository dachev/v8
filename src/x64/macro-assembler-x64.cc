// Copyright 2009 the V8 project authors. All rights reserved.
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
#include "assembler-x64.h"
#include "macro-assembler-x64.h"
#include "serialize.h"
#include "debug.h"

namespace v8 {
namespace internal {

MacroAssembler::MacroAssembler(void* buffer, int size)
  : Assembler(buffer, size),
    unresolved_(0),
    generating_stub_(false),
    allow_stub_calls_(true),
    code_object_(Heap::undefined_value()) {
}



static void RecordWriteHelper(MacroAssembler* masm,
                              Register object,
                              Register addr,
                              Register scratch) {
  Label fast;

  // Compute the page start address from the heap object pointer, and reuse
  // the 'object' register for it.
  ASSERT(is_int32(~Page::kPageAlignmentMask));
  masm->and_(object,
             Immediate(static_cast<int32_t>(~Page::kPageAlignmentMask)));
  Register page_start = object;

  // Compute the bit addr in the remembered set/index of the pointer in the
  // page. Reuse 'addr' as pointer_offset.
  masm->subq(addr, page_start);
  masm->shr(addr, Immediate(kPointerSizeLog2));
  Register pointer_offset = addr;

  // If the bit offset lies beyond the normal remembered set range, it is in
  // the extra remembered set area of a large object.
  masm->cmpq(pointer_offset, Immediate(Page::kPageSize / kPointerSize));
  masm->j(less, &fast);

  // Adjust 'page_start' so that addressing using 'pointer_offset' hits the
  // extra remembered set after the large object.

  // Load the array length into 'scratch'.
  masm->movl(scratch,
             Operand(page_start,
                     Page::kObjectStartOffset + FixedArray::kLengthOffset));
  Register array_length = scratch;

  // Extra remembered set starts right after the large object (a FixedArray), at
  //   page_start + kObjectStartOffset + objectSize
  // where objectSize is FixedArray::kHeaderSize + kPointerSize * array_length.
  // Add the delta between the end of the normal RSet and the start of the
  // extra RSet to 'page_start', so that addressing the bit using
  // 'pointer_offset' hits the extra RSet words.
  masm->lea(page_start,
            Operand(page_start, array_length, times_pointer_size,
                    Page::kObjectStartOffset + FixedArray::kHeaderSize
                        - Page::kRSetEndOffset));

  // NOTE: For now, we use the bit-test-and-set (bts) x86 instruction
  // to limit code size. We should probably evaluate this decision by
  // measuring the performance of an equivalent implementation using
  // "simpler" instructions
  masm->bind(&fast);
  masm->bts(Operand(page_start, Page::kRSetOffset), pointer_offset);
}


class RecordWriteStub : public CodeStub {
 public:
  RecordWriteStub(Register object, Register addr, Register scratch)
      : object_(object), addr_(addr), scratch_(scratch) { }

  void Generate(MacroAssembler* masm);

 private:
  Register object_;
  Register addr_;
  Register scratch_;

#ifdef DEBUG
  void Print() {
    PrintF("RecordWriteStub (object reg %d), (addr reg %d), (scratch reg %d)\n",
           object_.code(), addr_.code(), scratch_.code());
  }
#endif

  // Minor key encoding in 12 bits of three registers (object, address and
  // scratch) OOOOAAAASSSS.
  class ScratchBits: public BitField<uint32_t, 0, 4> {};
  class AddressBits: public BitField<uint32_t, 4, 4> {};
  class ObjectBits: public BitField<uint32_t, 8, 4> {};

  Major MajorKey() { return RecordWrite; }

  int MinorKey() {
    // Encode the registers.
    return ObjectBits::encode(object_.code()) |
           AddressBits::encode(addr_.code()) |
           ScratchBits::encode(scratch_.code());
  }
};


void RecordWriteStub::Generate(MacroAssembler* masm) {
  RecordWriteHelper(masm, object_, addr_, scratch_);
  masm->ret(0);
}


// Set the remembered set bit for [object+offset].
// object is the object being stored into, value is the object being stored.
// If offset is zero, then the scratch register contains the array index into
// the elements array represented as a Smi.
// All registers are clobbered by the operation.
void MacroAssembler::RecordWrite(Register object,
                                 int offset,
                                 Register value,
                                 Register scratch) {
  // First, check if a remembered set write is even needed. The tests below
  // catch stores of Smis and stores into young gen (which does not have space
  // for the remembered set bits.
  Label done;

  // Test that the object address is not in the new space.  We cannot
  // set remembered set bits in the new space.
  movq(value, object);
  ASSERT(is_int32(static_cast<int64_t>(Heap::NewSpaceMask())));
  and_(value, Immediate(static_cast<int32_t>(Heap::NewSpaceMask())));
  movq(kScratchRegister, ExternalReference::new_space_start());
  cmpq(value, kScratchRegister);
  j(equal, &done);

  if ((offset > 0) && (offset < Page::kMaxHeapObjectSize)) {
    // Compute the bit offset in the remembered set, leave it in 'value'.
    lea(value, Operand(object, offset));
    ASSERT(is_int32(Page::kPageAlignmentMask));
    and_(value, Immediate(static_cast<int32_t>(Page::kPageAlignmentMask)));
    shr(value, Immediate(kObjectAlignmentBits));

    // Compute the page address from the heap object pointer, leave it in
    // 'object' (immediate value is sign extended).
    and_(object, Immediate(~Page::kPageAlignmentMask));

    // NOTE: For now, we use the bit-test-and-set (bts) x86 instruction
    // to limit code size. We should probably evaluate this decision by
    // measuring the performance of an equivalent implementation using
    // "simpler" instructions
    bts(Operand(object, Page::kRSetOffset), value);
  } else {
    Register dst = scratch;
    if (offset != 0) {
      lea(dst, Operand(object, offset));
    } else {
      // array access: calculate the destination address in the same manner as
      // KeyedStoreIC::GenerateGeneric.  Multiply a smi by 4 to get an offset
      // into an array of pointers.
      lea(dst, Operand(object, dst, times_half_pointer_size,
                       FixedArray::kHeaderSize - kHeapObjectTag));
    }
    // If we are already generating a shared stub, not inlining the
    // record write code isn't going to save us any memory.
    if (generating_stub()) {
      RecordWriteHelper(this, object, dst, value);
    } else {
      RecordWriteStub stub(object, dst, value);
      CallStub(&stub);
    }
  }

  bind(&done);
}


void MacroAssembler::Assert(Condition cc, const char* msg) {
  if (FLAG_debug_code) Check(cc, msg);
}


void MacroAssembler::Check(Condition cc, const char* msg) {
  Label L;
  j(cc, &L);
  Abort(msg);
  // will not return here
  bind(&L);
}


void MacroAssembler::NegativeZeroTest(Register result,
                                      Register op,
                                      Label* then_label) {
  Label ok;
  testl(result, result);
  j(not_zero, &ok);
  testl(op, op);
  j(sign, then_label);
  bind(&ok);
}


void MacroAssembler::Abort(const char* msg) {
  // We want to pass the msg string like a smi to avoid GC
  // problems, however msg is not guaranteed to be aligned
  // properly. Instead, we pass an aligned pointer that is
  // a proper v8 smi, but also pass the alignment difference
  // from the real pointer as a smi.
  intptr_t p1 = reinterpret_cast<intptr_t>(msg);
  intptr_t p0 = (p1 & ~kSmiTagMask) + kSmiTag;
  // Note: p0 might not be a valid Smi *value*, but it has a valid Smi tag.
  ASSERT(reinterpret_cast<Object*>(p0)->IsSmi());
#ifdef DEBUG
  if (msg != NULL) {
    RecordComment("Abort message: ");
    RecordComment(msg);
  }
#endif
  push(rax);
  movq(kScratchRegister, p0, RelocInfo::NONE);
  push(kScratchRegister);
  movq(kScratchRegister,
       reinterpret_cast<intptr_t>(Smi::FromInt(p1 - p0)),
       RelocInfo::NONE);
  push(kScratchRegister);
  CallRuntime(Runtime::kAbort, 2);
  // will not return here
}


void MacroAssembler::CallStub(CodeStub* stub) {
  ASSERT(allow_stub_calls());  // calls are not allowed in some stubs
  Call(stub->GetCode(), RelocInfo::CODE_TARGET);
}


void MacroAssembler::StubReturn(int argc) {
  ASSERT(argc >= 1 && generating_stub());
  ret((argc - 1) * kPointerSize);
}


void MacroAssembler::IllegalOperation(int num_arguments) {
  if (num_arguments > 0) {
    addq(rsp, Immediate(num_arguments * kPointerSize));
  }
  movq(rax, Factory::undefined_value(), RelocInfo::EMBEDDED_OBJECT);
}


void MacroAssembler::CallRuntime(Runtime::FunctionId id, int num_arguments) {
  CallRuntime(Runtime::FunctionForId(id), num_arguments);
}


void MacroAssembler::CallRuntime(Runtime::Function* f, int num_arguments) {
  // If the expected number of arguments of the runtime function is
  // constant, we check that the actual number of arguments match the
  // expectation.
  if (f->nargs >= 0 && f->nargs != num_arguments) {
    IllegalOperation(num_arguments);
    return;
  }

  Runtime::FunctionId function_id =
      static_cast<Runtime::FunctionId>(f->stub_id);
  RuntimeStub stub(function_id, num_arguments);
  CallStub(&stub);
}


void MacroAssembler::TailCallRuntime(ExternalReference const& ext,
                                     int num_arguments) {
  // ----------- S t a t e -------------
  //  -- rsp[0] : return address
  //  -- rsp[8] : argument num_arguments - 1
  //  ...
  //  -- rsp[8 * num_arguments] : argument 0 (receiver)
  // -----------------------------------

  // TODO(1236192): Most runtime routines don't need the number of
  // arguments passed in because it is constant. At some point we
  // should remove this need and make the runtime routine entry code
  // smarter.
  movq(rax, Immediate(num_arguments));
  JumpToBuiltin(ext);
}


void MacroAssembler::JumpToBuiltin(const ExternalReference& ext) {
  // Set the entry point and jump to the C entry runtime stub.
  movq(rbx, ext);
  CEntryStub ces;
  movq(kScratchRegister, ces.GetCode(), RelocInfo::CODE_TARGET);
  jmp(kScratchRegister);
}


void MacroAssembler::GetBuiltinEntry(Register target, Builtins::JavaScript id) {
  bool resolved;
  Handle<Code> code = ResolveBuiltin(id, &resolved);

  const char* name = Builtins::GetName(id);
  int argc = Builtins::GetArgumentsCount(id);

  movq(target, code, RelocInfo::EMBEDDED_OBJECT);
  if (!resolved) {
    uint32_t flags =
        Bootstrapper::FixupFlagsArgumentsCount::encode(argc) |
        Bootstrapper::FixupFlagsIsPCRelative::encode(false) |
        Bootstrapper::FixupFlagsUseCodeObject::encode(true);
    Unresolved entry = { pc_offset() - sizeof(intptr_t), flags, name };
    unresolved_.Add(entry);
  }
  addq(target, Immediate(Code::kHeaderSize - kHeapObjectTag));
}


Handle<Code> MacroAssembler::ResolveBuiltin(Builtins::JavaScript id,
                                            bool* resolved) {
  // Move the builtin function into the temporary function slot by
  // reading it from the builtins object. NOTE: We should be able to
  // reduce this to two instructions by putting the function table in
  // the global object instead of the "builtins" object and by using a
  // real register for the function.
  movq(rdx, Operand(rsi, Context::SlotOffset(Context::GLOBAL_INDEX)));
  movq(rdx, FieldOperand(rdx, GlobalObject::kBuiltinsOffset));
  int builtins_offset =
      JSBuiltinsObject::kJSBuiltinsOffset + (id * kPointerSize);
  movq(rdi, FieldOperand(rdx, builtins_offset));


  return Builtins::GetCode(id, resolved);
}


void MacroAssembler::Set(Register dst, int64_t x) {
  if (x == 0) {
    xor_(dst, dst);
  } else if (is_int32(x)) {
    movq(dst, Immediate(x));
  } else if (is_uint32(x)) {
    movl(dst, Immediate(x));
  } else {
    movq(dst, x, RelocInfo::NONE);
  }
}


void MacroAssembler::Set(const Operand& dst, int64_t x) {
  if (x == 0) {
    xor_(kScratchRegister, kScratchRegister);
    movq(dst, kScratchRegister);
  } else if (is_int32(x)) {
    movq(dst, Immediate(x));
  } else if (is_uint32(x)) {
    movl(dst, Immediate(x));
  } else {
    movq(kScratchRegister, x, RelocInfo::NONE);
    movq(dst, kScratchRegister);
  }
}


bool MacroAssembler::IsUnsafeSmi(Smi* value) {
  return false;
}

void MacroAssembler::LoadUnsafeSmi(Register dst, Smi* source) {
  UNIMPLEMENTED();
}


void MacroAssembler::Move(Register dst, Handle<Object> source) {
  ASSERT(!source->IsFailure());
  if (source->IsSmi()) {
    if (IsUnsafeSmi(source)) {
      LoadUnsafeSmi(dst, source);
    } else {
      int32_t smi = static_cast<int32_t>(reinterpret_cast<intptr_t>(*source));
      movq(dst, Immediate(smi));
    }
  } else {
    movq(dst, source, RelocInfo::EMBEDDED_OBJECT);
  }
}


void MacroAssembler::Move(const Operand& dst, Handle<Object> source) {
  if (source->IsSmi()) {
    int32_t smi = static_cast<int32_t>(reinterpret_cast<intptr_t>(*source));
    movq(dst, Immediate(smi));
  } else {
    movq(kScratchRegister, source, RelocInfo::EMBEDDED_OBJECT);
    movq(dst, kScratchRegister);
  }
}


void MacroAssembler::Cmp(Register dst, Handle<Object> source) {
  Move(kScratchRegister, source);
  cmpq(dst, kScratchRegister);
}


void MacroAssembler::Cmp(const Operand& dst, Handle<Object> source) {
  if (source->IsSmi()) {
    if (IsUnsafeSmi(source)) {
      LoadUnsafeSmi(kScratchRegister, source);
      cmpl(dst, kScratchRegister);
    } else {
      // For smi-comparison, it suffices to compare the low 32 bits.
      int32_t smi = static_cast<int32_t>(reinterpret_cast<intptr_t>(*source));
      cmpl(dst, Immediate(smi));
    }
  } else {
    ASSERT(source->IsHeapObject());
    movq(kScratchRegister, source, RelocInfo::EMBEDDED_OBJECT);
    cmpq(dst, kScratchRegister);
  }
}


void MacroAssembler::Push(Handle<Object> source) {
  if (source->IsSmi()) {
    if (IsUnsafeSmi(source)) {
      LoadUnsafeSmi(kScratchRegister, source);
      push(kScratchRegister);
    } else {
      int32_t smi = static_cast<int32_t>(reinterpret_cast<intptr_t>(*source));
      push(Immediate(smi));
    }
  } else {
    ASSERT(source->IsHeapObject());
    movq(kScratchRegister, source, RelocInfo::EMBEDDED_OBJECT);
    push(kScratchRegister);
  }
}


void MacroAssembler::Push(Smi* source) {
  if (IsUnsafeSmi(source)) {
    LoadUnsafeSmi(kScratchRegister, source);
    push(kScratchRegister);
  } else {
    int32_t smi = static_cast<int32_t>(reinterpret_cast<intptr_t>(source));
    push(Immediate(smi));
  }
}


void MacroAssembler::Jump(ExternalReference ext) {
  movq(kScratchRegister, ext);
  jmp(kScratchRegister);
}


void MacroAssembler::Jump(Address destination, RelocInfo::Mode rmode) {
  movq(kScratchRegister, destination, rmode);
  jmp(kScratchRegister);
}


void MacroAssembler::Jump(Handle<Code> code_object, RelocInfo::Mode rmode) {
  ASSERT(RelocInfo::IsCodeTarget(rmode));
  movq(kScratchRegister, code_object, rmode);
#ifdef DEBUG
  Label target;
  bind(&target);
#endif
  jmp(kScratchRegister);
#ifdef DEBUG
  ASSERT_EQ(kPatchReturnSequenceLength,
            SizeOfCodeGeneratedSince(&target) + kPointerSize);
#endif
}


void MacroAssembler::Call(ExternalReference ext) {
  movq(kScratchRegister, ext);
  call(kScratchRegister);
}


void MacroAssembler::Call(Address destination, RelocInfo::Mode rmode) {
  movq(kScratchRegister, destination, rmode);
  call(kScratchRegister);
}


void MacroAssembler::Call(Handle<Code> code_object, RelocInfo::Mode rmode) {
  ASSERT(RelocInfo::IsCodeTarget(rmode));
  WriteRecordedPositions();
  movq(kScratchRegister, code_object, rmode);
#ifdef DEBUG
  // Patch target is kPointer size bytes *before* target label.
  Label target;
  bind(&target);
#endif
  call(kScratchRegister);
#ifdef DEBUG
  ASSERT_EQ(kPatchReturnSequenceLength,
            SizeOfCodeGeneratedSince(&target) + kPointerSize);
#endif
}


void MacroAssembler::PushTryHandler(CodeLocation try_location,
                                    HandlerType type) {
  // Adjust this code if not the case.
  ASSERT(StackHandlerConstants::kSize == 4 * kPointerSize);

  // The pc (return address) is already on TOS.  This code pushes state,
  // frame pointer and current handler.  Check that they are expected
  // next on the stack, in that order.
  ASSERT_EQ(StackHandlerConstants::kStateOffset,
            StackHandlerConstants::kPCOffset - kPointerSize);
  ASSERT_EQ(StackHandlerConstants::kFPOffset,
            StackHandlerConstants::kStateOffset - kPointerSize);
  ASSERT_EQ(StackHandlerConstants::kNextOffset,
            StackHandlerConstants::kFPOffset - kPointerSize);

  if (try_location == IN_JAVASCRIPT) {
    if (type == TRY_CATCH_HANDLER) {
      push(Immediate(StackHandler::TRY_CATCH));
    } else {
      push(Immediate(StackHandler::TRY_FINALLY));
    }
    push(rbp);
  } else {
    ASSERT(try_location == IN_JS_ENTRY);
    // The frame pointer does not point to a JS frame so we save NULL
    // for rbp. We expect the code throwing an exception to check rbp
    // before dereferencing it to restore the context.
    push(Immediate(StackHandler::ENTRY));
    push(Immediate(0));  // NULL frame pointer.
  }
  // Save the current handler.
  movq(kScratchRegister, ExternalReference(Top::k_handler_address));
  push(Operand(kScratchRegister, 0));
  // Link this handler.
  movq(Operand(kScratchRegister, 0), rsp);
}


void MacroAssembler::Ret() {
  ret(0);
}


void MacroAssembler::FCmp() {
  fcompp();
  push(rax);
  fnstsw_ax();
  // TODO(X64): Check that sahf is safe to use, using CPUProbe.
  sahf();
  pop(rax);
}


void MacroAssembler::CmpObjectType(Register heap_object,
                                   InstanceType type,
                                   Register map) {
  movq(map, FieldOperand(heap_object, HeapObject::kMapOffset));
  CmpInstanceType(map, type);
}


void MacroAssembler::CmpInstanceType(Register map, InstanceType type) {
  cmpb(FieldOperand(map, Map::kInstanceTypeOffset),
       Immediate(static_cast<int8_t>(type)));
}


void MacroAssembler::TryGetFunctionPrototype(Register function,
                                             Register result,
                                             Label* miss) {
  // Check that the receiver isn't a smi.
  testl(function, Immediate(kSmiTagMask));
  j(zero, miss);

  // Check that the function really is a function.
  CmpObjectType(function, JS_FUNCTION_TYPE, result);
  j(not_equal, miss);

  // Make sure that the function has an instance prototype.
  Label non_instance;
  testb(FieldOperand(result, Map::kBitFieldOffset),
        Immediate(1 << Map::kHasNonInstancePrototype));
  j(not_zero, &non_instance);

  // Get the prototype or initial map from the function.
  movq(result,
       FieldOperand(function, JSFunction::kPrototypeOrInitialMapOffset));

  // If the prototype or initial map is the hole, don't return it and
  // simply miss the cache instead. This will allow us to allocate a
  // prototype object on-demand in the runtime system.
  Cmp(result, Factory::the_hole_value());
  j(equal, miss);

  // If the function does not have an initial map, we're done.
  Label done;
  CmpObjectType(result, MAP_TYPE, kScratchRegister);
  j(not_equal, &done);

  // Get the prototype from the initial map.
  movq(result, FieldOperand(result, Map::kPrototypeOffset));
  jmp(&done);

  // Non-instance prototype: Fetch prototype from constructor field
  // in initial map.
  bind(&non_instance);
  movq(result, FieldOperand(result, Map::kConstructorOffset));

  // All done.
  bind(&done);
}


void MacroAssembler::SetCounter(StatsCounter* counter, int value) {
  if (FLAG_native_code_counters && counter->Enabled()) {
    movq(kScratchRegister, ExternalReference(counter));
    movl(Operand(kScratchRegister, 0), Immediate(value));
  }
}


void MacroAssembler::IncrementCounter(StatsCounter* counter, int value) {
  ASSERT(value > 0);
  if (FLAG_native_code_counters && counter->Enabled()) {
    movq(kScratchRegister, ExternalReference(counter));
    Operand operand(kScratchRegister, 0);
    if (value == 1) {
      incl(operand);
    } else {
      addl(operand, Immediate(value));
    }
  }
}


void MacroAssembler::DecrementCounter(StatsCounter* counter, int value) {
  ASSERT(value > 0);
  if (FLAG_native_code_counters && counter->Enabled()) {
    movq(kScratchRegister, ExternalReference(counter));
    Operand operand(kScratchRegister, 0);
    if (value == 1) {
      decl(operand);
    } else {
      subl(operand, Immediate(value));
    }
  }
}


#ifdef ENABLE_DEBUGGER_SUPPORT

void MacroAssembler::PushRegistersFromMemory(RegList regs) {
  ASSERT((regs & ~kJSCallerSaved) == 0);
  // Push the content of the memory location to the stack.
  for (int i = 0; i < kNumJSCallerSaved; i++) {
    int r = JSCallerSavedCode(i);
    if ((regs & (1 << r)) != 0) {
      ExternalReference reg_addr =
          ExternalReference(Debug_Address::Register(i));
      movq(kScratchRegister, reg_addr);
      push(Operand(kScratchRegister, 0));
    }
  }
}

void MacroAssembler::SaveRegistersToMemory(RegList regs) {
  ASSERT((regs & ~kJSCallerSaved) == 0);
  // Copy the content of registers to memory location.
  for (int i = 0; i < kNumJSCallerSaved; i++) {
    int r = JSCallerSavedCode(i);
    if ((regs & (1 << r)) != 0) {
      Register reg = { r };
      ExternalReference reg_addr =
          ExternalReference(Debug_Address::Register(i));
      movq(kScratchRegister, reg_addr);
      movq(Operand(kScratchRegister, 0), reg);
    }
  }
}


void MacroAssembler::RestoreRegistersFromMemory(RegList regs) {
  ASSERT((regs & ~kJSCallerSaved) == 0);
  // Copy the content of memory location to registers.
  for (int i = kNumJSCallerSaved - 1; i >= 0; i--) {
    int r = JSCallerSavedCode(i);
    if ((regs & (1 << r)) != 0) {
      Register reg = { r };
      ExternalReference reg_addr =
          ExternalReference(Debug_Address::Register(i));
      movq(kScratchRegister, reg_addr);
      movq(reg, Operand(kScratchRegister, 0));
    }
  }
}


void MacroAssembler::PopRegistersToMemory(RegList regs) {
  ASSERT((regs & ~kJSCallerSaved) == 0);
  // Pop the content from the stack to the memory location.
  for (int i = kNumJSCallerSaved - 1; i >= 0; i--) {
    int r = JSCallerSavedCode(i);
    if ((regs & (1 << r)) != 0) {
      ExternalReference reg_addr =
          ExternalReference(Debug_Address::Register(i));
      movq(kScratchRegister, reg_addr);
      pop(Operand(kScratchRegister, 0));
    }
  }
}


void MacroAssembler::CopyRegistersFromStackToMemory(Register base,
                                                    Register scratch,
                                                    RegList regs) {
  ASSERT(!scratch.is(kScratchRegister));
  ASSERT(!base.is(kScratchRegister));
  ASSERT(!base.is(scratch));
  ASSERT((regs & ~kJSCallerSaved) == 0);
  // Copy the content of the stack to the memory location and adjust base.
  for (int i = kNumJSCallerSaved - 1; i >= 0; i--) {
    int r = JSCallerSavedCode(i);
    if ((regs & (1 << r)) != 0) {
      movq(scratch, Operand(base, 0));
      ExternalReference reg_addr =
          ExternalReference(Debug_Address::Register(i));
      movq(kScratchRegister, reg_addr);
      movq(Operand(kScratchRegister, 0), scratch);
      lea(base, Operand(base, kPointerSize));
    }
  }
}

#endif  // ENABLE_DEBUGGER_SUPPORT


void MacroAssembler::InvokeBuiltin(Builtins::JavaScript id, InvokeFlag flag) {
  bool resolved;
  Handle<Code> code = ResolveBuiltin(id, &resolved);

  // Calls are not allowed in some stubs.
  ASSERT(flag == JUMP_FUNCTION || allow_stub_calls());

  // Rely on the assertion to check that the number of provided
  // arguments match the expected number of arguments. Fake a
  // parameter count to avoid emitting code to do the check.
  ParameterCount expected(0);
  InvokeCode(Handle<Code>(code), expected, expected,
             RelocInfo::CODE_TARGET, flag);

  const char* name = Builtins::GetName(id);
  int argc = Builtins::GetArgumentsCount(id);
  // The target address for the jump is stored as an immediate at offset
  // kInvokeCodeAddressOffset.
  if (!resolved) {
    uint32_t flags =
        Bootstrapper::FixupFlagsArgumentsCount::encode(argc) |
        Bootstrapper::FixupFlagsIsPCRelative::encode(false) |
        Bootstrapper::FixupFlagsUseCodeObject::encode(false);
    Unresolved entry =
        { pc_offset() - kPatchReturnSequenceLength, flags, name };
    unresolved_.Add(entry);
  }
}


void MacroAssembler::InvokePrologue(const ParameterCount& expected,
                                    const ParameterCount& actual,
                                    Handle<Code> code_constant,
                                    Register code_register,
                                    Label* done,
                                    InvokeFlag flag) {
  bool definitely_matches = false;
  Label invoke;
  if (expected.is_immediate()) {
    ASSERT(actual.is_immediate());
    if (expected.immediate() == actual.immediate()) {
      definitely_matches = true;
    } else {
      movq(rax, Immediate(actual.immediate()));
      if (expected.immediate() ==
          SharedFunctionInfo::kDontAdaptArgumentsSentinel) {
        // Don't worry about adapting arguments for built-ins that
        // don't want that done. Skip adaption code by making it look
        // like we have a match between expected and actual number of
        // arguments.
        definitely_matches = true;
      } else {
        movq(rbx, Immediate(expected.immediate()));
      }
    }
  } else {
    if (actual.is_immediate()) {
      // Expected is in register, actual is immediate. This is the
      // case when we invoke function values without going through the
      // IC mechanism.
      cmpq(expected.reg(), Immediate(actual.immediate()));
      j(equal, &invoke);
      ASSERT(expected.reg().is(rbx));
      movq(rax, Immediate(actual.immediate()));
    } else if (!expected.reg().is(actual.reg())) {
      // Both expected and actual are in (different) registers. This
      // is the case when we invoke functions using call and apply.
      cmpq(expected.reg(), actual.reg());
      j(equal, &invoke);
      ASSERT(actual.reg().is(rax));
      ASSERT(expected.reg().is(rbx));
    }
  }

  if (!definitely_matches) {
    Handle<Code> adaptor =
        Handle<Code>(Builtins::builtin(Builtins::ArgumentsAdaptorTrampoline));
    if (!code_constant.is_null()) {
      movq(rdx, code_constant, RelocInfo::EMBEDDED_OBJECT);
      addq(rdx, Immediate(Code::kHeaderSize - kHeapObjectTag));
    } else if (!code_register.is(rdx)) {
      movq(rdx, code_register);
    }

    if (flag == CALL_FUNCTION) {
      Call(adaptor, RelocInfo::CODE_TARGET);
      jmp(done);
    } else {
      Jump(adaptor, RelocInfo::CODE_TARGET);
    }
    bind(&invoke);
  }
}


void MacroAssembler::InvokeCode(Register code,
                                const ParameterCount& expected,
                                const ParameterCount& actual,
                                InvokeFlag flag) {
  Label done;
  InvokePrologue(expected, actual, Handle<Code>::null(), code, &done, flag);
  if (flag == CALL_FUNCTION) {
    call(code);
  } else {
    ASSERT(flag == JUMP_FUNCTION);
    jmp(code);
  }
  bind(&done);
}


void MacroAssembler::InvokeCode(Handle<Code> code,
                                const ParameterCount& expected,
                                const ParameterCount& actual,
                                RelocInfo::Mode rmode,
                                InvokeFlag flag) {
  Label done;
  Register dummy = rax;
  InvokePrologue(expected, actual, code, dummy, &done, flag);
  if (flag == CALL_FUNCTION) {
    Call(code, rmode);
  } else {
    ASSERT(flag == JUMP_FUNCTION);
    Jump(code, rmode);
  }
  bind(&done);
}


void MacroAssembler::InvokeFunction(Register function,
                                    const ParameterCount& actual,
                                    InvokeFlag flag) {
  ASSERT(function.is(rdi));
  movq(rdx, FieldOperand(function, JSFunction::kSharedFunctionInfoOffset));
  movq(rsi, FieldOperand(function, JSFunction::kContextOffset));
  movsxlq(rbx,
          FieldOperand(rdx, SharedFunctionInfo::kFormalParameterCountOffset));
  movq(rdx, FieldOperand(rdx, SharedFunctionInfo::kCodeOffset));
  // Advances rdx to the end of the Code object header, to the start of
  // the executable code.
  lea(rdx, FieldOperand(rdx, Code::kHeaderSize));

  ParameterCount expected(rbx);
  InvokeCode(rdx, expected, actual, flag);
}


void MacroAssembler::EnterFrame(StackFrame::Type type) {
  push(rbp);
  movq(rbp, rsp);
  push(rsi);  // Context.
  push(Immediate(Smi::FromInt(type)));
  movq(kScratchRegister, CodeObject(), RelocInfo::EMBEDDED_OBJECT);
  push(kScratchRegister);
  if (FLAG_debug_code) {
    movq(kScratchRegister,
         Factory::undefined_value(),
         RelocInfo::EMBEDDED_OBJECT);
    cmpq(Operand(rsp, 0), kScratchRegister);
    Check(not_equal, "code object not properly patched");
  }
}


void MacroAssembler::LeaveFrame(StackFrame::Type type) {
  if (FLAG_debug_code) {
    movq(kScratchRegister, Immediate(Smi::FromInt(type)));
    cmpq(Operand(rbp, StandardFrameConstants::kMarkerOffset), kScratchRegister);
    Check(equal, "stack frame types must match");
  }
  movq(rsp, rbp);
  pop(rbp);
}



void MacroAssembler::EnterExitFrame(StackFrame::Type type) {
  ASSERT(type == StackFrame::EXIT || type == StackFrame::EXIT_DEBUG);

  // Setup the frame structure on the stack.
  // All constants are relative to the frame pointer of the exit frame.
  ASSERT(ExitFrameConstants::kCallerSPDisplacement == +2 * kPointerSize);
  ASSERT(ExitFrameConstants::kCallerPCOffset == +1 * kPointerSize);
  ASSERT(ExitFrameConstants::kCallerFPOffset ==  0 * kPointerSize);
  push(rbp);
  movq(rbp, rsp);

  // Reserve room for entry stack pointer and push the debug marker.
  ASSERT(ExitFrameConstants::kSPOffset  == -1 * kPointerSize);
  push(Immediate(0));  // saved entry sp, patched before call
  push(Immediate(type == StackFrame::EXIT_DEBUG ? 1 : 0));

  // Save the frame pointer and the context in top.
  ExternalReference c_entry_fp_address(Top::k_c_entry_fp_address);
  ExternalReference context_address(Top::k_context_address);
  movq(r14, rax);  // Backup rax before we use it.

  movq(rax, rbp);
  store_rax(c_entry_fp_address);
  movq(rax, rsi);
  store_rax(context_address);

  // Setup argv in callee-saved register r15. It is reused in LeaveExitFrame,
  // so it must be retained across the C-call.
  int offset = StandardFrameConstants::kCallerSPOffset - kPointerSize;
  lea(r15, Operand(rbp, r14, times_pointer_size, offset));

#ifdef ENABLE_DEBUGGER_SUPPORT
  // Save the state of all registers to the stack from the memory
  // location. This is needed to allow nested break points.
  if (type == StackFrame::EXIT_DEBUG) {
    // TODO(1243899): This should be symmetric to
    // CopyRegistersFromStackToMemory() but it isn't! esp is assumed
    // correct here, but computed for the other call. Very error
    // prone! FIX THIS.  Actually there are deeper problems with
    // register saving than this asymmetry (see the bug report
    // associated with this issue).
    PushRegistersFromMemory(kJSCallerSaved);
  }
#endif

  // Reserve space for the Arguments object.  The Windows 64-bit ABI
  // requires us to pass this structure as a pointer to its location on
  // the stack.  We also need backing space for the pointer, even though
  // it is passed in a register.
  subq(rsp, Immediate(3 * kPointerSize));

  // Get the required frame alignment for the OS.
  static const int kFrameAlignment = OS::ActivationFrameAlignment();
  if (kFrameAlignment > 0) {
    ASSERT(IsPowerOf2(kFrameAlignment));
    movq(kScratchRegister, Immediate(-kFrameAlignment));
    and_(rsp, kScratchRegister);
  }

  // Patch the saved entry sp.
  movq(Operand(rbp, ExitFrameConstants::kSPOffset), rsp);
}


void MacroAssembler::LeaveExitFrame(StackFrame::Type type) {
  // Registers:
  // r15 : argv
#ifdef ENABLE_DEBUGGER_SUPPORT
  // Restore the memory copy of the registers by digging them out from
  // the stack. This is needed to allow nested break points.
  if (type == StackFrame::EXIT_DEBUG) {
    // It's okay to clobber register ebx below because we don't need
    // the function pointer after this.
    const int kCallerSavedSize = kNumJSCallerSaved * kPointerSize;
    int kOffset = ExitFrameConstants::kDebugMarkOffset - kCallerSavedSize;
    lea(rbx, Operand(rbp, kOffset));
    CopyRegistersFromStackToMemory(rbx, rcx, kJSCallerSaved);
  }
#endif

  // Get the return address from the stack and restore the frame pointer.
  movq(rcx, Operand(rbp, 1 * kPointerSize));
  movq(rbp, Operand(rbp, 0 * kPointerSize));

  // Pop the arguments and the receiver from the caller stack.
  lea(rsp, Operand(r15, 1 * kPointerSize));

  // Restore current context from top and clear it in debug mode.
  ExternalReference context_address(Top::k_context_address);
  movq(kScratchRegister, context_address);
  movq(rsi, Operand(kScratchRegister, 0));
#ifdef DEBUG
  movq(Operand(kScratchRegister, 0), Immediate(0));
#endif

  // Push the return address to get ready to return.
  push(rcx);

  // Clear the top frame.
  ExternalReference c_entry_fp_address(Top::k_c_entry_fp_address);
  movq(kScratchRegister, c_entry_fp_address);
  movq(Operand(kScratchRegister, 0), Immediate(0));
}


Register MacroAssembler::CheckMaps(JSObject* object, Register object_reg,
                                   JSObject* holder, Register holder_reg,
                                   Register scratch,
                                   Label* miss) {
  // Make sure there's no overlap between scratch and the other
  // registers.
  ASSERT(!scratch.is(object_reg) && !scratch.is(holder_reg));

  // Keep track of the current object in register reg.  On the first
  // iteration, reg is an alias for object_reg, on later iterations,
  // it is an alias for holder_reg.
  Register reg = object_reg;
  int depth = 1;

  // Check the maps in the prototype chain.
  // Traverse the prototype chain from the object and do map checks.
  while (object != holder) {
    depth++;

    // Only global objects and objects that do not require access
    // checks are allowed in stubs.
    ASSERT(object->IsJSGlobalProxy() || !object->IsAccessCheckNeeded());

    JSObject* prototype = JSObject::cast(object->GetPrototype());
    if (Heap::InNewSpace(prototype)) {
      // Get the map of the current object.
      movq(scratch, FieldOperand(reg, HeapObject::kMapOffset));
      Cmp(scratch, Handle<Map>(object->map()));
      // Branch on the result of the map check.
      j(not_equal, miss);
      // Check access rights to the global object.  This has to happen
      // after the map check so that we know that the object is
      // actually a global object.
      if (object->IsJSGlobalProxy()) {
        CheckAccessGlobalProxy(reg, scratch, miss);

        // Restore scratch register to be the map of the object.
        // We load the prototype from the map in the scratch register.
        movq(scratch, FieldOperand(reg, HeapObject::kMapOffset));
      }
      // The prototype is in new space; we cannot store a reference
      // to it in the code. Load it from the map.
      reg = holder_reg;  // from now the object is in holder_reg
      movq(reg, FieldOperand(scratch, Map::kPrototypeOffset));

    } else {
      // Check the map of the current object.
      Cmp(FieldOperand(reg, HeapObject::kMapOffset),
          Handle<Map>(object->map()));
      // Branch on the result of the map check.
      j(not_equal, miss);
      // Check access rights to the global object.  This has to happen
      // after the map check so that we know that the object is
      // actually a global object.
      if (object->IsJSGlobalProxy()) {
        CheckAccessGlobalProxy(reg, scratch, miss);
      }
      // The prototype is in old space; load it directly.
      reg = holder_reg;  // from now the object is in holder_reg
      Move(reg, Handle<JSObject>(prototype));
    }

    // Go to the next object in the prototype chain.
    object = prototype;
  }

  // Check the holder map.
  Cmp(FieldOperand(reg, HeapObject::kMapOffset),
      Handle<Map>(holder->map()));
  j(not_equal, miss);

  // Log the check depth.
  LOG(IntEvent("check-maps-depth", depth));

  // Perform security check for access to the global object and return
  // the holder register.
  ASSERT(object == holder);
  ASSERT(object->IsJSGlobalProxy() || !object->IsAccessCheckNeeded());
  if (object->IsJSGlobalProxy()) {
    CheckAccessGlobalProxy(reg, scratch, miss);
  }
  return reg;
}




void MacroAssembler::CheckAccessGlobalProxy(Register holder_reg,
                                            Register scratch,
                                            Label* miss) {
  Label same_contexts;

  ASSERT(!holder_reg.is(scratch));
  ASSERT(!scratch.is(kScratchRegister));
  // Load current lexical context from the stack frame.
  movq(scratch, Operand(rbp, StandardFrameConstants::kContextOffset));

  // When generating debug code, make sure the lexical context is set.
  if (FLAG_debug_code) {
    cmpq(scratch, Immediate(0));
    Check(not_equal, "we should not have an empty lexical context");
  }
  // Load the global context of the current context.
  int offset = Context::kHeaderSize + Context::GLOBAL_INDEX * kPointerSize;
  movq(scratch, FieldOperand(scratch, offset));
  movq(scratch, FieldOperand(scratch, GlobalObject::kGlobalContextOffset));

  // Check the context is a global context.
  if (FLAG_debug_code) {
    Cmp(FieldOperand(scratch, HeapObject::kMapOffset),
        Factory::global_context_map());
    Check(equal, "JSGlobalObject::global_context should be a global context.");
  }

  // Check if both contexts are the same.
  cmpq(scratch, FieldOperand(holder_reg, JSGlobalProxy::kContextOffset));
  j(equal, &same_contexts);

  // Compare security tokens.
  // Check that the security token in the calling global object is
  // compatible with the security token in the receiving global
  // object.

  // Check the context is a global context.
  if (FLAG_debug_code) {
    // Preserve original value of holder_reg.
    push(holder_reg);
    movq(holder_reg, FieldOperand(holder_reg, JSGlobalProxy::kContextOffset));
    Cmp(holder_reg, Factory::null_value());
    Check(not_equal, "JSGlobalProxy::context() should not be null.");

    // Read the first word and compare to global_context_map(),
    movq(holder_reg, FieldOperand(holder_reg, HeapObject::kMapOffset));
    Cmp(holder_reg, Factory::global_context_map());
    Check(equal, "JSGlobalObject::global_context should be a global context.");
    pop(holder_reg);
  }

  movq(kScratchRegister,
       FieldOperand(holder_reg, JSGlobalProxy::kContextOffset));
  int token_offset = Context::kHeaderSize +
                     Context::SECURITY_TOKEN_INDEX * kPointerSize;
  movq(scratch, FieldOperand(scratch, token_offset));
  cmpq(scratch, FieldOperand(kScratchRegister, token_offset));
  j(not_equal, miss);

  bind(&same_contexts);
}


void MacroAssembler::LoadAllocationTopHelper(
    Register result,
    Register result_end,
    Register scratch,
    bool result_contains_top_on_entry) {
  ExternalReference new_space_allocation_top =
      ExternalReference::new_space_allocation_top_address();

  // Just return if allocation top is already known.
  if (result_contains_top_on_entry) {
    // No use of scratch if allocation top is provided.
    ASSERT(scratch.is(no_reg));
    return;
  }

  // Move address of new object to result. Use scratch register if available.
  if (scratch.is(no_reg)) {
    movq(kScratchRegister, new_space_allocation_top);
    movq(result, Operand(kScratchRegister, 0));
  } else {
    ASSERT(!scratch.is(result_end));
    movq(scratch, new_space_allocation_top);
    movq(result, Operand(scratch, 0));
  }
}


void MacroAssembler::UpdateAllocationTopHelper(Register result_end,
                                               Register scratch) {
  ExternalReference new_space_allocation_top =
      ExternalReference::new_space_allocation_top_address();

  // Update new top.
  if (result_end.is(rax)) {
    // rax can be stored directly to a memory location.
    store_rax(new_space_allocation_top);
  } else {
    // Register required - use scratch provided if available.
    if (scratch.is(no_reg)) {
      movq(kScratchRegister, new_space_allocation_top);
      movq(Operand(kScratchRegister, 0), result_end);
    } else {
      movq(Operand(scratch, 0), result_end);
    }
  }
}


void MacroAssembler::AllocateObjectInNewSpace(
    int object_size,
    Register result,
    Register result_end,
    Register scratch,
    Label* gc_required,
    bool result_contains_top_on_entry) {
  ASSERT(!result.is(result_end));

  // Load address of new object into result.
  LoadAllocationTopHelper(result,
                          result_end,
                          scratch,
                          result_contains_top_on_entry);

  // Calculate new top and bail out if new space is exhausted.
  ExternalReference new_space_allocation_limit =
      ExternalReference::new_space_allocation_limit_address();
  lea(result_end, Operand(result, object_size));
  movq(kScratchRegister, new_space_allocation_limit);
  cmpq(result_end, Operand(kScratchRegister, 0));
  j(above, gc_required);

  // Update allocation top.
  UpdateAllocationTopHelper(result_end, scratch);
}


void MacroAssembler::AllocateObjectInNewSpace(
    int header_size,
    ScaleFactor element_size,
    Register element_count,
    Register result,
    Register result_end,
    Register scratch,
    Label* gc_required,
    bool result_contains_top_on_entry) {
  ASSERT(!result.is(result_end));

  // Load address of new object into result.
  LoadAllocationTopHelper(result,
                          result_end,
                          scratch,
                          result_contains_top_on_entry);

  // Calculate new top and bail out if new space is exhausted.
  ExternalReference new_space_allocation_limit =
      ExternalReference::new_space_allocation_limit_address();
  lea(result_end, Operand(result, element_count, element_size, header_size));
  movq(kScratchRegister, new_space_allocation_limit);
  cmpq(result_end, Operand(kScratchRegister, 0));
  j(above, gc_required);

  // Update allocation top.
  UpdateAllocationTopHelper(result_end, scratch);
}


void MacroAssembler::AllocateObjectInNewSpace(
    Register object_size,
    Register result,
    Register result_end,
    Register scratch,
    Label* gc_required,
    bool result_contains_top_on_entry) {

  // Load address of new object into result.
  LoadAllocationTopHelper(result,
                          result_end,
                          scratch,
                          result_contains_top_on_entry);


  // Calculate new top and bail out if new space is exhausted.
  ExternalReference new_space_allocation_limit =
      ExternalReference::new_space_allocation_limit_address();
  if (!object_size.is(result_end)) {
    movq(result_end, object_size);
  }
  addq(result_end, result);
  movq(kScratchRegister, new_space_allocation_limit);
  cmpq(result_end, Operand(kScratchRegister, 0));
  j(above, gc_required);

  // Update allocation top.
  UpdateAllocationTopHelper(result_end, scratch);
}


void MacroAssembler::UndoAllocationInNewSpace(Register object) {
  ExternalReference new_space_allocation_top =
      ExternalReference::new_space_allocation_top_address();

  // Make sure the object has no tag before resetting top.
  and_(object, Immediate(~kHeapObjectTagMask));
  movq(kScratchRegister, new_space_allocation_top);
#ifdef DEBUG
  cmpq(object, Operand(kScratchRegister, 0));
  Check(below, "Undo allocation of non allocated memory");
#endif
  movq(Operand(kScratchRegister, 0), object);
}


} }  // namespace v8::internal
