//===- FIRToMLIR.cpp - .fir to FIRRTL dialect parser ----------------------===//
//
// This implements a .fir file parser.
//
//===----------------------------------------------------------------------===//

#include "FIRLexer.h"
#include "mlir/Analysis/Verifier.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/Module.h"
#include "mlir/IR/StandardTypes.h"
#include "mlir/Translation.h"
#include "spt/Dialect/FIRRTL/FIRToMLIR.h"
#include "spt/Dialect/FIRRTL/IR/Ops.h"
#include "spt/Dialect/FIRRTL/IR/Types.h"
#include "llvm/ADT/ScopedHashTable.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

using namespace spt;
using namespace firrtl;
using namespace mlir;
using llvm::SMLoc;
using llvm::SourceMgr;

//===----------------------------------------------------------------------===//
// GlobalFIRParserState
//===----------------------------------------------------------------------===//

namespace {
/// This class refers to all of the state maintained globally by the parser,
/// such as the current lexer position.  This is separated out from the parser
/// so that individual subparsers can refer to the same state.
struct GlobalFIRParserState {
  GlobalFIRParserState(const llvm::SourceMgr &sourceMgr, FIRRTLDialect *dialect)
      : context(dialect->getContext()), dialect(dialect),
        lex(sourceMgr, context), curToken(lex.lexToken()) {}

  /// The context we're parsing into.
  MLIRContext *const context;

  /// The FIRRTL dialect.
  FIRRTLDialect *const dialect;

  /// The lexer for the source file we're parsing.
  FIRLexer lex;

  /// This is the next token that hasn't been consumed yet.
  FIRToken curToken;

private:
  GlobalFIRParserState(const GlobalFIRParserState &) = delete;
  void operator=(const GlobalFIRParserState &) = delete;
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
// FIRParser
//===----------------------------------------------------------------------===//

namespace {
/// This class implements logic common to all levels of the parser, including
/// things like types and helper logic.
struct FIRParser {
  FIRParser(GlobalFIRParserState &state) : state(state) {}

  // Helper methods to get stuff from the parser-global state.
  GlobalFIRParserState &getState() const { return state; }
  MLIRContext *getContext() const { return state.context; }
  const llvm::SourceMgr &getSourceMgr() { return state.lex.getSourceMgr(); }

  /// Return the indentation level of the specified token.
  unsigned getIndentation() const {
    return state.lex.getIndentation(state.curToken);
  }

  //===--------------------------------------------------------------------===//
  // Error Handling
  //===--------------------------------------------------------------------===//

  /// Emit an error and return failure.
  InFlightDiagnostic emitError(const Twine &message = {}) {
    return emitError(state.curToken.getLoc(), message);
  }
  InFlightDiagnostic emitError(SMLoc loc, const Twine &message = {});

  /// Encode the specified source location information into an attribute for
  /// attachment to the IR.
  Location getEncodedSourceLocation(llvm::SMLoc loc) {
    return state.lex.getEncodedSourceLocation(loc);
  }

  //===--------------------------------------------------------------------===//
  // Token Parsing
  //===--------------------------------------------------------------------===//

  /// Return the current token the parser is inspecting.
  const FIRToken &getToken() const { return state.curToken; }
  StringRef getTokenSpelling() const { return state.curToken.getSpelling(); }

  /// If the current token has the specified kind, consume it and return true.
  /// If not, return false.
  bool consumeIf(FIRToken::Kind kind, StringRef *spelling = nullptr,
                 SMLoc *loc = nullptr) {
    if (state.curToken.isNot(kind))
      return false;
    consumeToken(kind, spelling, loc);
    return true;
  }

  /// Advance the current lexer onto the next token.
  void consumeToken(StringRef *spelling = nullptr, SMLoc *loc = nullptr) {
    assert(state.curToken.isNot(FIRToken::eof, FIRToken::error) &&
           "shouldn't advance past EOF or errors");
    if (spelling)
      *spelling = getTokenSpelling();
    if (loc)
      *loc = getToken().getLoc();

    state.curToken = state.lex.lexToken();
  }

  /// Advance the current lexer onto the next token, asserting what the expected
  /// current token is.  This is preferred to the above method because it leads
  /// to more self-documenting code with better checking.
  void consumeToken(FIRToken::Kind kind, StringRef *spelling = nullptr,
                    SMLoc *loc = nullptr) {
    assert(state.curToken.is(kind) && "consumed an unexpected token");
    consumeToken(spelling, loc);
  }

  /// Consume the specified token if present and return success.  On failure,
  /// output a diagnostic and return failure.
  ParseResult parseToken(FIRToken::Kind expectedToken, const Twine &message,
                         StringRef *spelling = nullptr, SMLoc *loc = nullptr);

  //===--------------------------------------------------------------------===//
  // Common Parser Rules
  //===--------------------------------------------------------------------===//

  ParseResult parseOptionalInfo();

  // Parse the 'id' grammar, which is an identifier or an allowed keyword.
  ParseResult parseId(StringAttr &result, const Twine &message);
  ParseResult parseFieldId(StringRef &result, const Twine &message);
  ParseResult parseType(FIRRTLType &result, const Twine &message);

private:
  FIRParser(const FIRParser &) = delete;
  void operator=(const FIRParser &) = delete;

  /// FIRParser is subclassed and reinstantiated.  Do not add additional
  /// non-trivial state here, add it to the GlobalFIRParserState class.
  GlobalFIRParserState &state;
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Error Handling
//===----------------------------------------------------------------------===//

InFlightDiagnostic FIRParser::emitError(SMLoc loc, const Twine &message) {
  auto diag = mlir::emitError(getEncodedSourceLocation(loc), message);

  // If we hit a parse error in response to a lexer error, then the lexer
  // already reported the error.
  if (getToken().is(FIRToken::error))
    diag.abandon();
  return diag;
}

//===----------------------------------------------------------------------===//
// Token Parsing
//===----------------------------------------------------------------------===//

/// Consume the specified token if present and return success.  On failure,
/// output a diagnostic and return failure.
ParseResult FIRParser::parseToken(FIRToken::Kind expectedToken,
                                  const Twine &message, StringRef *spelling,
                                  SMLoc *loc) {
  if (consumeIf(expectedToken, spelling, loc))
    return success();
  return emitError(message);
}

//===--------------------------------------------------------------------===//
// Common Parser Rules.
//===--------------------------------------------------------------------===//

ParseResult FIRParser::parseOptionalInfo() {
  consumeIf(FIRToken::fileinfo);
  return success();
}

/// id  ::= Id | keywordAsId
///
/// Parse the 'id' grammar, which is an identifier or an allowed keyword.  On
/// success, this returns the identifier in the result attribute.
ParseResult FIRParser::parseId(StringAttr &result, const Twine &message) {
  switch (getToken().getKind()) {
  // The most common case is an identifier.
  case FIRToken::identifier:
// Otherwise it may be a keyword that we're allowing in an id position.
#define TOK_KEYWORD(spelling) case FIRToken::kw_##spelling:
#include "FIRTokenKinds.def"

    // Yep, this is a valid 'id'.  Turn it into an attribute.
    result = StringAttr::get(getTokenSpelling(), getContext());
    consumeToken();
    return success();

  default:
    emitError(message);
    return failure();
  }
}

/// fieldId ::= Id
///         ::= RelaxedId
///         ::= UnsignedInt
///         ::= keywordAsId
///
ParseResult FIRParser::parseFieldId(StringRef &result, const Twine &message) {
  // Handle the UnsignedInt case.
  if (consumeIf(FIRToken::integer, &result))
    return success();

  // Otherwise, it must be Id or keywordAsId.
  StringAttr name;
  if (parseId(name, message))
    return failure();

  // FIXME: Handle RelaxedId

  result = name.getValue();
  return success();
}

/// type ::= 'Clock'
///      ::= 'Reset'
///      ::= 'UInt' ('<' intLit '>')?
///      ::= 'SInt' ('<' intLit '>')?
///      ::= 'Analog' ('<' intLit '>')?
///      ::= '{' '}' | '{' field (',' field)* '}'
///      ::= type '[' intLit ']'
///
/// field: 'flip'? fieldId ':' type
///
// FIXME: 'AsyncReset' is also handled by the parser but is not in the spec.
ParseResult FIRParser::parseType(FIRRTLType &result, const Twine &message) {
  switch (getToken().getKind()) {
  default:
    return emitError(message), failure();

  case FIRToken::kw_Clock:
    consumeToken(FIRToken::kw_Clock);
    result = ClockType::get(getContext());
    break;

  case FIRToken::kw_Reset:
    consumeToken(FIRToken::kw_Reset);
    result = ResetType::get(getContext());
    break;

  case FIRToken::kw_UInt:
  case FIRToken::kw_SInt:
  case FIRToken::kw_Analog: {
    auto kind = getToken().getKind();
    consumeToken();

    // Parse a width specifier if present.
    int32_t width = -1;
    if (consumeIf(FIRToken::less)) {
      auto widthSpelling = getTokenSpelling();
      auto widthLoc = getToken().getLoc();
      if (parseToken(FIRToken::integer, "expected width") ||
          parseToken(FIRToken::greater, "expected >"))
        return failure();

      if (widthSpelling.getAsInteger(10, width) || width < 0)
        return emitError(widthLoc, "invalid width specifier"), failure();
    }

    if (kind == FIRToken::kw_SInt)
      result = SIntType::get(getContext(), width);
    else if (kind == FIRToken::kw_UInt)
      result = UIntType::get(getContext(), width);
    else {
      assert(kind == FIRToken::kw_Analog);
      result = AnalogType::get(getContext(), width);
    }
    break;
  }

  case FIRToken::l_brace: {
    consumeToken(FIRToken::l_brace);

    SmallVector<BundleType::BundleElement, 4> elements;

    // Handle {}.
    if (!consumeIf(FIRToken::r_brace)) {
      do {
        bool isFlipped = consumeIf(FIRToken::kw_flip);

        StringRef fieldName;
        FIRRTLType type;
        if (parseFieldId(fieldName, "expected bundle field name") ||
            parseToken(FIRToken::colon, "expected ':' in bundle") ||
            parseType(type, "expected bundle field type"))
          return failure();

        if (isFlipped)
          type = FlipType::get(type);

        elements.push_back({Identifier::get(fieldName, getContext()), type});
      } while (consumeIf(FIRToken::comma));

      // If we didn't have a comma, then we must have an '}' at the end of the
      // bundle.
      if (parseToken(FIRToken::r_brace, "expected '}' at end of bundle"))
        return failure();
    }
    result = BundleType::get(elements, getContext());
    break;
  }
  }

  // Handle postfix vector sizes.
  while (consumeIf(FIRToken::l_square)) {
    auto sizeSpelling = getTokenSpelling();
    auto sizeLoc = getToken().getLoc();
    if (parseToken(FIRToken::integer, "expected width") ||
        parseToken(FIRToken::r_square, "expected ]"))
      return failure();

    unsigned size;
    if (sizeSpelling.getAsInteger(10, size))
      return emitError(sizeLoc, "invalid size specifier"), failure();

    result = FVectorType::get(result, size);
  }

  return success();
}

//===----------------------------------------------------------------------===//
// FIRScopedParser
//===----------------------------------------------------------------------===//

namespace {
/// This class is a common base class for stuff that needs local scope
/// manipulation helpers.
class FIRScopedParser : public FIRParser {
public:
  using SymbolTable =
      llvm::ScopedHashTable<Identifier, std::pair<SMLoc, Value>>;
  FIRScopedParser(GlobalFIRParserState &state, SymbolTable &symbolTable)
      : FIRParser(state), symbolTable(symbolTable) {}

  /// Add a symbol entry with the specified name, returning failure if the name
  /// is already defined.
  ParseResult addSymbolEntry(StringRef name, Value value, SMLoc loc);

  /// Look up the specified name, emitting an error and returning failure if the
  /// name is unknown.
  ParseResult lookupSymbolEntry(Value &result, StringRef name, SMLoc loc);

protected:
  // This symbol table holds the names of ports, wires, and other local decls.
  // This is scoped because conditional statements introduce subscopes.
  SymbolTable &symbolTable;
};
} // end anonymous namespace

/// Add a symbol entry with the specified name, returning failure if the name
/// is already defined.
ParseResult FIRScopedParser::addSymbolEntry(StringRef name, Value value,
                                            SMLoc loc) {
  // TODO: Should we support name shadowing?  This will reject cases where
  // we try to define a new wire in a conditional where an outer name defined
  // the same name.
  auto nameId = Identifier::get(name, getContext());
  auto prev = symbolTable.lookup(nameId);
  if (prev.first.isValid()) {
    emitError(loc, "redefinition of name '" + name.str() + "'")
            .attachNote(getEncodedSourceLocation(prev.first))
        << "previous definition here";
    return failure();
  }

  symbolTable.insert(nameId, {loc, value});
  return success();
}

/// Look up the specified name, emitting an error and returning null if the
/// name is unknown.
ParseResult FIRScopedParser::lookupSymbolEntry(Value &result, StringRef name,
                                               SMLoc loc) {
  auto prev = symbolTable.lookup(Identifier::get(name, getContext()));
  if (!prev.first.isValid())
    return emitError(loc, "use of invalid name '" + name.str() + "'"),
           failure();

  assert(prev.second && "name in symbol table without definition");
  result = prev.second;
  return success();
}

//===----------------------------------------------------------------------===//
// FIRStmtParser
//===----------------------------------------------------------------------===//

namespace {
/// This class implements logic and state for parsing statements, suites, and
/// similar module body constructs.
struct FIRStmtParser : public FIRScopedParser {
  explicit FIRStmtParser(GlobalFIRParserState &state, SymbolTable &symbolTable,
                         OpBuilder builder)
      : FIRScopedParser(state, symbolTable), builder(builder) {}

  ParseResult parseSimpleStmt(unsigned stmtIndent);

private:
  ParseResult parseWire();
  OpBuilder builder;
};

} // end anonymous namespace

/// simple_stmt ::= stmt
/// stmt ::= wire
///
ParseResult FIRStmtParser::parseSimpleStmt(unsigned stmtIndent) {
  switch (getToken().getKind()) {
  case FIRToken::kw_wire:
    return parseWire();
  case FIRToken::identifier:
    // FIXME: This is a temporary hack.
    break;

  default:
    emitError("unexpected token in module");
    return failure();
  }

  // Hard coding identifier <= identifier
  // FIXME: This is wrong.

  StringRef name1, name2;
  SMLoc loc1, arrowLoc, loc2;
  consumeToken(FIRToken::identifier, &name1, &loc1);
  if (parseToken(FIRToken::less_equal, "expected <=", nullptr, &arrowLoc) ||
      parseToken(FIRToken::identifier, "expected identifier", &name2, &loc2))
    return failure();

  Value dest, src;
  if (lookupSymbolEntry(dest, name1, loc1) ||
      lookupSymbolEntry(src, name2, loc2))
    return failure();

  // FIXME: Connect shouldn't have a name.  This should have a @ location.
  auto resultLoc = getEncodedSourceLocation(arrowLoc);
  builder.create<FIRRTLConnectOp>(resultLoc, dest, src,
                                  /*name*/ StringAttr());
  return success();
}

/// wire ::= 'wire' id ':' type info?
ParseResult FIRStmtParser::parseWire() {
  SMLoc loc;
  consumeToken(FIRToken::kw_wire, nullptr, &loc);

  StringAttr id;
  FIRRTLType type;
  if (parseId(id, "expected wire name") ||
      parseToken(FIRToken::colon, "expected ':' in wire") ||
      parseType(type, "expected wire type") || parseOptionalInfo())
    return failure();

  auto resultLoc = getEncodedSourceLocation(loc);
  auto result = builder.create<FIRRTLWireOp>(resultLoc, type, id);
  if (addSymbolEntry(id.getValue(), result, loc))
    return failure();

  return success();
}

//===----------------------------------------------------------------------===//
// FIRModuleParser
//===----------------------------------------------------------------------===//

namespace {
/// This class implements logic and state for parsing module bodies.
struct FIRModuleParser : public FIRScopedParser {
  explicit FIRModuleParser(GlobalFIRParserState &state, CircuitOp circuit)
      : FIRScopedParser(state, symbolTable), circuit(circuit),
        firstScope(symbolTable) {}

  ParseResult parseExtModule(unsigned indent);
  ParseResult parseModule(unsigned indent);

private:
  using PortInfoAndLoc = std::pair<FModuleOp::PortInfo, SMLoc>;
  ParseResult parsePortList(SmallVectorImpl<PortInfoAndLoc> &result,
                            unsigned indent);

  CircuitOp circuit;
  SymbolTable symbolTable;
  SymbolTable::ScopeTy firstScope;
};

} // end anonymous namespace

/// portlist ::= port*
/// port     ::= dir id ':' type info? NEWLINE
/// dir      ::= 'input' | 'output'
///
ParseResult
FIRModuleParser::parsePortList(SmallVectorImpl<PortInfoAndLoc> &result,
                               unsigned indent) {
  // Parse any ports.
  while (getToken().isAny(FIRToken::kw_input, FIRToken::kw_output) &&
         // Must be nested under the module.
         getIndentation() > indent) {
    bool isOutput = getToken().is(FIRToken::kw_output);

    consumeToken();
    StringAttr name;
    FIRRTLType type;
    auto loc = getToken().getLoc();
    if (parseId(name, "expected port name") ||
        parseToken(FIRToken::colon, "expected ':' in port definition") ||
        parseType(type, "expected a type in port declaration") ||
        parseOptionalInfo())
      return failure();

    // If this is an output port, flip the type.
    if (isOutput)
      type = FlipType::get(type);

    result.push_back({{name, type}, loc});
  }

  return success();
}

/// module    ::=
///        'extmodule' id ':' info? INDENT portlist defname? parameter* DEDENT
/// defname   ::= 'defname' '=' id NEWLINE
///
/// parameter ::= 'parameter' id '=' intLit NEWLINE
/// parameter ::= 'parameter' id '=' StringLit NEWLINE
/// parameter ::= 'parameter' id '=' DoubleLit NEWLINE
/// parameter ::= 'parameter' id '=' RawString NEWLINE
ParseResult FIRModuleParser::parseExtModule(unsigned indent) {
  consumeToken(FIRToken::kw_extmodule);
  StringAttr name;
  SmallVector<PortInfoAndLoc, 4> portListAndLoc;

  if (parseId(name, "expected module name") ||
      parseToken(FIRToken::colon, "expected ':' in extmodule definition") ||
      parseOptionalInfo() || parsePortList(portListAndLoc, indent))
    return failure();

  // Add all the ports to the symbol table even though there are no SSA values
  // for arguments in an external module.  This detects multiple definitions
  // of the same name.
  for (auto &entry : portListAndLoc) {
    if (addSymbolEntry(entry.first.first.getValue(), Value(), entry.second))
      return failure();
  }

  // Parse a defname if present.
  if (consumeIf(FIRToken::kw_defname)) {
    StringAttr defName;
    if (parseToken(FIRToken::equal, "expected '=' in defname") ||
        parseId(defName, "expected defname name"))
      return failure();
  }

  // Parse the parameter list.
  while (consumeIf(FIRToken::kw_parameter)) {
    StringAttr paramName;
    if (parseId(paramName, "expected parameter name") ||
        parseToken(FIRToken::equal, "expected '=' in parameter"))
      return failure();
    if (getToken().isAny(FIRToken::integer, FIRToken::string))
      consumeToken();
    else
      return emitError("expected parameter value"), failure();
  }

  return success();
}

/// module ::= 'module' id ':' info? INDENT portlist moduleBlock DEDENT
/// moduleBlock ::= simple_stmt*
///
ParseResult FIRModuleParser::parseModule(unsigned indent) {
  // FIXME: This should be the .firtl loc, and get overriden by the fileinfo.
  auto loc = UnknownLoc::get(getContext());
  StringAttr name;
  SmallVector<PortInfoAndLoc, 4> portListAndLoc;

  consumeToken(FIRToken::kw_module);
  if (parseId(name, "expected module name") ||
      parseToken(FIRToken::colon, "expected ':' in module definition") ||
      parseOptionalInfo() || parsePortList(portListAndLoc, indent))
    return failure();

  auto builder = circuit.getBodyBuilder();

  // Create the module.
  SmallVector<FModuleOp::PortInfo, 4> portList;
  portList.reserve(portListAndLoc.size());
  for (auto &elt : portListAndLoc)
    portList.push_back(elt.first);
  auto fmodule = builder.create<FModuleOp>(loc, name, portList);

  // Install all of the ports into the symbol table, associated with their
  // block arguments.
  auto argIt = fmodule.args_begin();
  for (auto &entry : portListAndLoc) {
    if (addSymbolEntry(entry.first.first.getValue(), *argIt, entry.second))
      return failure();
    ++argIt;
  }

  FIRStmtParser stmtParser(getState(), symbolTable, fmodule.getBodyBuilder());

  // Parse the moduleBlock.
  while (true) {
    unsigned subIndent = getIndentation();
    if (subIndent <= indent)
      return success();

    // The outer level parser can handle these tokens.
    if (getToken().isAny(FIRToken::eof, FIRToken::error))
      return success();

    // Let the statement parser handle this.
    if (stmtParser.parseSimpleStmt(subIndent))
      return failure();
  }
}

//===----------------------------------------------------------------------===//
// FIRCircuitParser
//===----------------------------------------------------------------------===//

namespace {
/// This class implements the outer level of the parser, including things
/// like circuit and module.
struct FIRCircuitParser : public FIRParser {
  explicit FIRCircuitParser(GlobalFIRParserState &state, ModuleOp mlirModule)
      : FIRParser(state), mlirModule(mlirModule) {}

  ParseResult parseCircuit();

private:
  ModuleOp mlirModule;
};

} // end anonymous namespace

/// file ::= circuit
/// circuit ::= 'circuit' id ':' info? INDENT module* DEDENT EOF
///
ParseResult FIRCircuitParser::parseCircuit() {
  unsigned circuitIndent = getIndentation();

  // FIXME: This should be the .firtl loc, and get overriden by the fileinfo.
  auto loc = UnknownLoc::get(getContext());
  StringAttr name;

  // A file must contain a top level `circuit` definition.
  if (parseToken(FIRToken::kw_circuit,
                 "expected a top-level 'circuit' definition") ||
      parseId(name, "expected circuit name") ||
      parseToken(FIRToken::colon, "expected ':' in circuit definition") ||
      parseOptionalInfo())
    return failure();

  // Create the top-level circuit op in the MLIR module.
  OpBuilder b(mlirModule.getBodyRegion());
  auto circuit = b.create<CircuitOp>(loc, name);

  // Parse any contained modules.
  while (true) {
    switch (getToken().getKind()) {
    // If we got to the end of the file, then we're done.
    case FIRToken::eof:
      return success();

    // If we got an error token, then the lexer already emitted an error, just
    // stop.  We could introduce error recovery if there was demand for it.
    case FIRToken::error:
      return failure();

    default:
      emitError("unexpected token in circuit");
      return failure();

    case FIRToken::kw_module:
    case FIRToken::kw_extmodule: {
      unsigned moduleIndent = getIndentation();
      if (moduleIndent <= circuitIndent)
        return emitError("module should be indented more"), failure();

      FIRModuleParser mp(getState(), circuit);
      if (getToken().is(FIRToken::kw_module) ? mp.parseModule(moduleIndent)
                                             : mp.parseExtModule(moduleIndent))
        return failure();
      break;
    }
    }
  }
}

//===----------------------------------------------------------------------===//
// Driver
//===----------------------------------------------------------------------===//

// Parse the specified .fir file into the specified MLIR context.
static OwningModuleRef parseFIRFile(SourceMgr &sourceMgr,
                                    MLIRContext *context) {
  auto sourceBuf = sourceMgr.getMemoryBuffer(sourceMgr.getMainFileID());

  // This is the result module we are parsing into.
  OwningModuleRef module(ModuleOp::create(
      FileLineColLoc::get(sourceBuf->getBufferIdentifier(), /*line=*/0,
                          /*column=*/0, context)));

  auto *dialect = context->getRegisteredDialect<FIRRTLDialect>();
  assert(dialect && "Could not find FIRRTL dialect?");

  GlobalFIRParserState state(sourceMgr, dialect);
  if (FIRCircuitParser(state, *module).parseCircuit())
    return nullptr;

  // Make sure the parse module has no other structural problems detected by
  // the verifier.
  if (failed(verify(*module)))
    return {};

  return module;
}

void spt::firrtl::registerFIRRTLToMLIRTranslation() {
  static TranslateToMLIRRegistration fromLLVM(
      "import-firrtl", [](llvm::SourceMgr &sourceMgr, MLIRContext *context) {
        return parseFIRFile(sourceMgr, context);
      });
}