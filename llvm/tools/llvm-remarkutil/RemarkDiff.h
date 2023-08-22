#include "RemarkUtilHelpers.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/Support/JSON.h"

namespace llvm {
namespace remarks {

/// copy of Argument class using std::string instead of StringRef.
struct RemarkArgInfo {
  std::string Key;
  std::string Val;
  RemarkArgInfo(StringRef Key, StringRef Val)
      : Key(Key.str()), Val(Val.str()) {}
  void print(raw_ostream &OS) const;
};

hash_code hash_value(const RemarkArgInfo &Arg) {
  return hash_combine(Arg.Key, Arg.Val);
}

/// A copy of Remark class using std::string instead of StringRef.
struct RemarkInfo {
  std::string RemarkName;
  std::string FunctionName;
  std::string PassName;
  Type RemarkType;
  std::vector<RemarkArgInfo> Args;
  RemarkInfo();
  RemarkInfo(std::string RemarkName, std::string FunctionName,
             std::string PassName, Type RemarkType,
             std::vector<RemarkArgInfo> Args)
      : RemarkName(RemarkName), FunctionName(FunctionName), PassName(PassName),
        Args(Args) {}
  RemarkInfo(Remark &Remark)
      : RemarkName(Remark.RemarkName.str()),
        FunctionName(Remark.FunctionName.str()),
        PassName(Remark.PassName.str()), RemarkType(Remark.RemarkType) {
    for (const auto &Arg : Remark.Args)
      Args.push_back({Arg.Key.str(), Arg.Val.str()});
  }

  bool hasSameHeader(const RemarkInfo &RHS) const {
    return RemarkName == RHS.RemarkName && FunctionName == RHS.FunctionName &&
           PassName == RHS.PassName;
  };
  void print(raw_ostream &OS) const;
  void printHeader(raw_ostream &OS) const;
};

inline bool operator<(const RemarkArgInfo &LHS, const RemarkArgInfo &RHS) {
  return std::make_tuple(LHS.Key, LHS.Val) < std::make_tuple(RHS.Key, RHS.Val);
}

inline bool operator<(const RemarkInfo &LHS, const RemarkInfo &RHS) {
  return std::make_tuple(LHS.RemarkType, LHS.PassName, LHS.RemarkName,
                         LHS.FunctionName, LHS.Args) <
         std::make_tuple(RHS.RemarkType, RHS.PassName, RHS.RemarkName,
                         RHS.FunctionName, RHS.Args);
}
inline bool operator==(const RemarkArgInfo &LHS, const RemarkArgInfo &RHS) {
  return LHS.Key == RHS.Key && LHS.Val == RHS.Val;
}

inline bool operator==(const RemarkInfo &LHS, const RemarkInfo &RHS) {
  return LHS.RemarkName == RHS.RemarkName &&
         LHS.FunctionName == RHS.FunctionName && LHS.PassName == RHS.PassName &&
         LHS.RemarkType == RHS.RemarkType && LHS.Args == RHS.Args;
}

inline raw_ostream &operator<<(raw_ostream &OS,
                               const RemarkArgInfo &RemarkArgInfo) {
  RemarkArgInfo.print(OS);
  return OS;
}

inline raw_ostream &operator<<(raw_ostream &OS, const RemarkInfo &RemarkInfo) {
  RemarkInfo.print(OS);
  return OS;
}

/// Represent a location which combines RemarkLoc and function name.
struct DebugLocation {
  std::string SourceFilePath;
  std::string FunctionName;
  unsigned SourceLine = 0;
  unsigned SourceColumn = 0;
  DebugLocation() = default;
  DebugLocation(StringRef SourceFilePath, StringRef FunctionName,
                unsigned SourceLine, unsigned SourceColumn)
      : SourceFilePath(SourceFilePath.str()), FunctionName(FunctionName.str()),
        SourceLine(SourceLine), SourceColumn(SourceColumn) {}
};

/// Represents the diff at a remark where the remark header is the same and the
/// two versions of the remark differ in type or arguments.
struct DiffAtRemark {
  RemarkInfo BaseRemark;
  std::optional<std::pair<Type, Type>> RemarkTypeDiff;
  SmallVector<RemarkArgInfo, 4> OnlyA;
  SmallVector<RemarkArgInfo, 4> OnlyB;
  SmallVector<RemarkArgInfo, 4> InBoth;
  DiffAtRemark(RemarkInfo &BaseRemark) : BaseRemark(BaseRemark) {}
  void print(raw_ostream &OS);
  /// represent remark diff as a json object where the header is the same as the
  /// baseline remark and diff json key represents the differences between the
  /// two versions of the remark.
  json::Object toJson();
};

/// Represents the diff at a debug location. This can be unique remarks that
/// exist in file a or file b or remarks that share the same header but differ
/// in remark type or arguments. Any common remarks at the location are
/// discarded.
struct DiffAtLoc {
  DebugLocation Loc;
  SmallVector<RemarkInfo, 4> OnlyA;
  SmallVector<RemarkInfo, 4> OnlyB;
  // list of remarks that are different but share the same header.
  SmallVector<DiffAtRemark, 4> HasTheSameHeader;

public:
  bool isEmpty() {
    return OnlyA.empty() && OnlyB.empty() && HasTheSameHeader.empty();
  }
  void print(raw_ostream &OS);
  json::Object toJson();
};
} // namespace remarks

template <> struct DenseMapInfo<remarks::DebugLocation, void> {
  static inline remarks::DebugLocation getEmptyKey() {
    return remarks::DebugLocation();
  }

  static inline remarks::DebugLocation getTombstoneKey() {
    auto Loc = remarks::DebugLocation();
    Loc.SourceFilePath = StringRef(
        reinterpret_cast<const char *>(~static_cast<uintptr_t>(1)), 0);
    Loc.FunctionName = StringRef(
        reinterpret_cast<const char *>(~static_cast<uintptr_t>(1)), 0);
    Loc.SourceColumn = ~0U - 1;
    Loc.SourceLine = ~0U - 1;
    return Loc;
  }

  static unsigned getHashValue(const remarks::DebugLocation &Key) {
    return hash_combine(Key.SourceFilePath, Key.FunctionName, Key.SourceLine,
                        Key.SourceColumn);
  }

  static bool isEqual(const remarks::DebugLocation &LHS,
                      const remarks::DebugLocation &RHS) {
    return std::make_tuple(LHS.SourceFilePath, LHS.FunctionName, LHS.SourceLine,
                           LHS.SourceColumn) ==
           std::make_tuple(RHS.SourceFilePath, RHS.FunctionName, RHS.SourceLine,
                           RHS.SourceColumn);
  }
};

template <> struct DenseMapInfo<remarks::RemarkInfo, void> {
  static inline remarks::RemarkInfo getEmptyKey() {
    return remarks::RemarkInfo();
  }

  static inline remarks::RemarkInfo getTombstoneKey() {
    auto Info = remarks::RemarkInfo();
    Info.RemarkName =
        reinterpret_cast<const char *>(~static_cast<uintptr_t>(1));
    Info.FunctionName =
        reinterpret_cast<const char *>(~static_cast<uintptr_t>(1));
    Info.PassName = reinterpret_cast<const char *>(~static_cast<uintptr_t>(1));
    Info.RemarkType = remarks::Type::Unknown;
    return Info;
  }

  static unsigned getHashValue(const remarks::RemarkInfo &Key) {
    auto ArgCode = hash_combine_range(Key.Args.begin(), Key.Args.end());
    return hash_combine(Key.RemarkName, Key.FunctionName, Key.PassName,
                        remarks::typeToStr(Key.RemarkType), ArgCode);
  }

  static bool isEqual(const remarks::RemarkInfo &LHS,
                      const remarks::RemarkInfo &RHS) {
    return LHS == RHS;
  }
};
} // namespace llvm