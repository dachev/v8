// Copyright 2008 the V8 project authors. All rights reserved.
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

#ifndef V8_D8_H_
#define V8_D8_H_

#include "v8.h"
#include "hashmap.h"


namespace v8 {


namespace i = v8::internal;


// A single counter in a counter collection.
class Counter {
 public:
  static const int kMaxNameSize = 64;
  int32_t* Bind(const char* name);
  int32_t* ptr() { return &counter_; }
  int32_t value() { return counter_; }
 private:
  int32_t counter_;
  uint8_t name_[kMaxNameSize];
};


// A set of counters and associated information.  An instance of this
// class is stored directly in the memory-mapped counters file if
// the --map-counters options is used
class CounterCollection {
 public:
  CounterCollection();
  Counter* GetNextCounter();
 private:
  static const unsigned kMaxCounters = 256;
  uint32_t magic_number_;
  uint32_t max_counters_;
  uint32_t max_name_size_;
  uint32_t counters_in_use_;
  Counter counters_[kMaxCounters];
};


class CounterMap {
 public:
  CounterMap(): hash_map_(Match) { }
  Counter* Lookup(const char* name) {
    i::HashMap::Entry* answer = hash_map_.Lookup(
        const_cast<char*>(name),
        Hash(name),
        false);
    if (!answer) return NULL;
    return reinterpret_cast<Counter*>(answer->value);
  }
  void Set(const char* name, Counter* value) {
    i::HashMap::Entry* answer = hash_map_.Lookup(
        const_cast<char*>(name),
        Hash(name),
        true);
    ASSERT(answer != NULL);
    answer->value = value;
  }
  class Iterator {
   public:
    explicit Iterator(CounterMap* map)
        : map_(&map->hash_map_), entry_(map_->Start()) { }
    void Next() { entry_ = map_->Next(entry_); }
    bool More() { return entry_ != NULL; }
    const char* CurrentKey() { return static_cast<const char*>(entry_->key); }
    Counter* CurrentValue() { return static_cast<Counter*>(entry_->value); }
   private:
    i::HashMap* map_;
    i::HashMap::Entry* entry_;
  };
 private:
  static int Hash(const char* name);
  static bool Match(void* key1, void* key2);
  i::HashMap hash_map_;
};


class Shell: public i::AllStatic {
 public:
  static bool ExecuteString(Handle<String> source,
                            Handle<Value> name,
                            bool print_result,
                            bool report_exceptions);
  static void ReportException(TryCatch* try_catch);
  static void Initialize();
  static void OnExit();
  static int* LookupCounter(const char* name);
  static void MapCounters(const char* name);
  static Handle<String> ReadFile(const char* name);
  static void RunShell();
  static int Main(int argc, char* argv[]);
  static Handle<Array> GetCompletions(Handle<String> text,
                                      Handle<String> full);
  static Handle<Object> DebugMessageDetails(Handle<String> message);
  static Handle<Value> DebugCommandToJSONRequest(Handle<String> command);

  static Handle<Value> Print(const Arguments& args);
  static Handle<Value> Yield(const Arguments& args);
  static Handle<Value> Quit(const Arguments& args);
  static Handle<Value> Version(const Arguments& args);
  static Handle<Value> Load(const Arguments& args);

  static Handle<Context> utility_context() { return utility_context_; }

  static const char* kHistoryFileName;
  static const char* kPrompt;
 private:
  static Persistent<Context> utility_context_;
  static Persistent<Context> evaluation_context_;
  static CounterMap* counter_map_;
  // We statically allocate a set of local counters to be used if we
  // don't want to store the stats in a memory-mapped file
  static CounterCollection local_counters_;
  static CounterCollection* counters_;
  static i::OS::MemoryMappedFile* counters_file_;
};


class LineEditor {
 public:
  enum Type { DUMB = 0, READLINE = 1 };
  LineEditor(Type type, const char* name);
  virtual ~LineEditor() { }

  virtual i::SmartPointer<char> Prompt(const char* prompt) = 0;
  virtual bool Open() { return true; }
  virtual bool Close() { return true; }
  virtual void AddHistory(const char* str) { }

  const char* name() { return name_; }
  static LineEditor* Get();
 private:
  Type type_;
  const char* name_;
  LineEditor* next_;
  static LineEditor* first_;
};


}  // namespace v8


#endif  // V8_D8_H_
