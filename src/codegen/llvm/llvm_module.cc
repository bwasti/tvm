/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 *  Copyright (c) 2017 by Contributors
 * \file llvm_module.cc
 * \brief LLVM runtime module for TVM
 */
#ifdef TVM_LLVM_VERSION
#include <tvm/runtime/packed_func.h>
#include <tvm/codegen.h>
#include <mutex>
#include "llvm_common.h"
#include "codegen_llvm.h"
#include "../../runtime/file_util.h"
#include "../../runtime/module_util.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/SymbolSize.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/MachO.h"

#include <fstream>
#include <iostream>
#include <unistd.h>
#include <iostream>

namespace tvm {
namespace codegen {

using runtime::TVMArgs;
using runtime::TVMRetValue;
using runtime::PackedFunc;

using namespace llvm;
using namespace object;

static unsigned getSectionID(const ObjectFile &O, SectionRef Sec) {
   if (auto *M = dyn_cast<MachOObjectFile>(&O))
     return M->getSectionID(Sec);
   auto d = dyn_cast<COFFObjectFile>(&O);
   if (!d) { return 0; }
   return d->getSectionID(Sec);
 }
static unsigned getSymbolSectionID(const ObjectFile &O, SymbolRef Sym) {
   if (auto *M = dyn_cast<MachOObjectFile>(&O))
     return M->getSymbolSectionID(Sym);
   auto d = dyn_cast<COFFObjectFile>(&O);
   if (!d) { return 0; }
   return d->getSymbolSectionID(Sym);
 }

std::vector<std::pair<SymbolRef, uint64_t>> hcomputeSymbolSizes(const ObjectFile &O) {
   std::vector<std::pair<SymbolRef, uint64_t>> Ret;
 
   // Collect sorted symbol addresses. Include dummy addresses for the end
   // of each section.
   std::vector<SymEntry> Addresses;
   unsigned SymNum = 0;
   for (symbol_iterator I = O.symbol_begin(), E = O.symbol_end(); I != E; ++I) {
     SymbolRef Sym = *I;
     uint64_t Value = Sym.getValue();
     Addresses.push_back({I, Value, SymNum, getSymbolSectionID(O, Sym)});
     ++SymNum;
   }
   for (SectionRef Sec : O.sections()) {
     uint64_t Address = Sec.getAddress();
     uint64_t Size = Sec.getSize();
     Addresses.push_back(
         {O.symbol_end(), Address + Size, 0, getSectionID(O, Sec)});
   }
 
   if (Addresses.empty())
     return Ret;
 
   array_pod_sort(Addresses.begin(), Addresses.end(), compareAddress);
 
   // Compute the size as the gap to the next symbol
   for (unsigned I = 0, N = Addresses.size() - 1; I < N; ++I) {
     auto &P = Addresses[I];
     if (P.I == O.symbol_end())
       continue;
 
     // If multiple symbol have the same address, give both the same size.
     unsigned NextI = I + 1;
     while (NextI < N && Addresses[NextI].Address == P.Address)
       ++NextI;
 
     uint64_t Size = Addresses[NextI].Address - P.Address;
     P.Address = Size;
   }
 
   // Assign the sorted symbols in the original order.
   Ret.resize(SymNum);
   for (SymEntry &P : Addresses) {
     if (P.I == O.symbol_end())
       continue;
     Ret[P.Number] = {*P.I, P.Address};
   }
   return Ret;
 }

struct PerfMapEntry {
  PerfMapEntry(std::string s, uint64_t add, uint64_t size_) : symbol(s), addr(add), size(size_) {}
  std::string symbol;
  uint64_t addr;
  uint64_t size;
};

class HandrolledPerfJITEventListener : public llvm::JITEventListener {
 private:
  std::vector<PerfMapEntry>* perf_map_;
 public:
  HandrolledPerfJITEventListener(std::vector<PerfMapEntry>* perf_map) : perf_map_(perf_map) { }
  ~HandrolledPerfJITEventListener() = default;
  void notifyObjectLoaded(llvm::JITEventListener::ObjectKey K, const llvm::object::ObjectFile &Obj,
      const llvm::RuntimeDyld::LoadedObjectInfo &L) override {
    for (const std::pair<llvm::object::SymbolRef, uint64_t> &P : hcomputeSymbolSizes(Obj)) {
      llvm::object::SymbolRef Sym = P.first;
      llvm::Expected<llvm::StringRef> Name = Sym.getName();
      if (!Name) {
        llvm::consumeError(Name.takeError());
        continue;
      }
      llvm::Expected<uint64_t> AddrOrErr = Sym.getAddress();
      if (!AddrOrErr) {
        llvm::consumeError(AddrOrErr.takeError());
        continue;
      }
      uint64_t Addr = AddrOrErr.get();
      auto sec = Sym.getSection();
      if (!sec) {
        llvm::consumeError(sec.takeError());
        continue;
      }
      if (*sec == Obj.section_end()) {
        continue;
      }
      auto section = *sec.get();
      uint64_t global_addr = Addr + L.getSectionLoadAddress(section);
      uint64_t sec_size = section.getSize();
      uint64_t Size = P.second;
      perf_map_->emplace_back(Name->str(), global_addr, Size);
    }
  }

  void notifyFreeingObject(llvm::JITEventListener::ObjectKey K) override {
  }
};

class LLVMModuleNode final : public runtime::ModuleNode {
 public:
  ~LLVMModuleNode() {
    module_.reset();
    if (ee_ != nullptr) {
      ee_->runStaticConstructorsDestructors(true);
      delete ee_;
    }
  }

  const char* type_key() const {
    return "llvm";
  }

  PackedFunc GetFunction(
      const std::string& name,
      const std::shared_ptr<ModuleNode>& sptr_to_self) final {
    if (name == "__tvm_is_system_module") {
      bool flag =
          (mptr_->getFunction("__tvm_module_startup") != nullptr);
      return PackedFunc([flag](TVMArgs args, TVMRetValue *rv) {
          * rv = flag;
        });
    }
    if (ee_ == nullptr) LazyInitJIT();
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string& fname = (name == runtime::symbol::tvm_module_main ?
                                entry_func_ : name);

    BackendPackedCFunc faddr =
        reinterpret_cast<BackendPackedCFunc>(GetFunctionAddr(fname));

    if (faddr == nullptr) return PackedFunc();
    return WrapPackedFunc(faddr, sptr_to_self);
  }

  void SaveToFile(const std::string& file_name,
                  const std::string& format) final {
    std::string fmt = runtime::GetFileFormat(file_name, format);
    std::error_code ecode;
    llvm::raw_fd_ostream dest(file_name, ecode, llvm::sys::fs::F_None);
    CHECK_EQ(ecode.value(), 0) << "Cannot open file: " << file_name
                               << " " << ecode.message();
    if (fmt == "o" || fmt == "obj") {
#if TVM_LLVM_VERSION <= 60
      std::unique_ptr<llvm::Module> m = llvm::CloneModule(mptr_);
#else
      std::unique_ptr<llvm::Module> m = llvm::CloneModule(*mptr_);
#endif
      llvm::legacy::PassManager pass;
      CHECK(tm_);
#if TVM_LLVM_VERSION <= 60
      CHECK(tm_->addPassesToEmitFile(
          pass, dest, llvm::TargetMachine::CGFT_ObjectFile) == 0)
          << "Cannot emit target CGFT_ObjectFile";
#else
      CHECK(tm_->addPassesToEmitFile(
          pass, dest, nullptr, llvm::TargetMachine::CGFT_ObjectFile) == 0)
          << "Cannot emit target CGFT_ObjectFile";
#endif
      pass.run(*m);
    } else if (fmt == "s" || fmt == "asm") {
#if TVM_LLVM_VERSION <= 60
      std::unique_ptr<llvm::Module> m = llvm::CloneModule(mptr_);
#else
      std::unique_ptr<llvm::Module> m = llvm::CloneModule(*mptr_);
#endif
      llvm::legacy::PassManager pass;
      CHECK(tm_);
#if TVM_LLVM_VERSION <= 60
      CHECK(tm_->addPassesToEmitFile(
          pass, dest, llvm::TargetMachine::CGFT_AssemblyFile) == 0)
          << "Cannot emit target CGFT_AssemblyFile";
#else
      CHECK(tm_->addPassesToEmitFile(
          pass, dest, nullptr, llvm::TargetMachine::CGFT_AssemblyFile) == 0)
          << "Cannot emit target CGFT_AssemblyFile";
#endif
      pass.run(*m);
    } else if (fmt == "ll") {
      mptr_->print(dest, nullptr);
    } else if (fmt == "bc") {
#if TVM_LLVM_VERSION <= 60
      llvm::WriteBitcodeToFile(mptr_, dest);
#else
      llvm::WriteBitcodeToFile(*mptr_, dest);
#endif
    } else {
      LOG(FATAL) << "Do not know how to save file "
                 << file_name << " with format=\'"<< format << "\'";
    }
    dest.close();
  }

  void SaveToBinary(dmlc::Stream* stream) final {
    LOG(FATAL) << "LLVMModule: SaveToBinary not supported";
  }

  std::string GetSource(const std::string& format) final {
    std::string fmt = runtime::GetFileFormat("", format);
    std::string type_str;
    llvm::SmallString<256> str;
    llvm::raw_svector_ostream rso(str);

    if (fmt == "s" || fmt == "asm") {
    #if TVM_LLVM_VERSION <= 60
          std::unique_ptr<llvm::Module> m = llvm::CloneModule(mptr_);
    #else
          std::unique_ptr<llvm::Module> m = llvm::CloneModule(*mptr_);
    #endif
          llvm::legacy::PassManager pass;
          CHECK(tm_);
    #if TVM_LLVM_VERSION <= 60
          CHECK(tm_->addPassesToEmitFile(
              pass, rso, llvm::TargetMachine::CGFT_AssemblyFile) == 0)
              << "Cannot emit target CGFT_AssemblyFile";
    #else
          CHECK(tm_->addPassesToEmitFile(
              pass, rso, nullptr, llvm::TargetMachine::CGFT_AssemblyFile) == 0)
              << "Cannot emit target CGFT_AssemblyFile";
    #endif
          pass.run(*m);
          return rso.str().str();
    } else if (fmt == "" || fmt == "ll") {
      std::string type_str;
      llvm::raw_string_ostream rso(type_str);
      CHECK(mptr_ != nullptr);
      mptr_->print(rso, nullptr);
      return rso.str();
    } else {
      LOG(FATAL) << "Do not know how to get source code with format: "
                 << format << "\'";
    }
    return "";
  }

  void Init(const Array<LoweredFunc>& funcs, std::string target) {
    InitializeLLVM();
    tm_ = GetLLVMTargetMachine(target);
    bool system_lib = (target.find("-system-lib") != std::string::npos);
    CHECK_NE(funcs.size(), 0U);
    ctx_ = std::make_shared<llvm::LLVMContext>();
    std::unique_ptr<CodeGenLLVM> cg = CodeGenLLVM::Create(tm_.get());
    entry_func_ = funcs[0]->name;
    cg->Init(funcs[0]->name, tm_.get(), ctx_.get(), system_lib, system_lib);
    for (LoweredFunc f :  funcs) {
      cg->AddFunction(f);
    }
    cg->AddMainFunction(funcs[0]->name);
    module_ = cg->Finish();
    std::string verify_errors_storage;
    llvm::raw_string_ostream verify_errors(verify_errors_storage);
    LOG_IF(FATAL, llvm::verifyModule(*module_, &verify_errors))
        << "LLVM module verification failed with the following errors: \n"
        << verify_errors.str();
    module_->addModuleFlag(
        llvm::Module::Warning, "tvm_target",
        llvm::MDString::get(*ctx_, target));

    module_->addModuleFlag(llvm::Module::Override, "Debug Info Version",
                            llvm::DEBUG_METADATA_VERSION);
    module_->addModuleFlag(llvm::Module::Override, "Dwarf Version", 2);
    target_ = target;
    mptr_ = module_.get();
  }

  void LoadIR(const std::string& file_name) {
    InitializeLLVM();
    ctx_ = std::make_shared<llvm::LLVMContext>();
    llvm::SMDiagnostic err;
    module_ = llvm::parseIRFile(file_name, err, *ctx_);
    if (module_.get() == nullptr) {
      std::string msg = err.getMessage();
      LOG(FATAL) << "Fail to load ir file " << file_name << "\n"
                 << "line " << err.getLineNo() << ":" << msg;
    }
    std::string target_;
    llvm::Metadata* mtarget = module_->getModuleFlag("tvm_target");
    if (mtarget != nullptr) {
      llvm::MDString* pstr = llvm::dyn_cast<llvm::MDString>(mtarget);
      CHECK(pstr != nullptr);
      target_ = pstr->getString();
    } else {
      std::ostringstream os;
      os << "llvm -target " << module_->getTargetTriple();
      target_ = os.str();
    }
    mptr_ = module_.get();
    tm_ = GetLLVMTargetMachine(target_);
  }

 private:
  void processPerfMap(const std::vector<PerfMapEntry>& perf_map) {
    std::stringstream ss_perf_map;
    ss_perf_map << "/tmp/perf-" << getpid() << ".map";
    auto perf_map_path = ss_perf_map.str();
    auto tmp_perf_map_path = perf_map_path + ".tmp";
    std::ofstream tmp_perf_map_file;
    tmp_perf_map_file.open(tmp_perf_map_path, std::fstream::out | std::fstream::trunc);
    for (const auto& entry : perf_map) {
      if (entry.size == 0 || entry.addr == 0) continue;
      tmp_perf_map_file << std::hex << entry.addr << " " << entry.size << " " << entry.symbol << "\n";
    }
    tmp_perf_map_file.close();
    CHECK(std::rename(tmp_perf_map_path.c_str(), perf_map_path.c_str()) == 0);
  }

  void LazyInitJIT() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ee_) {
      return;
    }
    llvm::EngineBuilder builder(std::move(module_));
    std::string triple, mcpu, mattr;
    llvm::TargetOptions opt;
    ParseLLVMTargetOptions(target_, &triple, &mcpu, &mattr, &opt);
    builder.setEngineKind(llvm::EngineKind::JIT);
    builder.setOptLevel(llvm::CodeGenOpt::Aggressive);
    if (mcpu.length() != 0) {
      builder.setMCPU(mcpu);
    }
    if (mattr.length() != 0) {
      std::vector<std::string> mattrs{mattr};
      builder.setMAttrs(mattrs);
    }
    builder.setTargetOptions(opt);
    auto tm = std::unique_ptr<llvm::TargetMachine>(builder.selectTarget());
    std::unique_ptr<llvm::TargetMachine> tm_sys = GetLLVMTargetMachine("llvm");
    if (tm_sys->getTargetTriple().getArch() != tm->getTargetTriple().getArch()) {
      LOG(FATAL) << "Cannot run module, architecture mismatch "
                 << " module=" << tm->getTargetTriple().str()
                 << " system=" << tm_sys->getTargetTriple().str();
    }
    llvm::DataLayout layout(tm->createDataLayout());
    CHECK(layout == mptr_->getDataLayout())
        << "Data layout mismatch between module("
        << mptr_->getDataLayout().getStringRepresentation() << ")"
        << " and ExecutionEngine ("
        << layout.getStringRepresentation() << ")";
    ee_ = builder.create(tm.release());
    CHECK(ee_ != nullptr)
        << "Failed to initialize git engine for " << mptr_->getTargetTriple();
    std::vector<PerfMapEntry> perf_map;
    auto pel = HandrolledPerfJITEventListener(&perf_map);
    ee_->RegisterJITEventListener(&pel);
    ee_->runStaticConstructorsDestructors(false);
    // setup context address.
    entry_func_ =
        reinterpret_cast<const char*>(GetGlobalAddr(runtime::symbol::tvm_module_main));
    processPerfMap(perf_map);
    if (void** ctx_addr = reinterpret_cast<void**>(
            GetGlobalAddr(runtime::symbol::tvm_module_ctx))) {
      *ctx_addr = this;
    }
    runtime::InitContextFunctions([this](const char *name) {
        return GetGlobalAddr(name);
      });
  }
  // Get global address from execution engine.
  uint64_t GetGlobalAddr(const std::string& name) {
    // first verifies if GV exists.
    if (mptr_->getGlobalVariable(name) != nullptr) {
      return ee_->getGlobalValueAddress(name);
    } else {
      return 0;
    }
  }
  uint64_t GetFunctionAddr(const std::string& name) {
    // first verifies if GV exists.
    if (mptr_->getFunction(name) != nullptr) {
      return ee_->getFunctionAddress(name);
    } else {
      return 0;
    }
  }

  // The target configuration string
  std::string target_;
  // Name of entry function.
  std::string entry_func_;
  // JIT lock
  std::mutex mutex_;
  // execution engine
  llvm::ExecutionEngine *ee_{nullptr};
  // The raw pointer to the module.
  llvm::Module* mptr_{nullptr};
  // The target machine
  std::unique_ptr<llvm::TargetMachine> tm_{nullptr};
  // The module, can be moved to ee if JIT is enabled.
  std::unique_ptr<llvm::Module> module_;
  // the context.
  std::shared_ptr<llvm::LLVMContext> ctx_;
};

unsigned LookupLLVMIntrinsic(const std::string& name) {
  return llvm::Function::lookupIntrinsicID(name);
}

TVM_REGISTER_API("codegen.llvm_lookup_intrinsic_id")
.set_body([](TVMArgs args, TVMRetValue* rv) {
    *rv = static_cast<int64_t>(LookupLLVMIntrinsic(args[0]));
  });

TVM_REGISTER_API("codegen.build_llvm")
.set_body([](TVMArgs args, TVMRetValue* rv) {
    std::shared_ptr<LLVMModuleNode> n = std::make_shared<LLVMModuleNode>();
    n->Init(args[0], args[1]);
    *rv = runtime::Module(n);
  });

TVM_REGISTER_API("codegen.llvm_version_major")
.set_body([](TVMArgs args, TVMRetValue* rv) {
    std::ostringstream os;
    int major = TVM_LLVM_VERSION / 10;
    *rv = major;
  });

TVM_REGISTER_API("module.loadfile_ll")
.set_body([](TVMArgs args, TVMRetValue* rv) {
    std::shared_ptr<LLVMModuleNode> n = std::make_shared<LLVMModuleNode>();
    n->LoadIR(args[0]);
    *rv = runtime::Module(n);
  });

TVM_REGISTER_API("codegen.llvm_target_enabled")
.set_body([](TVMArgs args, TVMRetValue* rv) {
    InitializeLLVM();
    *rv = (GetLLVMTargetMachine(args[0], true) != nullptr);
  });
}  // namespace codegen
}  // namespace tvm
#endif  // TVM_LLVM_VERSION
