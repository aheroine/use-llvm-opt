#include <list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "Arch64or32bit.h"
#include "DirectoryHelper.h"
#include "Compat.h"
#include "FunctionDeclarations.h"
#include "MackeKTest.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/IR/TypeBuilder.h"
#include "llvm/IR/Function.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace
{

static llvm::cl::opt<std::string> PrependToFunction(
    "prependtofunction",
    llvm::cl::desc("Name of the function that is prepended with the errors"));

static llvm::cl::list<std::string> PreviousKleeRunDirectory(
    "previouskleerundirectory",
    llvm::cl::desc("klee-out-XX directory of a previous run"));

static llvm::cl::list<std::string> ErrorFileToPrepend(
    "errorfiletoprepend",
    llvm::cl::desc("klee .err file, that should be prepended"));

bool IsValidKTest(const std::unordered_map<std::string, llvm::Value *> &variablemap, const MackeKTest &ktest)
{
  if (ktest.hadError)
    return false;
  // For each variable defined in the ktest objecct
  for (auto &kobj : ktest.objects)
  {
    // Ignore all variables starting with MACKE and
    //  model_version, A-data, A-data-stat,
    //  stdin, stdin-stat from posix environment
    if (kobj.name.substr(0, 6) != "macke_" &&
        kobj.name != "model_version" && kobj.name != "A-data" &&
        kobj.name != "A-data-stat" && kobj.name != "B-data" &&
        kobj.name != "B-data-stat" && kobj.name != "C-data" &&
        kobj.name != "C-data-stat" && kobj.name != "stdin" &&
        kobj.name != "stdin-stat" && kobj.name != "macke_noname" &&
        kobj.name != "argv")
    { //jl modified for pass the ktest generated by use with some more para:argv of no use
      // Search for a matching variable in the function
      auto search = variablemap.find(kobj.name);
      if (search == variablemap.end())
        return false;
    }
  }

  /* Remove false positives that occurred because the object was empty */
  return true;
}

struct PrependError : public llvm::ModulePass
{
  static char ID; // uninitialized ID is needed for pass registration

  PrependError() : llvm::ModulePass(ID)
  {
    /* empty constructor, just call the parent's one */
  }

  bool runOnModule(llvm::Module &M)
  {

    FunctionType *srand_type = TypeBuilder<void(unsigned int), false>::get(M.getContext());

    Function *srand_func = cast<Function>(M.getOrInsertFunction("srand", srand_type));

    FunctionType *rand_type = TypeBuilder<int(), false>::get(M.getContext());

    Function *rand_func = cast<Function>(M.getOrInsertFunction("rand", rand_type));

    FunctionType *time_type = TypeBuilder<int64_t(int64_t *), false>::get(M.getContext());
    Function *time_func = cast<Function>(M.getOrInsertFunction("time", time_type));

    FunctionType *exit_type = TypeBuilder<void(int), false>::get(M.getContext());
    Function *exit_func = cast<Function>(M.getOrInsertFunction("exit", exit_type));

    // Check all the command line arguments
    if (PrependToFunction.empty())
    {
      llvm::errs() << "Error: -prependtofunction paramter is needed!" << '\n';
      return false;
    }

    if (PreviousKleeRunDirectory.empty() && ErrorFileToPrepend.empty())
    {
      llvm::errs() << "Error: -previouskleerundirectory paramter or "
                   << " -errorfiletoprepend parameter is needed!" << '\n';
      return false;
    }

    for (auto &pkrd : PreviousKleeRunDirectory)
    {
      if (!is_valid_directory(pkrd.c_str()))
      {
        llvm::errs() << "Error: " << pkrd << " is not a valid directory"
                     << '\n';
        return false;
      }
    }

    for (auto &eftp : ErrorFileToPrepend)
    {
      if (!hasEnding(eftp.c_str(), ".err") || !is_valid_file(eftp.c_str()))
      {
        llvm::errs() << "Error: " << eftp << " is not a valid .err-file"
                     << '\n';
        return false;
      }
    }

    // Look for the function to be encapsulated
    llvm::Function *backgroundFunc = M.getFunction(PrependToFunction);

    // Check if the function given by the user really exists
    if (backgroundFunc == nullptr)
    {
      llvm::errs() << "Error: " << PrependToFunction
                   << " is no function inside the module. " << '\n'
                   << "Prepend is not possible!" << '\n';
      return false;
    }

    // Get builder for the whole module
    llvm::IRBuilder<> modulebuilder(M.getContext());

    // Register relevant functions
    llvm::Function *kleegetobjsize = declare_klee_get_obj_size(&M);

    // Create the function to be prepended
    llvm::Function *prependedFunc = llvm::Function::Create(
        backgroundFunc->getFunctionType(), backgroundFunc->getLinkage(),
        "__macke_error_" + backgroundFunc->getName(), &M);

    // Give the correct name to all the arguments
    auto oldarg = backgroundFunc->arg_begin();
    std::unordered_map<std::string, llvm::Value *> variablemap;
    for (auto &newarg : prependedFunc->getArgumentList())
    {
      auto oldname = getArgumentName(&M, oldarg);
      newarg.setName(oldname);
      variablemap[oldname] = &newarg;
      ++oldarg;
    }

    // Mark the prepended function as not inline-able
    prependedFunc->addFnAttr(llvm::Attribute::NoInline);
    prependedFunc->addFnAttr(llvm::Attribute::OptimizeNone);
    // And also mark the background function - its needed for targeted search
    backgroundFunc->addFnAttr(llvm::Attribute::NoInline);
    backgroundFunc->addFnAttr(llvm::Attribute::OptimizeNone);

    // Create a basic block in the prepended function
    llvm::BasicBlock *block =
        llvm::BasicBlock::Create(M.getContext(), "", prependedFunc);
    llvm::IRBuilder<> builder(block);

    llvm::BasicBlock *defaultblock =
        llvm::BasicBlock::Create(M.getContext(), "", prependedFunc);

    llvm::IRBuilder<> defaultbuilder(defaultblock);

    llvm::BasicBlock *caseblock = llvm::BasicBlock::Create(M.getContext(), "", prependedFunc);
    llvm::IRBuilder<> casebuilder(caseblock);

    // Replace all calls to the original function with the prepended function
    backgroundFunc->replaceAllUsesWith(prependedFunc);

    // Build argument list for the prepended function
    std::vector<llvm::Value *> newargs = {};
    for (auto &argument : prependedFunc->getArgumentList())
    {
      newargs.push_back(&argument);
    }

    // Return the result of a original call to the function
    llvm::Instruction *origcall = defaultbuilder.CreateCall(
        backgroundFunc, llvm::ArrayRef<llvm::Value *>(newargs));

    if (backgroundFunc->getFunctionType()->getReturnType()->isVoidTy())
    {
      defaultbuilder.CreateRetVoid();
    }
    else
    {
      defaultbuilder.CreateRet(origcall);
    }

    // Read all required test data
    std::list<std::pair<std::string, std::string>> errlist = {};

    // From complete directories
    for (auto &pkrd : PreviousKleeRunDirectory)
    {
      auto newtests = errors_and_ktests_from_dir(pkrd);
      for (auto &nt : newtests)
      {
        errlist.push_back(nt);
      }
    }

    // From explicitly named error files
    for (auto &eftp : ErrorFileToPrepend)
    {
      errlist.push_back(std::make_pair(eftp, corresponding_ktest(eftp)));
    }

    for (auto &errfile : errlist)
    {
      // Load the date from the corresponding ktest file
      MackeKTest ktest = MackeKTest(errfile.second.c_str());
      if (!IsValidKTest(variablemap, ktest))
      {
        // Print ktest for debugging purposes
        // llvm::errs() << "KTest '" << errfile.second << "' is invalid.\n";
        continue;
      }

      // Block for comparing the error size
      llvm::BasicBlock *checkcontentblock =
          llvm::BasicBlock::Create(M.getContext(), "", prependedFunc);
      llvm::IRBuilder<> checkcontentbuilder(checkcontentblock);

      llvm::BasicBlock *errblock = llvm::BasicBlock::Create(M.getContext(), "", prependedFunc);
      llvm::BasicBlock *noterrblock = llvm::BasicBlock::Create(M.getContext(), "", prependedFunc);

      // The value for checking, if the sizes does match
      llvm::Value *correctsize = casebuilder.getTrue();
      // The value for checking, if the content does match
      llvm::Value *correctcontent = casebuilder.getTrue();

      // For each variable defined in the ktest objecct
      for (auto &kobj : ktest.objects)
      {
        // Ignore all variables starting with MACKE and
        //  model_version, A-data, A-data-stat,
        //  stdin, stdin-stat from posix environment
        if (kobj.name.substr(0, 6) != "macke_" &&
            kobj.name != "model_version" && kobj.name != "A-data" &&
            kobj.name != "A-data-stat" && kobj.name != "B-data" &&
            kobj.name != "B-data-stat" && kobj.name != "C-data" &&
            kobj.name != "C-data-stat" && kobj.name != "stdin" &&
            kobj.name != "stdin-stat" && kobj.name != "macke_noname" &&
            kobj.name != "argv")
        {
          //jl modify:because the ktest generated by use conclude argv of no use, here we pass it
          // Search for a matching variable in the function
          auto search = variablemap.find(kobj.name);
          if (search != variablemap.end())
          {
            llvm::Value *fvalue = search->second;
            llvm::Value *fvalueptr;

            if (fvalue->getType()->isPointerTy())
            {
              // If it is already a pointer, just keep working with it
              fvalueptr = fvalue;
            }
            else
            {
              // If its not a pointer, we add one level of indirection
              fvalueptr = casebuilder.CreateAlloca(fvalue->getType());
              casebuilder.CreateStore(fvalue, fvalueptr);
            }

            // Cast the pointer to void*
            fvalueptr = casebuilder.CreateBitCast(
                fvalueptr, builder.getInt8Ty()->getPointerTo());

            // Compare the object size
            correctsize = casebuilder.CreateAnd(
                correctsize, casebuilder.CreateICmpEQ(
                                 casebuilder.CreateCall(
                                     kleegetobjsize,
                                     llvm::ArrayRef<llvm::Value *>(fvalueptr)),
                                 getInt(kobj.value.size(), &M, &casebuilder)));

            // Compare the object content
            for (int i = 0; i < kobj.value.size(); i++)
            {
              // Get element pointer with the right element offset,
              // load the memory, and  Compare it
              correctcontent = checkcontentbuilder.CreateAnd(
                  correctcontent,
                  checkcontentbuilder.CreateICmpEQ(
                      checkcontentbuilder.CreateLoad(
                          checkcontentbuilder.CreateConstGEP1_64(fvalueptr, i)),
                      checkcontentbuilder.getInt8(kobj.value[i])));
            }
          }
          else
          {
            llvm::errs() << "ERROR: KTest variable " << kobj.name
                         << " not found in function" << '\n';
            abort();
          }
        }
      }

      auto init = builder.CreateCall(time_func, {Constant::getNullValue(Type::getInt64PtrTy(M.getContext()))});
      
      auto trunc_init = builder.CreateTrunc(init,Type::getInt32Ty(M.getContext()));
      
      builder.CreateCall(srand_func, {trunc_init});

      auto r = builder.CreateCall(rand_func);

      auto fr = builder.CreateFDiv(builder.CreateUIToFP(r, Type::getDoubleTy(M.getContext())),
                                   builder.CreateUIToFP(builder.getInt32(RAND_MAX),
                                                        Type::getDoubleTy(M.getContext())));

      auto half = ConstantFP::get(Type::getDoubleTy(M.getContext()), 0.5);

      auto predict = builder.CreateFCmpOGT(fr, half);

      builder.CreateCondBr(predict, defaultblock, caseblock);

      casebuilder.CreateCondBr(correctsize, checkcontentblock, noterrblock);

      // check content block
      checkcontentbuilder.CreateCondBr(correctcontent, errblock, noterrblock);

      // The error reporting if block
      casebuilder.SetInsertPoint(errblock);
      // The no error else block
       Value *zero = casebuilder.getInt32(0);
      casebuilder.CreateCall(exit_func, llvm::ArrayRef<Value *>{zero});
      casebuilder.CreateUnreachable();

      // prepare for the next summary
      caseblock = noterrblock;
      casebuilder.SetInsertPoint(caseblock);
     
    }
    Value *zero = casebuilder.getInt32(0);
    casebuilder.CreateCall(exit_func, llvm::ArrayRef<Value *>{zero});
    casebuilder.CreateUnreachable();

    return true;
  }
};

} // namespace

// Finally register the new pass
char PrependError::ID = 0;
static llvm::RegisterPass<PrependError> X(
    "preprenderror",
    "Add a new main that calls a function with symbolic arguments",
    false, // walks CFG without modifying it?
    false  // is only analysis?
);
