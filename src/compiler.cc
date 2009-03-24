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

#include "bootstrapper.h"
#include "codegen-inl.h"
#include "compilation-cache.h"
#include "compiler.h"
#include "debug.h"
#include "scopes.h"
#include "rewriter.h"
#include "usage-analyzer.h"
#include "oprofile-agent.h"

namespace v8 { namespace internal {

static Handle<Code> MakeCode(FunctionLiteral* literal,
                             Handle<Script> script,
                             Handle<Context> context,
                             bool is_eval) {
  ASSERT(literal != NULL);

  // Rewrite the AST by introducing .result assignments where needed.
  if (!Rewriter::Process(literal) || !AnalyzeVariableUsage(literal)) {
    // Signal a stack overflow by returning a null handle.  The stack
    // overflow exception will be thrown by the caller.
    return Handle<Code>::null();
  }

  // Compute top scope and allocate variables. For lazy compilation
  // the top scope only contains the single lazily compiled function,
  // so this doesn't re-allocate variables repeatedly.
  Scope* top = literal->scope();
  while (top->outer_scope() != NULL) top = top->outer_scope();
  top->AllocateVariables(context);

#ifdef DEBUG
  if (Bootstrapper::IsActive() ?
      FLAG_print_builtin_scopes :
      FLAG_print_scopes) {
    literal->scope()->Print();
  }
#endif

  // Optimize the AST.
  if (!Rewriter::Optimize(literal)) {
    // Signal a stack overflow by returning a null handle.  The stack
    // overflow exception will be thrown by the caller.
    return Handle<Code>::null();
  }

  // Generate code and return it.
  Handle<Code> result = CodeGenerator::MakeCode(literal, script, is_eval);
  return result;
}


static Handle<JSFunction> MakeFunction(bool is_global,
                                       bool is_eval,
                                       Handle<Script> script,
                                       Handle<Context> context,
                                       v8::Extension* extension,
                                       ScriptDataImpl* pre_data) {
  ZoneScope zone_scope(DELETE_ON_EXIT);

  // Make sure we have an initial stack limit.
  StackGuard guard;
  PostponeInterruptsScope postpone;

  // Notify debugger
  Debugger::OnBeforeCompile(script);

  // Only allow non-global compiles for eval.
  ASSERT(is_eval || is_global);

  // Build AST.
  FunctionLiteral* lit = MakeAST(is_global, script, extension, pre_data);

  // Check for parse errors.
  if (lit == NULL) {
    ASSERT(Top::has_pending_exception());
    return Handle<JSFunction>::null();
  }

  // Measure how long it takes to do the compilation; only take the
  // rest of the function into account to avoid overlap with the
  // parsing statistics.
  HistogramTimer* rate = is_eval
      ? &Counters::compile_eval
      : &Counters::compile;
  HistogramTimerScope timer(rate);

  // Compile the code.
  Handle<Code> code = MakeCode(lit, script, context, is_eval);

  // Check for stack-overflow exceptions.
  if (code.is_null()) {
    Top::StackOverflow();
    return Handle<JSFunction>::null();
  }

#if defined ENABLE_LOGGING_AND_PROFILING || defined ENABLE_OPROFILE_AGENT
  // Log the code generation for the script. Check explicit whether logging is
  // to avoid allocating when not required.
  if (Logger::is_enabled() || OProfileAgent::is_enabled()) {
    if (script->name()->IsString()) {
      SmartPointer<char> data =
          String::cast(script->name())->ToCString(DISALLOW_NULLS);
      LOG(CodeCreateEvent(is_eval ? "Eval" : "Script", *code, *data));
      OProfileAgent::CreateNativeCodeRegion(*data, code->address(),
                                            code->ExecutableSize());
    } else {
      LOG(CodeCreateEvent(is_eval ? "Eval" : "Script", *code, ""));
      OProfileAgent::CreateNativeCodeRegion(is_eval ? "Eval" : "Script",
          code->address(), code->ExecutableSize());
    }
  }
#endif

  // Allocate function.
  Handle<JSFunction> fun =
      Factory::NewFunctionBoilerplate(lit->name(),
                                      lit->materialized_literal_count(),
                                      lit->contains_array_literal(),
                                      code);

  CodeGenerator::SetFunctionInfo(fun, lit->scope()->num_parameters(),
                                 RelocInfo::kNoPosition,
                                 lit->start_position(), lit->end_position(),
                                 lit->is_expression(), true, script);

  // Hint to the runtime system used when allocating space for initial
  // property space by setting the expected number of properties for
  // the instances of the function.
  SetExpectedNofPropertiesFromEstimate(fun, lit->expected_property_count());

  // Notify debugger
  Debugger::OnAfterCompile(script, fun);

  return fun;
}


static StaticResource<SafeStringInputBuffer> safe_string_input_buffer;


Handle<JSFunction> Compiler::Compile(Handle<String> source,
                                     Handle<Object> script_name,
                                     int line_offset, int column_offset,
                                     v8::Extension* extension,
                                     ScriptDataImpl* input_pre_data) {
  int source_length = source->length();
  Counters::total_load_size.Increment(source_length);
  Counters::total_compile_size.Increment(source_length);

  // The VM is in the COMPILER state until exiting this function.
  VMState state(COMPILER);

  // Do a lookup in the compilation cache but not for extensions.
  Handle<JSFunction> result;
  if (extension == NULL) {
    result = CompilationCache::LookupScript(source,
                                            script_name,
                                            line_offset,
                                            column_offset);
  }

  if (result.is_null()) {
    // No cache entry found. Do pre-parsing and compile the script.
    ScriptDataImpl* pre_data = input_pre_data;
    if (pre_data == NULL && source_length >= FLAG_min_preparse_length) {
      Access<SafeStringInputBuffer> buf(&safe_string_input_buffer);
      buf->Reset(source.location());
      pre_data = PreParse(buf.value(), extension);
    }

    // Create a script object describing the script to be compiled.
    Handle<Script> script = Factory::NewScript(source);
    if (!script_name.is_null()) {
      script->set_name(*script_name);
      script->set_line_offset(Smi::FromInt(line_offset));
      script->set_column_offset(Smi::FromInt(column_offset));
    }

    // Compile the function and add it to the cache.
    result = MakeFunction(true,
                          false,
                          script,
                          Handle<Context>::null(),
                          extension,
                          pre_data);
    if (extension == NULL && !result.is_null()) {
      CompilationCache::PutScript(source, CompilationCache::SCRIPT, result);
    }

    // Get rid of the pre-parsing data (if necessary).
    if (input_pre_data == NULL && pre_data != NULL) {
      delete pre_data;
    }
  }

  if (result.is_null()) Top::ReportPendingMessages();
  return result;
}


Handle<JSFunction> Compiler::CompileEval(Handle<String> source,
                                         Handle<Context> context,
                                         int line_offset,
                                         bool is_global) {
  int source_length = source->length();
  Counters::total_eval_size.Increment(source_length);
  Counters::total_compile_size.Increment(source_length);

  // The VM is in the COMPILER state until exiting this function.
  VMState state(COMPILER);
  CompilationCache::Entry entry = is_global
      ? CompilationCache::EVAL_GLOBAL
      : CompilationCache::EVAL_CONTEXTUAL;

  // Do a lookup in the compilation cache; if the entry is not there,
  // invoke the compiler and add the result to the cache.
  Handle<JSFunction> result =
      CompilationCache::LookupEval(source, context, entry);
  if (result.is_null()) {
    // Create a script object describing the script to be compiled.
    Handle<Script> script = Factory::NewScript(source);
    script->set_line_offset(Smi::FromInt(line_offset));
    result = MakeFunction(is_global, true, script, context, NULL, NULL);
    if (!result.is_null()) {
      CompilationCache::PutEval(source, context, entry, result);
    }
  }

  return result;
}


bool Compiler::CompileLazy(Handle<SharedFunctionInfo> shared,
                           int loop_nesting) {
  ZoneScope zone_scope(DELETE_ON_EXIT);

  // The VM is in the COMPILER state until exiting this function.
  VMState state(COMPILER);

  // Make sure we have an initial stack limit.
  StackGuard guard;
  PostponeInterruptsScope postpone;

  // Compute name, source code and script data.
  Handle<String> name(String::cast(shared->name()));
  Handle<Script> script(Script::cast(shared->script()));

  int start_position = shared->start_position();
  int end_position = shared->end_position();
  bool is_expression = shared->is_expression();
  Counters::total_compile_size.Increment(end_position - start_position);

  // Generate the AST for the lazily compiled function. The AST may be
  // NULL in case of parser stack overflow.
  FunctionLiteral* lit = MakeLazyAST(script, name,
                                     start_position,
                                     end_position,
                                     is_expression);

  // Check for parse errors.
  if (lit == NULL) {
    ASSERT(Top::has_pending_exception());
    return false;
  }

  // Update the loop nesting in the function literal.
  lit->set_loop_nesting(loop_nesting);

  // Measure how long it takes to do the lazy compilation; only take
  // the rest of the function into account to avoid overlap with the
  // lazy parsing statistics.
  HistogramTimerScope timer(&Counters::compile_lazy);

  // Compile the code.
  Handle<Code> code = MakeCode(lit, script, Handle<Context>::null(), false);

  // Check for stack-overflow exception.
  if (code.is_null()) {
    Top::StackOverflow();
    return false;
  }

#if defined ENABLE_LOGGING_AND_PROFILING || defined ENABLE_OPROFILE_AGENT
  // Log the code generation. If source information is available include script
  // name and line number. Check explicit whether logging is enabled as finding
  // the line number is not for free.
  if (Logger::is_enabled() || OProfileAgent::is_enabled()) {
    if (script->name()->IsString()) {
      int line_num = GetScriptLineNumber(script, start_position);
      if (line_num > 0) {
        line_num += script->line_offset()->value() + 1;
      }
      LOG(CodeCreateEvent("LazyCompile", *code, *lit->name(),
                          String::cast(script->name()), line_num));
      OProfileAgent::CreateNativeCodeRegion(*lit->name(),
                                            String::cast(script->name()),
                                            line_num, code->address(),
                                            code->ExecutableSize());
    } else {
      LOG(CodeCreateEvent("LazyCompile", *code, *lit->name()));
      OProfileAgent::CreateNativeCodeRegion(*lit->name(), code->address(),
                                            code->ExecutableSize());
    }
  }
#endif

  // Update the shared function info with the compiled code.
  shared->set_code(*code);

  // Set the expected number of properties for instances.
  SetExpectedNofPropertiesFromEstimate(shared, lit->expected_property_count());

  // Check the function has compiled code.
  ASSERT(shared->is_compiled());
  return true;
}


} }  // namespace v8::internal
