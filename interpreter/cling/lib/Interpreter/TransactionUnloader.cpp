//------------------------------------------------------------------------------
// CLING - the C++ LLVM-based InterpreterG :)
// author:  Vassil Vassilev <vvasilev@cern.ch>
//
// This file is dual-licensed: you can choose to license it under the University
// of Illinois Open Source License or the GNU Lesser General Public License. See
// LICENSE.TXT for details.
//------------------------------------------------------------------------------

#include "TransactionUnloader.h"

#include "IncrementalExecutor.h"
#include "DeclUnloader.h"

#include "cling/Interpreter/Interpreter.h"
#include "cling/Interpreter/Transaction.h"

#include "clang/AST/Decl.h"
#include "clang/AST/DependentDiagnostic.h"
#include "clang/Sema/Sema.h"

using namespace clang;

namespace cling {
  bool TransactionUnloader::unloadDeclarations(Transaction* T,
                                               clang::DeclUnloader& DeclU) {
    bool Successful = true;

    for (Transaction::const_reverse_iterator I = T->rdecls_begin(),
           E = T->rdecls_end(); I != E; ++I) {
      const Transaction::ConsumerCallInfo& Call = I->m_Call;
      const DeclGroupRef& DGR = (*I).m_DGR;

      if (Call == Transaction::kCCIHandleVTable)
        continue;
      // The non templated classes come through HandleTopLevelDecl and
      // HandleTagDeclDefinition, this is why we need to filter.
      if (Call == Transaction::kCCIHandleTagDeclDefinition)
      if (const CXXRecordDecl* D
        = dyn_cast<CXXRecordDecl>(DGR.getSingleDecl()))
      if (D->getTemplateSpecializationKind() == TSK_Undeclared)
        continue;

      if (Call == Transaction::kCCINone)
        m_Interp->unload(*(*T->rnested_begin()));

      for (DeclGroupRef::const_iterator
             Di = DGR.end() - 1, E = DGR.begin() - 1; Di != E; --Di) {
        // Get rid of the declaration. If the declaration has name we should
        // heal the lookup tables as well
        Successful = DeclU.UnloadDecl(*Di) && Successful;
#ifndef NDEBUG
        assert(Successful && "Cannot handle that yet!");
#endif
      }
    }
    assert(T->rnested_begin() == T->rnested_end()
           && "nested transactions mismatch");
    return Successful;
  }

  bool TransactionUnloader::unloadFromPreprocessor(Transaction* T,
                                                   clang::DeclUnloader& DeclU) {
    bool Successful = true;
    for (Transaction::const_reverse_macros_iterator MI = T->rmacros_begin(),
           ME = T->rmacros_end(); MI != ME; ++MI) {
      // Get rid of the macro definition
      Successful = DeclU.UnloadMacro(*MI) && Successful;
#ifndef NDEBUG
      assert(Successful && "Cannot handle that yet!");
#endif
    }
    return Successful;
  }

  bool TransactionUnloader::unloadDeserializedDeclarations(Transaction* T,
                                                   clang::DeclUnloader& DeclU) {
    //FIXME: Terrible hack, we *must* get rid of parseForModule by implementing
    // a header file generator in cling.
    bool Successful = true;
    for (Transaction::const_reverse_iterator I = T->deserialized_rdecls_begin(),
           E = T->deserialized_rdecls_end(); I != E; ++I) {
      const DeclGroupRef& DGR = (*I).m_DGR;
      for (DeclGroupRef::const_iterator
             Di = DGR.end() - 1, E = DGR.begin() - 1; Di != E; --Di) {
        // We only want to revert all that came through parseForModule, and
        // not the PCH.
        if (!(*Di)->isFromASTFile())
          Successful = DeclU.UnloadDecl(*Di) && Successful;
#ifndef NDEBUG
        assert(Successful && "Cannot handle that yet!");
#endif
      }
    }
    return Successful;
  }

  bool TransactionUnloader::RevertTransaction(Transaction* T) {
    DeclUnloader DeclU(m_Sema, m_CodeGen, T);

    bool Successful = unloadDeclarations(T, DeclU);
    Successful = unloadFromPreprocessor(T, DeclU) && Successful;
    Successful = unloadDeserializedDeclarations(T, DeclU) && Successful;

#ifndef NDEBUG
    //FIXME: Move the nested transaction marker out of the decl lists and
    // reenable this assertion.
    //size_t DeclSize = std::distance(T->decls_begin(), T->decls_end());
    //if (T->getCompilationOpts().CodeGenerationForModule)
    //  assert (!DeclSize && "No parsed decls must happen in parse for module");
#endif

    // Clean up the pending instantiations
    m_Sema->PendingInstantiations.clear();
    m_Sema->PendingLocalImplicitInstantiations.clear();

    // Cleanup the module from unused global values.
    // if (T->getModule()) {
    //   llvm::ModulePass* globalDCE = llvm::createGlobalDCEPass();
    //   globalDCE->runOnModule(*T->getModule());
    // }

    if (getExecutor() && T->getModule())
      Successful = getExecutor()->unloadFromJIT(T->getModule(),
                                                T->getExeUnloadHandle())
        && Successful;

    if (Successful)
      T->setState(Transaction::kRolledBack);
    else
      T->setState(Transaction::kRolledBackWithErrors);

    return Successful;
  }

  bool TransactionUnloader::UnloadDecl(Decl* D) {
    return cling::UnloadDecl(m_Sema, m_CodeGen, D);
  }
} // end namespace cling

