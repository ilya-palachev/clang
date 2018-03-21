//===- Marshallers.h - Generic matcher function marshallers -----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Functions templates and classes to wrap matcher construct functions.
///
/// A collection of template function and classes that provide a generic
/// marshalling layer on top of matcher construct functions.
/// These are used by the registry to export all marshaller constructors with
/// the same generic interface.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_ASTMATCHERS_DYNAMIC_MARSHALLERS_H
#define LLVM_CLANG_LIB_ASTMATCHERS_DYNAMIC_MARSHALLERS_H

#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/OperationKinds.h"
#include "clang/ASTMatchers/ASTMatchersInternal.h"
#include "clang/ASTMatchers/Dynamic/Diagnostics.h"
#include "clang/ASTMatchers/Dynamic/VariantValue.h"
#include "clang/Basic/AttrKinds.h"
#include "clang/Basic/LLVM.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Twine.h"
#include <cassert>
#include <cstddef>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace ast_matchers {
namespace dynamic {
namespace internal {

/// Helper template class to just from argument type to the right is/get
///   functions in VariantValue.
/// Used to verify and extract the matcher arguments below.
template <class T> struct ArgTypeTraits;
template <class T> struct ArgTypeTraits<const T &> : public ArgTypeTraits<T> {
};

template <> struct ArgTypeTraits<std::string> {
  static bool is(const VariantValue &Value) { return Value.isString(); }

  static const std::string &get(const VariantValue &Value) {
    return Value.getString();
  }

  static ArgKind getKind() {
    return ArgKind(ArgKind::AK_String);
  }
};

template <>
struct ArgTypeTraits<StringRef> : public ArgTypeTraits<std::string> {
};

template <class T> struct ArgTypeTraits<ast_matchers::internal::Matcher<T>> {
  static bool is(const VariantValue &Value) {
    return Value.isMatcher() && Value.getMatcher().hasTypedMatcher<T>();
  }

  static ast_matchers::internal::Matcher<T> get(const VariantValue &Value) {
    return Value.getMatcher().getTypedMatcher<T>();
  }

  static ArgKind getKind() {
    return ArgKind(ento::ast_graph_type_traits::ASTGraphNodeKind::getFromNodeKind<T>());
  }
};

template <> struct ArgTypeTraits<bool> {
  static bool is(const VariantValue &Value) { return Value.isBoolean(); }

  static bool get(const VariantValue &Value) {
    return Value.getBoolean();
  }

  static ArgKind getKind() {
    return ArgKind(ArgKind::AK_Boolean);
  }
};

template <> struct ArgTypeTraits<double> {
  static bool is(const VariantValue &Value) { return Value.isDouble(); }

  static double get(const VariantValue &Value) {
    return Value.getDouble();
  }

  static ArgKind getKind() {
    return ArgKind(ArgKind::AK_Double);
  }
};

template <> struct ArgTypeTraits<unsigned> {
  static bool is(const VariantValue &Value) { return Value.isUnsigned(); }

  static unsigned get(const VariantValue &Value) {
    return Value.getUnsigned();
  }

  static ArgKind getKind() {
    return ArgKind(ArgKind::AK_Unsigned);
  }
};

template <> struct ArgTypeTraits<attr::Kind> {
private:
  static Optional<attr::Kind> getAttrKind(llvm::StringRef AttrKind) {
    return llvm::StringSwitch<Optional<attr::Kind>>(AttrKind)
#define ATTR(X) .Case("attr::" #X, attr:: X)
#include "clang/Basic/AttrList.inc"
        .Default(llvm::None);
  }

public:
  static bool is(const VariantValue &Value) {
    return Value.isString() && getAttrKind(Value.getString());
  }

  static attr::Kind get(const VariantValue &Value) {
    return *getAttrKind(Value.getString());
  }

  static ArgKind getKind() {
    return ArgKind(ArgKind::AK_String);
  }
};

template <> struct ArgTypeTraits<CastKind> {
private:
  static Optional<CastKind> getCastKind(llvm::StringRef AttrKind) {
    return llvm::StringSwitch<Optional<CastKind>>(AttrKind)
#define CAST_OPERATION(Name) .Case( #Name, CK_##Name)
#include "clang/AST/OperationKinds.def"
        .Default(llvm::None);
  }

public:
  static bool is(const VariantValue &Value) {
    return Value.isString() && getCastKind(Value.getString());
  }

  static CastKind get(const VariantValue &Value) {
    return *getCastKind(Value.getString());
  }

  static ArgKind getKind() {
    return ArgKind(ArgKind::AK_String);
  }
};

/// Matcher descriptor interface.
///
/// Provides a \c create() method that constructs the matcher from the provided
/// arguments, and various other methods for type introspection.
class MatcherDescriptor {
public:
  virtual ~MatcherDescriptor() = default;

  virtual VariantMatcher create(SourceRange NameRange,
                                ArrayRef<ParserValue> Args,
                                Diagnostics *Error) const = 0;

  /// Returns whether the matcher is variadic. Variadic matchers can take any
  /// number of arguments, but they must be of the same type.
  virtual bool isVariadic() const = 0;

  /// Returns the number of arguments accepted by the matcher if not variadic.
  virtual unsigned getNumArgs() const = 0;

  /// Given that the matcher is being converted to type \p ThisKind, append the
  /// set of argument types accepted for argument \p ArgNo to \p ArgKinds.
  // FIXME: We should provide the ability to constrain the output of this
  // function based on the types of other matcher arguments.
  virtual void getArgKinds(ento::ast_graph_type_traits::ASTGraphNodeKind ThisKind, unsigned ArgNo,
                           std::vector<ArgKind> &ArgKinds) const = 0;

  /// Returns whether this matcher is convertible to the given type.  If it is
  /// so convertible, store in *Specificity a value corresponding to the
  /// "specificity" of the converted matcher to the given context, and in
  /// *LeastDerivedKind the least derived matcher kind which would result in the
  /// same matcher overload.  Zero specificity indicates that this conversion
  /// would produce a trivial matcher that will either always or never match.
  /// Such matchers are excluded from code completion results.
  virtual bool isConvertibleTo(
      ento::ast_graph_type_traits::ASTGraphNodeKind Kind, unsigned *Specificity = nullptr,
      ento::ast_graph_type_traits::ASTGraphNodeKind *LeastDerivedKind = nullptr) const = 0;

  /// Returns whether the matcher will, given a matcher of any type T, yield a
  /// matcher of type T.
  virtual bool isPolymorphic() const { return false; }
};

inline bool isRetKindConvertibleTo(
    ArrayRef<ento::ast_graph_type_traits::ASTGraphNodeKind> RetKinds,
    ento::ast_graph_type_traits::ASTGraphNodeKind Kind, unsigned *Specificity,
    ento::ast_graph_type_traits::ASTGraphNodeKind *LeastDerivedKind) {
  for (const ento::ast_graph_type_traits::ASTGraphNodeKind &NodeKind : RetKinds) {
    if (ArgKind(NodeKind).isConvertibleTo(Kind, Specificity)) {
      if (LeastDerivedKind)
        *LeastDerivedKind = NodeKind;
      return true;
    }
  }
  return false;
}

/// Simple callback implementation. Marshaller and function are provided.
///
/// This class wraps a function of arbitrary signature and a marshaller
/// function into a MatcherDescriptor.
/// The marshaller is in charge of taking the VariantValue arguments, checking
/// their types, unpacking them and calling the underlying function.
class FixedArgCountMatcherDescriptor : public MatcherDescriptor {
public:
  using MarshallerType = VariantMatcher (*)(void (*Func)(),
                                            StringRef MatcherName,
                                            SourceRange NameRange,
                                            ArrayRef<ParserValue> Args,
                                            Diagnostics *Error);

  /// \param Marshaller Function to unpack the arguments and call \c Func
  /// \param Func Matcher construct function. This is the function that
  ///   compile-time matcher expressions would use to create the matcher.
  /// \param RetKinds The list of matcher types to which the matcher is
  ///   convertible.
  /// \param ArgKinds The types of the arguments this matcher takes.
  FixedArgCountMatcherDescriptor(
      MarshallerType Marshaller, void (*Func)(), StringRef MatcherName,
      ArrayRef<ento::ast_graph_type_traits::ASTGraphNodeKind> RetKinds,
      ArrayRef<ArgKind> ArgKinds)
      : Marshaller(Marshaller), Func(Func), MatcherName(MatcherName),
        RetKinds(RetKinds.begin(), RetKinds.end()),
        ArgKinds(ArgKinds.begin(), ArgKinds.end()) {}

  VariantMatcher create(SourceRange NameRange,
                        ArrayRef<ParserValue> Args,
                        Diagnostics *Error) const override {
    return Marshaller(Func, MatcherName, NameRange, Args, Error);
  }

  bool isVariadic() const override { return false; }
  unsigned getNumArgs() const override { return ArgKinds.size(); }

  void getArgKinds(ento::ast_graph_type_traits::ASTGraphNodeKind ThisKind, unsigned ArgNo,
                   std::vector<ArgKind> &Kinds) const override {
    Kinds.push_back(ArgKinds[ArgNo]);
  }

  bool isConvertibleTo(
      ento::ast_graph_type_traits::ASTGraphNodeKind Kind, unsigned *Specificity,
      ento::ast_graph_type_traits::ASTGraphNodeKind *LeastDerivedKind) const override {
    return isRetKindConvertibleTo(RetKinds, Kind, Specificity,
                                  LeastDerivedKind);
  }

private:
  const MarshallerType Marshaller;
  void (* const Func)();
  const std::string MatcherName;
  const std::vector<ento::ast_graph_type_traits::ASTGraphNodeKind> RetKinds;
  const std::vector<ArgKind> ArgKinds;
};

/// Helper methods to extract and merge all possible typed matchers
/// out of the polymorphic object.
template <class PolyMatcher>
static void mergePolyMatchers(const PolyMatcher &Poly,
                              std::vector<DynTypedMatcher> &Out,
                              ast_matchers::internal::EmptyTypeList) {}

template <class PolyMatcher, class TypeList>
static void mergePolyMatchers(const PolyMatcher &Poly,
                              std::vector<DynTypedMatcher> &Out, TypeList) {
  Out.push_back(ast_matchers::internal::Matcher<typename TypeList::head>(Poly));
  mergePolyMatchers(Poly, Out, typename TypeList::tail());
}

/// Convert the return values of the functions into a VariantMatcher.
///
/// There are 2 cases right now: The return value is a Matcher<T> or is a
/// polymorphic matcher. For the former, we just construct the VariantMatcher.
/// For the latter, we instantiate all the possible Matcher<T> of the poly
/// matcher.
static VariantMatcher outvalueToVariantMatcher(const DynTypedMatcher &Matcher) {
  return VariantMatcher::SingleMatcher(Matcher);
}

template <typename T>
static VariantMatcher outvalueToVariantMatcher(const T &PolyMatcher,
                                               typename T::ReturnTypes * =
                                                   nullptr) {
  std::vector<DynTypedMatcher> Matchers;
  mergePolyMatchers(PolyMatcher, Matchers, typename T::ReturnTypes());
  VariantMatcher Out = VariantMatcher::PolymorphicMatcher(std::move(Matchers));
  return Out;
}

template <typename T>
inline void buildReturnTypeVectorFromTypeList(
    std::vector<ento::ast_graph_type_traits::ASTGraphNodeKind> &RetTypes) {
  RetTypes.push_back(
      ento::ast_graph_type_traits::ASTGraphNodeKind::getFromNodeKind<typename T::head>());
  buildReturnTypeVectorFromTypeList<typename T::tail>(RetTypes);
}

template <>
inline void
buildReturnTypeVectorFromTypeList<ast_matchers::internal::EmptyTypeList>(
    std::vector<ento::ast_graph_type_traits::ASTGraphNodeKind> &RetTypes) {}

template <typename T>
struct BuildReturnTypeVector {
  static void build(std::vector<ento::ast_graph_type_traits::ASTGraphNodeKind> &RetTypes) {
    buildReturnTypeVectorFromTypeList<typename T::ReturnTypes>(RetTypes);
  }
};

template <typename T>
struct BuildReturnTypeVector<ast_matchers::internal::Matcher<T>> {
  static void build(std::vector<ento::ast_graph_type_traits::ASTGraphNodeKind> &RetTypes) {
    RetTypes.push_back(ento::ast_graph_type_traits::ASTGraphNodeKind::getFromNodeKind<T>());
  }
};

template <typename T>
struct BuildReturnTypeVector<ast_matchers::internal::BindableMatcher<T>> {
  static void build(std::vector<ento::ast_graph_type_traits::ASTGraphNodeKind> &RetTypes) {
    RetTypes.push_back(ento::ast_graph_type_traits::ASTGraphNodeKind::getFromNodeKind<T>());
  }
};

/// Variadic marshaller function.
template <typename ResultT, typename ArgT,
          ResultT (*Func)(ArrayRef<const ArgT *>)>
VariantMatcher
variadicMatcherDescriptor(StringRef MatcherName, SourceRange NameRange,
                          ArrayRef<ParserValue> Args, Diagnostics *Error) {
  ArgT **InnerArgs = new ArgT *[Args.size()]();

  bool HasError = false;
  for (size_t i = 0, e = Args.size(); i != e; ++i) {
    using ArgTraits = ArgTypeTraits<ArgT>;

    const ParserValue &Arg = Args[i];
    const VariantValue &Value = Arg.Value;
    if (!ArgTraits::is(Value)) {
      Error->addError(Arg.Range, Error->ET_RegistryWrongArgType)
          << (i + 1) << ArgTraits::getKind().asString() << Value.getTypeAsString();
      HasError = true;
      break;
    }
    InnerArgs[i] = new ArgT(ArgTraits::get(Value));
  }

  VariantMatcher Out;
  if (!HasError) {
    Out = outvalueToVariantMatcher(Func(llvm::makeArrayRef(InnerArgs,
                                                           Args.size())));
  }

  for (size_t i = 0, e = Args.size(); i != e; ++i) {
    delete InnerArgs[i];
  }
  delete[] InnerArgs;
  return Out;
}

/// Matcher descriptor for variadic functions.
///
/// This class simply wraps a VariadicFunction with the right signature to export
/// it as a MatcherDescriptor.
/// This allows us to have one implementation of the interface for as many free
/// functions as we want, reducing the number of symbols and size of the
/// object file.
class VariadicFuncMatcherDescriptor : public MatcherDescriptor {
public:
  using RunFunc = VariantMatcher (*)(StringRef MatcherName,
                                     SourceRange NameRange,
                                     ArrayRef<ParserValue> Args,
                                     Diagnostics *Error);

  template <typename ResultT, typename ArgT,
            ResultT (*F)(ArrayRef<const ArgT *>)>
  VariadicFuncMatcherDescriptor(
      ast_matchers::internal::VariadicFunction<ResultT, ArgT, F> Func,
      StringRef MatcherName)
      : Func(&variadicMatcherDescriptor<ResultT, ArgT, F>),
        MatcherName(MatcherName.str()),
        ArgsKind(ArgTypeTraits<ArgT>::getKind()) {
    BuildReturnTypeVector<ResultT>::build(RetKinds);
  }

  VariantMatcher create(SourceRange NameRange,
                        ArrayRef<ParserValue> Args,
                        Diagnostics *Error) const override {
    return Func(MatcherName, NameRange, Args, Error);
  }

  bool isVariadic() const override { return true; }
  unsigned getNumArgs() const override { return 0; }

  void getArgKinds(ento::ast_graph_type_traits::ASTGraphNodeKind ThisKind, unsigned ArgNo,
                   std::vector<ArgKind> &Kinds) const override {
    Kinds.push_back(ArgsKind);
  }

  bool isConvertibleTo(
      ento::ast_graph_type_traits::ASTGraphNodeKind Kind, unsigned *Specificity,
      ento::ast_graph_type_traits::ASTGraphNodeKind *LeastDerivedKind) const override {
    return isRetKindConvertibleTo(RetKinds, Kind, Specificity,
                                  LeastDerivedKind);
  }

private:
  const RunFunc Func;
  const std::string MatcherName;
  std::vector<ento::ast_graph_type_traits::ASTGraphNodeKind> RetKinds;
  const ArgKind ArgsKind;
};

/// Return CK_Trivial when appropriate for VariadicDynCastAllOfMatchers.
class DynCastAllOfMatcherDescriptor : public VariadicFuncMatcherDescriptor {
public:
  template <typename BaseT, typename DerivedT>
  DynCastAllOfMatcherDescriptor(
      ast_matchers::internal::VariadicDynCastAllOfMatcher<BaseT, DerivedT> Func,
      StringRef MatcherName)
      : VariadicFuncMatcherDescriptor(Func, MatcherName),
        DerivedKind(ento::ast_graph_type_traits::ASTGraphNodeKind::getFromNodeKind<DerivedT>()) {
  }

  bool
  isConvertibleTo(ento::ast_graph_type_traits::ASTGraphNodeKind Kind, unsigned *Specificity,
                ento::ast_graph_type_traits::ASTGraphNodeKind *LeastDerivedKind) const override {
    // If Kind is not a base of DerivedKind, either DerivedKind is a base of
    // Kind (in which case the match will always succeed) or Kind and
    // DerivedKind are unrelated (in which case it will always fail), so set
    // Specificity to 0.
    if (VariadicFuncMatcherDescriptor::isConvertibleTo(Kind, Specificity,
                                                 LeastDerivedKind)) {
      if (Kind.isSame(DerivedKind) || !Kind.isBaseOf(DerivedKind)) {
        if (Specificity)
          *Specificity = 0;
      }
      return true;
    } else {
      return false;
    }
  }

private:
  const ento::ast_graph_type_traits::ASTGraphNodeKind DerivedKind;
};

/// Helper macros to check the arguments on all marshaller functions.
#define CHECK_ARG_COUNT(count)                                                 \
  if (Args.size() != count) {                                                  \
    Error->addError(NameRange, Error->ET_RegistryWrongArgCount)                \
        << count << Args.size();                                               \
    return VariantMatcher();                                                   \
  }

#define CHECK_ARG_TYPE(index, type)                                            \
  if (!ArgTypeTraits<type>::is(Args[index].Value)) {                           \
    Error->addError(Args[index].Range, Error->ET_RegistryWrongArgType)         \
        << (index + 1) << ArgTypeTraits<type>::getKind().asString()            \
        << Args[index].Value.getTypeAsString();                                \
    return VariantMatcher();                                                   \
  }

/// 0-arg marshaller function.
template <typename ReturnType>
static VariantMatcher matcherMarshall0(void (*Func)(), StringRef MatcherName,
                                       SourceRange NameRange,
                                       ArrayRef<ParserValue> Args,
                                       Diagnostics *Error) {
  using FuncType = ReturnType (*)();
  CHECK_ARG_COUNT(0);
  return outvalueToVariantMatcher(reinterpret_cast<FuncType>(Func)());
}

/// 1-arg marshaller function.
template <typename ReturnType, typename ArgType1>
static VariantMatcher matcherMarshall1(void (*Func)(), StringRef MatcherName,
                                       SourceRange NameRange,
                                       ArrayRef<ParserValue> Args,
                                       Diagnostics *Error) {
  using FuncType = ReturnType (*)(ArgType1);
  CHECK_ARG_COUNT(1);
  CHECK_ARG_TYPE(0, ArgType1);
  return outvalueToVariantMatcher(reinterpret_cast<FuncType>(Func)(
      ArgTypeTraits<ArgType1>::get(Args[0].Value)));
}

/// 2-arg marshaller function.
template <typename ReturnType, typename ArgType1, typename ArgType2>
static VariantMatcher matcherMarshall2(void (*Func)(), StringRef MatcherName,
                                       SourceRange NameRange,
                                       ArrayRef<ParserValue> Args,
                                       Diagnostics *Error) {
  using FuncType = ReturnType (*)(ArgType1, ArgType2);
  CHECK_ARG_COUNT(2);
  CHECK_ARG_TYPE(0, ArgType1);
  CHECK_ARG_TYPE(1, ArgType2);
  return outvalueToVariantMatcher(reinterpret_cast<FuncType>(Func)(
      ArgTypeTraits<ArgType1>::get(Args[0].Value),
      ArgTypeTraits<ArgType2>::get(Args[1].Value)));
}

#undef CHECK_ARG_COUNT
#undef CHECK_ARG_TYPE

/// Helper class used to collect all the possible overloads of an
///   argument adaptative matcher function.
template <template <typename ToArg, typename FromArg> class ArgumentAdapterT,
          typename FromTypes, typename ToTypes>
class AdaptativeOverloadCollector {
public:
  AdaptativeOverloadCollector(
      StringRef Name, std::vector<std::unique_ptr<MatcherDescriptor>> &Out)
      : Name(Name), Out(Out) {
    collect(FromTypes());
  }

private:
  using AdaptativeFunc = ast_matchers::internal::ArgumentAdaptingMatcherFunc<
      ArgumentAdapterT, FromTypes, ToTypes>;

  /// End case for the recursion
  static void collect(ast_matchers::internal::EmptyTypeList) {}

  /// Recursive case. Get the overload for the head of the list, and
  ///   recurse to the tail.
  template <typename FromTypeList>
  inline void collect(FromTypeList);

  StringRef Name;
  std::vector<std::unique_ptr<MatcherDescriptor>> &Out;
};

/// MatcherDescriptor that wraps multiple "overloads" of the same
///   matcher.
///
/// It will try every overload and generate appropriate errors for when none or
/// more than one overloads match the arguments.
class OverloadedMatcherDescriptor : public MatcherDescriptor {
public:
  OverloadedMatcherDescriptor(
      MutableArrayRef<std::unique_ptr<MatcherDescriptor>> Callbacks)
      : Overloads(std::make_move_iterator(Callbacks.begin()),
                  std::make_move_iterator(Callbacks.end())) {}

  ~OverloadedMatcherDescriptor() override = default;

  VariantMatcher create(SourceRange NameRange,
                        ArrayRef<ParserValue> Args,
                        Diagnostics *Error) const override {
    std::vector<VariantMatcher> Constructed;
    Diagnostics::OverloadContext Ctx(Error);
    for (const auto &O : Overloads) {
      VariantMatcher SubMatcher = O->create(NameRange, Args, Error);
      if (!SubMatcher.isNull()) {
        Constructed.push_back(SubMatcher);
      }
    }

    if (Constructed.empty()) return VariantMatcher(); // No overload matched.
    // We ignore the errors if any matcher succeeded.
    Ctx.revertErrors();
    if (Constructed.size() > 1) {
      // More than one constructed. It is ambiguous.
      Error->addError(NameRange, Error->ET_RegistryAmbiguousOverload);
      return VariantMatcher();
    }
    return Constructed[0];
  }

  bool isVariadic() const override {
    bool Overload0Variadic = Overloads[0]->isVariadic();
#ifndef NDEBUG
    for (const auto &O : Overloads) {
      assert(Overload0Variadic == O->isVariadic());
    }
#endif
    return Overload0Variadic;
  }

  unsigned getNumArgs() const override {
    unsigned Overload0NumArgs = Overloads[0]->getNumArgs();
#ifndef NDEBUG
    for (const auto &O : Overloads) {
      assert(Overload0NumArgs == O->getNumArgs());
    }
#endif
    return Overload0NumArgs;
  }

  void getArgKinds(ento::ast_graph_type_traits::ASTGraphNodeKind ThisKind, unsigned ArgNo,
                   std::vector<ArgKind> &Kinds) const override {
    for (const auto &O : Overloads) {
      if (O->isConvertibleTo(ThisKind))
        O->getArgKinds(ThisKind, ArgNo, Kinds);
    }
  }

  bool isConvertibleTo(
      ento::ast_graph_type_traits::ASTGraphNodeKind Kind, unsigned *Specificity,
      ento::ast_graph_type_traits::ASTGraphNodeKind *LeastDerivedKind) const override {
    for (const auto &O : Overloads) {
      if (O->isConvertibleTo(Kind, Specificity, LeastDerivedKind))
        return true;
    }
    return false;
  }

private:
  std::vector<std::unique_ptr<MatcherDescriptor>> Overloads;
};

/// Variadic operator marshaller function.
class VariadicOperatorMatcherDescriptor : public MatcherDescriptor {
public:
  using VarOp = DynTypedMatcher::VariadicOperator;

  VariadicOperatorMatcherDescriptor(unsigned MinCount, unsigned MaxCount,
                                    VarOp Op, StringRef MatcherName)
      : MinCount(MinCount), MaxCount(MaxCount), Op(Op),
        MatcherName(MatcherName) {}

  VariantMatcher create(SourceRange NameRange,
                        ArrayRef<ParserValue> Args,
                        Diagnostics *Error) const override {
    if (Args.size() < MinCount || MaxCount < Args.size()) {
      const std::string MaxStr =
          (MaxCount == std::numeric_limits<unsigned>::max() ? ""
                                                            : Twine(MaxCount))
              .str();
      Error->addError(NameRange, Error->ET_RegistryWrongArgCount)
          << ("(" + Twine(MinCount) + ", " + MaxStr + ")") << Args.size();
      return VariantMatcher();
    }

    std::vector<VariantMatcher> InnerArgs;
    for (size_t i = 0, e = Args.size(); i != e; ++i) {
      const ParserValue &Arg = Args[i];
      const VariantValue &Value = Arg.Value;
      if (!Value.isMatcher()) {
        Error->addError(Arg.Range, Error->ET_RegistryWrongArgType)
            << (i + 1) << "Matcher<>" << Value.getTypeAsString();
        return VariantMatcher();
      }
      InnerArgs.push_back(Value.getMatcher());
    }
    return VariantMatcher::VariadicOperatorMatcher(Op, std::move(InnerArgs));
  }

  bool isVariadic() const override { return true; }
  unsigned getNumArgs() const override { return 0; }

  void getArgKinds(ento::ast_graph_type_traits::ASTGraphNodeKind ThisKind, unsigned ArgNo,
                   std::vector<ArgKind> &Kinds) const override {
    Kinds.push_back(ThisKind);
  }

  bool isConvertibleTo(ento::ast_graph_type_traits::ASTGraphNodeKind Kind, unsigned *Specificity,
                       ento::ast_graph_type_traits::ASTGraphNodeKind *LeastDerivedKind) const override {
    if (Specificity)
      *Specificity = 1;
    if (LeastDerivedKind)
      *LeastDerivedKind = Kind;
    return true;
  }

  bool isPolymorphic() const override { return true; }

private:
  const unsigned MinCount;
  const unsigned MaxCount;
  const VarOp Op;
  const StringRef MatcherName;
};

/// Helper functions to select the appropriate marshaller functions.
/// They detect the number of arguments, arguments types and return type.

/// 0-arg overload
template <typename ReturnType>
std::unique_ptr<MatcherDescriptor>
makeMatcherAutoMarshall(ReturnType (*Func)(), StringRef MatcherName) {
  std::vector<ento::ast_graph_type_traits::ASTGraphNodeKind> RetTypes;
  BuildReturnTypeVector<ReturnType>::build(RetTypes);
  return llvm::make_unique<FixedArgCountMatcherDescriptor>(
      matcherMarshall0<ReturnType>, reinterpret_cast<void (*)()>(Func),
      MatcherName, RetTypes, None);
}

/// 1-arg overload
template <typename ReturnType, typename ArgType1>
std::unique_ptr<MatcherDescriptor>
makeMatcherAutoMarshall(ReturnType (*Func)(ArgType1), StringRef MatcherName) {
  std::vector<ento::ast_graph_type_traits::ASTGraphNodeKind> RetTypes;
  BuildReturnTypeVector<ReturnType>::build(RetTypes);
  ArgKind AK = ArgTypeTraits<ArgType1>::getKind();
  return llvm::make_unique<FixedArgCountMatcherDescriptor>(
      matcherMarshall1<ReturnType, ArgType1>,
      reinterpret_cast<void (*)()>(Func), MatcherName, RetTypes, AK);
}

/// 2-arg overload
template <typename ReturnType, typename ArgType1, typename ArgType2>
std::unique_ptr<MatcherDescriptor>
makeMatcherAutoMarshall(ReturnType (*Func)(ArgType1, ArgType2),
                        StringRef MatcherName) {
  std::vector<ento::ast_graph_type_traits::ASTGraphNodeKind> RetTypes;
  BuildReturnTypeVector<ReturnType>::build(RetTypes);
  ArgKind AKs[] = { ArgTypeTraits<ArgType1>::getKind(),
                    ArgTypeTraits<ArgType2>::getKind() };
  return llvm::make_unique<FixedArgCountMatcherDescriptor>(
      matcherMarshall2<ReturnType, ArgType1, ArgType2>,
      reinterpret_cast<void (*)()>(Func), MatcherName, RetTypes, AKs);
}

/// Variadic overload.
template <typename ResultT, typename ArgT,
          ResultT (*Func)(ArrayRef<const ArgT *>)>
std::unique_ptr<MatcherDescriptor> makeMatcherAutoMarshall(
    ast_matchers::internal::VariadicFunction<ResultT, ArgT, Func> VarFunc,
    StringRef MatcherName) {
  return llvm::make_unique<VariadicFuncMatcherDescriptor>(VarFunc, MatcherName);
}

/// Overload for VariadicDynCastAllOfMatchers.
///
/// Not strictly necessary, but DynCastAllOfMatcherDescriptor gives us better
/// completion results for that type of matcher.
template <typename BaseT, typename DerivedT>
std::unique_ptr<MatcherDescriptor> makeMatcherAutoMarshall(
    ast_matchers::internal::VariadicDynCastAllOfMatcher<BaseT, DerivedT>
        VarFunc,
    StringRef MatcherName) {
  return llvm::make_unique<DynCastAllOfMatcherDescriptor>(VarFunc, MatcherName);
}

/// Argument adaptative overload.
template <template <typename ToArg, typename FromArg> class ArgumentAdapterT,
          typename FromTypes, typename ToTypes>
std::unique_ptr<MatcherDescriptor> makeMatcherAutoMarshall(
    ast_matchers::internal::ArgumentAdaptingMatcherFunc<ArgumentAdapterT,
                                                        FromTypes, ToTypes>,
    StringRef MatcherName) {
  std::vector<std::unique_ptr<MatcherDescriptor>> Overloads;
  AdaptativeOverloadCollector<ArgumentAdapterT, FromTypes, ToTypes>(MatcherName,
                                                                    Overloads);
  return llvm::make_unique<OverloadedMatcherDescriptor>(Overloads);
}

template <template <typename ToArg, typename FromArg> class ArgumentAdapterT,
          typename FromTypes, typename ToTypes>
template <typename FromTypeList>
inline void AdaptativeOverloadCollector<ArgumentAdapterT, FromTypes,
                                        ToTypes>::collect(FromTypeList) {
  Out.push_back(makeMatcherAutoMarshall(
      &AdaptativeFunc::template create<typename FromTypeList::head>, Name));
  collect(typename FromTypeList::tail());
}

/// Variadic operator overload.
template <unsigned MinCount, unsigned MaxCount>
std::unique_ptr<MatcherDescriptor> makeMatcherAutoMarshall(
    ast_matchers::internal::VariadicOperatorMatcherFunc<MinCount, MaxCount>
        Func,
    StringRef MatcherName) {
  return llvm::make_unique<VariadicOperatorMatcherDescriptor>(
      MinCount, MaxCount, Func.Op, MatcherName);
}

} // namespace internal
} // namespace dynamic
} // namespace ast_matchers
} // namespace clang

#endif // LLVM_CLANG_AST_MATCHERS_DYNAMIC_MARSHALLERS_H
