// $Id$
#include "UnnecessaryIncludeFinder.h"
#include "clang/Basic/FileManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>
#include <utility>

using namespace clang;
using namespace llvm;

void
SourceFile::traverse (SourceVisitor& visitor, bool shouldVisitThis)
{
  if (shouldVisitThis && !visitor.visit(this)) {
    return;
  }

  for (IncludeDirectives::iterator ppInclude = includeDirectives_.begin();
      ppInclude != includeDirectives_.end();
      ++ppInclude)
  {
    IncludeDirective::Ptr pIncludeDirective(*ppInclude);
    pIncludeDirective->pHeader_->traverse(visitor, true);
  }
}

typedef UsedHeaders VisitedHeaders;

/**
 * Checks if any of the headers included by this source file are used.
 */
class UsedHeaderFinder: public SourceVisitor
{
  const UsedHeaders& usedHeaders_;
  VisitedHeaders visitedHeaders_;

public:
  bool found_;

  UsedHeaderFinder (const UsedHeaders& usedHeaders):
    usedHeaders_(usedHeaders),
    found_(false)
  { }

  virtual bool visit (SourceFile* pSource)
  {
    VisitedHeaders::key_type fileName(pSource->name());
    if (visitedHeaders_.count(fileName)) {
      return false;
    }
    visitedHeaders_.insert(fileName);

    if (usedHeaders_.count(fileName)) {
      found_ = true;
      return false;
    }
    return true;
  }
};

bool
SourceFile::haveNestedUsedHeader (const UsedHeaders& usedHeaders)
{
  UsedHeaderFinder finder(usedHeaders);
  traverse(finder, false);
  return finder.found_;
}

/**
 * Reports the headers included by the source file that are used.
 */
class UsedHeaderReporter: public SourceVisitor
{
  const UsedHeaders& usedHeaders_;
  SourceManager& sourceManager_;
  VisitedHeaders visitedHeaders_;

public:
  UsedHeaderReporter (
      const UsedHeaders& usedHeaders, SourceManager& sourceManager):
    usedHeaders_(usedHeaders),
    sourceManager_(sourceManager)
  { }

  virtual bool visit (SourceFile* pSource)
  {
    VisitedHeaders::key_type fileName(pSource->name());
    if (visitedHeaders_.count(fileName)) {
      return false;
    }
    visitedHeaders_.insert(fileName);

    if (usedHeaders_.count(fileName)) {
      std::cout << std::endl << "  " << fileName;
    }
    return true;
  }
};

void
SourceFile::reportNestedUsedHeaders (
    const UsedHeaders& usedHeaders, SourceManager& sourceManager)
{
  UsedHeaderReporter reporter(usedHeaders, sourceManager);
  traverse(reporter, false);
}

bool
SourceFile::reportUnnecessaryIncludes (
    const UsedHeaders& allUsedHeaders, SourceManager& sourceManager)
{
  bool foundUnnecessary = false;

  for (IncludeDirectives::iterator ppInclude = includeDirectives_.begin();
      ppInclude != includeDirectives_.end();
      ++ppInclude)
  {
    IncludeDirective::Ptr pIncludeDirective(*ppInclude);

    SourceFile::Ptr pHeader = pIncludeDirective->pHeader_;
    if (!usedHeaders_.count(pHeader->name())) {
      SourceFile::Ptr pSource(this);
      bool haveNestedUsedHeader = pHeader->haveNestedUsedHeader(allUsedHeaders);
      if (haveNestedUsedHeader && pIncludeDirective->angled_) {
        // This header is unused but one of headers it includes is used.
        // Don't complain if the #include directive surrounded the file name
        // with angle brackets.
        continue;
      }

      foundUnnecessary = true;
      std::cout << pIncludeDirective->directiveLocation_
          << ": warning: #include "
          << (pIncludeDirective->angled_ ? '<' : '"')
          << pIncludeDirective->fileName_
          << (pIncludeDirective->angled_ ? '>' : '"')
          << ' ';
      if (haveNestedUsedHeader) {
        std::cout << "is optional "
            "but it includes these used headers:";
        pHeader->reportNestedUsedHeaders(allUsedHeaders, sourceManager);
      } else {
        std::cout << "is unnecessary";
      }

      std::cout << std::endl;
    }
  }

  return foundUnnecessary;
}

namespace {

class PreprocessorCallbacks: public clang::PPCallbacks
{
  UnnecessaryIncludeFinder& delegate_;

public:
  PreprocessorCallbacks (UnnecessaryIncludeFinder& delegate):
    delegate_(delegate)
  { }

  virtual void InclusionDirective (
      SourceLocation hashLoc,
      const clang::Token& includeToken,
      StringRef fileName,
      bool isAngled,
      const FileEntry* pFile,
      SourceLocation endLoc,
      const SmallVectorImpl<char>& rawPath)
  {
    delegate_.InclusionDirective(
        hashLoc, includeToken, fileName, isAngled, pFile, endLoc, rawPath);
  }

  virtual void FileChanged (
      SourceLocation newLocation,
      PPCallbacks::FileChangeReason reason,
      SrcMgr::CharacteristicKind fileType)
  {
    delegate_.FileChanged(newLocation, reason, fileType);
  }

  virtual void FileSkipped (
      const FileEntry& file,
      const Token& fileNameToken,
      SrcMgr::CharacteristicKind fileType)
  {
    delegate_.FileSkipped(file, fileNameToken, fileType);
  }

  virtual void MacroExpands (const Token& nameToken, const MacroInfo* pMacro)
  {
    delegate_.MacroExpands(nameToken, pMacro);
  }
};

}//namespace

PPCallbacks*
UnnecessaryIncludeFinder::createPreprocessorCallbacks ()
{
  return new PreprocessorCallbacks(*this);
}

void
UnnecessaryIncludeFinder::markUsed (
    SourceLocation declarationLocation, SourceLocation usageLocation)
{
  // Is the symbol declared in an included file and is it being used in the
  // main file?
  if (usageLocation.isInvalid() || !isFromMainFile(usageLocation)) {
    return;
  }

  if (declarationLocation.isInvalid()) {
    return;
  }

  FileID declarationFileID = sourceManager_.getFileID(declarationLocation);
  if (sourceManager_.getSLocEntry(declarationFileID).isFile() == false) {
    return;
  }

  if (declarationFileID != sourceManager_.getMainFileID()) {
    const FileEntry* pFile = sourceManager_.getFileEntryForID(
        declarationFileID);
    if (pFile == 0) {
      return;
    }

    UsedHeaders::key_type fileName(pFile->getName());
    pMainSource_->usedHeaders_.insert(fileName);
    action_.allUsedHeaders_.insert(fileName);
  }
}

void
UnnecessaryIncludeFinder::InclusionDirective (
    SourceLocation hashLoc,
    const Token& includeToken,
    StringRef fileName,
    bool isAngled,
    const FileEntry* pFile,
    SourceLocation endLoc,
    const SmallVectorImpl<char>& rawPath)
{
  std::string directiveLocation;
  raw_string_ostream rso(directiveLocation);
  hashLoc.print(rso, sourceManager_);
  rso.flush();

  // Remember #include directive that included the file.
  IncludeDirective::Ptr pIncludeDirective(
      new IncludeDirective(directiveLocation, fileName, isAngled));

  fileToIncludeDirectiveMap_.erase(pFile);
  fileToIncludeDirectiveMap_.insert(std::make_pair(pFile, pIncludeDirective));
}

SourceFile::Ptr
UnnecessaryIncludeFinder::getSource (const FileEntry* pFile)
{
  FileToSourceMap::iterator pPair = fileToSourceMap_.find(pFile);
  if (pPair != fileToSourceMap_.end()) {
    return pPair->second;
  }

  SourceFile::Ptr pSource(
      (pFile == 0)
      ? new SourceFile("") : new SourceFile(pFile->getName()));
  fileToSourceMap_.insert(std::make_pair(pFile, pSource));
  return pSource;
}

SourceFile::Ptr
UnnecessaryIncludeFinder::enterHeader (const FileEntry* pFile)
{
  SourceFile::Ptr pHeader = getSource(pFile);

  // Find the #include directive that included this header.
  FileToIncludeDirectiveMap::iterator pPair =
      fileToIncludeDirectiveMap_.find(pFile);
  IncludeDirective::Ptr pIncludeDirective(pPair->second);

  // The #include directive did not have the header during construction.
  // Set it now.
  pIncludeDirective->pHeader_ = pHeader;

  // Remember the parent file included this file.
  SourceFile::Ptr pParentSource(includeStack_.back());
  pParentSource->includeDirectives_.push_back(pIncludeDirective);

  return pHeader;
}

void
UnnecessaryIncludeFinder::FileChanged (
    SourceLocation newLocation,
    PPCallbacks::FileChangeReason reason,
    SrcMgr::CharacteristicKind fileType)
{
  if (reason == PPCallbacks::EnterFile) {
    FileID newFileID = sourceManager_.getFileID(newLocation);
    const FileEntry* pFile = sourceManager_.getFileEntryForID(newFileID);
    if (pFile != 0) {
      if (newFileID == sourceManager_.getMainFileID()) {
        // Entering main source file for the first time.
        pMainSource_ = getSource(pFile);
        action_.mainSources_.push_back(pMainSource_);
        includeStack_.clear();
        includeStack_.push_back(pMainSource_);
      } else {
        // Push new header onto include stack.
        SourceFile::Ptr pHeader(enterHeader(pFile));
        includeStack_.push_back(pHeader);
      }
    } else {
      // Entering built-in source.  There's no real file.  Push a dummy source
      // file onto the include stack so there's something to pop when exiting
      // the file.
      SourceFile::Ptr pHeader(getSource(0));
      includeStack_.push_back(pHeader);
    }
  } else if (reason == PPCallbacks::ExitFile) {
    // Pop include stack.
    includeStack_.pop_back();
  }
}

void
UnnecessaryIncludeFinder::FileSkipped (
      const FileEntry& file,
      const Token& fileNameToken,
      SrcMgr::CharacteristicKind fileType)
{
  enterHeader(&file);
}

void
UnnecessaryIncludeFinder::MacroExpands (
    const Token& nameToken, const MacroInfo* pMacro)
{
  // Ignore expansion of builtin macros like __LINE__.
  if (pMacro->isBuiltinMacro() == false) {
    markUsed(pMacro->getDefinitionLoc(), nameToken.getLocation());
  }
}

void
UnnecessaryIncludeFinder::HandleTranslationUnit (ASTContext& astContext)
{
  TraverseDecl(astContext.getTranslationUnitDecl());
}

bool
UnnecessaryIncludeFinder::VisitTypedefTypeLoc (TypedefTypeLoc typeLoc)
{
  markUsed(typeLoc.getTypedefDecl()->getLocation(), typeLoc.getBeginLoc());
  return true;
}

bool
UnnecessaryIncludeFinder::VisitTagTypeLoc(TagTypeLoc typeLoc)
{
  markUsed(typeLoc.getDecl()->getLocation(), typeLoc.getBeginLoc());
  return true;
}

bool
UnnecessaryIncludeFinder::VisitTemplateSpecializationTypeLoc (
    TemplateSpecializationTypeLoc typeLoc)
{
  CXXRecordDecl* pCXXRecordDecl = typeLoc.getTypePtr()->getAsCXXRecordDecl();
  if (pCXXRecordDecl) {
    markUsed(pCXXRecordDecl->getLocation(), typeLoc.getTemplateNameLoc());
  }
  return true;
}

bool
UnnecessaryIncludeFinder::VisitDeclRefExpr (DeclRefExpr* pExpr)
{
  markUsed(pExpr->getDecl()->getLocation(), pExpr->getLocation());
  return true;
}

bool
UnnecessaryIncludeFinder::VisitMemberExpr (MemberExpr* pExpr)
{
  markUsed(pExpr->getMemberDecl()->getLocation(), pExpr->getMemberLoc());
  return true;
}

bool
UnnecessaryIncludeFinder::VisitCXXMemberCallExpr (CXXMemberCallExpr* pExpr)
{
  if (pExpr->getMethodDecl() != 0) {
    markUsed(pExpr->getMethodDecl()->getLocation(), pExpr->getExprLoc());
  }
  return true;
}

ASTConsumer*
UnnecessaryIncludeFinderAction::CreateASTConsumer (
    CompilerInstance& compiler, StringRef inputFile)
{
  UnnecessaryIncludeFinder* pFinder = new UnnecessaryIncludeFinder(
      *this, compiler.getSourceManager());

  compiler.getPreprocessor().addPPCallbacks(
      pFinder->createPreprocessorCallbacks());

  return pFinder;
}

bool
UnnecessaryIncludeFinderAction::reportUnnecessaryIncludes (
    SourceManager& sourceManager)
{
  bool foundUnnecessary = false;

  for (SourceFiles::iterator ppSource = mainSources_.begin();
      ppSource != mainSources_.end();
      ++ppSource)
  {
    SourceFile::Ptr pMainSource(*ppSource);

    bool found = pMainSource->reportUnnecessaryIncludes(
        allUsedHeaders_, sourceManager);
    if (found) {
      foundUnnecessary = true;
    }
  }

  return foundUnnecessary;
}
