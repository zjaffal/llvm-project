//===- RemarkUtilHelpers.cpp ----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Helpers for remark utilites
//
//===----------------------------------------------------------------------===//
#include "RemarkUtilHelpers.h"

namespace llvm {
namespace remarks {

/// Convert Regex string error to an error object.
Error checkRegex(Regex &Regex) {
  std::string Error;
  if (!Regex.isValid(Error))
    return createStringError(make_error_code(std::errc::invalid_argument),
                             Twine("Regex: ", Error));
  return Error::success();
}

Error Filters::regexArgumentsValid() {
  if (RemarkNameFilter.has_value() && RemarkNameFilter->IsRegex)
    if (auto E = checkRegex(std::get<Regex>(RemarkNameFilter->FilterRE)))
      return E;
  if (PassNameFilter.has_value() && PassNameFilter->IsRegex)
    if (auto E = checkRegex(std::get<Regex>(PassNameFilter->FilterRE)))
      return E;
  if (ArgFilter.has_value() && ArgFilter->IsRegex)
    if (auto E = checkRegex(std::get<Regex>(ArgFilter->FilterRE)))
      return E;
  return Error::success();
}

bool Filters::filterRemark(const Remark &Remark) {
  if (RemarkNameFilter.has_value())
    if (!RemarkNameFilter->match(Remark.RemarkName))
      return false;
  if (PassNameFilter)
    if (!PassNameFilter->match(Remark.PassName))
      return false;
  if (RemarkTypeFilter.has_value())
    return *RemarkTypeFilter == Remark.RemarkType;
  if (ArgFilter) {
    bool IsMatch = false;
    for (const auto &Arg : Remark.Args)
      IsMatch |= ArgFilter->match(Arg.Val);
    if (!IsMatch)
      return false;
  }
  return true;
}

/// \returns A MemoryBuffer for the input file on success, and an Error
/// otherwise.
Expected<std::unique_ptr<MemoryBuffer>>
getInputMemoryBuffer(StringRef InputFileName) {
  auto MaybeBuf = MemoryBuffer::getFileOrSTDIN(InputFileName);
  if (auto ErrorCode = MaybeBuf.getError())
    return createStringError(ErrorCode,
                             Twine("Cannot open file '" + InputFileName +
                                   "': " + ErrorCode.message()));
  return std::move(*MaybeBuf);
}

/// \returns A ToolOutputFile which can be used for outputting the results of
/// some tool mode.
/// \p OutputFileName is the desired destination.
/// \p Flags controls whether or not the file is opened for writing in text
/// mode, as a binary, etc. See sys::fs::OpenFlags for more detail.
Expected<std::unique_ptr<ToolOutputFile>>
getOutputFileWithFlags(StringRef OutputFileName, sys::fs::OpenFlags Flags) {
  if (OutputFileName == "")
    OutputFileName = "-";
  std::error_code ErrorCode;
  auto OF = std::make_unique<ToolOutputFile>(OutputFileName, ErrorCode, Flags);
  if (ErrorCode)
    return errorCodeToError(ErrorCode);
  return std::move(OF);
}

/// \returns A ToolOutputFile which can be used for writing remarks on success,
/// and an Error otherwise.
/// \p OutputFileName is the desired destination.
/// \p OutputFormat
Expected<std::unique_ptr<ToolOutputFile>>
getOutputFileForRemarks(StringRef OutputFileName, Format OutputFormat) {
  assert((OutputFormat == Format::YAML || OutputFormat == Format::Bitstream) &&
         "Expected one of YAML or Bitstream!");
  return getOutputFileWithFlags(OutputFileName, OutputFormat == Format::YAML
                                                    ? sys::fs::OF_TextWithCRLF
                                                    : sys::fs::OF_None);
}
} // namespace remarks
} // namespace llvm
