//===-------------- RemarkDiff.cpp ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Diffs remarks between two remark files.
/// The tool offers different modes for comparing two versions of remarks.
/// 1. Look through common remarks between two files.
/// 2. Compare the remark type. This is useful to check if an optimzation
/// changed from passing to failing.
/// 3. Compare remark arguments. This is useful to check if a remark argument
/// changed after some compiler change.
///
/// The results are presented as a json file.
///
//===----------------------------------------------------------------------===//
#include "RemarkDiff.h"
#include "RemarkUtilRegistry.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Regex.h"

using namespace llvm;
using namespace remarks;
using namespace llvm::remarkutil;

static cl::SubCommand DiffSub("diff",
                              "diff remarks based on specified properties.");
static cl::opt<std::string> RemarkFileA(cl::Positional,
                                        cl::desc("<remarka_file>"),
                                        cl::Required, cl::sub(DiffSub));
static cl::opt<std::string> RemarkFileB(cl::Positional,
                                        cl::desc("<remarkb_file>"),
                                        cl::Required, cl::sub(DiffSub));

static cl::opt<bool> Verbose(
    "v", cl::init(false),
    cl::desc("Output detailed difference for remarks. By default the tool will "
             "only show the remark name, type and location. If the flag is "
             "added we display the arguments that are different."),
    cl::sub(DiffSub));
static cl::opt<bool>
    ShowArgDiffOnly("show-arg-diff-only", cl::init(false),
                    cl::desc("Show only the remarks that have the same header "
                             "and differ in arguments"),
                    cl::sub(DiffSub));
static cl::opt<bool> OnlyShowCommonRemarks(
    "only-show-common-remarks", cl::init(false),
    cl::desc("Ignore any remarks that don't exist in both <remarka_file> and "
             "<remarkb_file>."),
    cl::sub(DiffSub));
static cl::opt<bool> ShowOnlyDifferentRemarks(
    "only-show-different-remarks", cl::init(false),
    cl::desc("Show remarks that are exclusively at either A or B"),
    cl::sub(DiffSub));
static cl::opt<bool> ShowRemarkTypeDiffOnly(
    "show-remark-type-diff-only", cl::init(false),
    cl::desc("Only show diff if remarks have the same header but different "
             "type"),
    cl::sub(DiffSub));
static cl::opt<bool> StrictCompare(
    "use-strict-compare", cl::init(false),
    cl::desc("By default remark arguments may contain location information. If "
             "the flag is added then it will display arguments that are "
             "different if the location differs."),
    cl::sub(DiffSub));

static cl::opt<Format> InputFormat(
    "parser", cl::desc("Input remark format to parse"),
    cl::values(clEnumValN(Format::YAML, "yaml", "YAML"),
               clEnumValN(Format::Bitstream, "bitstream", "Bitstream")),
    cl::sub(DiffSub));
static cl::opt<ReportStyleOptions> ReportStyle(
    "report_style", cl::sub(DiffSub),
    cl::init(ReportStyleOptions::human_output),
    cl::desc("Choose the report output format:"),
    cl::values(clEnumValN(human_output, "human", "Human-readable format"),
               clEnumValN(json_output, "json", "JSON format")));
static cl::opt<std::string> OutputFileName("o", cl::init("-"), cl::sub(DiffSub),
                                           cl::desc("Output"),
                                           cl::value_desc("file"));
FILTER_COMMAND_LINE_OPTIONS(DiffSub)
static bool OnlyShowArgOrTypeDiffRemarks = false;

void RemarkArgInfo::print(raw_ostream &OS) const {
  OS << Key << ": " << Val << "\n";
}

void RemarkInfo::printHeader(raw_ostream &OS) const {
  OS << "Name: " << RemarkName << "\n";
  OS << "FunctionName: " << FunctionName << "\n";
  OS << "PassName: " << PassName << "\n";
}

void RemarkInfo::print(raw_ostream &OS) const {
  printHeader(OS);
  OS << "Type: " << typeToStr(RemarkType) << "\n";
  if (!Args.empty()) {
    OS << "Args:\n";
    for (auto Arg : Args)
      OS << "\t" << Arg;
  }
}

void DiffAtRemark::print(raw_ostream &OS) {
  BaseRemark.printHeader(OS);
  if (RemarkTypeDiff) {
    OS << "Only at A >>>>\n";
    OS << "Type: " << typeToStr(RemarkTypeDiff->first) << "\n";
    OS << "=====\n";
    OS << "Only at B <<<<\n";
    OS << "Type: " << typeToStr(RemarkTypeDiff->second) << "\n";
    OS << "=====\n";
  }
  if (!OnlyA.empty()) {
    OS << "Only at A >>>>\n";
    unsigned Idx = 0;
    for (auto &R : OnlyA) {
      OS << R;
      if (Idx < OnlyA.size() - 1)
        OS << "\n";
      Idx++;
    }
    OS << "=====\n";
  }
  if (!OnlyB.empty()) {
    OS << "Only at B <<<<\n";
    unsigned Idx = 0;
    for (auto &R : OnlyB) {
      OS << R;
      if (Idx < OnlyB.size() - 1)
        OS << "\n";
      Idx++;
    }
    OS << "=====\n";
  }
  for (auto &R : InBoth)
    OS << R << "\n";
}

void DiffAtLoc::print(raw_ostream &OS) {
  if (!OnlyA.empty()) {
    OS << "Only at A >>>>\n";
    unsigned Idx = 0;
    for (auto &R : OnlyA) {
      OS << R;
      if (Idx < OnlyA.size() - 1)
        OS << "\n";
      Idx++;
    }
    OS << "=====\n";
  }
  if (!OnlyB.empty()) {
    OS << "Only at B <<<<\n";
    unsigned Idx = 0;
    for (auto &R : OnlyB) {
      OS << R;
      if (Idx < OnlyB.size() - 1)
        OS << "\n";
      Idx++;
    }
    OS << "=====\n";
  }
  if (!HasTheSameHeader.empty()) {
    OS << "--- Has the same header ---\n";
    for (auto &R : HasTheSameHeader)
      R.print(OS);
  }
}

/// \returns json array representation of a vecotor of remark arguments.
static json::Array remarkArgsToJson(std::vector<RemarkArgInfo> &Args) {
  json::Array ArgArray;
  for (auto Arg : Args) {
    json::Object ArgPair({{Arg.Key, Arg.Val}});
    ArgArray.push_back(std::move(ArgPair));
  }
  return ArgArray;
}

/// \returns remark representation as a json object.
static json::Object remarkToJSON(RemarkInfo &Remark) {
  json::Object RemarkJSON;
  RemarkJSON["RemarkName"] = Remark.RemarkName;
  RemarkJSON["FunctionName"] = Remark.FunctionName;
  RemarkJSON["PassName"] = Remark.PassName;
  RemarkJSON["RemarkType"] = typeToStr(Remark.RemarkType);
  if (Verbose)
    RemarkJSON["Args"] = remarkArgsToJson(Remark.Args);
  return RemarkJSON;
}

json::Object DiffAtRemark::toJson() {
  json::Object Object;
  Object["FunctionName"] = BaseRemark.FunctionName;
  Object["PassName"] = BaseRemark.PassName;
  Object["RemarkName"] = BaseRemark.RemarkName;
  // display remark type if it is the same between the two remarks.
  if (!RemarkTypeDiff)
    Object["RemarkType"] = typeToStr(BaseRemark.RemarkType);
  json::Array InBothJSON;
  json::Array OnlyAJson;
  json::Array OnlyBJson;
  for (auto Arg : InBoth) {
    json::Object ArgPair({{Arg.Key, Arg.Val}});
    InBothJSON.push_back(std::move(ArgPair));
  }
  for (auto Arg : OnlyA) {
    json::Object ArgPair({{Arg.Key, Arg.Val}});
    OnlyAJson.push_back(std::move(ArgPair));
  }
  for (auto Arg : OnlyB) {
    json::Object ArgPair({{Arg.Key, Arg.Val}});
    OnlyBJson.push_back(std::move(ArgPair));
  }
  json::Object Diff;
  if (RemarkTypeDiff) {
    Diff["RemarkTypeA"] = typeToStr(RemarkTypeDiff->first);
    Diff["RemarkTypeB"] = typeToStr(RemarkTypeDiff->second);
  }
  // // Only display common remark arguments if verbose is passed.
  // if (Verbose)
  //   Object["ArgsInBoth"] = remarkArgsToJson(
  //       std::vector<std::string>{InBoth.begin(), InBoth.end()});
  // if (!OnlyAJson.empty())
  //   Diff["ArgsAtA"] = remarkArgsToJson(OnlyA);
  // if (!OnlyBJson.empty())
  //   Diff["ArgsAtB"] = remarkArgsToJson(OnlyB);
  // Object["Diff"] = std::move(Diff);
  return Object;
}
json::Object DiffAtLoc::toJson() {

  json::Object Obj;
  json::Array DiffObj;
  json::Array OnlyAObj;
  json::Array OnlyBObj;
  json::Array HasSameHeaderObj;
  for (auto R : OnlyA)
    OnlyAObj.push_back(remarkToJSON(R));
  for (auto R : OnlyB)
    OnlyBObj.push_back(remarkToJSON(R));
  for (auto R : HasTheSameHeader)
    HasSameHeaderObj.push_back(R.toJson());
  // Obj["Location"] = Loc;
  if (!OnlyShowCommonRemarks) {
    Obj["OnlyA"] = std::move(OnlyAObj);
    Obj["OnlyB"] = std::move(OnlyBObj);
  }
  Obj["HasSameHeaderObj"] = std::move(HasSameHeaderObj);
  return Obj;
}
static Error parseRemarkFile(
    std::unique_ptr<RemarkParser> &Parser,
    MapVector<DebugLocation, SmallVector<RemarkInfo, 4>> &DebugLoc2RemarkMap,
    Filters &Filter) {
  auto MaybeRemark = Parser->next();
  for (; MaybeRemark; MaybeRemark = Parser->next()) {
    auto &Remark = **MaybeRemark;
    if (!Filter.filterRemark(Remark))
      continue;
    std::string SourceFilePath = "";
    unsigned SourceLine = 0;
    unsigned SourceColumn = 0;
    if (Remark.Loc.has_value()) {
      SourceFilePath = Remark.Loc->SourceFilePath.str();
      SourceLine = Remark.Loc->SourceLine;
      SourceColumn = Remark.Loc->SourceColumn;
    }

    DebugLocation Key(SourceFilePath, Remark.FunctionName, SourceLine,
                      SourceColumn);
    auto Iter = DebugLoc2RemarkMap.insert({Key, {}});
    Iter.first->second.push_back(Remark);
  }
  auto E = MaybeRemark.takeError();
  if (!E.isA<remarks::EndOfFileError>())
    return E;
  consumeError(std::move(E));
  return Error::success();
}

/// \returns DiffAtRemark object where it will look through the arguments and
/// remark type in RA and RB.
/// \p RA remark from file a.
/// \p RB remark from file b.
static DiffAtRemark computeArgDiffAtRemark(RemarkInfo &RA, RemarkInfo &RB) {
  DiffAtRemark Diff(RA);
  unsigned ArgIdx = 0;
  // Loop through the remarks in RA and RB in order comparing both.
  for (; ArgIdx < std::min(RA.Args.size(), RB.Args.size()); ArgIdx++) {
    if (RA.Args[ArgIdx] == (RB.Args[ArgIdx]))
      Diff.InBoth.push_back(RA.Args[ArgIdx]);
    else {
      Diff.OnlyA.push_back(RA.Args[ArgIdx]);
      Diff.OnlyB.push_back(RB.Args[ArgIdx]);
    }
  }

  // Add the remaining remarks if they exist to OnlyA or OnlyB.
  std::vector<RemarkArgInfo> RemainingArgs =
      RA.Args.size() > RB.Args.size() ? RA.Args : RB.Args;
  bool IsARemaining = RA.Args.size() > RB.Args.size() ? true : false;
  for (; ArgIdx < RemainingArgs.size(); ArgIdx++)
    if (IsARemaining)
      Diff.OnlyA.push_back(RemainingArgs[ArgIdx]);
    else
      Diff.OnlyB.push_back(RemainingArgs[ArgIdx]);

  // Compare remark type between RA and RB.
  if (RA.RemarkType != RB.RemarkType)
    Diff.RemarkTypeDiff = {RA.RemarkType, RB.RemarkType};
  return Diff;
}

static void computeDiffAtLoc(SmallVector<RemarkInfo, 4> &RemarksA,
                             SmallVector<RemarkInfo, 4> &RemarksB,
                             DiffAtLoc &DiffLoc) {
  // A set of remarks where either they have a remark at the other file equaling
  // them or share the same header. This is used to reduce the duplicates when
  // looking at a location. If a remark has a counterpart in the other file then
  // we aren't interested if it shares the same header with another remark.
  SmallSet<RemarkInfo, 4> FoundRemarks;
  SmallVector<std::pair<RemarkInfo, RemarkInfo>, 4> HasSameHeader;
  // First look through the remarks that are exactly equal in the two files.
  for (auto &RA : RemarksA)
    for (auto &RB : RemarksB)
      if (RA == RB)
        FoundRemarks.insert(RA);
  for (auto &RA : RemarksA) {
    // skip
    if (FoundRemarks.contains(RA))
      continue;
    for (auto &RB : RemarksB) {
      if (FoundRemarks.contains(RB))
        continue;
      if (RA.hasSameHeader(RB)) {
        HasSameHeader.push_back({RA, RB});
        FoundRemarks.insert(RA);
        FoundRemarks.insert(RB);
      }
    }
  }

  for (auto &RA : RemarksA) {
    if (!FoundRemarks.contains(RA) && !OnlyShowCommonRemarks)
      DiffLoc.OnlyA.push_back(RA);
  }
  for (auto &RB : RemarksB) {
    if (!FoundRemarks.contains(RB) && !OnlyShowCommonRemarks)
      DiffLoc.OnlyB.push_back(RB);
  }
  if (ShowOnlyDifferentRemarks)
    return;
  for (auto &[RA, RB] : HasSameHeader) {
    if (!OnlyShowArgOrTypeDiffRemarks)
      DiffLoc.HasTheSameHeader.push_back(computeArgDiffAtRemark(RA, RB));
    // If -show-remark-type-diff-only is true only compare remarks that differ
    // in type.
    else if (ShowRemarkTypeDiffOnly && RA.RemarkType != RB.RemarkType)
      DiffLoc.HasTheSameHeader.push_back(computeArgDiffAtRemark(RA, RB));
    // If `show-arg-diff-only` is true only compare remarks that differ in
    // arguments.
    else if (ShowArgDiffOnly && RA.RemarkType == RB.RemarkType)
      DiffLoc.HasTheSameHeader.push_back(computeArgDiffAtRemark(RA, RB));
  }
}

static void computeDiff(
    SetVector<DebugLocation> &DebugLocs,
    MapVector<DebugLocation, SmallVector<RemarkInfo, 4>> &DebugLoc2RemarkA,
    MapVector<DebugLocation, SmallVector<RemarkInfo, 4>> &DebugLoc2RemarkB,
    SmallVector<DiffAtLoc, 4> &DiffAtLocs) {
  // Add all debug locs from file a and file b to a unique set of Locations.
  for (const DebugLocation &Loc : DebugLocs) {
    SmallVector<RemarkInfo, 4> RemarksLocAIt = DebugLoc2RemarkA.lookup(Loc);
    SmallVector<RemarkInfo, 4> RemarksLocBIt = DebugLoc2RemarkB.lookup(Loc);
    DiffAtLoc DiffLoc;
    DiffLoc.Loc = Loc;
    SmallVector<RemarkInfo, 4> EmptyRemarks;
    computeDiffAtLoc(RemarksLocAIt, RemarksLocBIt, DiffLoc);
    DiffAtLocs.push_back(DiffLoc);
  }
}

static Error printDiff(StringRef InputFileNameA, StringRef InputFileNameB,
                       SmallVector<DiffAtLoc, 4> &LocsDiff) {
  // Create the output buffer.
  auto MaybeOF = getOutputFileWithFlags(OutputFileName,
                                        /*Flags = */ sys::fs::OF_TextWithCRLF);
  if (!MaybeOF)
    return MaybeOF.takeError();
  std::unique_ptr<ToolOutputFile> OF = std::move(*MaybeOF);
  for (auto LocDiff : LocsDiff) {
    if (LocDiff.isEmpty())
      continue;
    OF->os() << "----------\n";
    OF->os() << LocDiff.Loc.SourceFilePath << ":" << LocDiff.Loc.FunctionName
             << "  Ln " << LocDiff.Loc.SourceLine << " Col "
             << LocDiff.Loc.SourceColumn << "\n";
    LocDiff.print(OF->os());
  }
#if 0
  json::Object Output;
  json::Object Files(
      {{"A", InputFileNameA.str()}, {"B", InputFileNameB.str()}});
  Output["Files"] = std::move(Files);
  json::Array DiffArr;
  for (auto LocDiff : LocsDiff)
    DiffArr.push_back(LocDiff.toJson());
  Output["Diff"] = std::move(DiffArr);
  json::OStream JOS(OF->os(), 2);
  JOS.value(std::move(Output));
  OF->os() << '\n';
#endif
  OF->keep();
  return Error::success();
}

Expected<Filters> getRemarkFilter2() {
  // Create Filter properties.
  std::optional<FilterMatcher> RemarkNameFilter;
  std::optional<FilterMatcher> PassNameFilter;
  std::optional<FilterMatcher> RemarkArgFilter;
  std::optional<Type> RemarkType;
  if (!RemarkNameOpt.empty())
    RemarkNameFilter = {RemarkNameOpt, false};
  else if (!RemarkNameOptRE.empty())
    RemarkNameFilter = {RemarkNameOptRE, true};
  if (!PassNameOpt.empty())
    PassNameFilter = {PassNameOpt, false};
  else if (!PassNameOptRE.empty())
    PassNameFilter = {PassNameOptRE, true};
  // Create RemarkFilter.
  return Filters::createRemarkFilter(std::move(RemarkNameFilter),
                                     std::move(PassNameFilter),
                                     std::move(RemarkArgFilter), RemarkType);
}

static Error createRemarkDiff() {
  // Get memory buffer for file a and file b.
  auto RemarkAMaybeBuf = getInputMemoryBuffer(RemarkFileA);
  if (!RemarkAMaybeBuf)
    return RemarkAMaybeBuf.takeError();
  auto RemarkBMaybeBuf = getInputMemoryBuffer(RemarkFileB);
  if (!RemarkBMaybeBuf)
    return RemarkBMaybeBuf.takeError();
  StringRef BufferA = (*RemarkAMaybeBuf)->getBuffer();
  StringRef BufferB = (*RemarkBMaybeBuf)->getBuffer();
  // Create parsers for file a and file b remarks.
  auto MaybeParser1 = createRemarkParserFromMeta(InputFormat, BufferA);
  if (!MaybeParser1)
    return MaybeParser1.takeError();
  auto MaybeParser2 = createRemarkParserFromMeta(InputFormat, BufferB);
  if (!MaybeParser2)
    return MaybeParser2.takeError();
  auto MaybeFilter = getRemarkFilter2();
  if (!MaybeFilter)
    return MaybeFilter.takeError();
  auto &Filter = *MaybeFilter;
  // Order the remarks based on their debug location and function name.
  MapVector<DebugLocation, SmallVector<RemarkInfo, 4>> DebugLoc2RemarkA;
  MapVector<DebugLocation, SmallVector<RemarkInfo, 4>> DebugLoc2RemarkB;
  if (auto E = parseRemarkFile(*MaybeParser1, DebugLoc2RemarkA, Filter))
    return E;
  if (auto E = parseRemarkFile(*MaybeParser2, DebugLoc2RemarkB, Filter))
    return E;
  SetVector<DebugLocation> DebugLocs;
  for (const auto &[Loc, _] : DebugLoc2RemarkA)
    DebugLocs.insert(Loc);
  for (const auto &[Loc, _] : DebugLoc2RemarkB)
    DebugLocs.insert(Loc);
  SmallVector<DiffAtLoc, 4> LocsDiff;
  computeDiff(DebugLocs, DebugLoc2RemarkA, DebugLoc2RemarkB, LocsDiff);
  if (auto E = printDiff(RemarkFileA, RemarkFileB, LocsDiff))
    return E;
  return Error::success();
}

static CommandRegistration DiffReg(&DiffSub, createRemarkDiff);