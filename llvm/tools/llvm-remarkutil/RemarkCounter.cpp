//===- RemarkCounter.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Generic tool to count remarks based on properties
//
//===----------------------------------------------------------------------===//
#include "RemarkCounter.h"
#include "RemarkUtilRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Regex.h"

using namespace llvm;
using namespace remarks;
using namespace llvm::remarkutil;

static cl::SubCommand CountSub("count",
                               "Collect remarks based on specified criteria");

INPUT_FORMAT_COMMAND_LINE_OPTIONS(CountSub)
INPUT_OUTPUT_COMMAND_LINE_OPTIONS(CountSub)

static cl::list<std::string> Keys("keys", cl::desc("Specify key(es) to count."),
                                  cl::value_desc("keys"), cl::sub(CountSub),
                                  cl::ValueOptional);
static cl::list<std::string>
    RKeys("rkeys",
          cl::desc("Specify key(es) to count using regular expression."),
          cl::value_desc("keys"), cl::sub(CountSub), cl::ValueOptional);
FILTER_COMMAND_LINE_OPTIONS(CountSub)
static cl::opt<CountBy> CountByOpt(
    "count-by", cl::desc("Specify the property to collect remarks by"),
    cl::values(
        clEnumValN(
            CountBy::REMARK, "remark-name",
            "Counts individual remarks based on how many of the remark exists"),
        clEnumValN(CountBy::KEYS, "key",
                   "Counts based on the value each specified key has. The key "
                   "has to have a number value to be considered.")),
    cl::init(CountBy::REMARK), cl::sub(CountSub));
static cl::opt<GroupBy> GroupByOpt(
    "group-by", cl::desc("Specify the property to groub remarks by"),
    cl::values(
        clEnumValN(
            GroupBy::PER_SOURCE, "source",
            "Display the count broken down by the filepath of each remark "
            "emitted. Requires remarks to have DebugLoc information."),
        clEnumValN(GroupBy::PER_FUNCTION, "function",
                   "Breakdown the count by function name."),
        clEnumValN(
            GroupBy::PER_FUNCTION_WITH_DEBUG_LOC, "function-with-loc",
            "Breakdown the count by function name taking into consideration "
            "the filepath info from the DebugLoc of the remark."),
        clEnumValN(GroupBy::TOTAL, "total",
                   "Output the total number corresponding to the count for the "
                   "provided input file.")),
    cl::init(GroupBy::PER_SOURCE), cl::sub(CountSub));

/// Look for matching argument for the key in the remark and return the parsed
/// integer value
static unsigned getValForKey(StringRef Key, const Remark &Remark) {
  auto *RemarkArg = find_if(Remark.Args, [&Key](const Argument &Arg) {
    return Arg.Key == Key && Arg.isValInt();
  });
  if (RemarkArg == Remark.Args.end())
    return 0;
  return *RemarkArg->getValAsInt();
}

Error KeyCounter::getAllKeysInRemarks(StringRef Buffer,
                                      SmallVector<FilterMatcher, 4> &Keys,
                                      Filters &Filter) {
  auto MaybeParser = createRemarkParser(InputFormat, Buffer);
  if (!MaybeParser)
    return MaybeParser.takeError();
  auto &Parser = **MaybeParser;
  auto MaybeRemark = Parser.next();
  for (; MaybeRemark; MaybeRemark = Parser.next()) {
    auto &Remark = **MaybeRemark;
    // Only collect keys from remarks included in the filter.
    if (!Filter.filterRemark(Remark))
      continue;
    for (auto &Key : Keys) {
      for (Argument Arg : Remark.Args)
        if (Key.match(Arg.Key) && Arg.isValInt())
          KeySetIdxMap.insert({Arg.Key, KeySetIdxMap.size()});
    }
  }

  auto E = MaybeRemark.takeError();
  if (!E.isA<EndOfFileError>())
    return E;
  consumeError(std::move(E));
  return Error::success();
}

std::optional<std::string> Counter::getGroupByKey(const Remark &Remark) {

  std::string Key;
  switch (GroupBy) {
  case GroupBy::PER_FUNCTION:
    return Key = Remark.FunctionName;
  case GroupBy::TOTAL:
    return Key = "Total";
  case GroupBy::PER_SOURCE:
  case GroupBy::PER_FUNCTION_WITH_DEBUG_LOC:
    if (!Remark.Loc.has_value())
      return std::nullopt;

    if (GroupBy == GroupBy::PER_FUNCTION_WITH_DEBUG_LOC)
      return Remark.Loc->SourceFilePath.str() + ":" + Remark.FunctionName.str();
    return Remark.Loc->SourceFilePath.str();
  }
}

void KeyCounter::collect(const Remark &Remark) {
  SmallVector<int, 4> Row(KeySetIdxMap.size());
  std::optional<std::string> GroupByKey = getGroupByKey(Remark);
  // Early return if we don't have a value
  if (!GroupByKey.has_value())
    return;
  auto GroupVal = *GroupByKey;
  CountByKeysMap.insert({GroupVal, Row});
  for (auto [Key, Idx] : KeySetIdxMap) {
    auto Count = getValForKey(Key, Remark);
    CountByKeysMap[GroupVal][Idx] += Count;
  }
}

void RemarkCounter::collect(const Remark &Remark) {
  std::optional<std::string> Key = getGroupByKey(Remark);
  if (!Key.has_value())
    return;
  auto Iter = CountedByRemarksMap.insert({*Key, 1});
  if (!Iter.second)
    Iter.first->second += 1;
}

Error KeyCounter::print(StringRef OutputFileName) {
  auto MaybeOF =
      getOutputFileWithFlags(OutputFileName, sys::fs::OF_TextWithCRLF);
  if (!MaybeOF)
    return MaybeOF.takeError();

  auto OF = std::move(*MaybeOF);
  OF->os() << groupByToStr(GroupBy) << ",";
  unsigned Idx = 0;
  for (auto [Key, _] : KeySetIdxMap) {
    OF->os() << Key;
    if (Idx != KeySetIdxMap.size() - 1)
      OF->os() << ",";
    Idx++;
  }
  OF->os() << "\n";

  for (auto [Header, Row] : CountByKeysMap) {
    OF->os() << Header << ",";
    unsigned Idx = 0;
    for (auto Count : Row) {
      OF->os() << Count;
      if (Idx != KeySetIdxMap.size() - 1)
        OF->os() << ",";
      Idx++;
    }
    OF->os() << "\n";
  }
  return Error::success();
}

Error RemarkCounter::print(StringRef OutputFileName) {
  auto MaybeOF =
      getOutputFileWithFlags(OutputFileName, sys::fs::OF_TextWithCRLF);
  if (!MaybeOF)
    return MaybeOF.takeError();

  auto OF = std::move(*MaybeOF);
  OF->os() << groupByToStr(GroupBy) << ","
           << "Count\n";
  for (auto [Key, Count] : CountedByRemarksMap)
    OF->os() << Key << "," << Count << "\n";
  OF->keep();
  return Error::success();
}

Expected<Filters> getRemarkFilter() {
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
  if (RemarkTypeOpt != Type::Failure)
    RemarkType = RemarkTypeOpt;
  if (!RemarkFilterArgByOpt.empty())
    RemarkArgFilter = {RemarkFilterArgByOpt, false};
  else if (!RemarkArgFilterOptRE.empty())
    RemarkArgFilter = {RemarkArgFilterOptRE, true};
  // Create RemarkFilter.
  return Filters::createRemarkFilter(std::move(RemarkNameFilter),
                                     std::move(PassNameFilter),
                                     std::move(RemarkArgFilter), RemarkType);
}

Error useCollectRemark(StringRef Buffer, Counter &Counter, Filters &Filter) {
  // Create Parser.
  auto MaybeParser = createRemarkParser(InputFormat, Buffer);
  if (!MaybeParser)
    return MaybeParser.takeError();
  auto &Parser = **MaybeParser;
  auto MaybeRemark = Parser.next();
  for (; MaybeRemark; MaybeRemark = Parser.next()) {
    const Remark &Remark = **MaybeRemark;
    if (Filter.filterRemark(Remark))
      Counter.collect(Remark);
  }

  if (auto E = Counter.print(OutputFileName))
    return E;
  auto E = MaybeRemark.takeError();
  if (!E.isA<EndOfFileError>())
    return E;
  consumeError(std::move(E));
  return Error::success();
}

static Error collectRemarks() {
  // Create a parser for the user-specified input format.
  auto MaybeBuf = getInputMemoryBuffer(InputFileName);
  if (!MaybeBuf)
    return MaybeBuf.takeError();
  StringRef Buffer = (*MaybeBuf)->getBuffer();
  auto MaybeFilter = getRemarkFilter();
  if (!MaybeFilter)
    return MaybeFilter.takeError();
  auto &Filter = *MaybeFilter;
  if (CountByOpt == CountBy::REMARK) {
    RemarkCounter RC(GroupByOpt);
    if (auto E = useCollectRemark(Buffer, RC, Filter))
      return E;
  } else if (CountByOpt == CountBy::KEYS) {
    SmallVector<FilterMatcher, 4> KeysVector;
    if (!Keys.empty()) {
      for (auto &Key : Keys)
        KeysVector.push_back({Key, false});
    } else if (!RKeys.empty())
      for (auto Key : RKeys)
        KeysVector.push_back({Key, true});
    else
      KeysVector.push_back({".*", true});

    Expected<KeyCounter> KC =
        KeyCounter::createKeyCounter(GroupByOpt, KeysVector, Buffer, Filter);
    if (!KC)
      return KC.takeError();
    if (auto E = useCollectRemark(Buffer, *KC, Filter))
      return E;
  }
  return Error::success();
}

static CommandRegistration CountReg(&CountSub, collectRemarks);
