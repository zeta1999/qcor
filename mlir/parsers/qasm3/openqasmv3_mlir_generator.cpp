#include "openqasmv3_mlir_generator.hpp"

#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "qasm3Lexer.h"
#include "qasm3Parser.h"
#include "qasm3_visitor.hpp"

namespace qcor {

void OpenQasmV3MLIRGenerator::initialize_mlirgen(bool _add_entry_point,
                                                 const std::string function) {
  file_name = function;
  add_entry_point = _add_entry_point;

  m_module = mlir::ModuleOp::create(builder.getUnknownLoc());

  // Useful opaque type defs
  llvm::StringRef qubit_type_name("Qubit"), array_type_name("Array"),
      result_type_name("Result");
  mlir::Identifier dialect = mlir::Identifier::get("quantum", &context);
  qubit_type = mlir::OpaqueType::get(&context, dialect, qubit_type_name);
  array_type = mlir::OpaqueType::get(&context, dialect, array_type_name);
  result_type = mlir::OpaqueType::get(&context, dialect, result_type_name);
  auto int_type = builder.getI32Type();
  auto argv_type =
      mlir::OpaqueType::get(&context, dialect, llvm::StringRef("ArgvType"));
  auto qreg_type =
      mlir::OpaqueType::get(&context, dialect, llvm::StringRef("qreg"));

  if (add_main) {
    std::vector<mlir::Type> arg_types_vec2{};
    auto func_type2 =
        builder.getFunctionType(llvm::makeArrayRef(arg_types_vec2), llvm::None);
    auto proto2 = mlir::FuncOp::create(
        builder.getUnknownLoc(), "__internal_mlir_" + file_name, func_type2);
    mlir::FuncOp function2(proto2);
    auto save_main_entry_block = function2.addEntryBlock();

    if (add_entry_point) {
      std::vector<mlir::Type> arg_types_vec{int_type, argv_type};
      auto func_type =
          builder.getFunctionType(llvm::makeArrayRef(arg_types_vec), int_type);
      auto proto =
          mlir::FuncOp::create(builder.getUnknownLoc(), "main", func_type);
      mlir::FuncOp function(proto);
      main_entry_block = function.addEntryBlock();
      auto &entryBlock = *main_entry_block;
      builder.setInsertionPointToStart(&entryBlock);

      auto main_args = main_entry_block->getArguments();
      builder.create<mlir::quantum::QRTInitOp>(builder.getUnknownLoc(),
                                               main_args[0], main_args[1]);

      // call the function from main, run finalize, and return 0
      builder.create<mlir::CallOp>(builder.getUnknownLoc(), function2);
      builder.create<mlir::quantum::QRTFinalizeOp>(builder.getUnknownLoc());

      auto integer_attr = mlir::IntegerAttr::get(builder.getI32Type(), 0);
      mlir::Value ret_zero = builder.create<mlir::ConstantOp>(
          builder.getUnknownLoc(), integer_attr);
      builder.create<mlir::ReturnOp>(builder.getUnknownLoc(), ret_zero);
      m_module.push_back(function);
      function_names.push_back("main");
    }

    std::vector<mlir::Type> arg_types_vec3{qreg_type};
    auto func_type3 =
        builder.getFunctionType(llvm::makeArrayRef(arg_types_vec3), llvm::None);
    auto proto3 =
        mlir::FuncOp::create(builder.getUnknownLoc(), file_name, func_type3);
    mlir::FuncOp function3(proto3);

    auto tmp = function3.addEntryBlock();
    builder.setInsertionPointToStart(tmp);
    builder.create<mlir::quantum::SetQregOp>(builder.getUnknownLoc(),
                                             tmp->getArguments()[0]);
    builder.create<mlir::CallOp>(builder.getUnknownLoc(), function2);
    builder.create<mlir::quantum::QRTFinalizeOp>(builder.getUnknownLoc());
    builder.create<mlir::ReturnOp>(builder.getUnknownLoc(),
                                   llvm::ArrayRef<mlir::Value>());
    builder.setInsertionPointToStart(save_main_entry_block);

    m_module.push_back(function2);
    m_module.push_back(function3);
    function_names.push_back("__internal_mlir_" + file_name);
    function_names.push_back(file_name);

    main_entry_block = save_main_entry_block;
  }
}

void OpenQasmV3MLIRGenerator::mlirgen(const std::string &src) {
  using namespace antlr4;
  using namespace qasm3;

  visitor = std::make_shared<qasm3_visitor>(builder, m_module, file_name);

  ANTLRInputStream input(src);
  qasm3Lexer lexer(&input);
  CommonTokenStream tokens(&lexer);
  qasm3Parser parser(&tokens);

  class Qasm3ParserErrorListener : public BaseErrorListener {
   private:
    std::string file_name;

   public:
    Qasm3ParserErrorListener(const std::string &s) : file_name(s) {}
    void syntaxError(Recognizer *recognizer, Token *offendingSymbol,
                     size_t line, size_t charPositionInLine,
                     const std::string &msg, std::exception_ptr e) override {
      std::stringstream ss;
      ss << "\n\n[OPENQASM3 MLIRGen] cannot parse the source.\nerror: " << msg
         << "\n";
      ss << "occurred at " << file_name << ":" << line << ":"
         << charPositionInLine
         << ", offending symbol = " << offendingSymbol->getText() << "\n\n";
      std::cout << ss.str();
      exit(1);
    }
  };

  // Setup our custom error listeners
  lexer.removeErrorListeners();
  parser.removeErrorListeners();
  Qasm3ParserErrorListener el(file_name);
  parser.addErrorListener(&el);

  // Get the parse tree and visit
  tree::ParseTree *tree = parser.program();
  visitor->visitChildren(tree);

  return;
}

void OpenQasmV3MLIRGenerator::finalize_mlirgen() {
  auto scoped_symbol_table = visitor->getScopedSymbolTable();
  if (auto b = scoped_symbol_table.get_last_created_block()) {
    builder.setInsertionPointToEnd(b);
  }
  auto all_qalloc_ops =
      scoped_symbol_table.get_global_symbols_of_type<mlir::quantum::QallocOp>();
  for (auto op : all_qalloc_ops) {
    builder.create<mlir::quantum::DeallocOp>(builder.getUnknownLoc(), op);
  }

  // Add any function names that we created.
  auto fnames = scoped_symbol_table.get_seen_function_names();
  for (auto f : fnames) {
    function_names.push_back(f);
  }

  if (add_main) {
    if (auto b = scoped_symbol_table.get_last_created_block()) {

      builder.setInsertionPointToEnd(b);
    } else {
      //   std::cout << "CURRENT BLOCK WAS NULL\n";
      builder.setInsertionPointToEnd(main_entry_block);
    }
    builder.create<mlir::ReturnOp>(builder.getUnknownLoc(),
                                   llvm::ArrayRef<mlir::Value>());
  }
}

}  // namespace qcor